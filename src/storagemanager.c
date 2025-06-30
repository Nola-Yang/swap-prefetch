#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include "core.h" // For LOG, PAGE_SIZE, etc.

#include "storagemanager.h"
#include "include/storage_rdma.h" // Added for RDMA functions

#ifdef USWAP_RDMA
#define CURRENT_STORAGE_TYPE STORAGE_RDMA
#endif

// Storage statistics
static uint64_t total_reads = 0;
static uint64_t total_writes = 0;

/**
 * Initialize storage backend (pure RDMA)
 */
int storage_init(void) {
    printf("Initializing pure RDMA storage backend...\n");
    
    int ret = rdma_storage_init();
    if (ret != 0) {
        fprintf(stderr, "Failed to initialize RDMA storage\n");
        return -1;
    }
    
    // Reset statistics
    total_reads = 0;
    total_writes = 0;
    
    printf("Pure RDMA storage backend initialized successfully\n");
    return 0;
}

/**
 * Shutdown storage backend
 */
void storage_shutdown(void) {
    printf("Shutting down pure RDMA storage backend...\n");
    
    // Print final statistics
    print_storage_stats();
    
    rdma_storage_shutdown();
    
    printf("Pure RDMA storage backend shut down\n");
}

/**
 * Write a page to storage (pure RDMA)
 */
int write_page(int fd, uint64_t offset, const void* src, size_t size) {
    total_writes++;
    
    int ret = rdma_write_page(fd, offset, (void*)src, size);
    if (ret != 0) {
        fprintf(stderr, "RDMA write failed for offset %lu\n", offset);
        return -1;
    }
    
    return 0;
}

/**
 * Read a page from storage (pure RDMA)
 */
int read_page(int fd, uint64_t offset, void* dest, size_t size) {
    total_reads++;
    
    int ret = rdma_read_page(fd, offset, dest, size);
    if (ret != 0) {
        fprintf(stderr, "RDMA read failed for offset %lu\n", offset);
        return -1;
    }
    
    return 0;
}

/**
 * Get storage statistics
 */
void get_storage_stats(uint64_t *reads, uint64_t *writes, uint64_t *hits, uint64_t *misses) {
    uint64_t rdma_reads, rdma_writes, rdma_hits, rdma_misses;
    rdma_get_storage_stats(&rdma_reads, &rdma_writes, &rdma_hits, &rdma_misses);
    
    if (reads) *reads = rdma_reads;
    if (writes) *writes = rdma_writes;
    if (hits) *hits = rdma_hits;
    if (misses) *misses = rdma_misses;
}

/**
 * Print storage statistics
 */
void print_storage_stats(void) {
    printf("\n=== Pure RDMA Storage Statistics ===\n");
    
    uint64_t reads, writes, hits, misses;
    rdma_get_storage_stats(&reads, &writes, &hits, &misses);
    
    printf("Total reads: %lu\n", reads);
    printf("Total writes: %lu\n", writes);
    printf("Cache hits: %lu (%.2f%%)\n", hits,
           (reads > 0) ? (double)hits / reads * 100.0 : 0.0);
    printf("Cache misses: %lu (%.2f%%)\n", misses,
           (reads > 0) ? (double)misses / reads * 100.0 : 0.0);
    
    printf("===================================\n\n");
    
    // 打印详细的RDMA统计
    rdma_print_detailed_stats();
}