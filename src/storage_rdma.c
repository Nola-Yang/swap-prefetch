#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <spawn.h>
#include <sys/time.h>

#include "core.h"
#include "rdma_sim.h"
#include "storage_rdma.h"

static pid_t rdma_process_pid = -1;
static bool rdma_initialized = false;
static pthread_mutex_t rdma_mutex = PTHREAD_MUTEX_INITIALIZER;

// Remote memory allocation table
#define MAX_REMOTE_REGIONS 65536
typedef struct {
    uint64_t disk_offset;     // Disk offset (used as key)
    uint64_t remote_addr;     // Address in remote memory
    size_t size;              // Size of allocation
    bool in_use;              // Whether this entry is in use
    uint64_t last_access;     // Last access timestamp
} remote_region_t;

static remote_region_t remote_regions[MAX_REMOTE_REGIONS];
static int num_remote_regions = 0;
static pthread_mutex_t remote_regions_mutex = PTHREAD_MUTEX_INITIALIZER;

// Statistics
static uint64_t rdma_reads = 0;
static uint64_t rdma_writes = 0;
static uint64_t rdma_hits = 0;
static uint64_t rdma_misses = 0;

// Function to find the RDMA process executable
static char* find_rdma_process() {
    const char* paths[] = {
        "./rdma_process",                   // Current directory
        "../bin/rdma_process",              // Bin directory relative to current directory
        "/usr/local/bin/rdma_process",      // System installation path
        NULL                                // End marker
    };
    
    // Check environment variable first
    const char* env_path = getenv("EXTMEM_RDMA_PROCESS_PATH");
    if (env_path != NULL) {
        if (access(env_path, X_OK) == 0) {
            char* path = strdup(env_path);
            return path;
        }
        fprintf(stderr, "RDMA process not found at EXTMEM_RDMA_PROCESS_PATH: %s\n", env_path);
    }
    
    // Try each of the predefined paths
    for (int i = 0; paths[i] != NULL; i++) {
        if (access(paths[i], X_OK) == 0) {
            return strdup(paths[i]);
        }
    }
    
    return NULL;
}

// Start RDMA process
static int start_rdma_process() {
    char* rdma_path = find_rdma_process();
    if (rdma_path == NULL) {
        fprintf(stderr, "Failed to find RDMA process executable\n");
        return -1;
    }
    
    char *argv[] = {rdma_path, NULL};
    char *envp[] = {NULL};
    int ret;
    
    // Use posix_spawn to launch RDMA process
    ret = posix_spawn(&rdma_process_pid, rdma_path, NULL, NULL, argv, envp);
    free(rdma_path);
    
    if (ret != 0) {
        perror("posix_spawn");
        return -1;
    }
    
    LOG("RDMA process started with PID: %d\n", rdma_process_pid);
    
    // Wait for process to initialize (with a timeout)
    for (int i = 0; i < 10; i++) {
        usleep(100000);  // 100ms
        
        // Check if process is still running
        if (kill(rdma_process_pid, 0) != 0) {
            if (errno == ESRCH) {
                fprintf(stderr, "RDMA process exited prematurely\n");
                return -1;
            }
            perror("kill check");
            return -1;
        }
        
        // Try to connect to shared memory
        if (rdma_sim_init(false) == 0) {
            // Successful connection, break out of loop
            return 0;
        }
    }
    
    // Timeout waiting for RDMA process to initialize
    fprintf(stderr, "Timeout waiting for RDMA process to initialize\n");
    kill(rdma_process_pid, SIGTERM);
    return -1;
}

// Find a remote region for a given disk offset
static remote_region_t* find_remote_region(uint64_t disk_offset) {
    for (int i = 0; i < num_remote_regions; i++) {
        if (remote_regions[i].in_use && remote_regions[i].disk_offset == disk_offset) {
            return &remote_regions[i];
        }
    }
    return NULL;
}

// Allocate a new remote region
static remote_region_t* allocate_remote_region(uint64_t disk_offset, size_t size) {
    int free_index = -1;
    
    // Find a free slot
    for (int i = 0; i < MAX_REMOTE_REGIONS; i++) {
        if (!remote_regions[i].in_use) {
            free_index = i;
            break;
        }
    }
    
    if (free_index == -1) {
        // No free slots, evict an entry using LRU
        uint64_t oldest_time = UINT64_MAX;
        int oldest_index = -1;
        
        for (int i = 0; i < num_remote_regions; i++) {
            if (remote_regions[i].in_use && remote_regions[i].last_access < oldest_time) {
                oldest_time = remote_regions[i].last_access;
                oldest_index = i;
            }
        }
        
        if (oldest_index == -1) {
            return NULL;  // Should not happen
        }
        
        // Free the oldest region in remote memory
        rdma_sim_free(remote_regions[oldest_index].remote_addr);
        free_index = oldest_index;
    }
    
    // Allocate memory in remote node
    uint64_t remote_addr;
    int ret = rdma_sim_allocate(size, &remote_addr);
    if (ret != 0) {
        return NULL;
    }
    
    // Update the region entry
    remote_regions[free_index].disk_offset = disk_offset;
    remote_regions[free_index].remote_addr = remote_addr;
    remote_regions[free_index].size = size;
    remote_regions[free_index].in_use = true;
    remote_regions[free_index].last_access = time(NULL);
    
    if (free_index >= num_remote_regions) {
        num_remote_regions = free_index + 1;
    }
    
    return &remote_regions[free_index];
}

// Initialize RDMA storage
int rdma_storage_init() {
    int ret;
    
    pthread_mutex_lock(&rdma_mutex);
    
    if (rdma_initialized) {
        pthread_mutex_unlock(&rdma_mutex);
        return 0;  // Already initialized
    }
    
    // Initialize remote regions tracking
    memset(remote_regions, 0, sizeof(remote_regions));
    num_remote_regions = 0;
    
    // Start RDMA process
    ret = start_rdma_process();
    if (ret != 0) {
        pthread_mutex_unlock(&rdma_mutex);
        return -1;
    }
    
    // Send initialization request to RDMA process
    uint64_t req_id = rdma_submit_request(RDMA_OP_INIT, 0, 0, 0, NULL);
    if (req_id < 0) {
        fprintf(stderr, "Failed to submit RDMA initialization request\n");
        rdma_sim_cleanup(false);
        kill(rdma_process_pid, SIGTERM);
        pthread_mutex_unlock(&rdma_mutex);
        return -1;
    }
    
    // Wait for initialization to complete
    int error_code;
    for (int i = 0; i < 5; i++) {
        ret = rdma_wait_request(req_id, &error_code);
        if (ret == 0) {
            break;
        }
        
        usleep(100000);  // 100ms
    }
    
    if (ret != 0) {
        fprintf(stderr, "Timeout waiting for RDMA process initialization\n");
        rdma_sim_cleanup(false);
        kill(rdma_process_pid, SIGTERM);
        pthread_mutex_unlock(&rdma_mutex);
        return -1;
    }
    
    rdma_initialized = true;
    
    // Reset statistics
    rdma_reads = 0;
    rdma_writes = 0;
    rdma_hits = 0;
    rdma_misses = 0;
    
    pthread_mutex_unlock(&rdma_mutex);
    
    return 0;
}

// Shutdown RDMA storage
void rdma_storage_shutdown() {
    pthread_mutex_lock(&rdma_mutex);
    
    if (!rdma_initialized) {
        pthread_mutex_unlock(&rdma_mutex);
        return;
    }
    
    // Send shutdown request
    uint64_t req_id = rdma_submit_request(RDMA_OP_SHUTDOWN, 0, 0, 0, NULL);
    
    // Wait briefly for shutdown to complete
    if (req_id >= 0) {
        int error_code;
        rdma_wait_request(req_id, &error_code);
    }
    
    // Clean up shared memory
    rdma_sim_cleanup(false);
    
    // Wait for RDMA process to exit (with timeout)
    int status;
    for (int i = 0; i < 10; i++) {
        if (waitpid(rdma_process_pid, &status, WNOHANG) == rdma_process_pid) {
            break;
        }
        
        usleep(100000);  // 100ms
    }
    
    // Force terminate if still running
    if (kill(rdma_process_pid, 0) == 0) {
        kill(rdma_process_pid, SIGTERM);
        usleep(100000);  // 100ms
        
        // Force kill if still running
        if (kill(rdma_process_pid, 0) == 0) {
            kill(rdma_process_pid, SIGKILL);
        }
    }
    
    rdma_initialized = false;
    
    // Print final statistics
    printf("RDMA Storage Statistics:\n");
    printf("  Reads: %lu\n", rdma_reads);
    printf("  Writes: %lu\n", rdma_writes);
    printf("  Hits: %lu (%.2f%%)\n", rdma_hits, 
           (rdma_reads > 0) ? (double)rdma_hits / rdma_reads * 100.0 : 0.0);
    printf("  Misses: %lu\n", rdma_misses);
    
    pthread_mutex_unlock(&rdma_mutex);
}

// Read a page from RDMA storage
int rdma_read_page(int fd, uint64_t disk_offset, void* dest, size_t size) {
    if (!rdma_initialized) {
        fprintf(stderr, "RDMA storage not initialized\n");
        return -1;
    }
    
    rdma_reads++;
    
    // Check if the page is in remote memory
    pthread_mutex_lock(&remote_regions_mutex);
    remote_region_t* region = find_remote_region(disk_offset);
    
    if (region) {
        // Page is in remote memory, read it using RDMA
        region->last_access = time(NULL);
        uint64_t remote_addr = region->remote_addr;
        pthread_mutex_unlock(&remote_regions_mutex);
        
        rdma_hits++;
        
        // Perform RDMA read
        uint64_t req_id = rdma_submit_request(RDMA_OP_READ, (uint64_t)dest, remote_addr, size, NULL);
        if (req_id < 0) {
            fprintf(stderr, "Failed to submit RDMA read request\n");
            return -1;
        }
        
        // Wait for read to complete
        int error_code;
        int ret = rdma_wait_request(req_id, &error_code);
        if (ret != 0) {
            fprintf(stderr, "RDMA read failed with error code: %d\n", error_code);
            return -1;
        }
        
        // Copy data from RDMA buffer to destination
        rdma_shm_t* shm = get_rdma_shm();
        if (shm == NULL) {
            fprintf(stderr, "Failed to get RDMA shared memory\n");
            return -1;
        }
        
        // Find request's buffer_offset
        pthread_mutex_lock(&shm->queue_mutex);
        uint64_t buffer_offset = 0;
        bool found = false;
        for (uint32_t i = 0; i < RDMA_MAX_QUEUE_SIZE; i++) {
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
        
        return -1;
    } else {
        // Page is not in remote memory, need to read from disk
        pthread_mutex_unlock(&remote_regions_mutex);
        
        rdma_misses++;
        
        // Read from disk (fall back to traditional I/O)
        int ret = pread(fd, dest, size, disk_offset);
        if (ret != size) {
            perror("pread");
            return -1;
        }
        
        // Allocate in remote memory for future access
        pthread_mutex_lock(&remote_regions_mutex);
        region = allocate_remote_region(disk_offset, size);
        if (region) {
            // Copy to remote memory
            uint64_t remote_addr = region->remote_addr;
            pthread_mutex_unlock(&remote_regions_mutex);
            
            // Perform RDMA write to cache the page
            uint64_t req_id = rdma_submit_request(RDMA_OP_WRITE, (uint64_t)dest, remote_addr, size, dest);
            if (req_id < 0) {
                fprintf(stderr, "Failed to submit RDMA write request for caching\n");
                // Not critical, we can still return success for the read
            } else {
                // Wait for write to complete
                int error_code;
                int ret = rdma_wait_request(req_id, &error_code);
                if (ret != 0) {
                    fprintf(stderr, "RDMA write for caching failed with error code: %d\n", error_code);
                    // Not critical, we can still return success for the read
                }
            }
        } else {
            pthread_mutex_unlock(&remote_regions_mutex);
            // Unable to allocate remote memory, but read was successful
        }
        
        return 0;
    }
}

// Write a page to RDMA storage
int rdma_write_page(int fd, uint64_t disk_offset, void* src, size_t size) {
    if (!rdma_initialized) {
        fprintf(stderr, "RDMA storage not initialized\n");
        return -1;
    }
    
    rdma_writes++;
    
    // First, write to disk to ensure durability
    int ret = pwrite(fd, src, size, disk_offset);
    if (ret != size) {
        perror("pwrite");
        return -1;
    }
    
    // Then, update remote memory if the page is cached
    pthread_mutex_lock(&remote_regions_mutex);
    remote_region_t* region = find_remote_region(disk_offset);
    
    if (region) {
        // Page is in remote memory, update it
        region->last_access = time(NULL);
        uint64_t remote_addr = region->remote_addr;
        pthread_mutex_unlock(&remote_regions_mutex);
        
        // Perform RDMA write
        uint64_t req_id = rdma_submit_request(RDMA_OP_WRITE, (uint64_t)src, remote_addr, size, src);
        if (req_id < 0) {
            fprintf(stderr, "Failed to submit RDMA write request\n");
            // Not critical, disk write was successful
            return 0;
        }
        
        // Wait for write to complete
        int error_code;
        ret = rdma_wait_request(req_id, &error_code);
        if (ret != 0) {
            fprintf(stderr, "RDMA write failed with error code: %d\n", error_code);
            // Not critical, disk write was successful
            return 0;
        }
    } else {
        // Page is not in remote memory, allocate and cache it
        region = allocate_remote_region(disk_offset, size);
        if (region) {
            // Cache the page in remote memory
            uint64_t remote_addr = region->remote_addr;
            pthread_mutex_unlock(&remote_regions_mutex);
            
            // Perform RDMA write
            uint64_t req_id = rdma_submit_request(RDMA_OP_WRITE, (uint64_t)src, remote_addr, size, src);
            if (req_id < 0) {
                fprintf(stderr, "Failed to submit RDMA write request for caching\n");
                // Not critical, disk write was successful
                return 0;
            }
            
            // Wait for write to complete
            int error_code;
            ret = rdma_wait_request(req_id, &error_code);
            if (ret != 0) {
                fprintf(stderr, "RDMA write for caching failed with error code: %d\n", error_code);
                // Not critical, disk write was successful
                return 0;
            }
        } else {
            pthread_mutex_unlock(&remote_regions_mutex);
            // Unable to allocate remote memory, but disk write was successful
        }
    }
    
    return 0;
}

// Get RDMA statistics
void rdma_get_storage_stats(uint64_t *reads, uint64_t *writes, uint64_t *hits, uint64_t *misses) {
    if (reads) *reads = rdma_reads;
    if (writes) *writes = rdma_writes;
    if (hits) *hits = rdma_hits;
    if (misses) *misses = rdma_misses;
}