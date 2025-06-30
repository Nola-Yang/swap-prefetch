#ifndef RDMA_SIM_H
#define RDMA_SIM_H

#include <stdint.h>
#include <stddef.h>    // For size_t
#include <stdbool.h>   // For bool
#include <pthread.h>   // For mutex, cond
#include <stdatomic.h> // For atomic types

// Forward declaration of core.h PAGE_SIZE if not directly included
// For simplicity, assume PAGE_SIZE is accessible. If not, core.h or a config header should be included.
// #include "../core.h" // Or a more generic path if rdma_sim is independent

#define RDMA_SHM_KEY 0x1234
#define MAX_RDMA_REQUESTS 128
#define RDMA_PAGE_BUFFER_SIZE 4096 // Should ideally be PAGE_SIZE from core.h

// Define RDMA operation types
typedef enum {
    RDMA_OP_READ,
    RDMA_OP_WRITE,
    RDMA_OP_INIT,
    RDMA_OP_SHUTDOWN,
    RDMA_OP_ALLOCATE,
    RDMA_OP_FREE
} rdma_op_type_e;

typedef enum {
    RDMA_REQ_PENDING,
    RDMA_REQ_PROCESSING,
    RDMA_REQ_COMPLETED,
    RDMA_REQ_FAILED
} rdma_req_status_e;

// Structure for an RDMA request in shared memory
typedef struct rdma_request {
    uint64_t request_id;
    rdma_op_type_e op_type;
    uint64_t local_addr;      // Client's local buffer address (for READs into, for WRITEs from if data_in_shm_buffer is false)
    uint64_t remote_addr;     // Simulated remote RDMA address
    size_t size;
    atomic_int status;        // rdma_req_status_e
    int error_code;
    bool data_in_shm_buffer; // True if data for WRITE is in rdma_shm.page_buffer
    size_t buffer_offset;     // Added for offset within shm page_buffer or other region
    uint64_t timestamp;       // Added: Timestamp of request submission or last update
    // For small data, a direct buffer could be here, but current rdma_sim.c uses page_buffer for writes
    // uint8_t embedded_data[SOME_SIZE]; 
    // size_t embedded_data_len;
} rdma_request_t;

// Structure for the shared memory segment
typedef struct rdma_shm {
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;

    rdma_request_t requests[MAX_RDMA_REQUESTS];
    atomic_int head; // Next slot for client to write request
    atomic_int tail; // Next slot for server to read request
    atomic_int count; // Number of pending requests

    atomic_bool initialized;
    atomic_bool shutdown_requested;

    // Shared page buffer for data transfers (e.g., client writes data here, server reads for RDMA write)
    // (Or server writes data here from RDMA read, client reads)
    unsigned char page_buffer[RDMA_PAGE_BUFFER_SIZE]; 

    // Remote memory simulation metadata
    uint64_t remote_memory_base;
    size_t remote_memory_size;
    atomic_uint_fast64_t remote_memory_used;

    // Added statistics fields
    atomic_ulong total_requests;
    atomic_ulong completed_requests;
    atomic_ulong failed_requests;
    atomic_size_t page_buffer_used; // For tracking usage of the page_buffer specifically

} rdma_shm_t;

/**
 * @brief Initializes the RDMA simulation environment.
 * For server: creates and initializes shared memory.
 * For client: attaches to existing shared memory.
 * @param is_server True if initializing as server, false for client.
 * @return 0 on success, -1 on failure.
 */
int rdma_sim_init(bool is_server);

/**
 * @brief Cleans up the RDMA simulation environment.
 * For server: detaches and removes shared memory.
 * For client: detaches shared memory.
 * @param is_server True if cleaning up as server, false for client.
 */
void rdma_sim_cleanup(bool is_server);

/**
 * @brief Retrieves a pointer to the RDMA shared memory structure.
 * @return Pointer to rdma_shm_t, or NULL if not initialized.
 */
rdma_shm_t* get_rdma_shm(void);

/**
 * @brief Allocates a simulated RDMA memory region on the 'remote' server.
 * @param size The size of the memory region to allocate.
 * @param remote_addr Pointer to a uint64_t to store the allocated remote address.
 * @return 0 on success, -1 on failure.
 */
int rdma_sim_allocate(size_t size, uint64_t* remote_addr);

/**
 * @brief Frees a simulated RDMA memory region on the 'remote' server.
 * @param remote_addr The remote address of the memory region to free.
 * @return 0 on success, -1 on failure.
 */
int rdma_sim_free(uint64_t remote_addr);

/**
 * @brief Submits an RDMA operation (read or write) to the simulated RDMA server.
 * @param op_type The type of RDMA operation.
 * @param local_addr For RDMA_OP_READ, this is client's dest buffer. For RDMA_OP_WRITE, client's source if data is NULL.
 * @param remote_addr The remote RDMA memory address.
 * @param length The length of the data to transfer.
 * @param data Pointer to data buffer for RDMA_OP_WRITE (copied by submitter into shm->page_buffer if op is WRITE).
 *             For RDMA_OP_READ, this is typically NULL.
 * @return A request ID (uint64_t) if successful, 0 or a negative value on failure.
 */
uint64_t rdma_submit_request(rdma_op_type_e op_type, uint64_t local_addr, uint64_t remote_addr, size_t length, void* data);

/**
 * @brief Waits for an RDMA operation to complete.
 * @param request_id The ID of the RDMA request to wait for.
 * @param error_code Pointer to an int to store an error code if the operation failed.
 * @return 0 if the operation completed successfully, non-zero otherwise.
 */
int rdma_wait_request(uint64_t request_id, int* error_code);

/**
 * @brief Gets a high-resolution timestamp.
 * @return A uint64_t timestamp value.
 */
uint64_t rdma_get_timestamp(void);

#endif // RDMA_SIM_H 