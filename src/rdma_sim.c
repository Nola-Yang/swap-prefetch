#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <semaphore.h>
#include <time.h>
#include "rdma_sim.h"

// Static variables
static int rdma_shm_fd = -1;
static rdma_shm_t* rdma_shm = NULL;
static sem_t* rdma_request_sem = NULL;
static sem_t* rdma_response_sem = NULL;
static uint64_t next_rdma_request_id = 1;

// "Remote memory" simulation variables
static void* remote_memory = NULL;
static pthread_mutex_t remote_memory_mutex = PTHREAD_MUTEX_INITIALIZER;
static rdma_region_t* memory_regions = NULL;
static int num_memory_regions = 0;
static int max_memory_regions = 1024;  // Can be adjusted based on needs

// Network simulation parameters
static int min_latency_us = 50;    // Minimum simulated latency in microseconds
static int max_latency_us = 200;   // Maximum simulated latency in microseconds
static int bandwidth_mbps = 100;   // Simulated bandwidth in MB/s

// Forward declaration for the function in rdma_process.c
extern int process_rdma_request(rdma_request_t* req);

// Simulate network latency
static void simulate_network_delay(size_t size, double *latency) {
    // Calculate base latency (random between min and max)
    double base_latency_us = min_latency_us + (rand() % (max_latency_us - min_latency_us + 1));
    
    // Add transfer time based on bandwidth (size in bytes / bandwidth in bytes per second)
    // bandwidth_mbps * 1024 * 1024 / 8 = bytes per second
    double transfer_time_us = (double)size / (bandwidth_mbps * 1024.0 * 1024.0 / 8.0) * 1000000.0;
    
    // Total simulated latency
    double total_latency_us = base_latency_us + transfer_time_us;
    
    // Return the latency and simulate it by sleeping
    if (latency) {
        *latency = total_latency_us;
    }
    
    // Convert to nanoseconds and sleep
    struct timespec ts;
    ts.tv_sec = (long)(total_latency_us / 1000000);
    ts.tv_nsec = (long)((total_latency_us - (ts.tv_sec * 1000000)) * 1000);
    nanosleep(&ts, NULL);
}

// Get shared memory pointer
rdma_shm_t* get_rdma_shm(void) {
    return rdma_shm;
}

// Initialize RDMA simulation
int rdma_sim_init(bool is_server) {
    int ret = 0;

    // Seed random number generator for latency simulation
    srand(time(NULL));

    // Initialize memory regions tracking
    if (is_server) {
        memory_regions = calloc(max_memory_regions, sizeof(rdma_region_t));
        if (!memory_regions) {
            perror("Failed to allocate memory region tracking");
            return -1;
        }
    }

    // Server (RDMA process) creates the shared memory, client (ExtMem) opens it
    if (is_server) {
        // Remove any existing shared memory
        shm_unlink(RDMA_SHM_NAME);
        
        // Create new shared memory for control
        rdma_shm_fd = shm_open(RDMA_SHM_NAME, O_CREAT | O_RDWR, 0666);
        if (rdma_shm_fd == -1) {
            perror("shm_open");
            free(memory_regions);
            return -1;
        }
        
        // Set the size of the shared memory
        if (ftruncate(rdma_shm_fd, RDMA_SHM_SIZE) == -1) {
            perror("ftruncate");
            close(rdma_shm_fd);
            shm_unlink(RDMA_SHM_NAME);
            free(memory_regions);
            return -1;
        }
        
        // Create semaphores
        sem_unlink(RDMA_SEM_REQUEST_NAME);
        sem_unlink(RDMA_SEM_RESPONSE_NAME);
        
        rdma_request_sem = sem_open(RDMA_SEM_REQUEST_NAME, O_CREAT, 0666, 0);
        if (rdma_request_sem == SEM_FAILED) {
            perror("sem_open request");
            close(rdma_shm_fd);
            shm_unlink(RDMA_SHM_NAME);
            free(memory_regions);
            return -1;
        }
        
        rdma_response_sem = sem_open(RDMA_SEM_RESPONSE_NAME, O_CREAT, 0666, 0);
        if (rdma_response_sem == SEM_FAILED) {
            perror("sem_open response");
            sem_close(rdma_request_sem);
            sem_unlink(RDMA_SEM_REQUEST_NAME);
            close(rdma_shm_fd);
            shm_unlink(RDMA_SHM_NAME);
            free(memory_regions);
            return -1;
        }
        
        // Allocate the "remote memory" - this simulates memory on a remote node
        remote_memory = mmap(NULL, RDMA_REMOTE_MEM_SIZE, PROT_READ | PROT_WRITE, 
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (remote_memory == MAP_FAILED) {
            perror("mmap remote memory");
            sem_close(rdma_request_sem);
            sem_close(rdma_response_sem);
            sem_unlink(RDMA_SEM_REQUEST_NAME);
            sem_unlink(RDMA_SEM_RESPONSE_NAME);
            close(rdma_shm_fd);
            shm_unlink(RDMA_SHM_NAME);
            free(memory_regions);
            return -1;
        }
        
        // Clear the memory
        memset(remote_memory, 0, RDMA_REMOTE_MEM_SIZE);
    } else {
        // Client opens existing shared memory
        rdma_shm_fd = shm_open(RDMA_SHM_NAME, O_RDWR, 0666);
        if (rdma_shm_fd == -1) {
            perror("shm_open");
            return -1;
        }
        
        // Open existing semaphores
        rdma_request_sem = sem_open(RDMA_SEM_REQUEST_NAME, 0);
        if (rdma_request_sem == SEM_FAILED) {
            perror("sem_open request");
            close(rdma_shm_fd);
            return -1;
        }
        
        rdma_response_sem = sem_open(RDMA_SEM_RESPONSE_NAME, 0);
        if (rdma_response_sem == SEM_FAILED) {
            perror("sem_open response");
            sem_close(rdma_request_sem);
            close(rdma_shm_fd);
            return -1;
        }
    }
    
    // Map the shared memory for control
    rdma_shm = (rdma_shm_t *)mmap(NULL, RDMA_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, rdma_shm_fd, 0);
    if (rdma_shm == MAP_FAILED) {
        perror("mmap");
        sem_close(rdma_request_sem);
        sem_close(rdma_response_sem);
        if (is_server) {
            sem_unlink(RDMA_SEM_REQUEST_NAME);
            sem_unlink(RDMA_SEM_RESPONSE_NAME);
            shm_unlink(RDMA_SHM_NAME);
            free(memory_regions);
            if (remote_memory != MAP_FAILED && remote_memory != NULL) {
                munmap(remote_memory, RDMA_REMOTE_MEM_SIZE);
            }
        }
        close(rdma_shm_fd);
        return -1;
    }
    
    // Initialize the shared memory if server
    if (is_server) {
        memset(rdma_shm, 0, sizeof(rdma_shm_t));
        
        // Initialize mutex as process-shared
        pthread_mutexattr_t mutex_attr;
        pthread_mutexattr_init(&mutex_attr);
        pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&rdma_shm->queue_mutex, &mutex_attr);
        pthread_mutexattr_destroy(&mutex_attr);
        
        rdma_shm->head = 0;
        rdma_shm->tail = 0;
        rdma_shm->count = 0;
        rdma_shm->total_requests = 0;
        rdma_shm->read_requests = 0;
        rdma_shm->write_requests = 0;
        rdma_shm->bytes_read = 0;
        rdma_shm->bytes_written = 0;
        rdma_shm->avg_latency = 0.0;
        rdma_shm->total_remote_mem = RDMA_REMOTE_MEM_SIZE;
        rdma_shm->available_remote_mem = RDMA_REMOTE_MEM_SIZE;
    }
    
    return 0;
}

// Clean up RDMA simulation
int rdma_sim_cleanup(bool is_server) {
    int ret = 0;
    
    if (rdma_shm != NULL) {
        ret = munmap(rdma_shm, RDMA_SHM_SIZE);
        if (ret == -1) {
            perror("munmap");
        }
        rdma_shm = NULL;
    }
    
    if (rdma_shm_fd != -1) {
        ret = close(rdma_shm_fd);
        if (ret == -1) {
            perror("close");
        }
        rdma_shm_fd = -1;
    }
    
    if (rdma_request_sem != NULL) {
        ret = sem_close(rdma_request_sem);
        if (ret == -1) {
            perror("sem_close request");
        }
        rdma_request_sem = NULL;
    }
    
    if (rdma_response_sem != NULL) {
        ret = sem_close(rdma_response_sem);
        if (ret == -1) {
            perror("sem_close response");
        }
        rdma_response_sem = NULL;
    }
    
    if (is_server) {
        // Free the "remote memory"
        if (remote_memory != NULL && remote_memory != MAP_FAILED) {
            ret = munmap(remote_memory, RDMA_REMOTE_MEM_SIZE);
            if (ret == -1) {
                perror("munmap remote memory");
            }
            remote_memory = NULL;
        }
        
        // Free the memory regions tracking
        if (memory_regions != NULL) {
            free(memory_regions);
            memory_regions = NULL;
        }
        
        ret = shm_unlink(RDMA_SHM_NAME);
        if (ret == -1) {
            perror("shm_unlink");
        }
        
        ret = sem_unlink(RDMA_SEM_REQUEST_NAME);
        if (ret == -1) {
            perror("sem_unlink request");
        }
        
        ret = sem_unlink(RDMA_SEM_RESPONSE_NAME);
        if (ret == -1) {
            perror("sem_unlink response");
        }
    }
    
    return ret;
}

// Allocate memory in the simulated remote memory
int rdma_sim_allocate(size_t size, uint64_t *remote_addr) {
    if (!remote_memory || !memory_regions) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&remote_memory_mutex);
    
    // Check if we have enough remote memory available
    if (rdma_shm->available_remote_mem < size) {
        pthread_mutex_unlock(&remote_memory_mutex);
        return -ENOMEM;
    }
    
    // Find a free slot in the memory regions array
    int free_index = -1;
    for (int i = 0; i < max_memory_regions; i++) {
        if (!memory_regions[i].in_use) {
            free_index = i;
            break;
        }
    }
    
    if (free_index == -1) {
        // No free slots
        pthread_mutex_unlock(&remote_memory_mutex);
        return -ENOMEM;
    }
    
    // Simple allocation strategy: just increment a counter
    // In a real implementation, you would need proper memory management
    static uint64_t next_addr = 0;
    uint64_t addr = (uint64_t)remote_memory + next_addr;
    next_addr += size;
    
    // Check for overflow
    if (next_addr > RDMA_REMOTE_MEM_SIZE) {
        pthread_mutex_unlock(&remote_memory_mutex);
        return -ENOMEM;
    }
    
    // Record the allocation
    memory_regions[free_index].remote_addr = addr;
    memory_regions[free_index].size = size;
    memory_regions[free_index].in_use = true;
    memory_regions[free_index].last_access = time(NULL);
    memory_regions[free_index].access_count = 0;
    
    // Update available memory
    rdma_shm->available_remote_mem -= size;
    
    if (num_memory_regions <= free_index) {
        num_memory_regions = free_index + 1;
    }
    
    pthread_mutex_unlock(&remote_memory_mutex);
    
    // Return the allocated address
    if (remote_addr) {
        *remote_addr = addr;
    }
    
    return 0;
}

// Free memory in the simulated remote memory
int rdma_sim_free(uint64_t remote_addr) {
    if (!remote_memory || !memory_regions) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&remote_memory_mutex);
    
    // Find the memory region
    int region_index = -1;
    for (int i = 0; i < num_memory_regions; i++) {
        if (memory_regions[i].in_use && memory_regions[i].remote_addr == remote_addr) {
            region_index = i;
            break;
        }
    }
    
    if (region_index == -1) {
        // Region not found
        pthread_mutex_unlock(&remote_memory_mutex);
        return -EINVAL;
    }
    
    // Mark the region as free
    memory_regions[region_index].in_use = false;
    
    // Update available memory
    rdma_shm->available_remote_mem += memory_regions[region_index].size;
    
    pthread_mutex_unlock(&remote_memory_mutex);
    
    return 0;
}

// Read from the simulated remote memory
int rdma_sim_read(uint64_t local_addr, uint64_t remote_addr, size_t length, void* buffer) {
    if (!remote_memory) {
        return -EINVAL;
    }
    
    // Calculate the offset in the remote memory
    uint64_t offset = remote_addr - (uint64_t)remote_memory;
    if (offset + length > RDMA_REMOTE_MEM_SIZE) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&remote_memory_mutex);
    
    // Find the memory region
    int region_index = -1;
    for (int i = 0; i < num_memory_regions; i++) {
        if (memory_regions[i].in_use && 
            remote_addr >= memory_regions[i].remote_addr && 
            remote_addr + length <= memory_regions[i].remote_addr + memory_regions[i].size) {
            region_index = i;
            break;
        }
    }
    
    if (region_index == -1) {
        // Region not found or access out of bounds
        pthread_mutex_unlock(&remote_memory_mutex);
        return -EINVAL;
    }
    
    // Update access statistics
    memory_regions[region_index].last_access = time(NULL);
    memory_regions[region_index].access_count++;
    
    // Simulate network latency
    double latency;
    simulate_network_delay(length, &latency);
    
    // Copy the data from remote memory to buffer
    memcpy(buffer, (void*)remote_addr, length);
    
    pthread_mutex_unlock(&remote_memory_mutex);
    
    return 0;
}

// Write to the simulated remote memory
int rdma_sim_write(uint64_t local_addr, uint64_t remote_addr, size_t length, const void* buffer) {
    if (!remote_memory) {
        return -EINVAL;
    }
    
    // Calculate the offset in the remote memory
    uint64_t offset = remote_addr - (uint64_t)remote_memory;
    if (offset + length > RDMA_REMOTE_MEM_SIZE) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&remote_memory_mutex);
    
    // Find the memory region
    int region_index = -1;
    for (int i = 0; i < num_memory_regions; i++) {
        if (memory_regions[i].in_use && 
            remote_addr >= memory_regions[i].remote_addr && 
            remote_addr + length <= memory_regions[i].remote_addr + memory_regions[i].size) {
            region_index = i;
            break;
        }
    }
    
    if (region_index == -1) {
        // Region not found or access out of bounds
        pthread_mutex_unlock(&remote_memory_mutex);
        return -EINVAL;
    }
    
    // Update access statistics
    memory_regions[region_index].last_access = time(NULL);
    memory_regions[region_index].access_count++;
    
    // Simulate network latency
    double latency;
    simulate_network_delay(length, &latency);
    
    // Copy the data from buffer to remote memory
    memcpy((void*)remote_addr, buffer, length);
    
    pthread_mutex_unlock(&remote_memory_mutex);
    
    return 0;
}

// Submit a request to the RDMA process
int rdma_submit_request(rdma_op_type_t op_type, uint64_t local_addr, 
                      uint64_t remote_addr, size_t length, void* data) {
    if (rdma_shm == NULL) {
        fprintf(stderr, "RDMA shared memory not initialized\n");
        return -1;
    }
    
    uint64_t request_id = __sync_fetch_and_add(&next_rdma_request_id, 1);
    
    pthread_mutex_lock(&rdma_shm->queue_mutex);
    
    if (rdma_shm->count >= RDMA_MAX_QUEUE_SIZE) {
        pthread_mutex_unlock(&rdma_shm->queue_mutex);
        fprintf(stderr, "RDMA request queue is full\n");
        return -1;
    }
    
    uint32_t tail = rdma_shm->tail;
    rdma_request_t* req = &rdma_shm->requests[tail];
    
    req->request_id = request_id;
    req->op_type = op_type;
    req->local_addr = local_addr;
    req->remote_addr = remote_addr;
    req->length = length;
    req->status = RDMA_STATUS_PENDING;
    req->error_code = 0;
    req->timestamp = time(NULL);
    req->simulated_latency = 0.0;
    
    // Handle specific operation types
    if (op_type == RDMA_OP_WRITE && data != NULL) {
        // Copy page data if writing
        // Use the entire page buffer if this is the only request
        if (rdma_shm->count == 0) {
            req->buffer_offset = 0;
        } else {
            // Otherwise, allocate from the end of the existing data
            rdma_request_t* prev_req = &rdma_shm->requests[(tail + RDMA_MAX_QUEUE_SIZE - 1) % RDMA_MAX_QUEUE_SIZE];
            req->buffer_offset = prev_req->buffer_offset + prev_req->length;
            
            // Check if we have enough space in the buffer
            if (req->buffer_offset + length > RDMA_PAGE_BUFFER_SIZE) {
                pthread_mutex_unlock(&rdma_shm->queue_mutex);
                fprintf(stderr, "RDMA page buffer is full\n");
                return -1;
            }
        }
        
        // Copy the page data to the buffer
        memcpy(rdma_shm->page_buffer + req->buffer_offset, data, length);
    }
    
    // Update queue
    rdma_shm->tail = (tail + 1) % RDMA_MAX_QUEUE_SIZE;
    rdma_shm->count++;
    
    // Update statistics
    rdma_shm->total_requests++;
    if (op_type == RDMA_OP_READ) {
        rdma_shm->read_requests++;
    } else if (op_type == RDMA_OP_WRITE) {
        rdma_shm->write_requests++;
    }
    
    pthread_mutex_unlock(&rdma_shm->queue_mutex);
    
    // Signal RDMA process that a request is available
    sem_post(rdma_request_sem);
    
    return request_id;
}

// Wait for a request to complete
int rdma_wait_request(uint64_t request_id, int* error_code) {
    if (rdma_shm == NULL) {
        fprintf(stderr, "RDMA shared memory not initialized\n");
        return -1;
    }
    
    // Wait for the RDMA process to signal that a response is available
    sem_wait(rdma_response_sem);
    
    // Lock the queue
    pthread_mutex_lock(&rdma_shm->queue_mutex);
    
    // Find the request
    bool found = false;
    rdma_request_t* req = NULL;
    
    for (uint32_t i = 0; i < RDMA_MAX_QUEUE_SIZE; i++) {
        if (rdma_shm->requests[i].request_id == request_id) {
            req = &rdma_shm->requests[i];
            found = true;
            break;
        }
    }
    
    if (!found) {
        pthread_mutex_unlock(&rdma_shm->queue_mutex);
        fprintf(stderr, "RDMA request not found: %lu\n", request_id);
        return -1;
    }
    
    // Check if request is completed
    if (req->status == RDMA_STATUS_COMPLETED) {
        if (error_code != NULL) {
            *error_code = req->error_code;
        }
        
        // If this is a read request, copy the data from the buffer
        if (req->op_type == RDMA_OP_READ) {
            // Copy from buffer to user memory
            // (This is handled by the caller if needed)
        }
    } else if (req->status == RDMA_STATUS_ERROR) {
        if (error_code != NULL) {
            *error_code = req->error_code;
        }
        pthread_mutex_unlock(&rdma_shm->queue_mutex);
        return -1;
    } else {
        pthread_mutex_unlock(&rdma_shm->queue_mutex);
        fprintf(stderr, "RDMA request not completed: %lu\n", request_id);
        return -1;
    }
    
    // Remove the request from the queue
    req->request_id = 0;  // Mark as unused
    
    // Unlock the queue
    pthread_mutex_unlock(&rdma_shm->queue_mutex);
    
    return 0;
}

// Process the next request (RDMA process)
int rdma_process_next_request(void) {
    if (rdma_shm == NULL) {
        fprintf(stderr, "RDMA shared memory not initialized\n");
        return -1;
    }
    
    // Wait for a request to be available
    sem_wait(rdma_request_sem);
    
    // Lock the queue
    pthread_mutex_lock(&rdma_shm->queue_mutex);
    
    // Check if there are any requests
    if (rdma_shm->count == 0) {
        pthread_mutex_unlock(&rdma_shm->queue_mutex);
        return 0;
    }
    
    // Get the next request
    uint32_t head = rdma_shm->head;
    rdma_request_t* req = &rdma_shm->requests[head];
    
    // Mark as in progress
    req->status = RDMA_STATUS_IN_PROGRESS;
    
    // Unlock the queue to allow more requests to be submitted
    pthread_mutex_unlock(&rdma_shm->queue_mutex);
    
    // Process the request
    int error_code = process_rdma_request(req);
    
    // Lock the queue again
    pthread_mutex_lock(&rdma_shm->queue_mutex);
    
    // Update request status
    if (error_code == 0) {
        req->status = RDMA_STATUS_COMPLETED;
    } else {
        req->status = RDMA_STATUS_ERROR;
        req->error_code = error_code;
    }
    
    // Update statistics based on request type
    if (req->op_type == RDMA_OP_READ) {
        rdma_shm->bytes_read += req->length;
    } else if (req->op_type == RDMA_OP_WRITE) {
        rdma_shm->bytes_written += req->length;
    }
    
    // Update latency statistics
    if (req->simulated_latency > 0.0) {
        double total_latency = rdma_shm->avg_latency * (rdma_shm->total_requests - 1);
        total_latency += req->simulated_latency;
        rdma_shm->avg_latency = total_latency / rdma_shm->total_requests;
    }
    
    // Update queue
    rdma_shm->head = (head + 1) % RDMA_MAX_QUEUE_SIZE;
    rdma_shm->count--;
    
    // Unlock the queue
    pthread_mutex_unlock(&rdma_shm->queue_mutex);
    
    // Signal the client that the response is available
    sem_post(rdma_response_sem);
    
    return 0;
}

// Check if there are any pending requests
bool rdma_has_pending_requests(void) {
    if (rdma_shm == NULL) {
        return false;
    }
    
    // Lock the queue
    pthread_mutex_lock(&rdma_shm->queue_mutex);
    
    bool has_requests = (rdma_shm->count > 0);
    
    // Unlock the queue
    pthread_mutex_unlock(&rdma_shm->queue_mutex);
    
    return has_requests;
}

// Get statistics about RDMA operations
int rdma_get_stats(double *avg_latency, uint64_t *bytes_read, uint64_t *bytes_written) {
    if (rdma_shm == NULL) {
        return -1;
    }
    
    pthread_mutex_lock(&rdma_shm->queue_mutex);
    
    if (avg_latency) {
        *avg_latency = rdma_shm->avg_latency;
    }
    
    if (bytes_read) {
        *bytes_read = rdma_shm->bytes_read;
    }
    
    if (bytes_written) {
        *bytes_written = rdma_shm->bytes_written;
    }
    
    pthread_mutex_unlock(&rdma_shm->queue_mutex);
    
    return 0;
}