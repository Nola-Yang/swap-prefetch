/**
 * @file rdma_sim.h
 * @brief Simulated RDMA interface for ExtMem
 * 
 * This provides a simulated RDMA environment for ExtMem where
 * "remote memory" is simulated within the same machine.
 */
#ifndef RDMA_SIM_H
#define RDMA_SIM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include <semaphore.h>

// Configuration
#define RDMA_SHM_NAME "/extmem_rdma_shm"
#define RDMA_SEM_REQUEST_NAME "/extmem_rdma_request_sem"
#define RDMA_SEM_RESPONSE_NAME "/extmem_rdma_response_sem"
#define RDMA_SHM_SIZE (64 * 1024 * 1024)  // 64MB shared memory region for control
#define RDMA_MAX_QUEUE_SIZE 1024          // Maximum number of pending operations
#define RDMA_PAGE_BUFFER_SIZE (4 * 1024 * 1024)  // 4MB for page data transfer
#define RDMA_REMOTE_MEM_SIZE (16UL * 1024UL * 1024UL * 1024UL)  // 16GB of simulated remote memory

// Operation types
typedef enum {
    RDMA_OP_WRITE,      // Write page to remote memory
    RDMA_OP_READ,       // Read page from remote memory
    RDMA_OP_INIT,       // Initialize RDMA
    RDMA_OP_SHUTDOWN,   // Shutdown RDMA process
    RDMA_OP_ALLOCATE,   // Allocate remote memory region
    RDMA_OP_FREE        // Free remote memory region
} rdma_op_type_t;

// Request status
typedef enum {
    RDMA_STATUS_PENDING,
    RDMA_STATUS_IN_PROGRESS,
    RDMA_STATUS_COMPLETED,
    RDMA_STATUS_ERROR
} rdma_status_t;

// Request structure
typedef struct {
    uint64_t request_id;       // Unique request identifier
    rdma_op_type_t op_type;    // Operation type
    uint64_t local_addr;       // Local virtual address of the page
    uint64_t remote_addr;      // Remote memory address for the page
    size_t length;             // Size of the transfer
    rdma_status_t status;      // Status of request
    int error_code;            // Error code if any
    uint64_t buffer_offset;    // Offset in the shared buffer for page data
    
    // Additional fields for RDMA simulation
    uint64_t timestamp;        // For timing statistics
    double simulated_latency;  // Simulated network latency in microseconds
} rdma_request_t;

// Remote memory region info
typedef struct {
    uint64_t remote_addr;   // Base address in remote memory
    size_t size;            // Size of region
    bool in_use;            // Whether region is currently allocated
    uint64_t last_access;   // Timestamp of last access
    uint32_t access_count;  // Number of accesses
} rdma_region_t;

// Shared memory structure for RDMA control
typedef struct {
    // Request queue
    rdma_request_t requests[RDMA_MAX_QUEUE_SIZE];
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
    double avg_latency;        // Average operation latency
    
    // Remote memory allocation info
    uint64_t total_remote_mem;       // Total simulated remote memory
    uint64_t available_remote_mem;   // Available simulated remote memory
    
    // Page data buffer for transfers
    char page_buffer[RDMA_PAGE_BUFFER_SIZE];  // Buffer for page data transfer
} rdma_shm_t;

// Initialize RDMA simulation (called by both processes)
int rdma_sim_init(bool is_server);

// Clean up RDMA simulation (called by both processes)
int rdma_sim_cleanup(bool is_server);

// Allocate a region in simulated remote memory
int rdma_sim_allocate(size_t size, uint64_t *remote_addr);

// Free a region in simulated remote memory
int rdma_sim_free(uint64_t remote_addr);

// Perform RDMA read (get data from remote memory)
int rdma_sim_read(uint64_t local_addr, uint64_t remote_addr, size_t length, void* buffer);

// Perform RDMA write (put data to remote memory)
int rdma_sim_write(uint64_t local_addr, uint64_t remote_addr, size_t length, const void* buffer);

// Submit a request to the RDMA process
int rdma_submit_request(rdma_op_type_t op_type, uint64_t local_addr, 
                      uint64_t remote_addr, size_t length, void* data);

// Wait for a request to complete
int rdma_wait_request(uint64_t request_id, int* error_code);

// Process the next request (called by the RDMA process)
int rdma_process_next_request(void);

// Check if there are any pending requests (called by the RDMA process)
bool rdma_has_pending_requests(void);

// Get RDMA shared memory pointer
rdma_shm_t* get_rdma_shm(void);

// Get statistics about RDMA operations
int rdma_get_stats(double *avg_latency, uint64_t *bytes_read, uint64_t *bytes_written);

#endif /* RDMA_SIM_H */