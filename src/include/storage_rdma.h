/**
 * @file storage_rdma.h
 * @brief RDMA-based storage manager for ExtMem
 * 
 * This provides an RDMA-based storage implementation for ExtMem,
 * where pages can be swapped to remote memory instead of disk.
 */
#ifndef STORAGE_RDMA_H
#define STORAGE_RDMA_H

#include <stdint.h>
#include <stddef.h>

/**
 * Initialize RDMA storage.
 * 
 * This function starts the RDMA simulation process and establishes
 * shared memory communication for RDMA operations.
 * 
 * @return 0 on success, -1 on failure
 */
int rdma_storage_init(void);

/**
 * Shutdown RDMA storage.
 * 
 * This function stops the RDMA simulation process and cleans up resources.
 */
void rdma_storage_shutdown(void);

/**
 * Read a page from RDMA storage.
 * 
 * This function reads a page from RDMA storage, either from remote memory
 * if it's cached there, or from disk if it's not cached.
 * 
 * @param fd File descriptor for disk (used as fallback)
 * @param disk_offset Offset in the disk file
 * @param dest Destination buffer
 * @param size Size of the page
 * @return 0 on success, -1 on failure
 */
int rdma_read_page(int fd, uint64_t disk_offset, void* dest, size_t size);

/**
 * Write a page to RDMA storage.
 * 
 * This function writes a page to both disk (for durability) and
 * to remote memory if it's cached there.
 * 
 * @param fd File descriptor for disk
 * @param disk_offset Offset in the disk file
 * @param src Source buffer
 * @param size Size of the page
 * @return 0 on success, -1 on failure
 */
int rdma_write_page(int fd, uint64_t disk_offset, void* src, size_t size);

/**
 * Get RDMA storage statistics.
 * 
 * @param reads Pointer to variable to store number of read operations
 * @param writes Pointer to variable to store number of write operations
 * @param hits Pointer to variable to store number of cache hits
 * @param misses Pointer to variable to store number of cache misses
 */
void rdma_get_storage_stats(uint64_t *reads, uint64_t *writes, uint64_t *hits, uint64_t *misses);

#endif /* STORAGE_RDMA_H */