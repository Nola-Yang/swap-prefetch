#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <spawn.h>
#include <sys/time.h>

#include "core.h"
#include "swap_shm.h"
#include "storage_swap_process.h"

static pid_t swap_process_pid = -1;
static bool swap_initialized = false;
static pthread_mutex_t swap_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function to find the swap process executable
static char* find_swap_process() {
    const char* paths[] = {
        "./swap_process",                    // Current directory
        "../bin/swap_process",               // Bin directory relative to current directory
        "/usr/local/bin/swap_process",       // System installation path
        NULL                                 // End marker
    };
    
    // Check environment variable first
    const char* env_path = getenv("EXTMEM_SWAP_PROCESS_PATH");
    if (env_path != NULL) {
        if (access(env_path, X_OK) == 0) {
            char* path = strdup(env_path);
            return path;
        }
        fprintf(stderr, "Swap process not found at EXTMEM_SWAP_PROCESS_PATH: %s\n", env_path);
    }
    
    // Try each of the predefined paths
    for (int i = 0; paths[i] != NULL; i++) {
        if (access(paths[i], X_OK) == 0) {
            return strdup(paths[i]);
        }
    }
    
    return NULL;
}

// Start swap process with improved error handling
static int start_swap_process() {
    char* swap_path = find_swap_process();
    if (swap_path == NULL) {
        fprintf(stderr, "Failed to find swap process executable\n");
        return -1;
    }
    
    char *argv[] = {swap_path, NULL};
    char *envp[] = {NULL};
    int ret;
    
    // Use posix_spawn to launch swap process
    ret = posix_spawn(&swap_process_pid, swap_path, NULL, NULL, argv, envp);
    free(swap_path);
    
    if (ret != 0) {
        perror("posix_spawn");
        return -1;
    }
    
    LOG("Swap process started with PID: %d\n", swap_process_pid);
    
    // Wait for process to initialize (with a timeout)
    for (int i = 0; i < 10; i++) {
        usleep(100000);  // 100ms
        
        // Check if process is still running
        if (kill(swap_process_pid, 0) != 0) {
            if (errno == ESRCH) {
                fprintf(stderr, "Swap process exited prematurely\n");
                return -1;
            }
            perror("kill check");
            return -1;
        }
        
        // Try to connect to shared memory
        if (swap_shm_init(false) == 0) {
            // Successful connection, break out of loop
            return 0;
        }
    }
    
    // Timeout waiting for swap process to initialize
    fprintf(stderr, "Timeout waiting for swap process to initialize\n");
    kill(swap_process_pid, SIGTERM);
    return -1;
}

// Initialize storage with robust error handling
int storage_init() {
    int ret;
    
    pthread_mutex_lock(&swap_mutex);
    
    if (swap_initialized) {
        pthread_mutex_unlock(&swap_mutex);
        return 0;  // Already initialized
    }
    
    // Start swap process
    ret = start_swap_process();
    if (ret != 0) {
        pthread_mutex_unlock(&swap_mutex);
        return -1;
    }
    
    // Send initialization request
    uint64_t req_id = swap_submit_request(OP_INIT, 0, 0, 0, NULL);
    if (req_id < 0) {
        fprintf(stderr, "Failed to submit initialization request\n");
        swap_shm_cleanup(false);
        kill(swap_process_pid, SIGTERM);
        pthread_mutex_unlock(&swap_mutex);
        return -1;
    }
    
    // Wait for initialization to complete (with timeout)
    int error_code;
    for (int i = 0; i < 5; i++) {
        ret = swap_wait_request(req_id, &error_code);
        if (ret == 0) {
            break;
        }
        
        usleep(100000);  // 100ms
    }
    
    if (ret != 0) {
        fprintf(stderr, "Timeout waiting for swap process initialization\n");
        swap_shm_cleanup(false);
        kill(swap_process_pid, SIGTERM);
        pthread_mutex_unlock(&swap_mutex);
        return -1;
    }
    
    swap_initialized = true;
    pthread_mutex_unlock(&swap_mutex);
    
    return 0;
}

// Shutdown with improved cleanup
void storage_shutdown() {
    pthread_mutex_lock(&swap_mutex);
    
    if (!swap_initialized) {
        pthread_mutex_unlock(&swap_mutex);
        return;
    }
    
    // Send shutdown request
    uint64_t req_id = swap_submit_request(OP_SHUTDOWN, 0, 0, 0, NULL);
    
    // Wait briefly for shutdown to complete
    if (req_id >= 0) {
        int error_code;
        swap_wait_request(req_id, &error_code);
    }
    
    // Clean up shared memory
    swap_shm_cleanup(false);
    
    // Wait for swap process to exit (with timeout)
    int status;
    for (int i = 0; i < 10; i++) {
        if (waitpid(swap_process_pid, &status, WNOHANG) == swap_process_pid) {
            break;
        }
        
        usleep(100000);  // 100ms
    }
    
    // Force terminate if still running
    if (kill(swap_process_pid, 0) == 0) {
        kill(swap_process_pid, SIGTERM);
        usleep(100000);  // 100ms
        
        // Force kill if still running
        if (kill(swap_process_pid, 0) == 0) {
            kill(swap_process_pid, SIGKILL);
        }
    }
    
    swap_initialized = false;
    pthread_mutex_unlock(&swap_mutex);
}

// Add retry logic for improved reliability
int swap_process_read_page(int fd, uint64_t offset, void* dest, size_t size) {
    int ret;
    int error_code;
    
    // Retry loop
    for (int retry = 0; retry < 3; retry++) {
        // Send read request
        uint64_t req_id = swap_submit_request(OP_READ_PAGE, 0, offset, size, NULL);
        if (req_id < 0) {
            if (retry == 2) {
                return -1;
            }
            usleep(100000);  // 100ms
            continue;
        }
        
        // Wait for read to complete (with timeout)
        for (int i = 0; i < 5; i++) {
            ret = swap_wait_request(req_id, &error_code);
            if (ret == 0) {
                break;
            }
            
            usleep(100000);  // 100ms
        }
        
        if (ret != 0) {
            if (retry == 2) {
                return -1;
            }
            continue;
        }
        
        // Copy data from shared memory
        swap_shm_t* shm = get_swap_shm();
        if (shm == NULL) {
            if (retry == 2) {
                return -1;
            }
            continue;
        }
        
        // Find request's buffer_offset
        pthread_mutex_lock(&shm->queue_mutex);
        uint64_t buffer_offset = 0;
        bool found = false;
        for (uint32_t i = 0; i < MAX_QUEUE_SIZE; i++) {
            if (shm->requests[i].request_id == req_id) {
                buffer_offset = shm->requests[i].buffer_offset;
                found = true;
                break;
            }
        }
        pthread_mutex_unlock(&shm->queue_mutex);
        
        if (found) {
            memcpy(dest, shm->page_buffer + buffer_offset, size);
            return 0;
        }
        
        // Request not found
        if (retry == 2) {
            return -1;
        }
    }
    
    // All retries failed
    return -1;
}

// Add retry logic for writes as well
int swap_process_write_page(int fd, uint64_t offset, void* src, size_t size) {
    int ret;
    int error_code;
    
    // Retry loop
    for (int retry = 0; retry < 3; retry++) {
        // Send write request
        uint64_t req_id = swap_submit_request(OP_WRITE_PAGE, 0, offset, size, src);
        if (req_id < 0) {
            if (retry == 2) {
                return -1;
            }
            usleep(100000);  // 100ms
            continue;
        }
        
        // Wait for write to complete (with timeout)
        for (int i = 0; i < 5; i++) {
            ret = swap_wait_request(req_id, &error_code);
            if (ret == 0) {
                return 0;
            }
            
            usleep(100000);  // 100ms
        }
        
        if (retry == 2) {
            return -1;
        }
    }
    
    // All retries failed
    return -1;
}

// Create a checkpoint of the current swap file state
int swap_process_checkpoint(const char* checkpoint_name) {
    if (!swap_initialized) {
        fprintf(stderr, "Swap process not initialized\n");
        return -1;
    }
    
    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);
    
    // Send checkpoint request
    uint64_t req_id = swap_submit_checkpoint_request(checkpoint_name);
    if (req_id < 0) {
        fprintf(stderr, "Failed to submit checkpoint request\n");
        return -1;
    }
    
    // Wait for checkpoint to complete
    int error_code;
    int ret = swap_wait_request(req_id, &error_code);
    
    gettimeofday(&end_time, NULL);
    double elapsed = (end_time.tv_sec - start_time.tv_sec) + 
                    (end_time.tv_usec - start_time.tv_usec) / 1000000.0;
    
    printf("Checkpoint '%s' completed in %.3f seconds\n", 
           checkpoint_name ? checkpoint_name : "unnamed", elapsed);
    
    if (ret != 0) {
        fprintf(stderr, "Checkpoint failed with error code: %d\n", error_code);
        return -1;
    }
    
    return 0;
}

// Get the time taken for the last checkpoint
double swap_process_get_last_checkpoint_time(void) {
    swap_shm_t* shm = get_swap_shm();
    if (shm == NULL) {
        return -1.0;
    }
    
    return shm->last_checkpoint_time;
}