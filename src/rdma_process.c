#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>

#include "include/rdma_sim.h"

static volatile bool shutdown_requested = false;

// 信号处理函数
void signal_handler(int sig) {
    printf("RDMA process received signal %d, shutting down...\n", sig);
    shutdown_requested = true;
}

// 安装信号处理器
void install_signal_handlers(void) {
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

// 打印使用说明
void print_usage(const char* program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -h, --help     Show this help message\n");
    printf("  -v, --verbose  Enable verbose logging\n");
    printf("  -d, --daemon   Run as daemon process\n");
    printf("\n");
    printf("RDMA Simulation Process for ExtMem\n");
    printf("This process simulates remote memory access over RDMA\n");
}

// 主循环
int main_loop(bool verbose) {
    printf("RDMA simulation process starting...\n");
    
    // 初始化RDMA模拟（作为服务器）
    if (rdma_sim_init(true) != 0) {
        fprintf(stderr, "Failed to initialize RDMA simulation\n");
        return -1;
    }
    
    printf("RDMA simulation initialized successfully\n");
    
    // 获取共享内存引用
    rdma_shm_t* shm = get_rdma_shm();
    if (shm == NULL) {
        fprintf(stderr, "Failed to get shared memory reference\n");
        rdma_sim_cleanup(true);
        return -1;
    }
    
    // 统计信息
    time_t last_stats_time = time(NULL);
    uint64_t last_total_requests = 0;
    
    printf("RDMA process ready, waiting for requests...\n");
    
    // 主处理循环
    while (!shutdown_requested && !shm->shutdown_requested) {
        // 处理RDMA请求
        int result = rdma_server_process_requests();
        if (result != 0 && !shutdown_requested && !shm->shutdown_requested) {
            fprintf(stderr, "Error processing RDMA requests: %d\n", result);
            break;
        }
        
        // 定期打印统计信息
        if (verbose) {
            time_t current_time = time(NULL);
            if (current_time - last_stats_time >= 30) {  // 每30秒
                uint64_t current_requests = shm->total_requests;
                uint64_t requests_per_sec = (current_requests - last_total_requests) / 30;
                
                printf("RDMA Stats: Total=%lu, Completed=%lu, Failed=%lu, Rate=%lu req/s\n",
                       shm->total_requests, shm->completed_requests, 
                       shm->failed_requests, requests_per_sec);
                
                last_stats_time = current_time;
                last_total_requests = current_requests;
            }
        }
        
        // 短暂休眠避免忙等待
        if (shm->count == 0) {
            usleep(1000);  // 1ms
        }
    }
    
    printf("RDMA process shutting down...\n");
    
    // 打印最终统计信息
    rdma_print_stats();
    
    // 清理资源
    rdma_sim_cleanup(true);
    
    printf("RDMA process terminated\n");
    return 0;
}

// 守护进程模式
int run_as_daemon(bool verbose) {
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    
    if (pid > 0) {
        // 父进程退出
        printf("RDMA daemon started with PID: %d\n", pid);
        exit(0);
    }
    
    // 子进程继续
    setsid();  // 创建新会话
    
    // 重定向标准输入输出
    if (!verbose) {
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
    }
    
    // 改变工作目录到根目录
    chdir("/");
    
    return main_loop(verbose);
}

int main(int argc, char* argv[]) {
    bool verbose = false;
    bool daemon_mode = false;
    
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
            daemon_mode = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // 安装信号处理器
    install_signal_handlers();
    
    // 运行模式选择
    if (daemon_mode) {
        return run_as_daemon(verbose);
    } else {
        return main_loop(verbose);
    }
}