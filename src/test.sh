#!/bin/bash

# Test script for ExtMem with Swap Process

# 创建 swap 目录
mkdir -p /tmp/extmem_swap

# 编译代码
make clean
make

# 确保 swap_process 编译成功
if [ ! -f "./swap_process" ]; then
  echo "ERROR: Failed to build swap_process"
  exit 1
fi

# 设置环境变量
export DRAMSIZE=1073741824  # 1GB
export SWAPDIR="/tmp/extmem_swap"  # 使用目录路径

# 启动 swap process
echo "Starting swap process..."
./swap_process &
SWAP_PID=$!

# 等待 swap process 启动
sleep 2
if ! ps -p $SWAP_PID > /dev/null; then
  echo "ERROR: Swap process failed to start"
  exit 1
fi

# 运行简单测试程序
echo "Running a simple memory allocation test..."
cat > test_alloc.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE_SIZE 4096
#define MB (1024 * 1024)
#define ALLOC_SIZE (512 * MB)  // 分配 512MB 内存

int main() {
    printf("Allocating %d MB of memory...\n", ALLOC_SIZE / MB);
    
    // 分配内存
    char *memory = malloc(ALLOC_SIZE);
    if (!memory) {
        perror("malloc failed");
        return 1;
    }
    
    // 写入数据
    printf("Writing data to memory...\n");
    for (size_t i = 0; i < ALLOC_SIZE; i += PAGE_SIZE) {
        memory[i] = (char)(i % 256);
    }
    
    // 读取并验证数据
    printf("Reading and verifying data...\n");
    for (size_t i = 0; i < ALLOC_SIZE; i += PAGE_SIZE) {
        if (memory[i] != (char)(i % 256)) {
            printf("Verification failed at offset %zu\n", i);
            break;
        }
    }
    
    printf("Memory test completed successfully!\n");
    
    // 等待用户输入，让内存保持分配状态
    printf("Press Enter to exit...");
    getchar();
    
    // 释放内存
    free(memory);
    return 0;
}
EOF

# 编译测试程序
gcc -O2 test_alloc.c -o test_alloc

# 使用 ExtMem 运行测试程序
echo "Running test with ExtMem..."
LD_PRELOAD=./libextmem-default.so ./test_alloc &
TEST_PID=$!

# 等待测试程序退出或超时
TIMEOUT=30
for ((i=0; i<TIMEOUT; i++)); do
    if ! ps -p $TEST_PID > /dev/null; then
        echo "Test program completed."
        break
    fi
    echo "Waiting for test to complete... ($i/$TIMEOUT)"
    sleep 1
done

# 如果测试程序仍在运行，发送回车键然后终止它
if ps -p $TEST_PID > /dev/null; then
    echo -e "\n" | kill -SIGINT $TEST_PID
    echo "Test program terminated after timeout."
fi

# 显示 swap 文件信息
echo "Swap file info:"
ls -lh /tmp/extmem_swap/

# 终止 swap 进程
echo "Stopping swap process..."
kill $SWAP_PID
sleep 1

# 清理临时文件
rm -f test_alloc test_alloc.c

echo "Test completed!"