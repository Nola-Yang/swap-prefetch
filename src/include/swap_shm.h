#ifndef SWAP_SHM_H
#define SWAP_SHM_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <semaphore.h>

#define SHM_NAME "/extmem_swap_shm"
#define SEM_REQUEST_NAME "/extmem_swap_request_sem"
#define SEM_RESPONSE_NAME "/extmem_swap_response_sem"
#define SHM_SIZE (64 * 1024 * 1024)  // 64MB shared memory region
#define MAX_QUEUE_SIZE 1024          // Maximum number of pending operations
#define PAGE_BUFFER_SIZE (4 * 1024 * 1024)  // 4MB for page data transfer

// Operation types
typedef enum {
    OP_WRITE_PAGE,   // Write page to swap
    OP_READ_PAGE,    // Read page from swap
    OP_INIT,         // Initialize swap
    OP_SHUTDOWN      // Shutdown swap process
} swap_op_type_t;

// Request status
typedef enum {
    STATUS_PENDING,
    STATUS_IN_PROGRESS,
    STATUS_COMPLETED,
    STATUS_ERROR
} swap_status_t;

// Request structure
typedef struct {
    uint64_t request_id;       // Unique request identifier
    swap_op_type_t op_type;    // Operation type
    uint64_t page_addr;        // Virtual address of the page
    uint64_t disk_offset;      // Offset in swap file for the page
    size_t page_size;          // Size of the page
    swap_status_t status;      // Status of request
    int error_code;            // Error code if any
    uint64_t buffer_offset;    // Offset in the shared buffer for page data
} swap_request_t;

// Shared memory structure
typedef struct {
    // Request queue
    swap_request_t requests[MAX_QUEUE_SIZE];
    uint32_t head;             // Head of the queue (next to be processed)
    uint32_t tail;             // Tail of the queue (next to be filled)
    uint32_t count;            // Number of requests in the queue
    
    // Synchronization
    pthread_mutex_t queue_mutex;  // Protects access to the queue
    
    // Statistics
    uint64_t total_requests;   // Total number of requests processed
    uint64_t read_requests;    // Number of read requests
    uint64_t write_requests;   // Number of write requests
    uint64_t bytes_read;       // Total bytes read
    uint64_t bytes_written;    // Total bytes written
    
    // Page data buffer
    char page_buffer[PAGE_BUFFER_SIZE];  // Buffer for page data transfer
} swap_shm_t;

// Initialize shared memory (called by both processes)
int swap_shm_init(bool is_server);

// Clean up shared memory (called by both processes)
int swap_shm_cleanup(bool is_server);

// Submit a request to the swap process (called by ExtMem)
int swap_submit_request(swap_op_type_t op_type, uint64_t page_addr, 
                      uint64_t disk_offset, size_t page_size, void* data);

// Wait for a request to complete (called by ExtMem)
int swap_wait_request(uint64_t request_id, int* error_code);

// Process the next request (called by the swap process)
int swap_process_next_request(void);

// Check if there are any pending requests (called by the swap process)
bool swap_has_pending_requests(void);

// Get shared memory pointer
swap_shm_t* get_swap_shm(void);

#endif /* SWAP_SHM_H */