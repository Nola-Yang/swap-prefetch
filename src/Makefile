# CC = gcc
# CFLAGS = -g -Wall -O3 -fPIC -fno-omit-frame-pointer
# #CFLAGS = -g3 -Wall -O0 -fPIC
# LDFLAGS = -shared -fno-omit-frame-pointer
# INCLUDES = -I../linux/usr/include/
# LIBS = -lm -lpthread -luring -ldl -lsyscall_intercept

# default: libextmem-default.so

# all: libextmem-default.so libextmem-pagerank.so


# libextmem-default.so: extmem-default.o policies/default.o intercept_layer.o fifo.o storagemanager.o storage_iouring.o observability.o
# 	$(CC) $(LDFLAGS) -o libextmem-default.so extmem-default.o policies/default.o intercept_layer.o observability.o fifo.o storagemanager.o storage_iouring.o $(LIBS)

# libextmem-pagerank.so: extmem-pagerank.o policies/pagerank.o intercept_layer.o fifo.o storagemanager.o storage_iouring.o observability.o
# 	$(CC) $(LDFLAGS) -o libextmem-pagerank.so extmem-pagerank.o policies/pagerank.o intercept_layer.o observability.o fifo.o storagemanager.o storage_iouring.o $(LIBS)

# extmem-default.o: core.c core.h policies/disklinux.h intercept_layer.h observability.h fifo.h storagemanager.h
# 	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_DISK -D LRU_SWAP -c core.c -o extmem-default.o

# extmem-pagerank.o: core.c core.h policies/pageRank.h intercept_layer.h observability.h fifo.h storagemanager.h
# 	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_DISK -D PAGE_RANK -D LRU_SWAP -c core.c -o extmem-pagerank.o

# observability.o: observability.c observability.h core.h
# 	$(CC) $(CFLAGS) $(INCLUDES) -c observability.c

# intercept_layer.o: intercept_layer.c intercept_layer.h core.h
# 	$(CC) $(CFLAGS) $(INCLUDES) -c intercept_layer.c

# timer.o: timer.c timer.h
# 	$(CC) $(CFLAGS) $(INCLUDES) -c timer.c

# policies/default.o: policies/disklinux.c policies/disklinux.h observability.h core.h fifo.h storagemanager.h
# 	$(CC) $(CFLAGS) $(INCLUDES) -D LRU_SWAP -D ALLOC_DISK -c policies/disklinux.c -o policies/default.o

# policies/pagerank.o: policies/pageRank.c policies/pageRank.h observability.h core.h fifo.h storagemanager.h
# 	$(CC) $(CFLAGS) $(INCLUDES) -D LRU_SWAP -D ALLOC_DISK -D PAGE_RANK -c policies/pageRank.c -o policies/pagerank.o

# storage_iouring.o: storage_iouring.c storage_iouring.h core.h fifo.h
# 	$(CC) $(CFLAGS) $(INCLUDES) -c storage_iouring.c

# storagemanager.o: storagemanager.c storagemanager.h core.h
# 	$(CC) $(CFLAGS) $(INCLUDES) -c storagemanager.c

# fifo.o: fifo.c fifo.h core.h
# 	$(CC) $(CFLAGS) $(INCLUDES) -c fifo.c

# clean:
# 	$(RM) *.o *.so policies/*.o

# ExtMem Makefile with RDMA support and Pointer Prefetch
CC = gcc
CFLAGS = -g -Wall -O3 -fPIC -fno-omit-frame-pointer
LDFLAGS = -shared -fno-omit-frame-pointer
INCLUDES = -I../linux/usr/include/ -Iinclude/ -I.
LIBS = -lm -lpthread -luring -ldl -lsyscall_intercept -lrt

default: libextmem-rdma.so rdma_process

all: libextmem-rdma.so rdma_process

# Create include directory if it doesn't exist
$(shell mkdir -p include)

# RDMA-enabled ExtMem library with pointer prefetch
libextmem-rdma.so: extmem-rdma.o policies/default.o intercept_layer.o fifo.o storagemanager.o storage_rdma.o rdma_sim.o observability.o pointer_prefetch.o
	$(CC) $(LDFLAGS) -o libextmem-rdma.so extmem-rdma.o policies/default.o intercept_layer.o observability.o fifo.o storagemanager.o storage_rdma.o rdma_sim.o pointer_prefetch.o $(LIBS)

# RDMA simulation process
rdma_process.o: rdma_process.c include/rdma_sim.h
	$(CC) $(CFLAGS) $(INCLUDES) -c rdma_process.c -o rdma_process.o

rdma_process: rdma_process.o rdma_sim.o
	$(CC) -o rdma_process rdma_process.o rdma_sim.o -lpthread -lrt

# ExtMem with RDMA support
extmem-rdma.o: core.c core.h policies/disklinux.h intercept_layer.h observability.h fifo.h storagemanager.h include/rdma_sim.h include/storage_rdma.h include/pointer_prefetch.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_DISK -D LRU_SWAP -D USWAP_RDMA -c core.c -o extmem-rdma.o

# RDMA simulation components
rdma_sim.o: rdma_sim.c include/rdma_sim.h
	$(CC) $(CFLAGS) $(INCLUDES) -c rdma_sim.c

storage_rdma.o: storage_rdma.c include/storage_rdma.h include/rdma_sim.h
	$(CC) $(CFLAGS) $(INCLUDES) -c storage_rdma.c

# Pointer prefetch implementation
pointer_prefetch.o: pointer_prefetch.c include/pointer_prefetch.h core.h include/rdma_sim.h include/storage_rdma.h
	$(CC) $(CFLAGS) $(INCLUDES) -c pointer_prefetch.c

# Shared components
observability.o: observability.c observability.h core.h
	$(CC) $(CFLAGS) $(INCLUDES) -c observability.c

intercept_layer.o: intercept_layer.c intercept_layer.h core.h
	$(CC) $(CFLAGS) $(INCLUDES) -c intercept_layer.c

timer.o: timer.c timer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c timer.c

policies/default.o: policies/disklinux.c policies/disklinux.h observability.h core.h fifo.h storagemanager.h
	$(CC) $(CFLAGS) $(INCLUDES) -D LRU_SWAP -D ALLOC_DISK -c policies/disklinux.c -o policies/default.o

storagemanager.o: storagemanager.c storagemanager.h core.h
	$(CC) $(CFLAGS) $(INCLUDES) -c storagemanager.c

fifo.o: fifo.c fifo.h core.h
	$(CC) $(CFLAGS) $(INCLUDES) -c fifo.c

# Test program
test_pointer_prefetch: test_pointer_prefetch.c
	$(CC) -O2 -g test_pointer_prefetch.c -o test_pointer_prefetch

# Install headers to include directory
# install-headers: include/rdma_sim.h include/storage_rdma.h include/pointer_prefetch.h

# include/rdma_sim.h: rdma_sim.h
# 	cp rdma_sim.h include/

# include/storage_rdma.h: storage_rdma.h
# 	cp storage_rdma.h include/

# include/pointer_prefetch.h: pointer_prefetch.h
# 	cp pointer_prefetch.h include/

clean:
	$(RM) *.o *.so policies/*.o rdma_process test_pointer_prefetch
#	$(RM) -rf include/ # Commented out to preserve headers in src/include

.PHONY: default all clean install-headers