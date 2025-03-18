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

CC = gcc
CFLAGS = -g -Wall -O3 -fPIC -fno-omit-frame-pointer
#CFLAGS = -g3 -Wall -O0 -fPIC
LDFLAGS = -shared -fno-omit-frame-pointer
INCLUDES = -I../linux/usr/include/ -Iinclude/
LIBS = -lm -lpthread -luring -ldl -lsyscall_intercept -lrt

default: libextmem-default.so swap_process

all: libextmem-default.so libextmem-pagerank.so swap_process


libextmem-default.so: extmem-default.o policies/default.o intercept_layer.o fifo.o storagemanager.o storage_iouring.o observability.o swap_shm.o storage_swap_process.o
	$(CC) $(LDFLAGS) -o libextmem-default.so extmem-default.o policies/default.o intercept_layer.o observability.o fifo.o storagemanager.o storage_iouring.o swap_shm.o storage_swap_process.o $(LIBS)

libextmem-pagerank.so: extmem-pagerank.o policies/pagerank.o intercept_layer.o fifo.o storagemanager.o storage_iouring.o observability.o swap_shm.o storage_swap_process.o
	$(CC) $(LDFLAGS) -o libextmem-pagerank.so extmem-pagerank.o policies/pagerank.o intercept_layer.o observability.o fifo.o storagemanager.o storage_iouring.o swap_shm.o storage_swap_process.o $(LIBS)

swap_process: swap_process.o swap_shm.o
	$(CC) -o swap_process swap_process.o swap_shm.o -lpthread -lrt

extmem-default.o: core.c core.h policies/disklinux.h intercept_layer.h observability.h fifo.h storagemanager.h include/swap_shm.h include/storage_swap_process.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_DISK -D LRU_SWAP -D USWAP_SWAP_PROCESS -c core.c -o extmem-default.o

extmem-pagerank.o: core.c core.h policies/pageRank.h intercept_layer.h observability.h fifo.h storagemanager.h include/swap_shm.h include/storage_swap_process.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_DISK -D PAGE_RANK -D LRU_SWAP -D USWAP_SWAP_PROCESS -c core.c -o extmem-pagerank.o

swap_process.o: swap_process.c include/swap_shm.h
	$(CC) $(CFLAGS) $(INCLUDES) -c swap_process.c

swap_shm.o: swap_shm.c include/swap_shm.h
	$(CC) $(CFLAGS) $(INCLUDES) -c swap_shm.c

storage_swap_process.o: storage_swap_process.c include/storage_swap_process.h include/swap_shm.h
	$(CC) $(CFLAGS) $(INCLUDES) -c storage_swap_process.c

observability.o: observability.c observability.h core.h
	$(CC) $(CFLAGS) $(INCLUDES) -c observability.c

intercept_layer.o: intercept_layer.c intercept_layer.h core.h
	$(CC) $(CFLAGS) $(INCLUDES) -c intercept_layer.c

timer.o: timer.c timer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c timer.c

policies/default.o: policies/disklinux.c policies/disklinux.h observability.h core.h fifo.h storagemanager.h
	$(CC) $(CFLAGS) $(INCLUDES) -D LRU_SWAP -D ALLOC_DISK -c policies/disklinux.c -o policies/default.o

policies/pagerank.o: policies/pageRank.c policies/pageRank.h observability.h core.h fifo.h storagemanager.h
	$(CC) $(CFLAGS) $(INCLUDES) -D LRU_SWAP -D ALLOC_DISK -D PAGE_RANK -c policies/pageRank.c -o policies/pagerank.o

storage_iouring.o: storage_iouring.c storage_iouring.h core.h fifo.h
	$(CC) $(CFLAGS) $(INCLUDES) -c storage_iouring.c

storagemanager.o: storagemanager.c storagemanager.h core.h
	$(CC) $(CFLAGS) $(INCLUDES) -c storagemanager.c

fifo.o: fifo.c fifo.h core.h
	$(CC) $(CFLAGS) $(INCLUDES) -c fifo.c

checkpoint_test: checkpoint_test.o
	$(CC) -o checkpoint_test checkpoint_test.o -lextmem-default -L. -lpthread -lrt

checkpoint_test.o: checkpoint_test.c include/storage_swap_process.h include/swap_shm.h
	$(CC) $(CFLAGS) $(INCLUDES) -c checkpoint_test.c

clean:
	$(RM) *.o *.so policies/*.o swap_process checkpoint_test
