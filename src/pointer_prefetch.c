#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/ioctl.h> // For ioctl
#include <linux/userfaultfd.h> // For UFFDIO_COPY

#include "extmem_test_env/ExtMem/src/core.h" // Provides struct user_page, find_page, PAGE_SIZE
#include "extmem_test_env/ExtMem/src/intercept_layer.h" // For libc_malloc/free/mmap
#include "include/pointer_prefetch.h"
#include "include/storage_rdma.h" // For rdma_read_page (which uses rdma_sim internally)
#include "include/rdma_sim.h"     // For rdma_sim_allocate, rdma_sim_free, rdma_submit_request, rdma_wait_request
#include "extmem_test_env/ExtMem/src/observability.h"  // For LOG, if used

static prefetch_record_t *prefetch_records_head = NULL;
static pthread_mutex_t prefetch_mutex = PTHREAD_MUTEX_INITIALIZER;
static prefetch_stats_t prefetch_stats_g = {0};
static bool prefetch_initialized = false;

static uint64_t max_prefetch_distance_g = MAX_PREFETCH_DISTANCE;
static uint64_t pointer_validation_threshold_g = PREFETCH_VALIDATION_THRESHOLD;

int pointer_prefetch_init(void) {
    pthread_mutex_lock(&prefetch_mutex);
    
    if (prefetch_initialized) {
        pthread_mutex_unlock(&prefetch_mutex);
        return 0;
    }
    
    memset(&prefetch_stats_g, 0, sizeof(prefetch_stats_t));
    prefetch_records_head = NULL;
    prefetch_initialized = true;
    
    pthread_mutex_unlock(&prefetch_mutex);
    LOG("Pointer prefetch system initialized\n");
    return 0;
}

void pointer_prefetch_cleanup(void) {
    pthread_mutex_lock(&prefetch_mutex);
    
    if (!prefetch_initialized) {
        pthread_mutex_unlock(&prefetch_mutex);
        return;
    }
    
    prefetch_record_t *current = prefetch_records_head;
    while (current != NULL) {
        prefetch_record_t *next = current->next;
        if (current->status == PREFETCH_IN_PROGRESS || current->status == PREFETCH_COMPLETED) {
            if(current->remote_addr != 0) { // If a remote buffer was allocated for it
                rdma_sim_free(current->remote_addr);
            }
        }
        free(current);
        current = next;
    }
    prefetch_records_head = NULL;
    prefetch_initialized = false;
    
    pthread_mutex_unlock(&prefetch_mutex);
    LOG("Pointer prefetch system cleaned up\n");
}

uint64_t read_page_end_pointer(struct user_page *page) {
    if (page == NULL || page->va == 0 || !page->in_dram) { // Ensure page is in DRAM
        return 0;
    }
    
    uint64_t pointer_addr = page->va + PAGE_SIZE - POINTER_OFFSET_FROM_END;
    uint64_t pointer_value = *(uint64_t*)pointer_addr;
    
    LOG("Read pointer value 0x%lx from page 0x%lx at offset 0x%lx\n", 
        pointer_value, page->va, (pointer_addr - page->va));
    
    pthread_mutex_lock(&prefetch_mutex);
    prefetch_stats_g.pointer_validations++;
    pthread_mutex_unlock(&prefetch_mutex);
    
    return pointer_value;
}

bool validate_pointer(uint64_t pointer_value, uint64_t current_va) {
    if (pointer_value == 0) {
        LOG("Invalid pointer: NULL\n");
        return false;
    }
    if (pointer_value < pointer_validation_threshold_g) {
        LOG("Invalid pointer: too small (0x%lx vs threshold 0x%lx)\n", pointer_value, pointer_validation_threshold_g);
        pthread_mutex_lock(&prefetch_mutex);
        prefetch_stats_g.invalid_pointers++;
        pthread_mutex_unlock(&prefetch_mutex);
        return false;
    }
    uint64_t distance = (pointer_value > current_va) ? 
                       (pointer_value - current_va) : 
                       (current_va - pointer_value);
    
    if (distance > max_prefetch_distance_g) {
        LOG("Invalid pointer: too far (distance=0x%lx, max=0x%lx)\n", 
            distance, max_prefetch_distance_g);
        pthread_mutex_lock(&prefetch_mutex);
        prefetch_stats_g.invalid_pointers++;
        pthread_mutex_unlock(&prefetch_mutex);
        return false;
    }
    
    // Pointer does not necessarily have to be page aligned, target_va will be.
    LOG("Pointer validation passed for 0x%lx\n", pointer_value);
    return true;
}

// Fetches page content from its current location (existing_page->virtual_offset, assumed RDMA remote)
// into a new dedicated RDMA prefetch buffer (prefetch_dest_remote_addr).
// Updates record with PREFETCH_IN_PROGRESS and the RDMA write request ID.
static int transfer_to_prefetch_buffer(struct user_page* existing_page, uint64_t prefetch_dest_remote_addr, prefetch_record_t* record) {
    void* temp_cpu_buffer = libc_malloc(PAGE_SIZE);
    if (!temp_cpu_buffer) {
        LOG("Failed to allocate temp_cpu_buffer for prefetch transfer\n");
        return -1;
    }

    // Read from original remote location (current swap location) into CPU buffer
    LOG("Prefetch Transfer: Reading from original remote addr 0x%lx into CPU buffer %p\n", existing_page->virtual_offset, temp_cpu_buffer);
    int read_err = rdma_read_page(-1 /*fd unused*/, existing_page->virtual_offset, temp_cpu_buffer, PAGE_SIZE);
    if (read_err != 0) {
        LOG("Prefetch Transfer: rdma_read_page from 0x%lx FAILED\n", existing_page->virtual_offset);
        libc_free(temp_cpu_buffer);
        return -1;
    }
    LOG("Prefetch Transfer: Successfully read from 0x%lx to CPU buffer.\n", existing_page->virtual_offset);

    // Write from CPU buffer to the new dedicated prefetch remote buffer
    LOG("Prefetch Transfer: Writing from CPU buffer %p to new prefetch remote_addr 0x%lx\n", temp_cpu_buffer, prefetch_dest_remote_addr);
    record->rdma_request_id = rdma_submit_request(RDMA_OP_WRITE, 
                                                0 /* local_addr, not used by client if data is not NULL */, 
                                                prefetch_dest_remote_addr, 
                                                PAGE_SIZE, 
                                                temp_cpu_buffer);
    libc_free(temp_cpu_buffer); // Data is copied by rdma_submit_request to its internal shm buffer

    if (record->rdma_request_id == 0) { // Assuming 0 is an invalid/failed request ID
        LOG("Prefetch Transfer: rdma_submit_request (WRITE to prefetch buffer) FAILED\n");
        return -1;
    }
    
    record->status = PREFETCH_IN_PROGRESS;
    LOG("Prefetch Transfer: Submitted RDMA WRITE req_id %lu to prefetch buffer 0x%lx. Status: IN_PROGRESS.\n", record->rdma_request_id, prefetch_dest_remote_addr);
    return 0;
}

int async_prefetch_page(uint64_t target_va, uint64_t source_va) {
    pthread_mutex_lock(&prefetch_mutex);
    prefetch_stats_g.total_attempts++; // Moved here from handle_page_fault_with_prefetch

    struct user_page *existing_page = get_user_page(target_va);
    
    if (existing_page != NULL && !existing_page->swapped_out && existing_page->in_dram) {
        LOG("Target page 0x%lx already in DRAM, skipping prefetch\n", target_va);
        pthread_mutex_unlock(&prefetch_mutex);
        return 0; // Already present
    }

    prefetch_record_t *record = find_prefetch_record(target_va);
    if (record != NULL && (record->status == PREFETCH_IN_PROGRESS || record->status == PREFETCH_COMPLETED)) {
        LOG("Prefetch for 0x%lx already in progress or completed. Status: %d\n", target_va, record->status);
        pthread_mutex_unlock(&prefetch_mutex);
        return 0; // Already being handled or done
    }

    uint64_t prefetch_dest_remote_addr = 0;
    if (rdma_sim_allocate(PAGE_SIZE, &prefetch_dest_remote_addr) != 0) {
        LOG("Failed to allocate remote memory (prefetch_dest_remote_addr) for prefetch of VA 0x%lx\n", target_va);
        prefetch_stats_g.failed_prefetch++;
        pthread_mutex_unlock(&prefetch_mutex);
        return -1;
    }
    LOG("Allocated prefetch_dest_remote_addr: 0x%lx for target VA 0x%lx\n", prefetch_dest_remote_addr, target_va);

    if (record == NULL) {
        if (add_prefetch_record(source_va, target_va, target_va /* as pointer val for now */) != 0) {
            LOG("Failed to add prefetch record for VA 0x%lx\n", target_va);
            rdma_sim_free(prefetch_dest_remote_addr);
            prefetch_stats_g.failed_prefetch++;
            pthread_mutex_unlock(&prefetch_mutex);
            return -1;
        }
        record = find_prefetch_record(target_va); // Get the newly added record
        if (record == NULL) { // Should not happen
             LOG("CRITICAL: Failed to find newly added prefetch record for VA 0x%lx\n", target_va);
             rdma_sim_free(prefetch_dest_remote_addr);
             prefetch_stats_g.failed_prefetch++;
             pthread_mutex_unlock(&prefetch_mutex);
             return -1;
        }
    }
    
    record->remote_addr = prefetch_dest_remote_addr; // Store the dedicated prefetch buffer address
    record->status = PREFETCH_REQUESTED;

    if (existing_page != NULL && existing_page->swapped_out) {
        // Page exists and is on swap (which is RDMA-backed).
        // Transfer its content from existing_page->virtual_offset to record->remote_addr.
        if (transfer_to_prefetch_buffer(existing_page, record->remote_addr, record) != 0) {
            LOG("Prefetch for VA 0x%lx failed during transfer_to_prefetch_buffer.\n", target_va);
            record->status = PREFETCH_FAILED;
            // rdma_sim_free for record->remote_addr will be handled by cleanup if record remains, or explicitly if we remove it
        }
    } else if (existing_page == NULL) {
        // Page does not exist in ExtMem's metadata (e.g., never touched or from unmanaged mmap).
        // This prefetcher currently relies on ExtMem metadata to find the source page on swap.
        // If it's a truly new page not yet in swap, prefetching usually means allocating a zero page.
        // For now, we can't prefetch it if we don't know its source.
        LOG("Cannot prefetch VA 0x%lx: page not found in ExtMem metadata (or not swapped out).\n", target_va);
        record->status = PREFETCH_FAILED;
        rdma_sim_free(record->remote_addr); // Free the allocated prefetch buffer as we can't fill it
        record->remote_addr = 0;
    } else { // existing_page != NULL but not swapped_out (e.g. in_dram but error in initial check)
         LOG("Cannot prefetch VA 0x%lx: page state not suitable (in_dram: %d, swapped_out: %d)\n", target_va, existing_page->in_dram, existing_page->swapped_out);
         record->status = PREFETCH_FAILED;
         rdma_sim_free(record->remote_addr);
         record->remote_addr = 0;
    }

    if (record->status == PREFETCH_IN_PROGRESS) {
        prefetch_stats_g.successful_prefetch++;
        LOG("Successfully initiated prefetch for 0x%lx to remote_addr 0x%lx. RDMA Req ID %lu\n", 
            target_va, record->remote_addr, record->rdma_request_id);
    } else { // Failed or couldn't start
        prefetch_stats_g.failed_prefetch++;
        // Cleanup if record was added but prefetch failed before IN_PROGRESS
        if(record && record->remote_addr == prefetch_dest_remote_addr && prefetch_dest_remote_addr != 0){
             // We might decide to keep the record as FAILED or remove it.
             // For now, if it failed to start transfer, free the remote buffer.
        }
        LOG("Prefetch initiation failed or not applicable for VA 0x%lx. Status: %d\n", target_va, record ? record->status : -1);
    }
    
    pthread_mutex_unlock(&prefetch_mutex);
    return (record && record->status == PREFETCH_IN_PROGRESS) ? 0 : -1;
}

// This function is called by handle_page_fault_with_prefetch.
// It ensures data from record->remote_addr is copied into local_cpu_buffer_for_copy.
// Then, UFFDIO_COPY is performed by the caller.
static int retrieve_prefetched_data_to_local_buffer(prefetch_record_t *record, void* local_cpu_buffer_for_copy) {
    if (record->status == PREFETCH_IN_PROGRESS) {
        LOG("Retrieving page 0x%lx: Prefetch in progress (req_id %lu), waiting...\n", record->target_va, record->rdma_request_id);
        int rdma_error_code = 0;
        int wait_ret = rdma_wait_request(record->rdma_request_id, &rdma_error_code);
        if (wait_ret == 0 && rdma_error_code == 0) {
            record->status = PREFETCH_COMPLETED;
            LOG("Retrieving page 0x%lx: RDMA Write to prefetch buffer completed.\n", record->target_va);
        } else {
            LOG("Retrieving page 0x%lx: RDMA Write to prefetch buffer FAILED (wait_ret %d, err %d).\n", record->target_va, wait_ret, rdma_error_code);
            record->status = PREFETCH_FAILED;
        }
    }

    if (record->status == PREFETCH_COMPLETED) {
        LOG("Retrieving page 0x%lx: Prefetch completed. Reading from prefetch_remote_addr 0x%lx to local_buffer %p\n", 
            record->target_va, record->remote_addr, local_cpu_buffer_for_copy);
        
        // Submit RDMA Read from the dedicated prefetch remote buffer to the provided local CPU buffer
        uint64_t rdma_read_req_id = rdma_submit_request(RDMA_OP_READ, 
                                                      (uint64_t)local_cpu_buffer_for_copy, 
                                                      record->remote_addr, 
                                                      PAGE_SIZE, 
                                                      NULL /* data is NULL for read, local_addr is dest */);
        if (rdma_read_req_id == 0) {
            LOG("Retrieving page 0x%lx: rdma_submit_request (READ from prefetch buffer) FAILED.\n", record->target_va);
            record->status = PREFETCH_FAILED; // Mark as failed again
            return -1;
        }

        int rdma_error_code = 0;
        int wait_ret = rdma_wait_request(rdma_read_req_id, &rdma_error_code);

        if (wait_ret == 0 && rdma_error_code == 0) {
            LOG("Retrieving page 0x%lx: Successfully read from prefetch buffer 0x%lx into local_buffer %p.\n", 
                record->target_va, record->remote_addr, local_cpu_buffer_for_copy);
            // Data is now in local_cpu_buffer_for_copy. UFFDIO_COPY will be done by caller.
            // Free the dedicated prefetch remote buffer as it has served its purpose
            rdma_sim_free(record->remote_addr);
            record->remote_addr = 0; // Mark as freed
            // Keep status as PREFETCH_COMPLETED to indicate it was used, or change to PREFETCH_NONE/remove.
            // For simplicity, let's assume it's consumed. The record might be cleaned up later.
            return 0; // Success
        } else {
            LOG("Retrieving page 0x%lx: rdma_wait_request (READ from prefetch buffer) FAILED (wait_ret %d, err %d).\n", 
                record->target_va, wait_ret, rdma_error_code);
            record->status = PREFETCH_FAILED;
        }
    }
    
    LOG("Retrieving page 0x%lx: Could not retrieve. Status: %d.\n", record->target_va, record->status);
    return -1; // Failure
}

// Called from core.c. Tries to satisfy the fault for 'page->va' from prefetch cache.
// If successful, maps the page and returns 0. Otherwise, returns -1.
int handle_page_fault_with_prefetch(struct user_page *page) {
    if (!prefetch_initialized || page == NULL) {
        return -1;
    }

    // Ensure per-thread buffer_space (from core.c) is allocated for RDMA read and UFFDIO_COPY src
    // This should be allocated by core.c's fault handling path before calling this.
    // We assert it here for safety, though pointer_prefetch.c cannot allocate it itself.
    assert(buffer_space != NULL && "buffer_space (thread-local) must be allocated by caller's path");

    pthread_mutex_lock(&prefetch_mutex); // Protects prefetch_records and stats
    
    prefetch_record_t *record = find_prefetch_record(page->va);

    if (record != NULL && (record->status == PREFETCH_IN_PROGRESS || record->status == PREFETCH_COMPLETED)) {
        LOG("Page 0x%lx found in prefetch records. Status: %d. Attempting retrieval.\n", page->va, record->status);
        
        // retrieve_prefetched_data_to_local_buffer will fill buffer_space
        // It handles mutex internally for record modification if needed during wait.
        // However, to avoid complex nested locking, this outer lock is simpler for now.
        // If retrieve_prefetched_data_to_local_buffer needs to take the prefetch_mutex, this will deadlock.
        // Let's assume retrieve_prefetched_data_to_local_buffer does NOT take prefetch_mutex itself.
        // Or, release and re-acquire around potentially blocking calls.
        // For now: keep it simple, retrieve_prefetched_data_to_local_buffer should not block on prefetch_mutex.
        // It blocks on RDMA ops, not on prefetch_mutex.

        int retrieve_ret = retrieve_prefetched_data_to_local_buffer(record, buffer_space);

        if (retrieve_ret == 0) { // Data is in buffer_space
            LOG("Page 0x%lx retrieved successfully into buffer_space %p. Performing UFFDIO_COPY.\n", page->va, buffer_space);
            struct uffdio_copy copy_op = {
                .dst = (uint64_t)page->va,
                .src = (uint64_t)buffer_space,
                .len = PAGE_SIZE,
                .mode = UFFDIO_COPY_MODE_DONTWAKE 
            };

            if (ioctl(uffd, UFFDIO_COPY, &copy_op) == -1) {
                perror("UFFDIO_COPY failed in handle_page_fault_with_prefetch");
                // If UFFDIO_COPY fails, the page is not mapped. Prefetch attempt failed.
                record->status = PREFETCH_FAILED; // Mark it as failed.
                prefetch_stats_g.failed_prefetch++; // Count as overall failure
                pthread_mutex_unlock(&prefetch_mutex);
                return -1; 
            }
            
            // Page successfully mapped from prefetch buffer
            page->in_dram = true;
            page->swapped_out = false;
            // page->migrating should be set to false by the policy/caller after fault is resolved.
            // For now, to be self-contained for this path:
            page->migrating = false; 

            prefetch_stats_g.cache_hits++;
            LOG("Page 0x%lx successfully mapped from prefetch. Cache hit!\n", page->va);
            
            // Optional: remove the record or mark as consumed
            // For now, retrieve_prefetched_data_to_local_buffer marks remote_addr = 0
            // And cleanup_old_prefetch_records can deal with old COMPLETED/FAILED records.
            
            pthread_mutex_unlock(&prefetch_mutex);
            return 0; // Fault handled by prefetcher
        } else {
            LOG("Page 0x%lx: Failed to retrieve from prefetch cache (status %d after attempt).\n", page->va, record->status);
            // Retrieval failed, record status should be PREFETCH_FAILED.
            prefetch_stats_g.failed_prefetch++; // Count as overall failure
        }
    } else {
        if (record == NULL) {
             LOG("Page 0x%lx not found in prefetch records.\n", page->va);
        } else {
             LOG("Page 0x%lx found in prefetch records but status not suitable for retrieval (Status: %d).\n", page->va, record->status);
        }
    }
    
    pthread_mutex_unlock(&prefetch_mutex);
    return -1; // Fault not handled by prefetcher
}

// Find a prefetch record by target VA
prefetch_record_t* find_prefetch_record(uint64_t target_va) {
    // Assumes prefetch_mutex is held by caller if modification is possible
    prefetch_record_t *current = prefetch_records_head;
    while (current != NULL) {
        if (current->target_va == target_va) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// Add a new prefetch record or update an existing one
int add_prefetch_record(uint64_t source_va, uint64_t target_va, uint64_t pointer_value) {
    // Assumes prefetch_mutex is held by caller
    prefetch_record_t *record = find_prefetch_record(target_va);
    if (record == NULL) {
        record = (prefetch_record_t*)malloc(sizeof(prefetch_record_t));
        if (record == NULL) {
            LOG("Failed to allocate memory for prefetch record\n");
            return -1;
        }
        record->next = prefetch_records_head;
        prefetch_records_head = record;
    }
    
    record->source_va = source_va;
    record->target_va = target_va;
    record->pointer_value = pointer_value;
    record->status = PREFETCH_REQUESTED; // Initial status
    record->timestamp = rdma_get_timestamp(); 
    record->remote_addr = 0; // Will be set by async_prefetch_page if allocation succeeds
    record->rdma_request_id = 0;
    
    LOG("Added/Updated prefetch record for target_va: 0x%lx, source_va: 0x%lx\n", target_va, source_va);
    return 0;
}

// Simple check if a page is marked as completed (data in its prefetch buffer)
// Does not check if the data is *still* in the prefetch buffer (could have been evicted by another prefetch)
// but retrieve_prefetched_page will re-verify.
bool is_prefetched_page_available(uint64_t va) {
    // This function is called from handle_page_fault_with_prefetch which already holds the mutex.
    // No need for separate mutex here.
    prefetch_record_t *record = find_prefetch_record(va);
    if (record != NULL && (record->status == PREFETCH_COMPLETED || record->status == PREFETCH_IN_PROGRESS)) {
        // If IN_PROGRESS, retrieve will wait. If COMPLETED, retrieve will fetch.
        return true;
    }
    return false;
}

// Cleanup old or failed prefetch records (simple time-based for now)
void cleanup_old_prefetch_records(void) {
    pthread_mutex_lock(&prefetch_mutex);
    uint64_t current_time = rdma_get_timestamp();
    uint64_t max_age = 30 * 1000000; // 30 seconds for cleanup

    prefetch_record_t *current = prefetch_records_head;
    prefetch_record_t *prev = NULL;

    while (current != NULL) {
        bool remove_record = false;
        if (current->status == PREFETCH_FAILED) {
            remove_record = true;
        } else if (current->status == PREFETCH_COMPLETED && (current_time - current->timestamp > max_age)) {
            // Prefetched but not used for a long time
            remove_record = true;
        } else if (current->status == PREFETCH_REQUESTED && (current_time - current->timestamp > max_age)) {
            // Stuck in requested state for too long
            current->status = PREFETCH_FAILED; // Mark as failed
            remove_record = true;
        } else if (current->status == PREFETCH_IN_PROGRESS && (current_time - current->timestamp > max_age * 2)) {
            // Stuck in IN_PROGRESS state for too long (e.g. RDMA op lost)
            LOG("Warning: Prefetch record for VA 0x%lx stuck IN_PROGRESS for too long. Marking FAILED.\n", current->target_va);
            current->status = PREFETCH_FAILED;
            // Should also attempt to cancel/cleanup the RDMA operation if possible, though rdma_sim doesn't support cancel.
            remove_record = true;
        }

        if (remove_record) {
            if (current->remote_addr != 0) { // If a remote buffer was associated with it
                rdma_sim_free(current->remote_addr);
                current->remote_addr = 0;
            }
            
            prefetch_record_t *to_free = current;
            if (prev == NULL) { // Removing head
                prefetch_records_head = current->next;
                current = prefetch_records_head;
            } else {
                prev->next = current->next;
                current = current->next;
            }
            free(to_free);
            continue; // current is already advanced
        }
        prev = current;
        current = current->next;
    }
    pthread_mutex_unlock(&prefetch_mutex);
}

void print_prefetch_stats(void) {
    pthread_mutex_lock(&prefetch_mutex);
    LOG_STATS("Prefetch Stats: Attempts: %lu, Successful: %lu, Failed: %lu, Hits: %lu, Invalid Ptrs: %lu, Validations: %lu\n",
              prefetch_stats_g.total_attempts, prefetch_stats_g.successful_prefetch, prefetch_stats_g.failed_prefetch,
              prefetch_stats_g.cache_hits, prefetch_stats_g.invalid_pointers, prefetch_stats_g.pointer_validations);
    pthread_mutex_unlock(&prefetch_mutex);
}

void reset_prefetch_stats(void) {
    pthread_mutex_lock(&prefetch_mutex);
    memset(&prefetch_stats_g, 0, sizeof(prefetch_stats_t));
    pthread_mutex_unlock(&prefetch_mutex);
}

prefetch_stats_t* get_prefetch_stats(void){
    // Caller should handle thread safety if prefetch_stats_g is read while being updated.
    // Or, this function could return a copy under mutex.
    return &prefetch_stats_g;
}

void set_prefetch_distance(uint64_t distance) {
    max_prefetch_distance_g = distance;
}

void set_pointer_validation_threshold(uint64_t threshold) {
    pointer_validation_threshold_g = threshold;
}