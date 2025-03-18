#!/bin/bash

# Script to monitor ExtMem swap process activity

# ANSI color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to convert bytes to human readable format
human_readable() {
    local bytes=$1
    local units=("B" "KB" "MB" "GB" "TB")
    local unit=0
    
    while ((bytes > 1024)); do
        bytes=$(echo "scale=2; $bytes/1024" | bc)
        ((unit++))
    done
    
    echo "$bytes ${units[$unit]}"
}

# Function to print swap statistics
print_stats() {
    local pid=$1
    
    if ! ps -p $pid > /dev/null; then
        echo -e "${RED}Swap process (PID: $pid) is not running!${NC}"
        return 1
    fi
    
    # Get memory usage
    local mem_kb=$(ps -o rss= -p $pid)
    local mem_bytes=$((mem_kb * 1024))
    local mem_human=$(human_readable $mem_bytes)
    
    # Get CPU usage
    local cpu=$(ps -o %cpu= -p $pid)
    
    # Get open file descriptors
    local fds=$(ls -l /proc/$pid/fd | wc -l)
    
    # Print process info
    echo -e "${BLUE}=== Swap Process (PID: $pid) ====${NC}"
    echo -e "${GREEN}Memory Usage:${NC} $mem_human"
    echo -e "${GREEN}CPU Usage:${NC} $cpu%"
    echo -e "${GREEN}File Descriptors:${NC} $fds"
    
    # If possible, extract additional statistics from shared memory
    # This would require a separate program to access the shared memory region
    
    echo ""
}

# Main monitoring loop
monitor() {
    local pid=$1
    local interval=${2:-2}  # Default update interval: 2 seconds
    
    clear
    while true; do
        echo -e "${YELLOW}ExtMem Swap Process Monitor${NC} (Press Ctrl+C to exit)"
        echo -e "${YELLOW}Update interval:${NC} ${interval}s"
        echo ""
        
        print_stats $pid || break
        
        echo -e "${YELLOW}System Memory Usage:${NC}"
        free -h
        echo ""
        
        # Check if the swap file is growing
        if [ -f "/tmp/extmem_swap.bin" ]; then
            local swap_size=$(du -h /tmp/extmem_swap.bin | cut -f1)
            echo -e "${GREEN}Swap File Size:${NC} $swap_size"
        fi
        
        sleep $interval
        clear
    done
}

# Find the swap process PID
find_swap_pid() {
    pgrep -f "^./swap_process$"
}

# Main execution starts here

# Show usage if requested
if [ "$1" == "-h" ] || [ "$1" == "--help" ]; then
    echo "Usage: $0 [PID] [interval]"
    echo ""
    echo "  PID       - Process ID of swap_process (optional, auto-detected if not provided)"
    echo "  interval  - Update interval in seconds (default: 2)"
    echo ""
    echo "Examples:"
    echo "  $0             # Auto-detect swap process and monitor with default interval"
    echo "  $0 1234        # Monitor swap process with PID 1234"
    echo "  $0 1234 5      # Monitor swap process with PID 1234, update every 5 seconds"
    exit 0
fi

# Get the swap process PID
SWAP_PID=$1
if [ -z "$SWAP_PID" ]; then
    SWAP_PID=$(find_swap_pid)
    if [ -z "$SWAP_PID" ]; then
        echo -e "${RED}No swap process found!${NC}"
        echo "Make sure the swap process is running or specify the PID manually."
        exit 1
    fi
    echo -e "${GREEN}Detected swap process with PID: ${SWAP_PID}${NC}"
else
    # Verify the provided PID belongs to a swap process
    if ! ps -p $SWAP_PID -o cmd= | grep -q "swap_process"; then
        echo -e "${RED}Warning: PID $SWAP_PID does not appear to be a swap process!${NC}"
        echo "Continue anyway? (y/n)"
        read -r response
        if [[ ! "$response" =~ ^[Yy]$ ]]; then
            exit 1
        fi
    fi
fi

# Get the update interval
INTERVAL=$2
if [ -z "$INTERVAL" ]; then
    INTERVAL=2
fi

# Start monitoring
monitor $SWAP_PID $INTERVAL