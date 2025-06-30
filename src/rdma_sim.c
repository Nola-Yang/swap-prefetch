#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/time.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "include/rdma_sim.h"

// Forward declaration
static int rdma_server_handle_request(rdma_request_t* req);

static rdma_shm_t* rdma_shm = NULL;
static int shmid = -1; 
static bool is_initialized = false; // Local init flag for client/server status
static atomic_uint_fast64_t request_id_counter = 1; // Start from 1, 0 can mean error

#define SIM_REMOTE_MEMORY_SIZE (1ULL * 1024 * 1024 * 1024)  
#define SIM_REMOTE_MEMORY_ALIGN 4096

typedef struct mem_block {
    uint64_t addr;
    size_t size;
    bool free;
    struct mem_block* next;
} mem_block_t;

static mem_block_t* sim_remote_memory_blocks = NULL;
static pthread_mutex_t sim_mem_alloc_mutex = PTHREAD_MUTEX_INITIALIZER;
static char* sim_remote_memory_ptr = NULL; // Pointer to the simulated server's memory block

uint64_t rdma_get_timestamp(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000ULL + tv.tv_usec;
}

static int init_sim_remote_memory_allocator(void) {
    pthread_mutex_lock(&sim_mem_alloc_mutex);
    
    if (sim_remote_memory_blocks == NULL) {
        // Allocate the large block for the server simulation first
        sim_remote_memory_ptr = (char*)malloc(SIM_REMOTE_MEMORY_SIZE);
        if (sim_remote_memory_ptr == NULL) {
            pthread_mutex_unlock(&sim_mem_alloc_mutex);
            perror("Failed to allocate simulated remote memory block");
            return -1;
        }
        memset(sim_remote_memory_ptr, 0, SIM_REMOTE_MEMORY_SIZE); // Initialize

        sim_remote_memory_blocks = (mem_block_t*)malloc(sizeof(mem_block_t));
        if (sim_remote_memory_blocks == NULL) {
            free(sim_remote_memory_ptr);
            sim_remote_memory_ptr = NULL;
            pthread_mutex_unlock(&sim_mem_alloc_mutex);
            return -1;
        }
        
        // This address is conceptual for the allocator; actual data is in sim_remote_memory_ptr
        sim_remote_memory_blocks->addr = 0x4000000000ULL; // Arbitrary large base for conceptual addresses
        sim_remote_memory_blocks->size = SIM_REMOTE_MEMORY_SIZE;
        sim_remote_memory_blocks->free = true;
        sim_remote_memory_blocks->next = NULL;
        
        
        if (rdma_shm) { 
             rdma_shm->remote_memory_base = sim_remote_memory_blocks->addr; 
             rdma_shm->remote_memory_size = SIM_REMOTE_MEMORY_SIZE; 
             atomic_store(&rdma_shm->remote_memory_used, 0);
        }
    }
    
    pthread_mutex_unlock(&sim_mem_alloc_mutex);
    return 0;
}

int rdma_sim_allocate(size_t size, uint64_t* remote_addr) {
    if (remote_addr == NULL || size == 0) return EINVAL;
    if (rdma_shm == NULL || !atomic_load(&rdma_shm->initialized)) return ENOTCONN;

    size = (size + SIM_REMOTE_MEMORY_ALIGN - 1) & ~(SIM_REMOTE_MEMORY_ALIGN - 1);
    
    pthread_mutex_lock(&sim_mem_alloc_mutex);
    
    mem_block_t* current = sim_remote_memory_blocks;
    while (current != NULL) {
        if (current->free && current->size >= size) {
            *remote_addr = current->addr;
            if (current->size > size) {
                mem_block_t* new_block = (mem_block_t*)malloc(sizeof(mem_block_t));
                if (new_block != NULL) {
                    new_block->addr = current->addr + size;
                    new_block->size = current->size - size;
                    new_block->free = true;
                    new_block->next = current->next;
                    current->next = new_block;
                }
            }
            current->size = size;
            current->free = false;
            atomic_fetch_add(&rdma_shm->remote_memory_used, size);
            pthread_mutex_unlock(&sim_mem_alloc_mutex);
            return 0;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&sim_mem_alloc_mutex);
    return ENOMEM;
}

int rdma_sim_free(uint64_t remote_addr) {
    if (rdma_shm == NULL || !atomic_load(&rdma_shm->initialized)) return ENOTCONN;

    pthread_mutex_lock(&sim_mem_alloc_mutex);
    
    mem_block_t* current = sim_remote_memory_blocks;
    mem_block_t* prev = NULL;
    while (current != NULL) {
        if (!current->free && current->addr == remote_addr) {
            current->free = true;
            atomic_fetch_sub(&rdma_shm->remote_memory_used, current->size);
            
            // Try to merge with next block
            if (current->next != NULL && current->next->free && 
                current->addr + current->size == current->next->addr) {
                mem_block_t* to_free = current->next;
                current->size += to_free->size;
                current->next = to_free->next;
                free(to_free);
            }
            
            // Try to merge with previous block
            if (prev != NULL && prev->free && 
                prev->addr + prev->size == current->addr) {
                prev->size += current->size;
                prev->next = current->next;
                free(current);
                current = prev; // current block is now prev, which absorbed it
            }
            
            pthread_mutex_unlock(&sim_mem_alloc_mutex);
            return 0;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&sim_mem_alloc_mutex);
    return EINVAL;
}

rdma_shm_t* get_rdma_shm(void) {
    if (rdma_shm == NULL) {
        fprintf(stderr, "GET_RDMA_SHM (PID %d): Shared memory not initialized by this process yet. Attempting client init...\n", getpid());
        if (rdma_sim_init(false /*is_server*/) != 0) {
            fprintf(stderr, "GET_RDMA_SHM (PID %d): Client-side rdma_sim_init failed within get_rdma_shm.\n", getpid());
            return NULL; 
        }
        fprintf(stderr, "GET_RDMA_SHM (PID %d): Client-side rdma_sim_init succeeded. rdma_shm: %p\n", getpid(), (void*)rdma_shm);
    }
    return rdma_shm;
}

int rdma_sim_init(bool is_server) {
    if (is_initialized && rdma_shm != NULL && atomic_load(&rdma_shm->initialized)) {
        return 0; // Already initialized correctly
    }
    
    if (is_server) {
        fprintf(stderr, "RDMA_SIM_SERVER: Initializing shared memory (key: %d)...\n", RDMA_SHM_KEY);
        shmid = shmget(RDMA_SHM_KEY, sizeof(rdma_shm_t), IPC_CREAT | 0666);
        if (shmid == -1) {
            perror("RDMA_SIM_SERVER: shmget failed");
            fprintf(stderr, "RDMA_SIM_SERVER: shmget errno: %d\n", errno);
            return -1;
        }
        fprintf(stderr, "RDMA_SIM_SERVER: shmget successful, shmid: %d\n", shmid);

        void* shm_ptr = shmat(shmid, NULL, 0);
        if (shm_ptr == (void*)-1) {
            perror("RDMA_SIM_SERVER: shmat failed");
            fprintf(stderr, "RDMA_SIM_SERVER: shmat errno: %d\n", errno);
            shmctl(shmid, IPC_RMID, NULL);
            return -1;
        }
        fprintf(stderr, "RDMA_SIM_SERVER: shmat successful, shm_ptr: %p\n", shm_ptr);

        rdma_shm = (rdma_shm_t*)shm_ptr;
        fprintf(stderr, "RDMA_SIM_SERVER: rdma_shm structure mapped at %p\n", (void*)rdma_shm);
        
        memset(rdma_shm, 0, sizeof(rdma_shm_t)); // Zero out the entire structure first

        pthread_mutexattr_t mutex_attr;
        pthread_mutexattr_init(&mutex_attr);
        pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&rdma_shm->queue_mutex, &mutex_attr);
        pthread_mutexattr_destroy(&mutex_attr);

        pthread_condattr_t cond_attr;
        pthread_condattr_init(&cond_attr);
        pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
        pthread_cond_init(&rdma_shm->queue_cond, &cond_attr);
        pthread_condattr_destroy(&cond_attr);
        
        // Initialize atomic variables for the queue management
        atomic_init(&rdma_shm->head, 0);
        atomic_init(&rdma_shm->tail, 0);
        atomic_init(&rdma_shm->count, 0);

        atomic_init(&rdma_shm->initialized, false); // Server will set to true after full init
        atomic_init(&rdma_shm->shutdown_requested, false);

        // Initialize statistics and other atomic fields
        atomic_init(&rdma_shm->remote_memory_used, 0);
        atomic_init(&rdma_shm->total_requests, 0);
        atomic_init(&rdma_shm->completed_requests, 0);
        atomic_init(&rdma_shm->failed_requests, 0);
        atomic_init(&rdma_shm->page_buffer_used, 0);

        // Initialize simulated remote memory allocator (server only)
        if (init_sim_remote_memory_allocator() != 0) {
             fprintf(stderr, "RDMA_SIM_SERVER: Failed to initialize simulated remote memory allocator.\n");
             shmdt(shm_ptr);
             shmctl(shmid, IPC_RMID, NULL);
             rdma_shm = NULL;
             return -1;
        }
        fprintf(stderr, "RDMA_SIM_SERVER: Simulated remote memory initialized, sim_remote_memory_ptr: %p\n", sim_remote_memory_ptr);

        atomic_store(&rdma_shm->initialized, true); // Mark shm as initialized by server
        fprintf(stderr, "RDMA_SIM_SERVER: Initialization complete. Shared memory marked as initialized.\n");

    } else { // Client
        fprintf(stderr, "RDMA_SIM_CLIENT (PID %d): Attaching to shared memory (key: %d)...\n", getpid(), RDMA_SHM_KEY);
        shmid = shmget(RDMA_SHM_KEY, 0, 0);
        if (shmid == -1) {
            perror("RDMA_SIM_CLIENT: shmget failed to find/access segment");
            fprintf(stderr, "RDMA_SIM_CLIENT: shmget errno: %d (%s)\n", errno, strerror(errno));
            return -1;
        }
        fprintf(stderr, "RDMA_SIM_CLIENT (PID %d): shmget successful, shmid: %d\n", getpid(), shmid);

        void* shm_ptr = shmat(shmid, NULL, 0);
        if (shm_ptr == (void*)-1) {
            perror("RDMA_SIM_CLIENT: shmat failed");
            fprintf(stderr, "RDMA_SIM_CLIENT: shmat errno: %d (%s)\n", errno, strerror(errno));
            return -1;
        }
        fprintf(stderr, "RDMA_SIM_CLIENT (PID %d): shmat successful, shm_ptr: %p\n", getpid(), shm_ptr);
        
        rdma_shm = (rdma_shm_t*)shm_ptr;
        fprintf(stderr, "RDMA_SIM_CLIENT (PID %d): rdma_shm structure mapped at %p\n", getpid(), (void*)rdma_shm);

        int timeout = 200; // 20 seconds (200 * 100ms)
        bool server_initialized = false;
        fprintf(stderr, "RDMA_SIM_CLIENT (PID %d): Waiting for server to initialize shared memory (timeout %d s)...\n", getpid(), timeout/10);
        while (timeout > 0) {
            if (atomic_load(&rdma_shm->initialized)) {
                server_initialized = true;
                break;
            }
            usleep(100000); 
            timeout--;
            if(timeout % 50 == 0 && timeout > 0) { // Print every 5 seconds
                 fprintf(stderr, "RDMA_SIM_CLIENT (PID %d): Still waiting for server init (%d s remaining)...\n", getpid(), timeout/10);
            }
        }
        if (!server_initialized) {
            fprintf(stderr, "RDMA_SIM_CLIENT (PID %d): Timeout waiting for RDMA server to initialize shared memory.\n", getpid());
            shmdt(shm_ptr);
            rdma_shm = NULL;
            return -1; 
        }
        fprintf(stderr, "RDMA_SIM_CLIENT (PID %d): Server is initialized. Client attachment successful.\n", getpid());
    }
    
    is_initialized = true; // Local flag
    return 0;
}


void rdma_sim_cleanup(bool is_server) {
    fprintf(stderr, "RDMA_SIM_CLEANUP (PID %d, is_server: %d): Starting cleanup.\n", getpid(), is_server);
    if (rdma_shm != NULL) {
        if (is_server) {
            atomic_store(&rdma_shm->shutdown_requested, true);
            // Potentially signal or join server processing thread if it's more complex
            fprintf(stderr, "RDMA_SIM_SERVER: Shutdown requested. Detaching and removing shared memory (shmid: %d)...\n", shmid);
        }
        if (shmdt(rdma_shm) == -1) {
            perror("RDMA_SIM_CLEANUP: shmdt failed");
        } else {
            fprintf(stderr, "RDMA_SIM_CLEANUP (PID %d, is_server: %d): shmdt successful.\n", getpid(), is_server);
        }
        rdma_shm = NULL;
    }

    if (is_server && shmid != -1) {
        if (shmctl(shmid, IPC_RMID, NULL) == -1) {
            perror("RDMA_SIM_SERVER: shmctl IPC_RMID failed");
        } else {
            fprintf(stderr, "RDMA_SIM_SERVER: shmctl IPC_RMID successful for shmid: %d.\n", shmid);
        }
        shmid = -1;
    }
    
    if (is_server && sim_remote_memory_ptr != NULL) {
        fprintf(stderr, "RDMA_SIM_SERVER: Freeing simulated remote memory at %p.\n", sim_remote_memory_ptr);
        free(sim_remote_memory_ptr);
        sim_remote_memory_ptr = NULL;
    }
    fprintf(stderr, "RDMA_SIM_CLEANUP (PID %d, is_server: %d): Cleanup finished.\n", getpid(), is_server);
    is_initialized = false;
}

uint64_t rdma_submit_request(rdma_op_type_e op_type, uint64_t local_addr, 
                            uint64_t remote_addr, size_t size, void* data) {
    if (rdma_shm == NULL || !atomic_load(&rdma_shm->initialized)) {
        fprintf(stderr, "DEBUG: rdma_submit_request failed - shm not initialized\n");
        return 0; 
    }
    
    pthread_mutex_lock(&rdma_shm->queue_mutex);
    
    uint32_t count = atomic_load(&rdma_shm->count);
    if (count >= MAX_RDMA_REQUESTS) {
        fprintf(stderr, "DEBUG: rdma_submit_request failed - queue full (%u >= %d)\n", count, MAX_RDMA_REQUESTS);
        pthread_mutex_unlock(&rdma_shm->queue_mutex);
        return 0; 
    }
    
    uint64_t req_id = atomic_fetch_add(&request_id_counter, 1);
    
    uint32_t index = atomic_load(&rdma_shm->tail);
    rdma_request_t* req = &rdma_shm->requests[index];
    
    req->request_id = req_id;
    req->op_type = op_type;
    req->local_addr = local_addr;
    req->remote_addr = remote_addr;
    req->size = size;
    atomic_store(&req->status, RDMA_REQ_PENDING);
    req->error_code = 0;
    req->timestamp = rdma_get_timestamp();
    req->data_in_shm_buffer = false;
    req->buffer_offset = 0; 

    if (op_type == RDMA_OP_WRITE && data != NULL && size > 0) {
        if (size <= RDMA_PAGE_BUFFER_SIZE) {
            memcpy(rdma_shm->page_buffer, data, size); // Copy to start of page_buffer
            req->data_in_shm_buffer = true;
            // buffer_offset remains 0, data is at start of page_buffer
            atomic_store(&rdma_shm->page_buffer_used, size); 
        } else {
            fprintf(stderr, "RDMA_OP_WRITE data size (%zu) > page_buffer size (%d)\n", size, RDMA_PAGE_BUFFER_SIZE);
            pthread_mutex_unlock(&rdma_shm->queue_mutex);
            return 0;
        }
    }
    
    atomic_store(&rdma_shm->tail, (index + 1) % MAX_RDMA_REQUESTS);
    uint32_t new_count = atomic_fetch_add(&rdma_shm->count, 1) + 1;
    atomic_fetch_add(&rdma_shm->total_requests, 1);
    
    // Force memory barrier to ensure all writes are visible
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    
    fprintf(stderr, "DEBUG: Request queued. Queue state: head=%u, tail=%u, count=%u\n", 
            atomic_load(&rdma_shm->head), atomic_load(&rdma_shm->tail), new_count);
    fprintf(stderr, "DEBUG: About to signal condition variable (count=%u)\n", new_count);
    
    // Broadcast to wake up all waiting threads/processes
    pthread_cond_broadcast(&rdma_shm->queue_cond);
    fprintf(stderr, "DEBUG: Condition variable broadcast successfully\n");
    pthread_mutex_unlock(&rdma_shm->queue_mutex);
    
    return req_id;
}

int rdma_wait_request(uint64_t request_id, int* error_code) {
    if (rdma_shm == NULL || !atomic_load(&rdma_shm->initialized)) {
        if(error_code) *error_code = ENOTCONN;
        return -1;
    }
    if (request_id == 0) { // Invalid request ID
        if(error_code) *error_code = EINVAL;
        return -1;
    }

    int timeout_ms = 10000; 
    int poll_interval_us = 1000; 
    int attempts = timeout_ms * 1000 / poll_interval_us;

    for(int k=0; k < attempts; k++) {
        rdma_request_t* req_ptr = NULL;
        
        for (uint32_t i = 0; i < MAX_RDMA_REQUESTS; i++) { 
            
            if (rdma_shm->requests[i].request_id == request_id && atomic_load(&rdma_shm->requests[i].status) != 0 /*not empty or recycled*/) {
                req_ptr = &rdma_shm->requests[i];
                break;
            }
        }

        if (!req_ptr) {
            if(error_code) *error_code = ENOENT; // No such request (or already gone)
            
             if (k > attempts / 2) { // If not found after some time, assume it's a bad ID
                return -1;
            }
            usleep(poll_interval_us); // Wait and retry search
            continue;
        }

        rdma_req_status_e status = (rdma_req_status_e)atomic_load(&req_ptr->status);
        
        if (status == RDMA_REQ_COMPLETED) {
            if(error_code) *error_code = req_ptr->error_code; 
            
            if (req_ptr->op_type == RDMA_OP_READ && req_ptr->local_addr != 0 && req_ptr->size > 0) {
                 if (req_ptr->size <= RDMA_PAGE_BUFFER_SIZE - req_ptr->buffer_offset) { // Check against remaining buffer
                    memcpy((void*)req_ptr->local_addr, rdma_shm->page_buffer + req_ptr->buffer_offset, req_ptr->size);
                 } else {
                    fprintf(stderr, "RDMA_OP_READ size %zu too large for shm buffer (offset %zu) in wait_request\n", req_ptr->size, req_ptr->buffer_offset);
                    if(error_code && *error_code==0) *error_code = EMSGSIZE; 
                    return -1;
                 }
            }
            
            return 0; 
        } else if (status == RDMA_REQ_FAILED) {
            if(error_code) *error_code = req_ptr->error_code;
            return -1; 
        }
        
        usleep(poll_interval_us); 
    }
    
    if(error_code) *error_code = ETIMEDOUT;
    return -1;
}

int rdma_server_process_requests(void) {
    if (rdma_shm == NULL || !atomic_load(&rdma_shm->initialized)) {
        return -1;
    }
    
    while (!atomic_load(&rdma_shm->shutdown_requested)) {
        pthread_mutex_lock(&rdma_shm->queue_mutex);
        
        while (atomic_load(&rdma_shm->count) == 0 && !atomic_load(&rdma_shm->shutdown_requested)) {
            fprintf(stderr, "DEBUG_SERVER: Waiting for requests (count=%u)...\n", atomic_load(&rdma_shm->count));
            
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1; 
            int wait_ret = pthread_cond_timedwait(&rdma_shm->queue_cond, &rdma_shm->queue_mutex, &ts);
            if (wait_ret != 0 && wait_ret != ETIMEDOUT) {
                fprintf(stderr, "DEBUG_SERVER: pthread_cond_timedwait failed: %d\n", wait_ret);
            }
            
            // Force memory barrier and re-check count after waking up
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            uint32_t current_count = atomic_load(&rdma_shm->count);
            if (current_count > 0) {
                fprintf(stderr, "DEBUG_SERVER: Detected requests after wakeup (count=%u)\n", current_count);
                break;
            }
        }
        
        if (atomic_load(&rdma_shm->shutdown_requested)) {
            pthread_mutex_unlock(&rdma_shm->queue_mutex);
            break;
        }
        
        while (atomic_load(&rdma_shm->count) > 0) {
            uint32_t index = atomic_load(&rdma_shm->head);
            rdma_request_t* req = &rdma_shm->requests[index];
            
            fprintf(stderr, "DEBUG_SERVER: Processing request at index %u, id=%lu, op=%d\n", 
                    index, req->request_id, req->op_type);
            
            // Atomically try to change PENDING to PROCESSING
            int expected_status = RDMA_REQ_PENDING;
            if (atomic_compare_exchange_strong(&req->status, &expected_status, RDMA_REQ_PROCESSING)) {
                
                req->error_code = 0; 
                int result = rdma_server_handle_request(req);
                
                if (result == 0) {
                    atomic_store(&req->status, RDMA_REQ_COMPLETED);
                    atomic_fetch_add(&rdma_shm->completed_requests, 1);
                    fprintf(stderr, "DEBUG_SERVER: Request %lu completed successfully\n", req->request_id);
                } else {
                    req->error_code = result; 
                    atomic_store(&req->status, RDMA_REQ_FAILED);
                    atomic_fetch_add(&rdma_shm->failed_requests, 1);
                    fprintf(stderr, "DEBUG_SERVER: Request %lu failed with error %d\n", req->request_id, result);
                }
            } else if (expected_status == RDMA_REQ_PROCESSING) {
                
                fprintf(stderr, "DEBUG_SERVER: Request %lu already being processed\n", req->request_id);
            } else {
                
                fprintf(stderr, "DEBUG_SERVER: Request %lu in unexpected state %d\n", req->request_id, expected_status);
            }
            
            atomic_store(&rdma_shm->head, (index + 1) % MAX_RDMA_REQUESTS);
            atomic_fetch_sub(&rdma_shm->count, 1); 
        }
        
        pthread_mutex_unlock(&rdma_shm->queue_mutex);
    }
    
    return 0;
}

static int rdma_server_handle_request(rdma_request_t* req) {
    if (!sim_remote_memory_ptr && req->op_type != RDMA_OP_INIT && req->op_type != RDMA_OP_SHUTDOWN) {
       
        fprintf(stderr, "Simulated remote memory not initialized for RDMA op %d\n", req->op_type);
        return EFAULT; 
    }

    uint64_t conceptual_offset; // Offset from conceptual base_addr

    switch (req->op_type) {
        case RDMA_OP_INIT:
        
            return 0;
            
        case RDMA_OP_READ: // Server reads from its conceptual remote_addr, writes to shm->page_buffer
            if (req->size == 0 || req->size > RDMA_PAGE_BUFFER_SIZE - req->buffer_offset) {
                return EINVAL; // Invalid size or would overflow page_buffer
            }
            conceptual_offset = req->remote_addr - rdma_shm->remote_memory_base; // Use direct access
            if (req->remote_addr < rdma_shm->remote_memory_base || 
                conceptual_offset + req->size > SIM_REMOTE_MEMORY_SIZE) {
                return EFAULT; // Address out of bounds of simulated memory
            }
            // Server copies from its simulated memory to the shared page_buffer
            memcpy(rdma_shm->page_buffer + req->buffer_offset, 
                   sim_remote_memory_ptr + conceptual_offset, 
                   req->size);
            // atomic_store(&rdma_shm->page_buffer_used, req->buffer_offset + req->size); // Client will read it
            return 0;
            
        case RDMA_OP_WRITE: // Server reads from shm->page_buffer, writes to its conceptual remote_addr
            if (!req->data_in_shm_buffer || req->size == 0) {
                return EINVAL;
            }
            conceptual_offset = req->remote_addr - rdma_shm->remote_memory_base; // Use direct access
             if (req->remote_addr < rdma_shm->remote_memory_base || 
                conceptual_offset + req->size > SIM_REMOTE_MEMORY_SIZE) {
                return EFAULT; // Address out of bounds
            }
            // Server copies from shared page_buffer to its simulated memory
            memcpy(sim_remote_memory_ptr + conceptual_offset, 
                   rdma_shm->page_buffer + req->buffer_offset, // data is at this offset in page_buffer
                   req->size);
            return 0; 

        case RDMA_OP_ALLOCATE: {
            uint64_t remote_addr;
            int alloc_ret = rdma_sim_allocate(req->size, &remote_addr);
            if (alloc_ret == 0) {
                req->remote_addr = remote_addr;  
                return 0;
            }
            return alloc_ret;
        } 
            
        case RDMA_OP_FREE: // Should be handled by client side via rdma_sim_free directly
            return ENOSYS;
            
        case RDMA_OP_SHUTDOWN:
            atomic_store(&rdma_shm->shutdown_requested, true);
            return 0;
            
        default:
            return ENOSYS; 
    }
}

void rdma_print_stats(void) {
    if (rdma_shm == NULL || !atomic_load(&rdma_shm->initialized)) {
        printf("RDMA Sim not initialized, no stats.\n");
        return;
    }
    
    printf("RDMA Simulation Statistics:\n");
    printf("  SHM Initialized: %s\n", atomic_load(&rdma_shm->initialized) ? "Yes" : "No");
    printf("  SHM Shutdown Requested: %s\n", atomic_load(&rdma_shm->shutdown_requested) ? "Yes" : "No");
    printf("  Queue Head: %d, Tail: %d, Count: %d\n", atomic_load(&rdma_shm->head), atomic_load(&rdma_shm->tail), atomic_load(&rdma_shm->count));
    printf("  Total requests: %lu\n", (unsigned long)atomic_load(&rdma_shm->total_requests));
    printf("  Completed requests: %lu\n", (unsigned long)atomic_load(&rdma_shm->completed_requests));
    printf("  Failed requests: %lu\n", (unsigned long)atomic_load(&rdma_shm->failed_requests));
    // Use direct access for non-atomic remote_memory_base and remote_memory_size
    printf("  Remote conceptual base: 0x%lx\n", (unsigned long)rdma_shm->remote_memory_base);
    printf("  Remote conceptual size: %lu bytes\n", (unsigned long)rdma_shm->remote_memory_size);
    printf("  Remote memory used (conceptual): %lu bytes\n", (unsigned long)atomic_load(&rdma_shm->remote_memory_used));
    printf("  Page Buffer current used size (indicative): %lu / %d bytes\n", 
           (unsigned long)atomic_load(&rdma_shm->page_buffer_used), RDMA_PAGE_BUFFER_SIZE);
    printf("  Simulated server actual mem ptr: %p\n", (void*)sim_remote_memory_ptr);
}