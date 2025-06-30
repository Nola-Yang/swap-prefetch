#define _POSIX_C_SOURCE 199309L
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

void signal_handler(int sig) {
    printf("RDMA process received signal %d, shutting down...\n", sig);
    shutdown_requested = true;
}

void install_signal_handlers(void) {
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}


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

int main_loop(bool verbose) {
    printf("RDMA simulation process starting...\n");
    
    if (rdma_sim_init(true) != 0) {
        fprintf(stderr, "Failed to initialize RDMA simulation\n");
        return -1;
    }
    
    printf("RDMA simulation initialized successfully\n");
    
    rdma_shm_t* shm = get_rdma_shm();
    if (shm == NULL) {
        fprintf(stderr, "Failed to get shared memory reference\n");
        rdma_sim_cleanup(true);
        return -1;
    }
    
    time_t last_stats_time = time(NULL);
    uint64_t last_total_requests = 0;
    
    printf("RDMA process ready, waiting for requests...\n");
    
    while (!shutdown_requested && !shm->shutdown_requested) {
        fprintf(stderr, "RDMA_PROCESS: About to call rdma_server_process_requests()...\n");
        int result = rdma_server_process_requests();
        fprintf(stderr, "RDMA_PROCESS: rdma_server_process_requests() returned: %d\n", result);
        if (result != 0 && !shutdown_requested && !shm->shutdown_requested) {
            fprintf(stderr, "Error processing RDMA requests: %d\n", result);
            break;
        }
        
        // if (verbose) {
        //     time_t current_time = time(NULL);
        //     if (current_time - last_stats_time >= 30) {  
        //         uint64_t current_requests = shm->total_requests;
        //         uint64_t requests_per_sec = (current_requests - last_total_requests) / 30;
                
        //         printf("RDMA Stats: Total=%lu, Completed=%lu, Failed=%lu, Rate=%lu req/s\n",
        //                shm->total_requests, shm->completed_requests, 
        //                shm->failed_requests, requests_per_sec);
                
        //         last_stats_time = current_time;
        //         last_total_requests = current_requests;
        //     }
        // }
        
        // 短暂休眠避免忙等待
        if (shm->count == 0) {
            usleep(1000);  // 1ms
        }
    }
    
    printf("RDMA process shutting down...\n");

    rdma_print_stats();
    
    rdma_sim_cleanup(true);
    
    printf("RDMA process terminated\n");
    return 0;
}

int run_as_daemon(bool verbose) {
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    
    if (pid > 0) {
       
        printf("RDMA daemon started with PID: %d\n", pid);
        exit(0);
    }
    
   
    setsid();  
    
    if (!verbose) {
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
    }
    
    chdir("/");
    
    return main_loop(verbose);
}

int main(int argc, char* argv[]) {
    bool verbose = false;
    bool daemon_mode = false;
    
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
    
    install_signal_handlers();
    
    if (daemon_mode) {
        return run_as_daemon(verbose);
    } else {
        return main_loop(verbose);
    }
}