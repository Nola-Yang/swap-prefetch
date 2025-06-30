#ifndef POINTER_PREFETCH_H
#define POINTER_PREFETCH_H

#include <stdint.h>
#include <stdbool.h>
#include "../extmem_test_env/ExtMem/src/core.h" // For struct user_page, PAGE_SIZE
#include "rdma_sim.h"         // For rdma_get_timestamp

// Default prefetch parameters (can be overridden by setters)
#define DEFAULT_MAX_PREFETCH_DISTANCE (16 * 1024 * 1024) // 16MB
#define DEFAULT_PREFETCH_VALIDATION_THRESHOLD (0x100000) // 1MB
#define DEFAULT_POINTER_OFFSET_FROM_END (sizeof(uint64_t)) // Read the last 8 bytes as a pointer

// Make the constants from .c file available via the header, using the defaults
#define MAX_PREFETCH_DISTANCE DEFAULT_MAX_PREFETCH_DISTANCE
#define PREFETCH_VALIDATION_THRESHOLD DEFAULT_PREFETCH_VALIDATION_THRESHOLD
#define POINTER_OFFSET_FROM_END DEFAULT_POINTER_OFFSET_FROM_END


// Enum for prefetch status
typedef enum {
    PREFETCH_NONE,
    PREFETCH_REQUESTED,
    PREFETCH_IN_PROGRESS,
    PREFETCH_COMPLETED,
    PREFETCH_FAILED
} prefetch_status_e;

// Structure to keep track of prefetch records
typedef struct prefetch_record {
    uint64_t source_va;          // VA of the page that triggered this prefetch
    uint64_t target_va;          // VA of the page being prefetched
    uint64_t pointer_value;      // The actual pointer value that led to this target_va
    uint64_t remote_addr;        // Remote RDMA buffer address for the prefetched page
    uint64_t rdma_request_id;    // ID of the RDMA operation
    prefetch_status_e status;    // Current status of this prefetch operation
    uint64_t timestamp;          // Timestamp of when this record was created/updated
    struct prefetch_record *next; // Pointer to the next record in the list
} prefetch_record_t;

// Structure for prefetch statistics
typedef struct {
    uint64_t total_attempts;
    uint64_t successful_prefetch; // Initiated (IN_PROGRESS)
    uint64_t failed_prefetch;     // Could not be initiated or failed during transfer
    uint64_t cache_hits;          // Fault handled by prefetcher
    uint64_t invalid_pointers;
    uint64_t pointer_validations;
} prefetch_stats_t;

// Function declarations
int pointer_prefetch_init(void);
void pointer_prefetch_cleanup(void);
uint64_t read_page_end_pointer(struct user_page *page);
bool validate_pointer(uint64_t pointer_value, uint64_t current_va);
int async_prefetch_page(uint64_t target_va, uint64_t source_va);
int handle_page_fault_with_prefetch(struct user_page *page);
prefetch_record_t* find_prefetch_record(uint64_t target_va);
int add_prefetch_record(uint64_t source_va, uint64_t target_va, uint64_t pointer_value);
bool is_prefetched_page_available(uint64_t va);
void cleanup_old_prefetch_records(void);
void print_prefetch_stats(void);
void reset_prefetch_stats(void);
prefetch_stats_t* get_prefetch_stats(void);
void set_prefetch_distance(uint64_t distance);
void set_pointer_validation_threshold(uint64_t threshold);

#endif // POINTER_PREFETCH_H 