#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

// 使用ExtMem提供的API
#include "storage_swap_process.h"

// 测试中使用的常量
#define TEST_PAGE_SIZE 4096
#define MB (1024 * 1024)
#define GB (1024 * MB)

// 获取当前时间（秒）
double get_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

// 将字节数格式化为人类可读的字符串
void format_size(char *buf, size_t buf_size, uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = bytes;
    
    while (size >= 1024 && unit < 4) {
        size /= 1024;
        unit++;
    }
    
    snprintf(buf, buf_size, "%.2f %s", size, units[unit]);
}

// 用指定模式填充内存并返回触及的页面数
int fill_memory(void *memory, size_t size, int pattern) {
    unsigned char *ptr = (unsigned char *)memory;
    int pages_touched = 0;
    
    for (size_t i = 0; i < size; i += TEST_PAGE_SIZE) {
        // 在每页开始处写入
        ptr[i] = (unsigned char)(pattern & 0xFF);
        pages_touched++;
        
        // 每10%进度打印一次
        if (i % (size / 10) == 0) {
            printf("\rProgress: %.0f%%", (float)i * 100 / size);
            fflush(stdout);
        }
    }
    printf("\rProgress: 100%%\n");
    
    return pages_touched;
}

// 验证内存中的模式并返回错误数
int verify_memory(void *memory, size_t size, int pattern) {
    unsigned char *ptr = (unsigned char *)memory;
    int errors = 0;
    
    for (size_t i = 0; i < size; i += TEST_PAGE_SIZE) {
        if (ptr[i] != (unsigned char)(pattern & 0xFF)) {
            errors++;
        }
        
        // 每10%进度打印一次
        if (i % (size / 10) == 0) {
            printf("\rVerifying: %.0f%%", (float)i * 100 / size);
            fflush(stdout);
        }
    }
    printf("\rVerifying: 100%%\n");
    
    return errors;
}

// 模拟随机访问内存
void random_access(void *memory, size_t size, int num_accesses) {
    unsigned char *ptr = (unsigned char *)memory;
    int pages = size / TEST_PAGE_SIZE;
    
    printf("Performing %d random memory accesses...\n", num_accesses);
    
    for (int i = 0; i < num_accesses; i++) {
        size_t page = rand() % pages;
        size_t offset = page * TEST_PAGE_SIZE;
        
        // 读取
        unsigned char value = ptr[offset];
        
        // 写入（增加值）
        ptr[offset] = value + 1;
        
        // 每10%进度打印一次
        if (i % (num_accesses / 10) == 0) {
            printf("\rProgress: %.0f%%", (float)i * 100 / num_accesses);
            fflush(stdout);
        }
    }
    printf("\rProgress: 100%%\n");
}

// 获取交换文件大小
uint64_t get_swap_file_size() {
    const char* swap_dir = getenv("SWAPDIR");
    char swap_file_path[1024];
    
    if (swap_dir) {
        struct stat st;
        if (stat(swap_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(swap_file_path, sizeof(swap_file_path), "%s/extmem_swap.bin", swap_dir);
        } else {
            strcpy(swap_file_path, swap_dir);
        }
    } else {
        strcpy(swap_file_path, "/tmp/extmem_swap.bin");
    }
    
    struct stat st;
    if (stat(swap_file_path, &st) == 0) {
        return st.st_size;
    }
    
    return 0;
}

int main(int argc, char* argv[]) {
    // 解析参数
    size_t memory_size = 512 * MB;  // 默认: 512MB
    int checkpoint_interval = 5;    // 默认: 5秒
    int run_time = 60;              // 默认: 60秒
    
    if (argc > 1) memory_size = (size_t)atol(argv[1]) * MB;
    if (argc > 2) checkpoint_interval = atoi(argv[2]);
    if (argc > 3) run_time = atoi(argv[3]);
    
    // 初始化
    char size_str[32];
    format_size(size_str, sizeof(size_str), memory_size);
    printf("=== ExtMem Checkpoint Test ===\n");
    printf("Memory Size:          %s\n", size_str);
    printf("Checkpoint Interval:  %d seconds\n", checkpoint_interval);
    printf("Total Run Time:       %d seconds\n", run_time);
    printf("\n");
    
    // 初始化存储系统
    printf("Initializing storage...\n");
    if (storage_init() != 0) {
        fprintf(stderr, "Failed to initialize storage\n");
        return 1;
    }
    
    // 使用mmap分配内存
    printf("Allocating %s of memory...\n", size_str);
    double start_time = get_time();
    
    void *memory = mmap(NULL, memory_size, PROT_READ | PROT_WRITE, 
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        perror("mmap failed");
        storage_shutdown();
        return 1;
    }
    
    double alloc_time = get_time() - start_time;
    printf("Memory allocation completed in %.2f seconds\n", alloc_time);
    
    // 用模式填充内存
    printf("Writing to memory...\n");
    start_time = get_time();
    int pages_touched = fill_memory(memory, memory_size, 0xAA);
    double write_time = get_time() - start_time;
    printf("Memory write completed in %.2f seconds (%.2f MB/s)\n", 
           write_time, (memory_size / MB) / write_time);
    
    // 主测试循环
    printf("\nStarting checkpoint test loop...\n");
    double test_start_time = get_time();
    double next_checkpoint_time = test_start_time + checkpoint_interval;
    int checkpoint_count = 0;
    double total_checkpoint_time = 0.0;
    double max_checkpoint_time = 0.0;
    double min_checkpoint_time = 999999.0;
    
    while (get_time() - test_start_time < run_time) {
        double current_time = get_time();
        
        // 是否到创建检查点的时间？
        if (current_time >= next_checkpoint_time) {
            char checkpoint_name[64];
            sprintf(checkpoint_name, "cp_%d", checkpoint_count);
            
            printf("\nCreating checkpoint: %s\n", checkpoint_name);
            start_time = get_time();
            
            if (swap_process_checkpoint(checkpoint_name) != 0) {
                fprintf(stderr, "Checkpoint failed\n");
            } else {
                double cp_time = get_time() - start_time;
                double cp_rate = (memory_size / MB) / cp_time;
                
                total_checkpoint_time += cp_time;
                if (cp_time > max_checkpoint_time) max_checkpoint_time = cp_time;
                if (cp_time < min_checkpoint_time) min_checkpoint_time = cp_time;
                
                checkpoint_count++;
                printf("Checkpoint completed in %.2f seconds (%.2f MB/s)\n", 
                       cp_time, cp_rate);
                
                // 测量交换文件大小
                uint64_t swap_size = get_swap_file_size();
                char swap_size_str[32];
                format_size(swap_size_str, sizeof(swap_size_str), swap_size);
                printf("Current swap file size: %s\n", swap_size_str);
            }
            
            next_checkpoint_time = current_time + checkpoint_interval;
        }
        
        // 执行一些随机内存访问
        int accesses = memory_size / TEST_PAGE_SIZE / 100;  // 触及约1%的页面
        if (accesses < 10) accesses = 10;
        if (accesses > 1000) accesses = 1000;
        
        start_time = get_time();
        random_access(memory, memory_size, accesses);
        double access_time = get_time() - start_time;
        printf("Random access of %d pages completed in %.2f seconds (%.2f pages/sec)\n", 
               accesses, access_time, accesses / access_time);
        
        // 短暂睡眠以避免占用过多CPU
        usleep(100000);  // 100ms
    }
    
    // 最终统计
    printf("\n=== Checkpoint Test Results ===\n");
    printf("Total checkpoints created:  %d\n", checkpoint_count);
    if (checkpoint_count > 0) {
        printf("Average checkpoint time:    %.2f seconds\n", total_checkpoint_time / checkpoint_count);
        printf("Minimum checkpoint time:    %.2f seconds\n", min_checkpoint_time);
        printf("Maximum checkpoint time:    %.2f seconds\n", max_checkpoint_time);
        
        // 测量交换文件大小
        uint64_t swap_size = get_swap_file_size();
        char swap_size_str[32];
        format_size(swap_size_str, sizeof(swap_size_str), swap_size);
        printf("Final swap file size:       %s\n", swap_size_str);
    }
    
    // 清理
    printf("\nCleaning up...\n");
    if (munmap(memory, memory_size) == -1) {
        perror("munmap failed");
        storage_shutdown();
        return 1;
    }
    
    storage_shutdown();
    printf("Test completed successfully!\n");
    
    return 0;
}