#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <assert.h>
#include <time.h>

#define PAGE_SIZE 4096
#define NUM_PAGES 30
#define TOTAL_SIZE (NUM_PAGES * PAGE_SIZE)


typedef struct test_node {
    char data[PAGE_SIZE - 8];  
    uint64_t next_ptr;         
} test_node_t;

void setup_pointer_chain(void* base_addr, int num_pages) {
    printf("Setting up pointer chain with %d pages starting at %p\n", 
           num_pages, base_addr);
    
    for (int i = 0; i < num_pages - 1; i++) {
        test_node_t* current = (test_node_t*)((char*)base_addr + i * PAGE_SIZE);
        test_node_t* next = (test_node_t*)((char*)base_addr + (i + 1) * PAGE_SIZE);
        
        memset(current->data, 'A' + (i % 26), sizeof(current->data));
        
        current->next_ptr = (uint64_t)next;
        
        volatile char dummy = current->data[100];
        (void)dummy;
        
        if (i < 3 || i >= num_pages - 3) {
            printf("Page %d: addr=%p, next_ptr=0x%lx, points_to=%p\n", 
                   i, current, current->next_ptr, next);
        } else if (i == 3) {
            printf("... (pages 3-%d) ...\n", num_pages - 4);
        }
    }
    
    test_node_t* last = (test_node_t*)((char*)base_addr + (num_pages - 1) * PAGE_SIZE);
    memset(last->data, 'Z', sizeof(last->data));
    last->next_ptr = 0;
    
    volatile char dummy = last->data[100];
    (void)dummy;
    
    printf("Last page: addr=%p, next_ptr=0x%lx (NULL)\n", last, last->next_ptr);
}

void create_memory_pressure() {
    printf("\nCreating memory pressure to force swapping...\n");
    

    const size_t pressure_size = 512 * 1024; // 512KB per allocation  
    const int num_allocations = 20; // 10MB total - reasonable for 5MB DRAM
    void* pressure_memory[num_allocations];
    
    for (int i = 0; i < num_allocations; i++) {
        pressure_memory[i] = malloc(pressure_size);
        if (pressure_memory[i]) {
            
            memset(pressure_memory[i], 0xAA + (i % 10), pressure_size);
            if (i % 20 == 0) {
                printf("Allocated pressure block %d/%d (%zu MB total)\n", 
                       i+1, num_allocations, ((i+1) * pressure_size) / (1024*1024));
            }
        }
        
        usleep(2000); // 2ms
    }
    
    printf("Allocated %d blocks of %zu MB each (total: %zu MB)\n", 
           num_allocations, pressure_size / (1024*1024), 
           (num_allocations * pressure_size) / (1024*1024));
    
    // 等待让系统处理swap操作
    printf("Waiting 10 seconds for swap operations to complete...\n");
    sleep(10);
    
    // 释放部分内存，但保留一些压力
    for (int i = 0; i < num_allocations * 2 / 3; i++) {
        if (pressure_memory[i]) {
            free(pressure_memory[i]);
            pressure_memory[i] = NULL;
        }
    }
    
    printf("Released 2/3 of pressure memory, keeping some pressure active\n");
    sleep(3);
    
    // 最终清理剩余内存
    for (int i = num_allocations * 2 / 3; i < num_allocations; i++) {
        if (pressure_memory[i]) {
            free(pressure_memory[i]);
        }
    }
    
    printf("Released all pressure memory\n");
}

void traverse_pointer_chain(void* base_addr, const char* phase_name) {
    printf("\n=== %s ===\n", phase_name);
    
    test_node_t* current = (test_node_t*)base_addr;
    int page_count = 0;
    struct timespec start_time, end_time;
    int slow_accesses = 0;
    double total_access_time = 0;
    
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    while (current != NULL && page_count < NUM_PAGES) {
        struct timespec page_start, page_end;
        clock_gettime(CLOCK_MONOTONIC, &page_start);
        
        // 访问页面数据的多个位置以触发页面错误
        volatile char dummy1 = current->data[100];   // 页面前部
        volatile char dummy2 = current->data[2000];  // 页面中部 
        volatile char dummy3 = current->data[4000];  // 页面后部
        (void)dummy1; (void)dummy2; (void)dummy3;    // 避免编译器警告
        
        clock_gettime(CLOCK_MONOTONIC, &page_end);
        double page_time = (page_end.tv_sec - page_start.tv_sec) + 
                          (page_end.tv_nsec - page_start.tv_nsec) / 1e9;
        total_access_time += page_time;
        
        if (page_time > 0.001) { // > 1ms被认为是慢访问
            slow_accesses++;
        }
        
        if (page_count < 3 || page_count >= NUM_PAGES - 3 || page_time > 0.001) {
            printf("Page %d at %p: %.3f ms, data='%c', next=0x%lx%s\n", 
                   page_count, current, page_time * 1000, current->data[0], current->next_ptr,
                   page_time > 0.001 ? " (SLOW)" : "");
        } else if (page_count == 3) {
            printf("... (intermediate pages) ...\n");
        }
        
        // 较短的延迟以观察预取效果
        usleep(20000);  // 20ms
        
        // 移动到下一页
        if (current->next_ptr == 0) {
            printf("Reached end of chain at page %d\n", page_count);
            break;
        }
        
        current = (test_node_t*)current->next_ptr;
        page_count++;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (end_time.tv_sec - start_time.tv_sec) + 
                    (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
    
    printf("Traversal results:\n");
    printf("  Total time: %.3f seconds\n", elapsed);
    printf("  Average time per page: %.3f ms\n", (total_access_time * 1000.0) / (page_count + 1));
    printf("  Slow accesses (>1ms): %d/%d (%.1f%%)\n", 
           slow_accesses, page_count + 1, (slow_accesses * 100.0) / (page_count + 1));
    
    if (slow_accesses > 0) {
        printf("  Note: Slow accesses likely indicate page faults from swapped pages\n");
    }
}

int main(void)
{
    /* 1. 分配 2 MiB（>= 2 MiB，必定被 mmap_filter 重定向到 extmem_mmap） */
    printf("Allocating 2 MiB using mmap …\n");
    void *memory = mmap(NULL, 2 * 1024 * 1024,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    printf("Allocated at %p\n", memory);

    /* 2. 逐页写入数据，同时在页尾存放“下一页”指针               *
     *    注意：页尾地址 = page_base + PAGE_SIZE - sizeof(uint64_t) */
    printf("Writing page data & chaining pointers …\n");
    for (int i = 0; i < NUM_PAGES; ++i) {
        char *page        = (char *)memory + (size_t)i * PAGE_SIZE;
        uint64_t *tailptr = (uint64_t *)(page + PAGE_SIZE - sizeof(uint64_t));

        /* 任意填充整页（可省略，仅示范） */
        memset(page, 0x42, PAGE_SIZE - sizeof(uint64_t));

        /* 填写指向下一页的指针；最后一页写 0 表示链表结束 */
        if (i < NUM_PAGES - 1)
            *tailptr = (uint64_t)((char *)memory + (size_t)(i + 1) * PAGE_SIZE);
        else
            *tailptr = 0ULL;
    }

    /* 3. 可添加遍历测试（触碰链表以触发缺页 & 预取） */
    printf("Traversing pointer chain …\n");
    uint64_t *curr = (uint64_t *)memory;
    int page_idx   = 0;
    while (curr && *curr && page_idx < NUM_PAGES) {
        /* 读几处数据触发页面访问 */
        volatile char dummy = ((char *)curr)[128];
        (void)dummy;

        printf("Page %d at %p → next 0x%lx\n",
               page_idx, (void *)curr, (unsigned long)*curr);

        curr = (uint64_t *)(*curr);
        ++page_idx;
    }

    /* 4. 清理 */
    munmap(memory, 2 * 1024 * 1024);
    puts("Test completed successfully");
    return 0;
}
