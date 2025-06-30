#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <assert.h>

#define PAGE_SIZE 4096
#define NUM_PAGES 10
#define TOTAL_SIZE (NUM_PAGES * PAGE_SIZE)

// 测试结构，模拟有指针链接的数据结构
typedef struct test_node {
    char data[PAGE_SIZE - 8];  // 填充数据
    uint64_t next_ptr;         // 指向下一页的指针（在页面末端）
} test_node_t;

void setup_pointer_chain(void* base_addr, int num_pages) {
    printf("Setting up pointer chain with %d pages starting at %p\n", 
           num_pages, base_addr);
    
    for (int i = 0; i < num_pages - 1; i++) {
        test_node_t* current = (test_node_t*)((char*)base_addr + i * PAGE_SIZE);
        test_node_t* next = (test_node_t*)((char*)base_addr + (i + 1) * PAGE_SIZE);
        
        // 初始化数据
        memset(current->data, 'A' + i, sizeof(current->data));
        
        // 设置指向下一页的指针
        current->next_ptr = (uint64_t)next;
        
        printf("Page %d: addr=%p, next_ptr=0x%lx, points_to=%p\n", 
               i, current, current->next_ptr, next);
    }
    
    // 最后一页的指针设为NULL
    test_node_t* last = (test_node_t*)((char*)base_addr + (num_pages - 1) * PAGE_SIZE);
    memset(last->data, 'A' + num_pages - 1, sizeof(last->data));
    last->next_ptr = 0;
    
    printf("Last page: addr=%p, next_ptr=0x%lx (NULL)\n", last, last->next_ptr);
}

void traverse_pointer_chain(void* base_addr) {
    printf("\nTraversing pointer chain:\n");
    
    test_node_t* current = (test_node_t*)base_addr;
    int page_count = 0;
    
    while (current != NULL && page_count < NUM_PAGES) {
        printf("Visiting page %d at %p, data starts with '%c', next_ptr=0x%lx\n", 
               page_count, current, current->data[0], current->next_ptr);
        
        // 访问页面数据以触发页面错误
        volatile char dummy = current->data[100];  // 强制内存访问
        (void)dummy;  // 避免编译器警告
        
        // 短暂延迟以观察预取效果
        usleep(100000);  // 100ms
        
        // 移动到下一页
        if (current->next_ptr == 0) {
            printf("Reached end of chain at page %d\n", page_count);
            break;
        }
        
        current = (test_node_t*)current->next_ptr;
        page_count++;
    }
    
    printf("Chain traversal completed, visited %d pages\n", page_count + 1);
}

void print_memory_layout(void* base_addr, int num_pages) {
    printf("\nMemory layout:\n");
    for (int i = 0; i < num_pages; i++) {
        void* page_addr = (char*)base_addr + i * PAGE_SIZE;
        uint64_t* ptr_addr = (uint64_t*)((char*)page_addr + PAGE_SIZE - 8);
        printf("Page %d: %p, pointer at end: 0x%lx\n", i, page_addr, *ptr_addr);
    }
}

int main() {
    printf("Pointer Prefetch Test Program\n");
    printf("=============================\n");
    
    // 分配大块内存，这将由ExtMem管理
    void* memory = mmap(NULL, TOTAL_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (memory == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }
    
    printf("Allocated %d bytes (%d pages) at address %p\n", 
           TOTAL_SIZE, NUM_PAGES, memory);
    
    // 设置指针链
    setup_pointer_chain(memory, NUM_PAGES);
    
    // 打印内存布局
    print_memory_layout(memory, NUM_PAGES);
    
    // 等待一下让系统稳定
    printf("\nWaiting 2 seconds before traversal...\n");
    sleep(2);
    
    // 遍历指针链，这应该触发预取
    traverse_pointer_chain(memory);
    
    // 再次遍历以测试预取效果
    printf("\n\nSecond traversal (should be faster due to prefetch):\n");
    traverse_pointer_chain(memory);
    
    // 清理
    if (munmap(memory, TOTAL_SIZE) == -1) {
        perror("munmap failed");
        return 1;
    }
    
    printf("\nTest completed successfully\n");
    return 0;
}