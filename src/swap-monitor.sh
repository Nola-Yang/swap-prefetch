#!/bin/bash

# Function to find the swap process PID
find_swap_pid() {
    # Try different methods to find the PID
    # 1. Look for exact process name
    local pid=$(pgrep -xf "swap_process" 2>/dev/null)
    
    # 2. If not found, try more general approach
    if [ -z "$pid" ]; then
        pid=$(pgrep -f "swap_process$" 2>/dev/null)
    fi
    
    # 3. If still not found, try ps with grep
    if [ -z "$pid" ]; then
        pid=$(ps aux | grep "[s]wap_process" | awk '{print $2}' 2>/dev/null)
    fi
    
    echo "$pid"
}

# Function for human-readable size
human_readable() {
    local bytes=$1
    if [ -z "$bytes" ] || [ "$bytes" -lt 1 ]; then
        echo "0 B"
        return
    fi
    
    local suffixes=("B" "KB" "MB" "GB" "TB")
    local i=0
    
    while [ $bytes -ge 1024 ] && [ $i -lt ${#suffixes[@]}-1 ]; do
        bytes=$((bytes / 1024))
        i=$((i + 1))
    done
    
    echo "$bytes ${suffixes[$i]}"
}

# Function to check PID is valid
is_valid_pid() {
    local pid=$1
    if [[ "$pid" =~ ^[0-9]+$ ]] && [ -d "/proc/$pid" ]; then
        return 0
    else
        return 1
    fi
}

# Function to check swap process health
check_health() {
    local pid=$1
    
    # Check PID is valid
    if ! is_valid_pid "$pid"; then
        echo "ERROR: Invalid or non-existent PID: $pid"
        return 1
    fi
    
    # Check process is actually the swap process
    local cmd=$(ps -p "$pid" -o comm= 2>/dev/null)
    if [ "$cmd" != "swap_process" ]; then
        echo "ERROR: PID $pid is not the swap process (it's '$cmd')"
        return 1
    fi
    
    # Get process stats
    local mem_kb=$(ps -p "$pid" -o rss= 2>/dev/null)
    local mem_bytes=$((mem_kb * 1024))
    local cpu=$(ps -p "$pid" -o %cpu= 2>/dev/null)
    local uptime=$(ps -p "$pid" -o etime= 2>/dev/null)
    
    echo "Swap Process (PID: $pid) Status:"
    echo "- Command: $cmd"
    echo "- Memory Usage: $(human_readable $mem_bytes)"
    echo "- CPU Usage: $cpu%"
    echo "- Uptime: $uptime"
    
    # Check if swap file exists
    local swap_paths=(
        "/tmp/extmem_swap.bin"
        "/tmp/extmem_swap/extmem_swap.bin"
    )
    
    local swap_found=0
    for path in "${swap_paths[@]}"; do
        if [ -f "$path" ]; then
            local swap_size=$(du -b "$path" 2>/dev/null | cut -f1)
            echo "- Swap File: $path"
            echo "- Swap Size: $(human_readable $swap_size)"
            swap_found=1
            break
        fi
    done
    
    if [ $swap_found -eq 0 ]; then
        echo "- No swap file found"
    fi
    
    return 0
}

# Main function
main() {
    local pid="$1"
    local interval="${2:-5}"
    local count="${3:-1}"
    
    # Auto-detect PID if not provided
    if [ -z "$pid" ]; then
        pid=$(find_swap_pid)
        if [ -z "$pid" ]; then
            echo "ERROR: No swap process found!"
            return 1
        fi
        echo "Auto-detected swap process with PID: $pid"
    fi
    
    # Check process once
    if ! check_health "$pid"; then
        return 1
    fi
    
    # Continue monitoring if requested
    if [ "$count" = "continuous" ] || [ "$count" -gt 1 ]; then
        echo "Monitoring every $interval seconds. Press Ctrl+C to stop."
        if [ "$count" = "continuous" ]; then
            count=0
        fi
        
        local i=1
        while [ $count -eq 0 ] || [ $i -lt $count ]; do
            sleep "$interval"
            echo ""
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] Check #$i"
            if ! check_health "$pid"; then
                return 1
            fi
            i=$((i + 1))
        done
    fi
    
    return 0
}

# Show help if requested
if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    echo "Usage: $0 [PID] [interval] [count]"
    echo ""
    echo "  PID       - Process ID of swap_process (optional, auto-detected if not provided)"
    echo "  interval  - Update interval in seconds (default: 5)"
    echo "  count     - Number of checks to perform (default: 1, 'continuous' for indefinite)"
    echo ""
    echo "Examples:"
    echo "  $0                   # Check once, auto-detect PID"
    echo "  $0 1234              # Check process with PID 1234 once"
    echo "  $0 1234 10           # Check process with PID 1234 every 10 seconds once"
    echo "  $0 1234 10 5         # Check process with PID 1234 every 10 seconds, 5 times"
    echo "  $0 1234 10 continuous # Monitor continuously every 10 seconds"
    exit 0
fi

# Run main function with provided arguments
main "$@"