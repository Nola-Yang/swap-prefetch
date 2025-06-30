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
#include "include/rdma_sim.h"
#include "include/storage_rdma.h"
#include "include/pointer_prefetch.h"
#include "include/address_translation.h"

static pid_t rdma_process_pid = -1;
static bool rdma_initialized = false;
static pthread_mutex_t rdma_mutex = PTHREAD_MUTEX_INITIALIZER;


#define MAX_REMOTE_REGIONS 65536
typedef struct {
    uint64_t virtual_offset;      
    uint64_t remote_addr;         
    size_t size;                  
    bool in_use;                 
    uint64_t last_access;         
    bool dirty;                   
} remote_region_t;

static remote_region_t remote_regions[MAX_REMOTE_REGIONS];
static int num_remote_regions = 0;
static pthread_mutex_t remote_regions_mutex = PTHREAD_MUTEX_INITIALIZER;


static uint64_t rdma_reads = 0;
static uint64_t rdma_writes = 0;
static uint64_t rdma_hits = 0;
static uint64_t rdma_misses = 0;
static uint64_t rdma_allocations = 0;
static uint64_t rdma_frees = 0;


static char* find_rdma_process() {
    const char* paths[] = {
        "./rdma_process",
        "../bin/rdma_process", 
        "/usr/local/bin/rdma_process",
        NULL
    };
    
    const char* env_path = getenv("EXTMEM_RDMA_PROCESS_PATH");
    if (env_path != NULL) {
        if (access(env_path, X_OK) == 0) {
            return strdup(env_path);
        }
        fprintf(stderr, "RDMA process not found at EXTMEM_RDMA_PROCESS_PATH: %s\n", env_path);
    }
    
    for (int i = 0; paths[i] != NULL; i++) {
        if (access(paths[i], X_OK) == 0) {
            return strdup(paths[i]);
        }
    }
    
    return NULL;
}

static int start_rdma_process() {
    char* rdma_path = find_rdma_process();
    if (rdma_path == NULL) {
        fprintf(stderr, "Failed to find RDMA process executable\n");
        return -1;
    }
    
    char *argv[] = {rdma_path, "-v", NULL};  
    char *envp[] = {NULL};
    int ret;
    
    ret = posix_spawn(&rdma_process_pid, rdma_path, NULL, NULL, argv, envp);
    free(rdma_path);
    
    if (ret != 0) {
        perror("posix_spawn");
        return -1;
    }
    
    LOG("RDMA process started with PID: %d\n", rdma_process_pid);
    
    for (int i = 0; i < 10; i++) {
        usleep(100000);  // 100ms
        
        if (kill(rdma_process_pid, 0) != 0) {
            if (errno == ESRCH) {
                fprintf(stderr, "RDMA process exited prematurely\n");
                return -1;
            }
        }
        
        if (rdma_sim_init(false) == 0) {
            return 0;
        }
    }
    
    fprintf(stderr, "Timeout waiting for RDMA process to initialize\n");
    kill(rdma_process_pid, SIGTERM);
    return -1;
}

static remote_region_t* find_remote_region(uint64_t virtual_offset) {
    for (int i = 0; i < num_remote_regions; i++) {
        if (remote_regions[i].in_use && remote_regions[i].virtual_offset == virtual_offset) {
            return &remote_regions[i];
        }
    }
    return NULL;
}

static remote_region_t* allocate_remote_region(uint64_t key_virtual_offset, size_t size) {
    int free_index = -1;
    
    for (int i = 0; i < MAX_REMOTE_REGIONS; i++) {
        if (!remote_regions[i].in_use) {
            free_index = i;
            break;
        }
    }
    
    if (free_index == -1) {
        uint64_t oldest_time = UINT64_MAX;
        int oldest_index = -1;
        
        for (int i = 0; i < num_remote_regions; i++) {
            if (remote_regions[i].in_use && remote_regions[i].last_access < oldest_time) {
                oldest_time = remote_regions[i].last_access;
                oldest_index = i;
            }
        }
        
        if (oldest_index == -1) {
            return NULL;
        }
        
        rdma_sim_free(remote_regions[oldest_index].remote_addr);
        free_index = oldest_index;
        rdma_frees++;
    }
    
    uint64_t req_id = rdma_submit_request(RDMA_OP_ALLOCATE, 0, 0, size, NULL);
    if (req_id == 0) {
        return NULL;
    }

    int error_code;
    int ret = rdma_wait_request(req_id, &error_code);
    if (ret != 0) {
        return NULL;
    }

    uint64_t remote_addr = 0;
    for (uint32_t i = 0; i < MAX_RDMA_REQUESTS; i++) {
        rdma_shm_t* shm = get_rdma_shm();
        if (shm && shm->requests[i].request_id == req_id) {
            remote_addr = shm->requests[i].remote_addr;
            break;
        }
    }
    
    if (remote_addr == 0) {
        return NULL;
    }
    
    remote_regions[free_index].virtual_offset = key_virtual_offset;
    remote_regions[free_index].remote_addr = remote_addr;
    remote_regions[free_index].size = size;
    remote_regions[free_index].in_use = true;
    remote_regions[free_index].last_access = time(NULL);
    remote_regions[free_index].dirty = false;
    
    if (free_index >= num_remote_regions) {
        num_remote_regions = free_index + 1;
    }
    
    rdma_allocations++;
    return &remote_regions[free_index];
}

int rdma_storage_init() {
    pthread_mutex_lock(&rdma_mutex);
    
    if (rdma_initialized) {
        pthread_mutex_unlock(&rdma_mutex);
        return 0;
    }
    
    printf("Initializing pure RDMA storage (no disk backend)...\n");
    
    memset(remote_regions, 0, sizeof(remote_regions));
    num_remote_regions = 0;
    
    int ret = -1;
    fprintf(stderr, "RDMA_STORAGE_INIT: Attempting to connect to existing RDMA server (rdma_sim_init(false))...\n");
    if (rdma_sim_init(false) == 0) { 
        fprintf(stderr, "RDMA_STORAGE_INIT: Successfully connected to existing RDMA server via rdma_sim_init(false).\n");
        ret = 0; 
    } else {
        fprintf(stderr, "RDMA_STORAGE_INIT: Failed to connect to existing RDMA server. Attempting to start new server via start_rdma_process()...\n");
        ret = start_rdma_process(); 
        if (ret == 0) {
            fprintf(stderr, "RDMA_STORAGE_INIT: Successfully started and connected to new RDMA server via start_rdma_process().\n");
        } else {
            fprintf(stderr, "RDMA_STORAGE_INIT: Failed to start and connect to new RDMA server via start_rdma_process(). Error code from start_rdma_process: %d\n", ret);
        }
    }
    
    if (ret != 0) {
        pthread_mutex_unlock(&rdma_mutex);
        return -1; // Failed to connect or start & connect
    }
    
    uint64_t req_id = rdma_submit_request(RDMA_OP_INIT, 0, 0, 0, NULL);
    if (req_id < 0) {
        fprintf(stderr, "Failed to submit RDMA initialization request\n");
        rdma_sim_cleanup(false);
        kill(rdma_process_pid, SIGTERM);
        pthread_mutex_unlock(&rdma_mutex);
        return -1;
    }
    
    int error_code;
    for (int i = 0; i < 5; i++) {
        ret = rdma_wait_request(req_id, &error_code);
        if (ret == 0) {
            break;
        }
        usleep(100000);
    }
    
    if (ret != 0) {
        fprintf(stderr, "Timeout waiting for RDMA process initialization\n");
        rdma_sim_cleanup(false);
        kill(rdma_process_pid, SIGTERM);
        pthread_mutex_unlock(&rdma_mutex);
        return -1;
    }
    
    rdma_initialized = true;
    
    rdma_reads = rdma_writes = rdma_hits = rdma_misses = 0;
    rdma_allocations = rdma_frees = 0;
    
    printf("Pure RDMA storage initialized successfully\n");
    pthread_mutex_unlock(&rdma_mutex);
    return 0;
}

void rdma_storage_shutdown() {
    pthread_mutex_lock(&rdma_mutex);
    
    if (!rdma_initialized) {
        pthread_mutex_unlock(&rdma_mutex);
        return;
    }
    
    printf("Shutting down pure RDMA storage...\n");
    
    uint64_t req_id = rdma_submit_request(RDMA_OP_SHUTDOWN, 0, 0, 0, NULL);
    if (req_id >= 0) {
        int error_code;
        rdma_wait_request(req_id, &error_code);
    }
    
    rdma_sim_cleanup(false);
    
    int status;
    for (int i = 0; i < 10; i++) {
        if (waitpid(rdma_process_pid, &status, WNOHANG) == rdma_process_pid) {
            break;
        }
        usleep(100000);
    }
    
    if (kill(rdma_process_pid, 0) == 0) {
        kill(rdma_process_pid, SIGTERM);
        usleep(100000);
        if (kill(rdma_process_pid, 0) == 0) {
            kill(rdma_process_pid, SIGKILL);
        }
    }
    
    rdma_initialized = false;
    
    printf("\n=== Pure RDMA Storage Statistics ===\n");
    printf("Reads: %lu\n", rdma_reads);
    printf("Writes: %lu\n", rdma_writes); 
    printf("Hits: %lu (%.2f%%)\n", rdma_hits,
           (rdma_reads > 0) ? (double)rdma_hits / rdma_reads * 100.0 : 0.0);
    printf("Misses: %lu\n", rdma_misses);
    printf("Allocations: %lu\n", rdma_allocations);
    printf("Frees: %lu\n", rdma_frees);
    printf("Active regions: %d\n", num_remote_regions);
    printf("=====================================\n");
    
    pthread_mutex_unlock(&rdma_mutex);
}

uint64_t rdma_allocate_page_offset(size_t size) {
    if (!rdma_initialized) {
        return UINT64_MAX;
    }
    
    pthread_mutex_lock(&remote_regions_mutex);
    remote_region_t* region = allocate_remote_region(0, size);
    uint64_t offset = UINT64_MAX;
    
    if (region) {
        offset = region->virtual_offset;
        LOG("Allocated RDMA region: offset=%lu, size=%zu, remote_addr=0x%lx\n",
            offset, size, region->remote_addr);
    }
    
    pthread_mutex_unlock(&remote_regions_mutex);
    return offset;
}

int rdma_read_page(int fd, uint64_t virtual_offset, void* dest, size_t size) {
    fprintf(stderr, "DEBUG_SWAP: rdma_read_page CALLED for virtual_offset: 0x%lx\n", virtual_offset); fflush(stderr);
    if (!rdma_initialized) {

        fprintf(stderr, "RDMA storage not initialized\n");
        return -1;
    }
    
    rdma_reads++;
    
    pthread_mutex_lock(&remote_regions_mutex);
    remote_region_t* region = find_remote_region(virtual_offset);
    
    if (region == NULL) {
        LOG("RDMA Read Error: virtual_offset 0x%lx not found in remote_regions.\n", virtual_offset);
        rdma_misses++;
        pthread_mutex_unlock(&remote_regions_mutex);
        return -EFAULT; // Or another suitable error code
    }
    
    region->last_access = time(NULL);
    uint64_t remote_addr = region->remote_addr;
    pthread_mutex_unlock(&remote_regions_mutex);
    
    rdma_hits++;
    
    uint64_t req_id = rdma_submit_request(RDMA_OP_READ, (uint64_t)dest, remote_addr, size, NULL);
    if (req_id < 0) {
        fprintf(stderr, "Failed to submit RDMA read request\n");
        return -1;
    }
    
    int error_code;
    int ret = rdma_wait_request(req_id, &error_code);
    if (ret != 0) {
        fprintf(stderr, "RDMA read failed with error code: %d\n", error_code);
        return -1;
    }
    
    rdma_shm_t* shm = get_rdma_shm();
    if (shm) {
        memcpy(dest, shm->page_buffer, size);
    }
    
    if (size == PAGE_SIZE) {
        int conv_ret = convert_pointers_from_remote_storage(dest, virtual_offset);
        if (conv_ret != 0) {
            LOG("Warning: Pointer conversion failed for virtual_offset 0x%lx during read\n", virtual_offset);
        }
    }
    
    LOG("RDMA read completed with pointer conversion: offset=%lu, size=%zu\n", virtual_offset, size);
    return 0;
}

int rdma_write_page(int fd, uint64_t virtual_offset, void* src, size_t size) {
    fprintf(stderr, "DEBUG_SWAP: rdma_write_page CALLED for virtual_offset: 0x%lx\n", virtual_offset); fflush(stderr);
    if (!rdma_initialized) {
        fprintf(stderr, "RDMA storage not initialized\n");
        return -1;
    }
    
    rdma_writes++;
    
    pthread_mutex_lock(&remote_regions_mutex);
    remote_region_t* region = find_remote_region(virtual_offset);
    
    if (region == NULL) {
        LOG("RDMA Write: virtual_offset 0x%lx not found. Allocating new region.\n", virtual_offset);
        region = allocate_remote_region(virtual_offset, size); 
        if (region == NULL) {
            pthread_mutex_unlock(&remote_regions_mutex);
            LOG("RDMA Write Error: Failed to allocate remote_region for virtual_offset 0x%lx.\n", virtual_offset);
            return -ENOMEM; 
        }
        rdma_misses++; 
    } else {
        rdma_hits++;
    }
    
    
    if (region->size != size) {
        LOG("RDMA Write Warning: virtual_offset 0x%lx region size %zu an PPage size %zu for remote_addr 0x%lx. Re-allocating.\n",
            virtual_offset, region->size, size, region->remote_addr);
        // Free the old one and allocate a new one with the correct size
        rdma_sim_free(region->remote_addr);
        rdma_frees++;
        region->in_use = false; // Mark as not in use before re-calling allocate
        
        region = allocate_remote_region(virtual_offset, size);
        if (region == NULL) {
            pthread_mutex_unlock(&remote_regions_mutex);
            LOG("RDMA Write Error: Failed to re-allocate remote_region for virtual_offset 0x%lx after size mismatch.\n", virtual_offset);
            return -ENOMEM;
        }
    }

    region->last_access = time(NULL);
    region->dirty = true; 
    pthread_mutex_unlock(&remote_regions_mutex);

    void* converted_page_data = malloc(size);
    if (converted_page_data == NULL) {
        fprintf(stderr, "Failed to allocate memory for pointer conversion\n");
        return -ENOMEM;
    }

    memcpy(converted_page_data, src, size);
    
    
    if (size == PAGE_SIZE) {
        // Register address region for this page if not already done
        register_address_region(virtual_offset, size, region->remote_addr);
        
        int conv_ret = convert_pointers_for_remote_storage(converted_page_data, virtual_offset);
        if (conv_ret != 0) {
            LOG("Warning: Pointer conversion failed for virtual_offset 0x%lx\n", virtual_offset);
        }
    }
    
    uint64_t req_id = rdma_submit_request(RDMA_OP_WRITE, (uint64_t)converted_page_data, region->remote_addr, size, converted_page_data);
    if (req_id == 0) {
        free(converted_page_data);
        fprintf(stderr, "DEBUG: Failed to submit RDMA write request (req_id=0) for offset %lu\n", virtual_offset);
        return -1;
    }
    
    int error_code;
    int ret = rdma_wait_request(req_id, &error_code);
    
    free(converted_page_data);
    
    if (ret != 0) {
        fprintf(stderr, "RDMA write failed for offset %lu with error code: %d (ret=%d)\n", virtual_offset, error_code, ret);
        return -1;
    }
    
    LOG("RDMA write completed with pointer conversion: offset=%lu, size=%zu\n", virtual_offset, size);
    return 0;
}

void rdma_get_storage_stats(uint64_t *reads, uint64_t *writes, uint64_t *hits, uint64_t *misses) {
    if (reads) *reads = rdma_reads;
    if (writes) *writes = rdma_writes;
    if (hits) *hits = rdma_hits;
    if (misses) *misses = rdma_misses;
}

void rdma_print_detailed_stats() {
    pthread_mutex_lock(&remote_regions_mutex);
    
    printf("\n=== Detailed RDMA Statistics ===\n");
    printf("Operations:\n");
    printf("  Reads: %lu (hits: %lu, misses: %lu)\n", rdma_reads, rdma_hits, rdma_misses);
    printf("  Writes: %lu\n", rdma_writes);
    printf("  Hit rate: %.2f%%\n", (rdma_reads > 0) ? (double)rdma_hits / rdma_reads * 100.0 : 0.0);
    
    printf("Memory management:\n");
    printf("  Allocations: %lu\n", rdma_allocations);
    printf("  Frees: %lu\n", rdma_frees);
    printf("  Active regions: %d / %d\n", num_remote_regions, MAX_REMOTE_REGIONS);
    
    printf("Active regions:\n");
    int shown = 0;
    for (int i = 0; i < num_remote_regions && shown < 10; i++) {
        if (remote_regions[i].in_use) {
            printf("  [%d] offset=%lu, size=%zu, remote=0x%lx, dirty=%s\n",
                   i, remote_regions[i].virtual_offset, remote_regions[i].size,
                   remote_regions[i].remote_addr, remote_regions[i].dirty ? "yes" : "no");
            shown++;
        }
    }
    if (num_remote_regions > 10) {
        printf("  ... and %d more\n", num_remote_regions - 10);
    }
    
    printf("===============================\n");
    
    pthread_mutex_unlock(&remote_regions_mutex);
}