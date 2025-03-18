#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#define SHM_NAME "/extmem_swap"
#define MAX_SWAP_SIZE (4096) // 先用一个页面大小进行测试

typedef struct {
    int command;  // 0=idle, 1=swap_in, 2=swap_out
    uint64_t virtual_addr;
    uint64_t disk_offset;
    size_t page_size;
    int status; // 0=pending, 1=completed, -1=error
} swap_request;

typedef struct {
    int command;
    uint64_t virtual_addr;
    uint64_t disk_offset;
    size_t page_size;
    int status;
} swap_request;

int main() {
    // 创建共享内存
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed");
        return 1;
    }
    
    // 设置大小
    if (ftruncate(shm_fd, sizeof(swap_request) + MAX_SWAP_SIZE) == -1) {
        perror("ftruncate failed");
        return 1;
    }
    
    printf("Swap server started. Press Ctrl+C to exit.\n");
    
    // 简单的保持程序运行
    while(1) {
        sleep(1);
    }
    
    return 0;
}