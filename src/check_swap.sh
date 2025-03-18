#!/bin/bash

# ExtMem Swap Process 状态检查脚本

# ANSI 颜色代码
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m' # 无颜色

# 检查 swap 进程是否运行
check_swap_process() {
    local pid=$(pgrep -f "^./swap_process$")
    if [ -z "$pid" ]; then
        echo -e "${RED}Swap 进程未运行!${NC}"
        return 1
    else
        echo -e "${GREEN}Swap 进程正在运行，PID: $pid${NC}"
        
        # 显示详细信息
        echo -e "\n${YELLOW}进程详情:${NC}"
        ps -p $pid -o pid,ppid,cmd,stat,rss,vsz,time
        
        return 0
    fi
}

# 检查共享内存段
check_shm() {
    echo -e "\n${YELLOW}共享内存段:${NC}"
    
    local shm_exists=$(ipcs -m | grep "/extmem_swap_shm")
    if [ -z "$shm_exists" ]; then
        echo -e "${RED}未找到 ExtMem 共享内存段${NC}"
    else
        echo -e "${GREEN}找到 ExtMem 共享内存段:${NC}"
        ipcs -m | head -n 2  # 表头
        ipcs -m | grep "/extmem_swap_shm"
    fi
}

# 检查 swap 文件
check_swap_file() {
    echo -e "\n${YELLOW}Swap 文件:${NC}"
    
    local swap_file="/tmp/extmem_swap/extmem_swap.bin"
    if [ -f "$swap_file" ]; then
        echo -e "${GREEN}Swap 文件存在:${NC}"
        ls -lh "$swap_file"
        
        # 显示文件类型
        file "$swap_file"
        
        # 显示文件大小
        echo -e "\n文件大小:"
        du -h "$swap_file"
    elif [ -d "/tmp/extmem_swap" ]; then
        echo -e "${YELLOW}Swap 目录存在，但文件不存在:${NC}"
        ls -la "/tmp/extmem_swap"
    else
        echo -e "${RED}未找到 Swap 文件或目录${NC}"
    fi
    
    # 检查环境变量
    echo -e "\n${YELLOW}环境变量:${NC}"
    echo -e "SWAPDIR=${SWAPDIR:-未设置}"
    echo -e "DRAMSIZE=${DRAMSIZE:-未设置}"
}

# 检查系统内存使用
check_memory() {
    echo -e "\n${YELLOW}系统内存使用:${NC}"
    free -h
    
    echo -e "\n${YELLOW}Top 内存进程:${NC}"
    ps -eo pid,ppid,cmd,%mem,%cpu --sort=-%mem | head -n 6
}

# 主函数
main() {
    echo -e "${BLUE}===== ExtMem Swap Process 状态检查 =====${NC}"
    echo -e "当前时间: $(date)"
    
    check_swap_process
    check_shm
    check_swap_file
    check_memory
    
    echo -e "\n${BLUE}===== 检查完成 =====${NC}"
}

# 执行主函数
main