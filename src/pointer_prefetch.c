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

#include "core.h" // Provides struct user_page, find_page, PAGE_SIZE
#include "intercept_layer.h" // For libc_malloc/free/mmap
#include "include/pointer_prefetch.h"
#include "include/storage_rdma.h" // For rdma_read_page (which uses rdma_sim internally)
#include "include/rdma_sim.h"     // For rdma_sim_allocate, rdma_sim_free, rdma_submit_request, rdma_wait_request
#include "include/address_translation.h" // For pointer relocation
#include "observability.h"  // For LOG

static prefetch_record_t *prefetch_records_head = NULL;
static pthread_mutex_t prefetch_mutex = PTHREAD_MUTEX_INITIALIZER;
static prefetch_stats_t prefetch_stats_g = {0};
static bool prefetch_initialized = false;

static uint64_t max_prefetch_distance_g = MAX_PREFETCH_DISTANCE;
static uint64_t pointer_validation_threshold_g = PREFETCH_VALIDATION_THRESHOLD;

int pointer_prefetch_init(void) {
    // fprintf(stderr, "DEBUG_PREFETCHER: pointer_prefetch_init() CALLED AND EXECUTING!\n");
    // fflush(stderr);

    pthread_mutex_lock(&prefetch_mutex);
    
    if (prefetch_initialized) {
        pthread_mutex_unlock(&prefetch_mutex);
        return 0;
    }
    
    // Initialize address translator for pointer relocation
    if (address_translator_init() != 0) {
        LOG("Failed to initialize address translator\n");
        pthread_mutex_unlock(&prefetch_mutex);
        return -1;
    }
    
    memset(&prefetch_stats_g, 0, sizeof(prefetch_stats_t));
    prefetch_records_head = NULL;
    prefetch_initialized = true;
    
    pthread_mutex_unlock(&prefetch_mutex);
    LOG("Pointer prefetch system initialized with address translation\n");
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
    
    // Cleanup address translator
    address_translator_cleanup();
    
    pthread_mutex_unlock(&prefetch_mutex);
    LOG("Pointer prefetch system cleaned up\n");
}

uint64_t read_page_end_pointer(struct user_page *page) {
    if (page == NULL || page->va == 0 || !page->in_dram) { // Ensure page is in DRAM
        return 0;
    }
    
    uint64_t pointer_addr = page->va + PAGE_SIZE - POINTER_OFFSET_FROM_END;
    uint64_t raw_pointer_value = *(uint64_t*)pointer_addr;
    
    LOG("Read raw pointer value 0x%lx from page 0x%lx at offset 0x%lx\n", 
        raw_pointer_value, page->va, (pointer_addr - page->va));
    
    pthread_mutex_lock(&prefetch_mutex);
    prefetch_stats_g.pointer_validations++;
    pthread_mutex_unlock(&prefetch_mutex);
    
    if (raw_pointer_value == 0) {
        return 0; 
    }
    
    if (is_valid_remote_pointer(raw_pointer_value)) {
        pointer_conversion_result_t result = convert_remote_to_local_pointer(raw_pointer_value);
        if (result.valid) {
            LOG("Converted remote pointer 0x%lx -> local 0x%lx\n", 
                raw_pointer_value, result.converted_pointer);
            return result.converted_pointer;
        } else {
            LOG("Failed to convert remote pointer 0x%lx\n", raw_pointer_value);
            return 0;
        }
    } else if (is_valid_local_pointer(raw_pointer_value)) {
        LOG("Using local pointer 0x%lx as-is\n", raw_pointer_value);
        return raw_pointer_value;
    } else {
        LOG("Invalid pointer value 0x%lx (neither local nor remote)\n", raw_pointer_value);
        return 0;
    }
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

// Fetches page content from its current location
// into a new dedicated RDMA prefetch buffer .
// Updates record with PREFETCH_IN_PROGRESS and the RDMA write request ID.
static int transfer_to_prefetch_buffer(struct user_page* existing_page, uint64_t prefetch_dest_remote_addr, prefetch_record_t* record) {
    void* temp_cpu_buffer = libc_malloc(PAGE_SIZE);
    if (!temp_cpu_buffer) {
        LOG("Failed to allocate temp_cpu_buffer for prefetch transfer\n");
        return -1;
    }

    // Read from original remote location 
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
    libc_free(temp_cpu_buffer); 

    if (record->rdma_request_id == 0) { 
        LOG("Prefetch Transfer: rdma_submit_request (WRITE to prefetch buffer) FAILED\n");
        return -1;
    }
    
    record->status = PREFETCH_IN_PROGRESS;
    LOG("Prefetch Transfer: Submitted RDMA WRITE req_id %lu to prefetch buffer 0x%lx. Status: IN_PROGRESS.\n", record->rdma_request_id, prefetch_dest_remote_addr);
    return 0;
}

int async_prefetch_page(uint64_t target_va, uint64_t source_va) {
    pthread_mutex_lock(&prefetch_mutex);
    prefetch_stats_g.total_attempts++; // Count all attempts

    struct user_page *existing_page = get_user_page(target_va);
    
    if (existing_page != NULL && !existing_page->swapped_out && existing_page->in_dram) {
        LOG("Target page 0x%lx already in DRAM, skipping prefetch\n", target_va);
        pthread_mutex_unlock(&prefetch_mutex);
        return 0; // Already present, no need to prefetch
    }
    
    if (existing_page != NULL && existing_page->migrating) {
        LOG("Target page 0x%lx is currently being migrated, skipping prefetch\n", target_va);
        pthread_mutex_unlock(&prefetch_mutex);
        return 0; 
    }

    prefetch_record_t *record = find_prefetch_record(target_va);
    if (record != NULL && (record->status == PREFETCH_IN_PROGRESS || record->status == PREFETCH_COMPLETED)) {
        LOG("Prefetch for 0x%lx already in progress or completed. Status: %d\n", target_va, record->status);
        pthread_mutex_unlock(&prefetch_mutex);
        return 0; 
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
        // Page exists and is swapped out 
        LOG("Prefetching swapped out page 0x%lx from virtual_offset 0x%lx\n", target_va, existing_page->virtual_offset);
        if (transfer_to_prefetch_buffer(existing_page, record->remote_addr, record) != 0) {
            LOG("Prefetch for VA 0x%lx failed during transfer_to_prefetch_buffer.\n", target_va);
            record->status = PREFETCH_FAILED;
        }
    } else if (existing_page == NULL) {
        // Page not in ExtMem metadata 
        LOG("Target page 0x%lx not found in ExtMem metadata, cannot prefetch\n", target_va);
        record->status = PREFETCH_FAILED;
        rdma_sim_free(record->remote_addr);
        record->remote_addr = 0;
    } else {
        // Page exists but not swapped out
        LOG("Target page 0x%lx exists but not in swappable state (in_dram: %d, swapped_out: %d)\n", 
            target_va, existing_page->in_dram, existing_page->swapped_out);
        record->status = PREFETCH_FAILED;
        rdma_sim_free(record->remote_addr);
        record->remote_addr = 0;
    }

    if (record->status == PREFETCH_IN_PROGRESS) {
        prefetch_stats_g.successful_prefetch++;
        LOG("Successfully initiated prefetch for 0x%lx to remote_addr 0x%lx. RDMA Req ID %lu\n", 
            target_va, record->remote_addr, record->rdma_request_id);
    } else { 
        prefetch_stats_g.failed_prefetch++;
        if(record && record->remote_addr == prefetch_dest_remote_addr && prefetch_dest_remote_addr != 0){
            
        }
        LOG("Prefetch initiation failed or not applicable for VA 0x%lx. Status: %d\n", target_va, record ? record->status : -1);
    }
    
    pthread_mutex_unlock(&prefetch_mutex);
    return (record && record->status == PREFETCH_IN_PROGRESS) ? 0 : -1;
}

// called by handle_page_fault_with_prefetch.
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
                                                      NULL );
        if (rdma_read_req_id == 0) {
            LOG("Retrieving page 0x%lx: rdma_submit_request (READ from prefetch buffer) FAILED.\n", record->target_va);
            record->status = PREFETCH_FAILED; 
            return -1;
        }

        int rdma_error_code = 0;
        int wait_ret = rdma_wait_request(rdma_read_req_id, &rdma_error_code);

        if (wait_ret == 0 && rdma_error_code == 0) {
            LOG("Retrieving page 0x%lx: Successfully read from prefetch buffer 0x%lx into local_buffer %p.\n", 
                record->target_va, record->remote_addr, local_cpu_buffer_for_copy);
            
            rdma_sim_free(record->remote_addr);
            record->remote_addr = 0; 
            return 0; 
        } else {
            LOG("Retrieving page 0x%lx: rdma_wait_request (READ from prefetch buffer) FAILED (wait_ret %d, err %d).\n", 
                record->target_va, wait_ret, rdma_error_code);
            record->status = PREFETCH_FAILED;
        }
    }
    
    LOG("Retrieving page 0x%lx: Could not retrieve. Status: %d.\n", record->target_va, record->status);
    return -1; 
}

// Called from core.c.
int handle_page_fault_with_prefetch(struct user_page *page) {
    if (!prefetch_initialized || page == NULL) {
        return -1;
    }

   
    assert(buffer_space != NULL && "buffer_space (thread-local) must be allocated by caller's path");

    pthread_mutex_lock(&prefetch_mutex); // Protects prefetch_records and stats
    
    prefetch_record_t *record = find_prefetch_record(page->va);

    if (record != NULL && (record->status == PREFETCH_IN_PROGRESS || record->status == PREFETCH_COMPLETED)) {
        LOG("Page 0x%lx found in prefetch records. Status: %d. Attempting retrieval.\n", page->va, record->status);
        

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
               
                record->status = PREFETCH_FAILED; // Mark it as failed.
                prefetch_stats_g.failed_prefetch++; 
                pthread_mutex_unlock(&prefetch_mutex);
                return -1; 
            }
            
        
            page->in_dram = true;
            page->swapped_out = false;
            page->migrating = false; 

            prefetch_stats_g.cache_hits++;
            LOG("Page 0x%lx successfully mapped from prefetch. Cache hit!\n", page->va);
            
            
            pthread_mutex_unlock(&prefetch_mutex);
            return 0; 
        } else {
            LOG("Page 0x%lx: Failed to retrieve from prefetch cache (status %d after attempt).\n", page->va, record->status);
            prefetch_stats_g.failed_prefetch++; 
        }
    } else {
        if (record == NULL) {
             LOG("Page 0x%lx not found in prefetch records.\n", page->va);
        } else {
             LOG("Page 0x%lx found in prefetch records but status not suitable for retrieval (Status: %d).\n", page->va, record->status);
        }
    }
    
    pthread_mutex_unlock(&prefetch_mutex);
    return -1; 
}

prefetch_record_t* find_prefetch_record(uint64_t target_va) {
    prefetch_record_t *current = prefetch_records_head;
    while (current != NULL) {
        if (current->target_va == target_va) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

int add_prefetch_record(uint64_t source_va, uint64_t target_va, uint64_t pointer_value) {
   
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
    record->remote_addr = 0; 
    record->rdma_request_id = 0;
    
    LOG("Added/Updated prefetch record for target_va: 0x%lx, source_va: 0x%lx\n", target_va, source_va);
    return 0;
}


bool is_prefetched_page_available(uint64_t va) {
    prefetch_record_t *record = find_prefetch_record(va);
    if (record != NULL && (record->status == PREFETCH_COMPLETED || record->status == PREFETCH_IN_PROGRESS)) {
        return true;
    }
    return false;
}


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
            remove_record = true;
        } else if (current->status == PREFETCH_REQUESTED && (current_time - current->timestamp > max_age)) {
            current->status = PREFETCH_FAILED; 
            remove_record = true;
        } else if (current->status == PREFETCH_IN_PROGRESS && (current_time - current->timestamp > max_age * 2)) {
            
            LOG("Warning: Prefetch record for VA 0x%lx stuck IN_PROGRESS for too long. Marking FAILED.\n", current->target_va);
            current->status = PREFETCH_FAILED;
            remove_record = true;
        }

        if (remove_record) {
            if (current->remote_addr != 0) { 
                rdma_sim_free(current->remote_addr);
                current->remote_addr = 0;
            }
            
            prefetch_record_t *to_free = current;
            if (prev == NULL) {
                prefetch_records_head = current->next;
                current = prefetch_records_head;
            } else {
                prev->next = current->next;
                current = current->next;
            }
            free(to_free);
            continue; 
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
    return &prefetch_stats_g;
}

void set_prefetch_distance(uint64_t distance) {
    max_prefetch_distance_g = distance;
}

void set_pointer_validation_threshold(uint64_t threshold) {
    pointer_validation_threshold_g = threshold;
}

int convert_pointers_for_remote_storage(void* page_data, uint64_t page_va) {
    if (page_data == NULL) {
        return -1;
    }
    
    uint64_t* pointer_addr = (uint64_t*)((char*)page_data + PAGE_SIZE - POINTER_OFFSET_FROM_END);
    uint64_t local_pointer = *pointer_addr;
    
    LOG("Converting pointer for remote storage: page_va=0x%lx, pointer=0x%lx\n", 
        page_va, local_pointer);
    
    if (local_pointer == 0) {
        return 0;
    }
    
    if (is_valid_local_pointer(local_pointer)) {
        pointer_conversion_result_t result = convert_local_to_remote_pointer(local_pointer);
        if (result.valid) {
            *pointer_addr = result.converted_pointer;
            LOG("Converted local pointer 0x%lx -> remote 0x%lx for storage\n", 
                local_pointer, result.converted_pointer);
            return 0;
        } else {
            LOG("Failed to convert local pointer 0x%lx for remote storage\n", local_pointer);
            *pointer_addr = 0;
            return -1;
        }
    }
    LOG("Pointer 0x%lx kept as-is for remote storage\n", local_pointer);
    return 0;
}

int convert_pointers_from_remote_storage(void* page_data, uint64_t page_va) {
    if (page_data == NULL) {
        return -1;
    }
    
    uint64_t* pointer_addr = (uint64_t*)((char*)page_data + PAGE_SIZE - POINTER_OFFSET_FROM_END);
    uint64_t remote_pointer = *pointer_addr;
    
    LOG("Converting pointer from remote storage: page_va=0x%lx, pointer=0x%lx\n", 
        page_va, remote_pointer);
    
    if (remote_pointer == 0) {
        return 0;
    }
    
    if (is_valid_remote_pointer(remote_pointer)) {
        pointer_conversion_result_t result = convert_remote_to_local_pointer(remote_pointer);
        if (result.valid) {
            *pointer_addr = result.converted_pointer;
            LOG("Converted remote pointer 0x%lx -> local 0x%lx from storage\n", 
                remote_pointer, result.converted_pointer);
            return 0;
        } else {
            LOG("Failed to convert remote pointer 0x%lx from storage\n", remote_pointer);
            *pointer_addr = 0;
            return -1;
        }
    }
    
    LOG("Pointer 0x%lx kept as-is from remote storage\n", remote_pointer);
    return 0;
}