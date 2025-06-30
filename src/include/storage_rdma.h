#ifndef STORAGE_RDMA_H
#define STORAGE_RDMA_H

#include <stdint.h>
#include <stddef.h> // For size_t


/**
 * @brief Initializes the RDMA storage system.
 * This typically involves starting the rdma_process and initializing communication.
 * @return 0 on success, non-zero on failure.
 */
int rdma_storage_init(void);

/**
 * @brief Shuts down the RDMA storage system.
 * This involves stopping the rdma_process and cleaning up resources.
 */
void rdma_storage_shutdown(void);

/**
 * @brief Allocates a new page/block in the RDMA-backed storage and returns its virtual offset.
 * The virtual offset is used as a key for subsequent read/write operations.
 * @param size The size of the page/block to allocate (e.g., PAGE_SIZE).
 * @return A unique virtual offset (uint64_t) on success, or a special value (e.g., 0 or -1 cast to uint64_t) on failure.
 */
uint64_t rdma_allocate_page_offset(size_t size);

/**
 * @brief Reads a page from the RDMA storage into a local buffer.
 * @param fd File descriptor (likely unused in a pure RDMA context, but kept for API consistency if adapted from disk I/O).
 * @param virtual_offset The virtual offset of the page to read (obtained from rdma_allocate_page_offset).
 * @param dest Pointer to the local buffer where the page data will be copied.
 * @param size The size of the page to read (e.g., PAGE_SIZE).
 * @return 0 on success, non-zero on failure.
 */
int rdma_read_page(int fd, uint64_t virtual_offset, void* dest, size_t size);

/**
 * @brief Writes a page from a local buffer to the RDMA storage.
 * @param fd File descriptor (likely unused).
 * @param virtual_offset The virtual offset of the page to write to (obtained from rdma_allocate_page_offset).
 * @param src Pointer to the local buffer containing the page data to be written.
 * @param size The size of the page to write (e.g., PAGE_SIZE).
 * @return 0 on success, non-zero on failure.
 */
int rdma_write_page(int fd, uint64_t virtual_offset, void* src, size_t size);

/**
 * @brief Retrieves RDMA storage statistics.
 * @param reads Pointer to store the total number of RDMA read operations.
 * @param writes Pointer to store the total number of RDMA write operations.
 * @param hits Pointer to store the total number of cache hits (if applicable to the storage layer).
 * @param misses Pointer to store the total number of cache misses (if applicable).
 */
void rdma_get_storage_stats(uint64_t *reads, uint64_t *writes, uint64_t *hits, uint64_t *misses);

/**
 * @brief Prints detailed RDMA storage statistics to the console/log.
 */
void rdma_print_detailed_stats(void);


#endif // STORAGE_RDMA_H 