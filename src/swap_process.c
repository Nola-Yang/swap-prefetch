#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "swap_shm.h"

// Configuration
#define SWAP_FILE_PATH "/tmp/extmem_swap.bin"
#define SWAP_FILE_SIZE (4UL * 1024UL * 1024UL * 1024UL)  // 4GB swap file

// Global variables
static int swap_fd = -1;
static volatile sig_atomic_t running = 1;

// Signal handler
static void sig_handler(int signum) {
    running = 0;
}

// Initialize the swap file
static int init_swap_file() {
    struct stat st;
    
    // Check if the swap file already exists
    if (stat(SWAP_FILE_PATH, &st) == 0) {
        // File exists, open it
        swap_fd = open(SWAP_FILE_PATH, O_RDWR | O_DIRECT);
        if (swap_fd == -1) {
            perror("open swap file");
            return -1;
        }
        
        // Check size
        if (st.st_size < SWAP_FILE_SIZE) {
            // Extend file
            if (ftruncate(swap_fd, SWAP_FILE_SIZE) == -1) {
                perror("ftruncate swap file");
                close(swap_fd);
                swap_fd = -1;
                return -1;
            }
        }
    } else {
        // File doesn't exist, create it
        swap_fd = open(SWAP_FILE_PATH, O_RDWR | O_CREAT | O_DIRECT, 0666);
        if (swap_fd == -1) {
            perror("create swap file");
            return -1;
        }
        
        // Set file size
        if (ftruncate(swap_fd, SWAP_FILE_SIZE) == -1) {
            perror("ftruncate swap file");
            close(swap_fd);
            swap_fd = -1;
            return -1;
        }
        
        // Optionally, you could pre-initialize the file here
        // For example, fill it with zeros
    }
    
    return 0;
}

// Cleanup swap file
static void cleanup_swap_file() {
    if (swap_fd != -1) {
        close(swap_fd);
        swap_fd = -1;
    }
}

// Read a page from the swap file
static int read_swap_page(uint64_t disk_offset, void* buffer, size_t size) {
    if (swap_fd == -1) {
        fprintf(stderr, "Swap file not initialized\n");
        return -1;
    }
    
    // Seek to the offset
    if (lseek(swap_fd, disk_offset, SEEK_SET) == -1) {
        perror("lseek");
        return -1;
    }
    
    // Read the page
    ssize_t bytes_read = read(swap_fd, buffer, size);
    if (bytes_read == -1) {
        perror("read");
        return -1;
    }
    
    if ((size_t)bytes_read != size) {
        fprintf(stderr, "Partial read: %zd of %zu bytes\n", bytes_read, size);
        return -1;
    }
    
    return 0;
}

// Write a page to the swap file
static int write_swap_page(uint64_t disk_offset, const void* buffer, size_t size) {
    if (swap_fd == -1) {
        fprintf(stderr, "Swap file not initialized\n");
        return -1;
    }
    
    // Seek to the offset
    if (lseek(swap_fd, disk_offset, SEEK_SET) == -1) {
        perror("lseek");
        return -1;
    }
    
    // Write the page
    ssize_t bytes_written = write(swap_fd, buffer, size);
    if (bytes_written == -1) {
        perror("write");
        return -1;
    }
    
    if ((size_t)bytes_written != size) {
        fprintf(stderr, "Partial write: %zd of %zu bytes\n", bytes_written, size);
        return -1;
    }
    
    // Ensure data is written to disk
    fsync(swap_fd);
    
    return 0;
}

// Process a request
static int process_request(swap_request_t* req) {
    int ret = 0;
    swap_shm_t* shm = get_swap_shm();
    
    switch (req->op_type) {
        case OP_READ_PAGE:
            // Read page from swap file
            ret = read_swap_page(req->disk_offset, 
                               shm->page_buffer + req->buffer_offset, 
                               req->page_size);
            if (ret == 0) {
                shm->bytes_read += req->page_size;
            }
            break;
            
        case OP_WRITE_PAGE:
            // Write page to swap file
            ret = write_swap_page(req->disk_offset, 
                                shm->page_buffer + req->buffer_offset, 
                                req->page_size);
            if (ret == 0) {
                shm->bytes_written += req->page_size;
            }
            break;
            
        case OP_INIT:
            // Nothing to do, already initialized
            break;
            
        case OP_SHUTDOWN:
            running = 0;
            break;
            
        default:
            fprintf(stderr, "Unknown operation type: %d\n", req->op_type);
            ret = -1;
            break;
    }
    
    return ret;
}

// Main function
int main(int argc, char* argv[]) {
    int ret;
    
    // Set up signal handlers
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    
    printf("Swap process starting...\n");
    
    // Initialize shared memory
    ret = swap_shm_init(true);  // true = server (swap process)
    if (ret != 0) {
        fprintf(stderr, "Failed to initialize shared memory\n");
        return 1;
    }
    
    // Initialize swap file
    ret = init_swap_file();
    if (ret != 0) {
        fprintf(stderr, "Failed to initialize swap file\n");
        swap_shm_cleanup(true);
        return 1;
    }
    
    printf("Swap process initialized. Waiting for requests...\n");
    
    // Main loop
    while (running) {
        // Process requests
        ret = swap_process_next_request();
        if (ret != 0) {
            fprintf(stderr, "Error processing request\n");
            break;
        }
    }
    
    printf("Swap process shutting down...\n");
    
    // Clean up
    cleanup_swap_file();
    swap_shm_cleanup(true);
    
    printf("Swap process terminated.\n");
    
    return 0;
}