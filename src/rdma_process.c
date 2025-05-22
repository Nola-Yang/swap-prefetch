#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include "rdma_sim.h"

// Configuration
#define RDMA_CONFIG_FILE "/tmp/extmem_rdma.conf"

// Global flag for signal handling
volatile sig_atomic_t running = 1;

// Signal handler
static void sig_handler(int signum) {
    running = 0;
}

// Process RDMA requests
int process_rdma_request(rdma_request_t* req) {
    int ret = 0;
    rdma_shm_t* shm = get_rdma_shm();
    struct timeval start, end;
    double latency;
    
    // Record start time
    gettimeofday(&start, NULL);
    
    switch (req->op_type) {
        case RDMA_OP_READ:
            // Read from remote memory to buffer
            ret = rdma_sim_read(req->local_addr, req->remote_addr, 
                              req->length, shm->page_buffer + req->buffer_offset);
            break;
            
        case RDMA_OP_WRITE:
            // Write from buffer to remote memory
            ret = rdma_sim_write(req->local_addr, req->remote_addr, 
                               req->length, shm->page_buffer + req->buffer_offset);
            break;
            
        case RDMA_OP_ALLOCATE: {
            // Allocate memory in remote node
            uint64_t remote_addr;
            ret = rdma_sim_allocate(req->length, &remote_addr);
            if (ret == 0) {
                req->remote_addr = remote_addr;
            }
            break;
        }
            
        case RDMA_OP_FREE:
            // Free memory in remote node
            ret = rdma_sim_free(req->remote_addr);
            break;
            
        case RDMA_OP_INIT:
            // Initialize RDMA - already done
            printf("RDMA initialization request received\n");
            break;
            
        case RDMA_OP_SHUTDOWN:
            // Shutdown RDMA
            printf("RDMA shutdown request received\n");
            running = 0;
            break;
            
        default:
            fprintf(stderr, "Unknown RDMA operation type: %d\n", req->op_type);
            ret = -EINVAL;
            break;
    }
    
    // Record end time and calculate latency
    gettimeofday(&end, NULL);
    latency = (end.tv_sec - start.tv_sec) * 1000000.0 + (end.tv_usec - start.tv_usec);
    req->simulated_latency = latency;
    
    return ret;
}

static void print_usage(const char* program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -h, --help                Show this help message\n");
    printf("  -c, --config <file>       Specify configuration file (default: %s)\n", RDMA_CONFIG_FILE);
    printf("  -l, --min-latency <us>    Set minimum simulated latency in microseconds\n");
    printf("  -L, --max-latency <us>    Set maximum simulated latency in microseconds\n");
    printf("  -b, --bandwidth <Mbps>    Set simulated bandwidth in Mbps\n");
    printf("  -v, --verbose             Enable verbose output\n");
}

static void read_config_file(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        printf("Config file %s not found, using defaults\n", filename);
        return;
    }
    
    char line[256];
    char key[128];
    char value[128];
    
    while (fgets(line, sizeof(line), file)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        
        // Parse key=value pairs
        if (sscanf(line, "%127[^=]=%127s", key, value) == 2) {
            // Trim whitespace
            char* k = key;
            while (*k == ' ' || *k == '\t') k++;
            char* v = value;
            while (*v == ' ' || *v == '\t') v++;
            
            // Remove trailing whitespace from key
            char* end = k + strlen(k) - 1;
            while (end > k && (*end == ' ' || *end == '\t')) {
                *end = '\0';
                end--;
            }
            
            // Handle configuration options
            if (strcmp(k, "min_latency") == 0) {
                int latency = atoi(v);
                if (latency > 0) {
                    printf("Setting min_latency to %d us\n", latency);
                    // Set min_latency_us = latency;
                }
            } else if (strcmp(k, "max_latency") == 0) {
                int latency = atoi(v);
                if (latency > 0) {
                    printf("Setting max_latency to %d us\n", latency);
                    // Set max_latency_us = latency;
                }
            } else if (strcmp(k, "bandwidth") == 0) {
                int bw = atoi(v);
                if (bw > 0) {
                    printf("Setting bandwidth to %d Mbps\n", bw);
                    // Set bandwidth_mbps = bw;
                }
            }
        }
    }
    
    fclose(file);
}

int main(int argc, char* argv[]) {
    int ret;
    const char* config_file = RDMA_CONFIG_FILE;
    bool verbose = false;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (i + 1 < argc) {
                config_file = argv[++i];
            } else {
                fprintf(stderr, "Error: Missing config file path\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--min-latency") == 0) {
            if (i + 1 < argc) {
                int latency = atoi(argv[++i]);
                if (latency > 0) {
                    printf("Setting min_latency to %d us\n", latency);
                    // Set min_latency_us = latency;
                }
            }
        } else if (strcmp(argv[i], "-L") == 0 || strcmp(argv[i], "--max-latency") == 0) {
            if (i + 1 < argc) {
                int latency = atoi(argv[++i]);
                if (latency > 0) {
                    printf("Setting max_latency to %d us\n", latency);
                    // Set max_latency_us = latency;
                }
            }
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bandwidth") == 0) {
            if (i + 1 < argc) {
                int bw = atoi(argv[++i]);
                if (bw > 0) {
                    printf("Setting bandwidth to %d Mbps\n", bw);
                    // Set bandwidth_mbps = bw;
                }
            }
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // Read configuration file
    read_config_file(config_file);
    
    // Set up signal handlers
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    
    printf("RDMA simulation process starting...\n");
    
    // Initialize RDMA simulation
    ret = rdma_sim_init(true);  // true = server (RDMA process)
    if (ret != 0) {
        fprintf(stderr, "Failed to initialize RDMA simulation\n");
        return 1;
    }
    
    printf("RDMA simulation initialized. Waiting for requests...\n");
    
    // Main loop
    while (running) {
        // Process requests
        ret = rdma_process_next_request();
        if (ret != 0) {
            fprintf(stderr, "Error processing RDMA request\n");
            break;
        }
        
        // Print statistics periodically if verbose
        if (verbose) {
            static time_t last_stats_time = 0;
            time_t now = time(NULL);
            
            if (now - last_stats_time >= 5) {  // Every 5 seconds
                double avg_latency;
                uint64_t bytes_read, bytes_written;
                
                if (rdma_get_stats(&avg_latency, &bytes_read, &bytes_written) == 0) {
                    printf("RDMA Stats: Avg Latency=%.2f us, Read=%lu bytes, Written=%lu bytes\n",
                           avg_latency, bytes_read, bytes_written);
                }
                
                last_stats_time = now;
            }
        }
    }
    
    printf("RDMA simulation shutting down...\n");
    
    // Clean up
    rdma_sim_cleanup(true);
    
    printf("RDMA simulation terminated.\n");
    
    return 0;
}