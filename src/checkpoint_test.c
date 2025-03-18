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
#include <sys/time.h>
#include <time.h>
#include <libgen.h>
#include "swap_shm.h"

// Global variables - defined in swap_process.c
extern int swap_fd;
extern char swap_file_path[1024];
extern volatile sig_atomic_t running;

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

// Create a checkpoint of the swap file
static int create_checkpoint(const char* checkpoint_name) {
    struct timeval start, end;
    double elapsed_time;
    char checkpoint_path[1024];
    char checkpoint_dir[1024];
    struct stat st;
    int ret;
    
    // Start timing
    gettimeofday(&start, NULL);
    
    if (swap_fd == -1) {
        fprintf(stderr, "Swap file not initialized\n");
        return -1;
    }
    
    // Make sure all data is written to disk
    fsync(swap_fd);
    
    // Create checkpoint directory if it doesn't exist
    char temp_path[1024];
    strncpy(temp_path, swap_file_path, sizeof(temp_path) - 1);
    char *swap_file_dir = dirname(temp_path);
    snprintf(checkpoint_dir, sizeof(checkpoint_dir), "%s/checkpoints", swap_file_dir);
    
    if (stat(checkpoint_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        printf("Creating checkpoint directory: %s\n", checkpoint_dir);
        char cmd[1100];
        snprintf(cmd, sizeof(cmd), "mkdir -p %s", checkpoint_dir);
        system(cmd);
    }
    
    // Generate checkpoint filename
    if (checkpoint_name && checkpoint_name[0]) {
        snprintf(checkpoint_path, sizeof(checkpoint_path), 
                 "%s/%s.bin", checkpoint_dir, checkpoint_name);
    } else {
        // Generate timestamp-based name if no name provided
        time_t t = time(NULL);
        struct tm* tm_info = localtime(&t);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);
        
        snprintf(checkpoint_path, sizeof(checkpoint_path), 
                 "%s/checkpoint_%s.bin", checkpoint_dir, timestamp);
    }
    
    // Create a copy of the swap file
    FILE *source = fdopen(dup(swap_fd), "rb");
    if (!source) {
        perror("fdopen source");
        return -1;
    }
    
    FILE *dest = fopen(checkpoint_path, "wb");
    if (!dest) {
        perror("fopen destination");
        fclose(source);
        return -1;
    }
    
    // Copy file contents using a buffer
    char buffer[64 * 1024]; // 64KB buffer
    size_t bytes;
    
    // Reset to beginning of file
    rewind(source);
    
    while ((bytes = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        if (fwrite(buffer, 1, bytes, dest) != bytes) {
            perror("fwrite");
            fclose(source);
            fclose(dest);
            return -1;
        }
    }
    
    // Clean up
    fclose(source);
    fclose(dest);
    
    // End timing
    gettimeofday(&end, NULL);
    elapsed_time = (end.tv_sec - start.tv_sec) + 
                   (end.tv_usec - start.tv_usec) / 1000000.0;
    
    printf("Checkpoint created at %s in %.2f seconds\n", checkpoint_path, elapsed_time);
    
    // Store checkpoint time in shared memory
    swap_shm_t* shm = get_swap_shm();
    if (shm != NULL) {
        shm->last_checkpoint_time = elapsed_time;
    }
    
    return 0;
}

// Process a request - implementation exported for use by swap_shm.c
int process_request(swap_request_t* req) {
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
            
        case OP_CHECKPOINT:
            // Create a checkpoint of the swap file
            ret = create_checkpoint(req->checkpoint_name);
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
