#ifndef STORAGE_SWAP_PROCESS_H
#define STORAGE_SWAP_PROCESS_H

#include <stdint.h>
#include <stdlib.h>

// 初始化存储系统，启动swap进程
int storage_init();

// 关闭存储系统，停止swap进程
void storage_shutdown();

// 从swap读取页面
int read_page(int fd, uint64_t offset, void* dest, size_t size);

// 向swap写入页面
int write_page(int fd, uint64_t offset, void* src, size_t size);

#endif /* STORAGE_SWAP_PROCESS_H */