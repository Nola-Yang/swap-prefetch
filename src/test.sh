#!/bin/bash

# Configuration
SWAP_DIR="/tmp/extmem_swap"
DRAM_SIZE="2G"  # 2GB DRAM limit
TEST_TIMEOUT=30  # Test timeout in seconds

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Cleanup function
cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    
    # Kill any running processes
    if [ -n "$TEST_PID" ] && ps -p $TEST_PID > /dev/null 2>&1; then
        echo "Terminating test program (PID: $TEST_PID)"
        kill -TERM $TEST_PID 2>/dev/null || true
    fi
    
    if [ -n "$SWAP_PID" ] && ps -p $SWAP_PID > /dev/null 2>&1; then
        echo "Terminating swap process (PID: $SWAP_PID)"
        kill -TERM $SWAP_PID 2>/dev/null || true
        sleep 1
        # Force kill if still running
        if ps -p $SWAP_PID > /dev/null 2>&1; then
            kill -KILL $SWAP_PID 2>/dev/null || true
        fi
    fi
    
    echo -e "${GREEN}Cleanup complete.${NC}"
}

# Set trap for cleanup
trap cleanup EXIT INT TERM

# Create swap directory
echo -e "${BLUE}Creating swap directory: $SWAP_DIR${NC}"
mkdir -p "$SWAP_DIR"

# Compile everything
echo -e "${BLUE}Building ExtMem and swap process...${NC}"
make clean
make swap_process
make libextmem-default.so

# Verify the binaries exist
if [ ! -f "./swap_process" ]; then
    echo -e "${RED}ERROR: swap_process executable not found!${NC}"
    exit 1
fi

if [ ! -f "./libextmem-default.so" ]; then
    echo -e "${RED}ERROR: libextmem-default.so not found!${NC}"
    exit 1
fi

# Ensure executables are executable
chmod +x ./swap_process

# Compile test program
echo -e "${BLUE}Compiling test program...${NC}"
gcc -O2 test_program.c -o test_program -lrt
if [ ! -f "./test_program" ]; then
    echo -e "${RED}ERROR: Failed to compile test program!${NC}"
    exit 1
fi

# Export environment variables
export DRAMSIZE=$(numfmt --from=iec $DRAM_SIZE)
export SWAPDIR="$SWAP_DIR"

echo -e "${GREEN}Using DRAMSIZE=$DRAMSIZE ($DRAM_SIZE)${NC}"
echo -e "${GREEN}Using SWAPDIR=$SWAP_DIR${NC}"

# Start swap process manually
echo -e "${BLUE}Starting swap process...${NC}"
./swap_process &
SWAP_PID=$!

# Wait for swap process to initialize
echo -e "${BLUE}Waiting for swap process to initialize...${NC}"
sleep 2

# Verify swap process is running
if ! ps -p $SWAP_PID > /dev/null; then
    echo -e "${RED}ERROR: Swap process failed to start!${NC}"
    exit 1
fi

echo -e "${GREEN}Swap process running with PID: $SWAP_PID${NC}"

# Display information about the library
echo -e "${BLUE}Library information:${NC}"
ls -la ./libextmem-default.so
file ./libextmem-default.so

# Run test program with ExtMem
echo -e "${BLUE}Running test program with ExtMem...${NC}"
LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH LD_PRELOAD=$PWD/libextmem-default.so ./swap_test 2 0 1 50 50 &
TEST_PID=$!

# Wait for test to complete or timeout
echo -e "${BLUE}Test running, waiting up to $TEST_TIMEOUT seconds...${NC}"
waited=0
while ps -p $TEST_PID > /dev/null && [ $waited -lt $TEST_TIMEOUT ]; do
    echo -e "${YELLOW}Test running for $waited seconds...${NC}"
    sleep 5
    waited=$((waited + 5))
    
    # Check swap process is still running
    if ! ps -p $SWAP_PID > /dev/null; then
        echo -e "${RED}ERROR: Swap process died during test!${NC}"
        # Attempt to capture logs or other diagnostics
        if [ -f "/tmp/extmem_swap.bin" ]; then
            ls -la /tmp/extmem_swap.bin
        fi
        exit 1
    fi
done

# Check if test timed out
if ps -p $TEST_PID > /dev/null; then
    echo -e "${YELLOW}Test reached timeout, terminating...${NC}"
    kill -TERM $TEST_PID
    wait $TEST_PID 2>/dev/null || true
else
    echo -e "${GREEN}Test completed within time limit!${NC}"
    # Check exit status
    wait $TEST_PID
    TEST_STATUS=$?
    if [ $TEST_STATUS -eq 0 ]; then
        echo -e "${GREEN}Test program exited successfully with status 0${NC}"
    else
        echo -e "${RED}Test program exited with status $TEST_STATUS${NC}"
    fi
fi

# Display swap file information
echo -e "${BLUE}Swap file information:${NC}"
ls -la "$SWAP_DIR"

# Display swap process information
echo -e "${BLUE}Swap process information:${NC}"
ps -p $SWAP_PID -o pid,ppid,cmd,%cpu,%mem,rss,vsz

echo -e "\n${GREEN}Test completed!${NC}"