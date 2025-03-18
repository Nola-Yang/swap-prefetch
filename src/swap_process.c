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

// Configuration
#define DEFAULT_SWAP_FILE_PATH "/tmp/extmem_swap.bin"
#define SWAP_FILE_SIZE (4UL * 1024UL * 1024UL * 1024UL)  // 4GB swap file

int swap_fd = -1;
volatile sig_atomic_t running = 1;
char swap_file_path[1024] = {0};

// Signal handler
static void sig_handler(int signum) {
    running = 0;
}

// Get the swap file path from environment or use default
static void init_swap_file_path() {
    const char* env_path = getenv("SWAPDIR");
    
    if (env_path != NULL) {
        // Use the environment variable
        strncpy(swap_file_path, env_path, sizeof(swap_file_path) - 1);
        
        // If the path is a device file or an existing directory, append a filename
        struct stat st;
        if (stat(swap_file_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                // Append a file name if it's a directory
                size_t len = strlen(swap_file_path);
                if (swap_file_path[len-1] != '/') {
                    strncat(swap_file_path, "/", sizeof(swap_file_path) - len - 1);
                    len++;
                }
                strncat(swap_file_path, "extmem_swap.bin", sizeof(swap_file_path) - len - 1);
            }
            // If it's a device file, use as is
        }
    } else {
        // Use default path
        strncpy(swap_file_path, DEFAULT_SWAP_FILE_PATH, sizeof(swap_file_path) - 1);
    }
    
    printf("Using swap file: %s\n", swap_file_path);
}

// Initialize the swap file
static int init_swap_file() {
    struct stat st;
    
    // Initialize the swap file path
    init_swap_file_path();
    
    // Check if the swap file already exists
    if (stat(swap_file_path, &st) == 0) {
        // File exists, open it
        swap_fd = open(swap_file_path, O_RDWR);
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
        // First ensure the directory exists
        char dir_path[1024] = {0};
        char *last_slash = strrchr(swap_file_path, '/');
        
        if (last_slash != NULL) {
            // Copy the directory part
            size_t dir_len = last_slash - swap_file_path;
            strncpy(dir_path, swap_file_path, dir_len);
            dir_path[dir_len] = '\0';
            
            // Create directory if it doesn't exist
            struct stat dir_st;
            if (stat(dir_path, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode)) {
                printf("Creating directory: %s\n", dir_path);
                char cmd[1100];
                snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir_path);
                system(cmd);
            }
        }
        
        // Now create the file
        swap_fd = open(swap_file_path, O_RDWR | O_CREAT, 0666);
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

#ifdef BUILD_EXECUTABLE
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
#endif