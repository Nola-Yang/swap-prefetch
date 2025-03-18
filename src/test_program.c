#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

#define PAGE_SIZE 4096
#define GB (1024UL * 1024UL * 1024UL)
#define ALLOC_SIZE (4 * GB)  // Allocate 4GB of memory

// Function to measure time
double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char *argv[]) {
    printf("Starting ExtMem test program...\n");
    
    // Allocate a large chunk of memory
    printf("Allocating %lu bytes of memory...\n", ALLOC_SIZE);
    double start_time = get_time();
    
    void *mem = mmap(NULL, ALLOC_SIZE, PROT_READ | PROT_WRITE, 
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }
    
    double alloc_time = get_time() - start_time;
    printf("Memory allocation completed in %.2f seconds\n", alloc_time);
    
    // Write to memory
    printf("Writing to memory...\n");
    start_time = get_time();
    
    unsigned char *ptr = (unsigned char *)mem;
    for (size_t i = 0; i < ALLOC_SIZE; i += PAGE_SIZE) {
        ptr[i] = (unsigned char)(i & 0xFF);
    }
    
    double write_time = get_time() - start_time;
    printf("Memory write completed in %.2f seconds\n", write_time);
    
    // Random access pattern
    printf("Performing random accesses...\n");
    start_time = get_time();
    
    srand(time(NULL));
    const int num_accesses = 10000;
    for (int i = 0; i < num_accesses; i++) {
        size_t offset = ((size_t)rand() * PAGE_SIZE) % ALLOC_SIZE;
        volatile unsigned char value = ptr[offset];
        ptr[offset] = value + 1;
    }
    
    double random_access_time = get_time() - start_time;
    printf("Random access completed in %.2f seconds (%.0f accesses/sec)\n", 
           random_access_time, num_accesses / random_access_time);
    
    // Sequential scan
    printf("Performing sequential scan...\n");
    start_time = get_time();
    
    size_t sum = 0;
    for (size_t i = 0; i < ALLOC_SIZE; i += PAGE_SIZE) {
        sum += ptr[i];
    }
    
    double scan_time = get_time() - start_time;
    printf("Sequential scan completed in %.2f seconds\n", scan_time);
    printf("Checksum: %lu\n", sum);
    
    // Free memory
    printf("Freeing memory...\n");
    if (munmap(mem, ALLOC_SIZE) == -1) {
        perror("munmap failed");
        return 1;
    }
    
    printf("Test completed successfully!\n");
    return 0;
}