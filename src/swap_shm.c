#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include "swap_shm.h"

// Static variables
static int shm_fd = -1;
static swap_shm_t* shm = NULL;
static sem_t* request_sem = NULL;
static sem_t* response_sem = NULL;
static uint64_t next_request_id = 1;

// Forward declaration for the function in swap_process.c
extern int process_request(swap_request_t* req);

// Get shared memory pointer
swap_shm_t* get_swap_shm(void) {
    return shm;
}

// Initialize shared memory
int swap_shm_init(bool is_server) {
    int ret = 0;

    // Server (swap process) creates the shared memory, client (ExtMem) opens it
    if (is_server) {
        // Remove any existing shared memory
        shm_unlink(SHM_NAME);
        
        // Create new shared memory
        shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
        if (shm_fd == -1) {
            perror("shm_open");
            return -1;
        }
        
        // Set the size of the shared memory
        if (ftruncate(shm_fd, SHM_SIZE) == -1) {
            perror("ftruncate");
            close(shm_fd);
            shm_unlink(SHM_NAME);
            return -1;
        }
        
        // Create semaphores
        sem_unlink(SEM_REQUEST_NAME);
        sem_unlink(SEM_RESPONSE_NAME);
        
        request_sem = sem_open(SEM_REQUEST_NAME, O_CREAT, 0666, 0);
        if (request_sem == SEM_FAILED) {
            perror("sem_open request");
            close(shm_fd);
            shm_unlink(SHM_NAME);
            return -1;
        }
        
        response_sem = sem_open(SEM_RESPONSE_NAME, O_CREAT, 0666, 0);
        if (response_sem == SEM_FAILED) {
            perror("sem_open response");
            sem_close(request_sem);
            sem_unlink(SEM_REQUEST_NAME);
            close(shm_fd);
            shm_unlink(SHM_NAME);
            return -1;
        }
    } else {
        // Client opens existing shared memory
        shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (shm_fd == -1) {
            perror("shm_open");
            return -1;
        }
        
        // Open existing semaphores
        request_sem = sem_open(SEM_REQUEST_NAME, 0);
        if (request_sem == SEM_FAILED) {
            perror("sem_open request");
            close(shm_fd);
            return -1;
        }
        
        response_sem = sem_open(SEM_RESPONSE_NAME, 0);
        if (response_sem == SEM_FAILED) {
            perror("sem_open response");
            sem_close(request_sem);
            close(shm_fd);
            return -1;
        }
    }
    
    // Map the shared memory
    shm = (swap_shm_t *)mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        sem_close(request_sem);
        sem_close(response_sem);
        if (is_server) {
            sem_unlink(SEM_REQUEST_NAME);
            sem_unlink(SEM_RESPONSE_NAME);
            shm_unlink(SHM_NAME);
        }
        close(shm_fd);
        return -1;
    }
    
    // Initialize the shared memory if server
    if (is_server) {
        memset(shm, 0, sizeof(swap_shm_t));
        
        // Initialize mutex as process-shared
        pthread_mutexattr_t mutex_attr;
        pthread_mutexattr_init(&mutex_attr);
        pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&shm->queue_mutex, &mutex_attr);
        pthread_mutexattr_destroy(&mutex_attr);
        
        shm->head = 0;
        shm->tail = 0;
        shm->count = 0;
        shm->total_requests = 0;
        shm->read_requests = 0;
        shm->write_requests = 0;
        shm->checkpoint_requests = 0;
        shm->bytes_read = 0;
        shm->bytes_written = 0;
        shm->last_checkpoint_time = -1.0;
    }
    
    return 0;
}

// Clean up shared memory
int swap_shm_cleanup(bool is_server) {
    int ret = 0;
    
    if (shm != NULL) {
        ret = munmap(shm, SHM_SIZE);
        if (ret == -1) {
            perror("munmap");
        }
        shm = NULL;
    }
    
    if (shm_fd != -1) {
        ret = close(shm_fd);
        if (ret == -1) {
            perror("close");
        }
        shm_fd = -1;
    }
    
    if (request_sem != NULL) {
        ret = sem_close(request_sem);
        if (ret == -1) {
            perror("sem_close request");
        }
        request_sem = NULL;
    }
    
    if (response_sem != NULL) {
        ret = sem_close(response_sem);
        if (ret == -1) {
            perror("sem_close response");
        }
        response_sem = NULL;
    }
    
    if (is_server) {
        ret = shm_unlink(SHM_NAME);
        if (ret == -1) {
            perror("shm_unlink");
        }
        
        ret = sem_unlink(SEM_REQUEST_NAME);
        if (ret == -1) {
            perror("sem_unlink request");
        }
        
        ret = sem_unlink(SEM_RESPONSE_NAME);
        if (ret == -1) {
            perror("sem_unlink response");
        }
    }
    
    return ret;
}

// Submit a request to the swap process
int swap_submit_request(swap_op_type_t op_type, uint64_t page_addr, 
                      uint64_t disk_offset, size_t page_size, void* data) {
    if (shm == NULL) {
        fprintf(stderr, "Shared memory not initialized\n");
        return -1;
    }
    
    uint64_t request_id = __sync_fetch_and_add(&next_request_id, 1);
    
    pthread_mutex_lock(&shm->queue_mutex);
    
    if (shm->count >= MAX_QUEUE_SIZE) {
        pthread_mutex_unlock(&shm->queue_mutex);
        fprintf(stderr, "Request queue is full\n");
        return -1;
    }
    
    uint32_t tail = shm->tail;
    swap_request_t* req = &shm->requests[tail];
    
    req->request_id = request_id;
    req->op_type = op_type;
    req->page_addr = page_addr;
    req->disk_offset = disk_offset;
    req->page_size = page_size;
    req->status = STATUS_PENDING;
    req->error_code = 0;
    
    // Handle specific operation types
    if (op_type == OP_CHECKPOINT && data != NULL) {
        strncpy(req->checkpoint_name, (const char*)data, MAX_CHECKPOINT_NAME_LEN - 1);
        req->checkpoint_name[MAX_CHECKPOINT_NAME_LEN - 1] = '\0';
    }
    else if (op_type == OP_WRITE_PAGE && data != NULL) {
        // Copy page data if writing
        // Use the entire page buffer if this is the only request
        if (shm->count == 0) {
            req->buffer_offset = 0;
        } else {
            // Otherwise, allocate from the end of the existing data
            swap_request_t* prev_req = &shm->requests[(tail + MAX_QUEUE_SIZE - 1) % MAX_QUEUE_SIZE];
            req->buffer_offset = prev_req->buffer_offset + prev_req->page_size;
            
            // Check if we have enough space in the buffer
            if (req->buffer_offset + page_size > PAGE_BUFFER_SIZE) {
                pthread_mutex_unlock(&shm->queue_mutex);
                fprintf(stderr, "Page buffer is full\n");
                return -1;
            }
        }
        
        // Copy the page data to the buffer
        memcpy(shm->page_buffer + req->buffer_offset, data, page_size);
    }
    
    // Update queue
    shm->tail = (tail + 1) % MAX_QUEUE_SIZE;
    shm->count++;
    
    // Update statistics
    shm->total_requests++;
    if (op_type == OP_READ_PAGE) {
        shm->read_requests++;
    } else if (op_type == OP_WRITE_PAGE) {
        shm->write_requests++;
    } else if (op_type == OP_CHECKPOINT) {
        shm->checkpoint_requests++;
    }
    
    pthread_mutex_unlock(&shm->queue_mutex);
    
    // Signal swap process that a request is available
    sem_post(request_sem);
    
    return request_id;
}

// Submit a checkpoint request
int swap_submit_checkpoint_request(const char* checkpoint_name) {
    return swap_submit_request(OP_CHECKPOINT, 0, 0, 0, (void*)checkpoint_name);
}

// Wait for a request to complete
int swap_wait_request(uint64_t request_id, int* error_code) {
    if (shm == NULL) {
        fprintf(stderr, "Shared memory not initialized\n");
        return -1;
    }
    
    // Wait for the swap process to signal that a response is available
    sem_wait(response_sem);
    
    // Lock the queue
    pthread_mutex_lock(&shm->queue_mutex);
    
    // Find the request
    bool found = false;
    swap_request_t* req = NULL;
    
    for (uint32_t i = 0; i < MAX_QUEUE_SIZE; i++) {
        if (shm->requests[i].request_id == request_id) {
            req = &shm->requests[i];
            found = true;
            break;
        }
    }
    
    if (!found) {
        pthread_mutex_unlock(&shm->queue_mutex);
        fprintf(stderr, "Request not found: %lu\n", request_id);
        return -1;
    }
    
    // Check if request is completed
    if (req->status == STATUS_COMPLETED) {
        if (error_code != NULL) {
            *error_code = req->error_code;
        }
        
        // If this is a read request, copy the data from the buffer
        if (req->op_type == OP_READ_PAGE) {
            // (The caller would need to provide a buffer and copy from shm->page_buffer + req->buffer_offset)
        }
    } else if (req->status == STATUS_ERROR) {
        if (error_code != NULL) {
            *error_code = req->error_code;
        }
        pthread_mutex_unlock(&shm->queue_mutex);
        return -1;
    } else {
        pthread_mutex_unlock(&shm->queue_mutex);
        fprintf(stderr, "Request not completed: %lu\n", request_id);
        return -1;
    }
    
    // Remove the request from the queue
    req->request_id = 0;  // Mark as unused
    
    // Unlock the queue
    pthread_mutex_unlock(&shm->queue_mutex);
    
    return 0;
}

// Process the next request (swap process)
int swap_process_next_request(void) {
    if (shm == NULL) {
        fprintf(stderr, "Shared memory not initialized\n");
        return -1;
    }
    
    // Wait for a request to be available
    sem_wait(request_sem);
    
    // Lock the queue
    pthread_mutex_lock(&shm->queue_mutex);
    
    // Check if there are any requests
    if (shm->count == 0) {
        pthread_mutex_unlock(&shm->queue_mutex);
        return 0;
    }
    
    // Get the next request
    uint32_t head = shm->head;
    swap_request_t* req = &shm->requests[head];
    
    // Mark as in progress
    req->status = STATUS_IN_PROGRESS;
    
    // Unlock the queue to allow more requests to be submitted
    pthread_mutex_unlock(&shm->queue_mutex);
    
    // Process the request (actual file I/O happens here in process_request)
    int error_code = process_request(req);
    
    // Lock the queue again
    pthread_mutex_lock(&shm->queue_mutex);
    
    // Update request status
    if (error_code == 0) {
        req->status = STATUS_COMPLETED;
    } else {
        req->status = STATUS_ERROR;
        req->error_code = error_code;
    }
    
    // Update queue
    shm->head = (head + 1) % MAX_QUEUE_SIZE;
    shm->count--;
    
    // Unlock the queue
    pthread_mutex_unlock(&shm->queue_mutex);
    
    // Signal the client that the response is available
    sem_post(response_sem);
    
    return 0;
}

// Check if there are any pending requests
bool swap_has_pending_requests(void) {
    if (shm == NULL) {
        return false;
    }
    
    // Lock the queue
    pthread_mutex_lock(&shm->queue_mutex);
    
    bool has_requests = (shm->count > 0);
    
    // Unlock the queue
    pthread_mutex_unlock(&shm->queue_mutex);
    
    return has_requests;
}