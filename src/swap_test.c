#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define MB (1024 * 1024)
#define PAGE_SIZE 4096

int main(int argc, char *argv[]) {
    // Parse args
    size_t alloc_size = 512 * MB; // Default to 512MB
    if (argc > 1) {
        alloc_size = atol(argv[1]) * MB;
    }
    
    printf("Simple ExtMem Test Program\n");
    printf("Allocating %zu bytes (%.2f MB)\n", alloc_size, (float)alloc_size / MB);
    
    // Allocate memory with mmap
    void *memory = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, 
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                         
    if (memory == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }
    
    printf("Memory allocated at %p\n", memory);
    
    // Touch all pages to ensure allocation
    printf("Writing to each page...\n");
    unsigned char *ptr = (unsigned char *)memory;
    size_t num_pages = alloc_size / PAGE_SIZE;
    
    for (size_t i = 0; i < num_pages; i++) {
        ptr[i * PAGE_SIZE] = (unsigned char)i;
        
        // Print progress every 10%
        if (i % (num_pages / 10) == 0) {
            printf("Progress: %.0f%%\n", (float)i * 100 / num_pages);
        }
    }
    
    printf("Memory initialized successfully.\n");
    
    // Perform some random accesses
    printf("Performing random memory accesses...\n");
    srand(42); // Fixed seed for reproducibility
    
    for (int i = 0; i < 1000; i++) {
        size_t page = rand() % num_pages;
        size_t offset = page * PAGE_SIZE;
        
        // Read
        unsigned char value = ptr[offset];
        
        // Write
        ptr[offset] = value + 1;
    }
    
    printf("Random access test completed.\n");
    
    // Free memory
    printf("Freeing memory...\n");
    if (munmap(memory, alloc_size) != 0) {
        perror("munmap failed");
        return 1;
    }
    
    printf("Test completed successfully!\n");
    return 0;
}