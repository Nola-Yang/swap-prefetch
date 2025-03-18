#ifndef STORAGE_SWAP_PROCESS_H
#define STORAGE_SWAP_PROCESS_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/**
 * Initialize the swap process system.
 * 
 * This function starts the swap process and establishes shared memory
 * communication between ExtMem and the swap process.
 * 
 * @return 0 on success, -1 on failure
 */
int storage_init(void);

/**
 * Shutdown the swap process system.
 * 
 * This function stops the swap process and cleans up shared memory.
 * It should be called during program termination.
 */
void storage_shutdown(void);

/**
 * Read a page from the swap file through the swap process.
 * 
 * @param fd Storage file descriptor (ignored, maintained for API compatibility)
 * @param offset Offset in the swap file to read from
 * @param dest Destination buffer where the read data will be stored
 * @param size Size of the data to read (typically PAGE_SIZE)
 * @return 0 on success, -1 on failure
 */
int swap_process_read_page(int fd, uint64_t offset, void* dest, size_t size);

/**
 * Write a page to the swap file through the swap process.
 * 
 * @param fd Storage file descriptor (ignored, maintained for API compatibility)
 * @param offset Offset in the swap file to write to
 * @param src Source buffer containing the data to write
 * @param size Size of the data to write (typically PAGE_SIZE)
 * @return 0 on success, -1 on failure
 */
int swap_process_write_page(int fd, uint64_t offset, void* src, size_t size);

/**
 * Create a checkpoint of the current swap file state.
 * 
 * This function creates a checkpoint by instructing the swap process
 * to make a copy of the swap file at the current state.
 * 
 * @param checkpoint_name Name of the checkpoint (used in filename)
 * @return 0 on success, -1 on failure
 */
int swap_process_checkpoint(const char* checkpoint_name);

/**
 * Get the time taken for the last checkpoint operation in seconds.
 * 
 * @return Time in seconds, or -1 if no checkpoint has been created
 */
double swap_process_get_last_checkpoint_time(void);

#endif /* STORAGE_SWAP_PROCESS_H */