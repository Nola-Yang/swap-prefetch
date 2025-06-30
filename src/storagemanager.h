/** Simple storage manager for reading and writing pages to disk
 * Later can replace this with more sophisticated implementations
 or nvme over SPDK for high performance
*/
#ifndef STORAGEMANAGER_H
#define STORAGEMANAGER_H

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stddef.h> // For size_t

// Enum for storage types (example)
typedef enum {
    STORAGE_DISK,    // Traditional disk
    STORAGE_RDMA,    // RDMA-based storage
    // Add other types as needed
} storage_type_e;

/*typedef struct {
    int fd;
    int num_pages;
    int filesize;
} StorageManager;*/

// Function to initialize the storage system
int storage_init(void);

// Function to shut down the storage system
void storage_shutdown(void);

// Function to write a page to storage
int write_page(int fd, uint64_t offset, const void* src, size_t size);

// Function to read a page from storage
int read_page(int fd, uint64_t offset, void* dest, size_t size);

// Function to get storage statistics
void get_storage_stats(uint64_t *reads, uint64_t *writes, uint64_t *hits, uint64_t *misses);

// Function to print storage statistics
void print_storage_stats(void);

// Function to allocate a page offset (if applicable to the general API)
// uint64_t allocate_page_offset(size_t size);

#endif