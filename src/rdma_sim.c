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
static int shm_id = -1;
static bool is_initialized = false; // Local init flag for client/server status
static atomic_uint_fast64_t request_id_counter = 1; // Start from 1, 0 can mean error

// 远程内存分配器
#define SIM_REMOTE_MEMORY_SIZE (1ULL * 1024 * 1024 * 1024)  // 1GB远程内存 for simulation
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

// 获取时间戳
uint64_t rdma_get_timestamp(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000ULL + tv.tv_usec;
}

// 初始化远程内存分配器
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
        
        // These are for the *shared memory metadata*, not the server's actual memory size
        // rdma_shm->remote_memory_base reflects this conceptual base
        if (rdma_shm) { // rdma_shm might not be set yet if called before shmat
             rdma_shm->remote_memory_base = sim_remote_memory_blocks->addr; // Direct assignment
             rdma_shm->remote_memory_size = SIM_REMOTE_MEMORY_SIZE; // Direct assignment
             atomic_store(&rdma_shm->remote_memory_used, 0);
        }
    }
    
    pthread_mutex_unlock(&sim_mem_alloc_mutex);
    return 0;
}

// 分配远程内存
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

// 释放远程内存
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

// 获取共享内存
rdma_shm_t* get_rdma_shm(void) {
    return rdma_shm;
}

// 初始化RDMA模拟
int rdma_sim_init(bool is_server) {
    if (is_initialized && rdma_shm != NULL && atomic_load(&rdma_shm->initialized)) {
        return 0; // Already initialized correctly
    }
    
    shm_id = shmget(RDMA_SHM_KEY, sizeof(rdma_shm_t), is_server ? (IPC_CREAT | IPC_EXCL | 0666) : 0666);
    if (shm_id == -1) {
        if (is_server && errno == EEXIST) { // Server: SHM already exists, try to attach and use/clean
            shm_id = shmget(RDMA_SHM_KEY, sizeof(rdma_shm_t), 0666);
            if (shm_id == -1) { perror("shmget (server, EEXIST recovery)"); return -1; }
            // If successful, proceed to attach, but may need cleanup logic if state is bad
        } else {
            perror(is_server ? "shmget (server, create)" : "shmget (client)");
            return -1;
        }
    }
    
    rdma_shm = (rdma_shm_t*)shmat(shm_id, NULL, 0);
    if (rdma_shm == (void*)-1) {
        perror("shmat");
        rdma_shm = NULL; // Ensure rdma_shm is NULL on failure
        if (is_server && shm_id != -1) shmctl(shm_id, IPC_RMID, NULL); // Clean up if server created it then failed to attach
        return -1;
    }
    
    if (is_server) {
        // Initialize server's simulated memory first, as it's used by rdma_shm metadata init
        if (init_sim_remote_memory_allocator() != 0) {
             // Cleanup shm before returning
            shmdt(rdma_shm); rdma_shm = NULL;
            shmctl(shm_id, IPC_RMID, NULL); shm_id = -1;
            return -1;
        }

        memset(rdma_shm, 0, sizeof(rdma_shm_t)); // Zero out shared memory
        
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
        
        atomic_init(&rdma_shm->head, 0);
        atomic_init(&rdma_shm->tail, 0);
        atomic_init(&rdma_shm->count, 0);
        atomic_init(&rdma_shm->shutdown_requested, false);
        // Initialize non-atomic fields directly
        rdma_shm->remote_memory_base = sim_remote_memory_blocks ? sim_remote_memory_blocks->addr : 0;
        rdma_shm->remote_memory_size = sim_remote_memory_blocks ? SIM_REMOTE_MEMORY_SIZE : 0;
        atomic_init(&rdma_shm->remote_memory_used, 0);
        atomic_init(&rdma_shm->total_requests, 0);
        atomic_init(&rdma_shm->completed_requests, 0);
        atomic_init(&rdma_shm->failed_requests, 0);
        atomic_init(&rdma_shm->page_buffer_used, 0);
        
        atomic_store(&rdma_shm->initialized, true); // Mark shm as initialized
    } else { // Client
        int timeout = 100; 
        while (!atomic_load(&rdma_shm->initialized) && timeout > 0) {
            usleep(100000); 
            timeout--;
        }
        if (!atomic_load(&rdma_shm->initialized)) {
            fprintf(stderr, "Timeout waiting for RDMA server initialization\n");
            shmdt(rdma_shm); rdma_shm = NULL; // Detach on failure
            // Don't remove shm as client
            return -1;
        }
    }
    
    is_initialized = true; // Local flag
    return 0;
}

// 清理RDMA模拟
void rdma_sim_cleanup(bool is_server) {
    if (rdma_shm != NULL) {
        if (is_server) {
            atomic_store(&rdma_shm->shutdown_requested, true);
            pthread_cond_broadcast(&rdma_shm->queue_cond); // Wake up server thread if waiting
            // Give server thread a moment to exit its loop
            usleep(10000); 

            pthread_mutex_destroy(&rdma_shm->queue_mutex);
            pthread_cond_destroy(&rdma_shm->queue_cond);
        }
        shmdt(rdma_shm);
        rdma_shm = NULL;
    }
    
    if (is_server && shm_id != -1) {
        shmctl(shm_id, IPC_RMID, NULL);
        shm_id = -1;
    }

    // Server also cleans up its simulated memory
    if (is_server) {
        pthread_mutex_lock(&sim_mem_alloc_mutex);
        if (sim_remote_memory_ptr) {
            free(sim_remote_memory_ptr);
            sim_remote_memory_ptr = NULL;
        }
        mem_block_t* current = sim_remote_memory_blocks;
        while (current != NULL) {
            mem_block_t* next = current->next;
            free(current);
            current = next;
        }
        sim_remote_memory_blocks = NULL;
        pthread_mutex_unlock(&sim_mem_alloc_mutex);
    }
    is_initialized = false;
}

// 提交RDMA请求
uint64_t rdma_submit_request(rdma_op_type_e op_type, uint64_t local_addr, 
                            uint64_t remote_addr, size_t size, void* data) {
    if (rdma_shm == NULL || !atomic_load(&rdma_shm->initialized)) {
        return 0; 
    }
    
    pthread_mutex_lock(&rdma_shm->queue_mutex);
    
    if (atomic_load(&rdma_shm->count) >= MAX_RDMA_REQUESTS) {
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
    // For RDMA_OP_READ, client expects data in its local_addr. Server will write to shm->page_buffer first.
    // Client will copy from shm->page_buffer to local_addr in rdma_wait_request.
    // Server needs to know where to put data in page_buffer. Let's assume offset 0 for RDMA_OP_READ server write to shm.
    // So, req->buffer_offset = 0 for server to write to start of page_buffer.
    
    atomic_store(&rdma_shm->tail, (index + 1) % MAX_RDMA_REQUESTS);
    atomic_fetch_add(&rdma_shm->count, 1);
    atomic_fetch_add(&rdma_shm->total_requests, 1);
    
    pthread_cond_signal(&rdma_shm->queue_cond);
    pthread_mutex_unlock(&rdma_shm->queue_mutex);
    
    return req_id;
}

// 等待请求完成
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
        // This search could be slow. Client could also store the index from submit_request.
        // For now, search all slots. This search itself should be safe if request_id is stable after submission.
        for (uint32_t i = 0; i < MAX_RDMA_REQUESTS; i++) { 
            // A potential issue: if requests are overwritten quickly, this might pick the wrong one
            // if request IDs are not unique enough over time or if MAX_RDMA_REQUESTS is too small.
            // Assuming request_id is unique enough for in-flight/recent requests.
            // A lock around this loop or a generation count per slot could make it safer.
            if (rdma_shm->requests[i].request_id == request_id && atomic_load(&rdma_shm->requests[i].status) != 0 /*not empty or recycled*/) {
                req_ptr = &rdma_shm->requests[i];
                break;
            }
        }

        if (!req_ptr) {
            if(error_code) *error_code = ENOENT; // No such request (or already gone)
            // This could happen if request_id is bad, or if it completed and was cleaned up very fast
            // which shouldn't happen if client holds the only reference for waiting.
            // Let's assume for now a valid request_id will be found if it's truly pending/completed.
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
            // Consider marking request as consumed or cleaning up slot if client won't wait again.
            // For now, status remains COMPLETED.
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

// 服务器端处理请求
int rdma_server_process_requests(void) {
    if (rdma_shm == NULL || !atomic_load(&rdma_shm->initialized)) {
        return -1;
    }
    
    while (!atomic_load(&rdma_shm->shutdown_requested)) {
        pthread_mutex_lock(&rdma_shm->queue_mutex);
        
        while (atomic_load(&rdma_shm->count) == 0 && !atomic_load(&rdma_shm->shutdown_requested)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1; 
            pthread_cond_timedwait(&rdma_shm->queue_cond, &rdma_shm->queue_mutex, &ts);
        }
        
        if (atomic_load(&rdma_shm->shutdown_requested)) {
            pthread_mutex_unlock(&rdma_shm->queue_mutex);
            break;
        }
        
        while (atomic_load(&rdma_shm->count) > 0) {
            uint32_t index = atomic_load(&rdma_shm->head);
            rdma_request_t* req = &rdma_shm->requests[index];
            
            // Atomically try to change PENDING to PROCESSING
            int expected_status = RDMA_REQ_PENDING;
            if (atomic_compare_exchange_strong(&req->status, &expected_status, RDMA_REQ_PROCESSING)) {
                
                req->error_code = 0; 
                int result = rdma_server_handle_request(req);
                
                if (result == 0) {
                    atomic_store(&req->status, RDMA_REQ_COMPLETED);
                    atomic_fetch_add(&rdma_shm->completed_requests, 1);
                } else {
                    req->error_code = result; 
                    atomic_store(&req->status, RDMA_REQ_FAILED);
                    atomic_fetch_add(&rdma_shm->failed_requests, 1);
                }
            } else if (expected_status == RDMA_REQ_PROCESSING) {
                // Another server thread is processing, or it's already done/failed. Skip.
                // This case is less likely if only one server thread.
            } else {
                // Already completed or failed by some other means, or an invalid state.
                // This means the request was somehow processed or changed status out of band.
                // For robust queue, this req should be skipped.
            }
            
            atomic_store(&rdma_shm->head, (index + 1) % MAX_RDMA_REQUESTS);
            atomic_fetch_sub(&rdma_shm->count, 1); 
        }
        
        pthread_mutex_unlock(&rdma_shm->queue_mutex);
    }
    
    return 0;
}

// 处理单个请求
static int rdma_server_handle_request(rdma_request_t* req) {
    if (!sim_remote_memory_ptr && req->op_type != RDMA_OP_INIT && req->op_type != RDMA_OP_SHUTDOWN) {
        // This check ensures simulated memory is available for ops that need it
        // init_sim_remote_memory_allocator should have been called by server's rdma_sim_init
        fprintf(stderr, "Simulated remote memory not initialized for RDMA op %d\n", req->op_type);
        return EFAULT; 
    }

    uint64_t conceptual_offset; // Offset from conceptual base_addr

    switch (req->op_type) {
        case RDMA_OP_INIT:
            // Server side initialization of sim_remote_memory_ptr is done in rdma_sim_init.
            // This op could be used for other sync if needed.
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

        case RDMA_OP_ALLOCATE: // Should be handled by client side via rdma_sim_allocate directly
            return ENOSYS; 
            
        case RDMA_OP_FREE: // Should be handled by client side via rdma_sim_free directly
            return ENOSYS;
            
        case RDMA_OP_SHUTDOWN:
            atomic_store(&rdma_shm->shutdown_requested, true);
            return 0;
            
        default:
            return ENOSYS; 
    }
}

// 打印统计信息
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