#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <spawn.h>

#include "core.h"
#include "swap_shm.h"
#include "storage_swap_process.h"

// 全局变量
static pid_t swap_process_pid = -1;
static bool swap_initialized = false;
static pthread_mutex_t swap_mutex = PTHREAD_MUTEX_INITIALIZER;

// 启动swap进程
static int start_swap_process() {
    char *argv[] = {"swap_process", NULL};
    char *envp[] = {NULL};
    int ret;
    
    // 使用posix_spawn启动swap进程
    ret = posix_spawn(&swap_process_pid, "./swap_process", NULL, NULL, argv, envp);
    if (ret != 0) {
        perror("posix_spawn");
        return -1;
    }
    
    LOG("Swap process started with PID: %d\n", swap_process_pid);
    
    // 等待进程初始化
    usleep(100000);  // 100ms
    
    return 0;
}

// 初始化存储系统
int storage_init() {
    int ret;
    
    pthread_mutex_lock(&swap_mutex);
    
    if (swap_initialized) {
        pthread_mutex_unlock(&swap_mutex);
        return 0;  // 已经初始化
    }
    
    // 启动swap进程
    ret = start_swap_process();
    if (ret != 0) {
        pthread_mutex_unlock(&swap_mutex);
        return -1;
    }
    
    // 初始化共享内存连接
    ret = swap_shm_init(false);  // false = client (ExtMem)
    if (ret != 0) {
        pthread_mutex_unlock(&swap_mutex);
        return -1;
    }
    
    // 发送初始化请求
    uint64_t req_id = swap_submit_request(OP_INIT, 0, 0, 0, NULL);
    if (req_id < 0) {
        swap_shm_cleanup(false);
        pthread_mutex_unlock(&swap_mutex);
        return -1;
    }
    
    // 等待初始化完成
    int error_code;
    ret = swap_wait_request(req_id, &error_code);
    if (ret != 0) {
        swap_shm_cleanup(false);
        pthread_mutex_unlock(&swap_mutex);
        return -1;
    }
    
    swap_initialized = true;
    pthread_mutex_unlock(&swap_mutex);
    
    return 0;
}

// 关闭存储系统
void storage_shutdown() {
    pthread_mutex_lock(&swap_mutex);
    
    if (!swap_initialized) {
        pthread_mutex_unlock(&swap_mutex);
        return;
    }
    
    // 发送关闭请求
    uint64_t req_id = swap_submit_request(OP_SHUTDOWN, 0, 0, 0, NULL);
    if (req_id >= 0) {
        // 等待关闭完成 (可选)
        int error_code;
        swap_wait_request(req_id, &error_code);
    }
    
    // 清理共享内存
    swap_shm_cleanup(false);
    
    // 等待swap进程退出
    int status;
    waitpid(swap_process_pid, &status, 0);
    
    swap_initialized = false;
    pthread_mutex_unlock(&swap_mutex);
}

// 从swap读取页面
int swap_process_read_page(int fd, uint64_t offset, void* dest, size_t size) {
    int ret;
    int error_code;
    
    // 忽略fd参数，使用offset定位数据
    
    // 发送读请求
    uint64_t req_id = swap_submit_request(OP_READ_PAGE, 0, offset, size, NULL);
    if (req_id < 0) {
        return -1;
    }
    
    // 等待读取完成
    ret = swap_wait_request(req_id, &error_code);
    if (ret != 0) {
        return -1;
    }
    
    // 从共享内存复制数据
    swap_shm_t* shm = get_swap_shm();
    if (shm == NULL) {
        return -1;
    }
    
    // 找到请求的buffer_offset
    for (uint32_t i = 0; i < MAX_QUEUE_SIZE; i++) {
        if (shm->requests[i].request_id == req_id) {
            memcpy(dest, shm->page_buffer + shm->requests[i].buffer_offset, size);
            return 0;
        }
    }
    
    // 没有找到请求
    return -1;
}

// 向swap写入页面
int swap_process_write_page(int fd, uint64_t offset, void* src, size_t size) {
    int ret;
    int error_code;
    
    // 忽略fd参数，使用offset定位数据
    
    // 发送写请求
    uint64_t req_id = swap_submit_request(OP_WRITE_PAGE, 0, offset, size, src);
    if (req_id < 0) {
        return -1;
    }
    
    // 等待写入完成
    ret = swap_wait_request(req_id, &error_code);
    if (ret != 0) {
        return -1;
    }
    
    return 0;
}