#ifndef ADDRESS_TRANSLATION_H
#define ADDRESS_TRANSLATION_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

typedef struct address_region {
    uint64_t local_base;        
    uint64_t local_end;         
    uint64_t remote_base;      
    uint64_t size;             
    bool active;                
    struct address_region* next;
} address_region_t;


typedef struct {
    address_region_t* regions;
    pthread_mutex_t mutex;
    uint64_t local_heap_base;
    uint64_t local_heap_end;
    uint64_t remote_heap_base;
    uint64_t remote_heap_end;
    bool initialized;
} address_translator_t;

typedef struct {
    uint64_t converted_pointer;
    bool valid;
    address_region_t* region;
} pointer_conversion_result_t;

int address_translator_init(void);
void address_translator_cleanup(void);
int register_address_region(uint64_t local_base, uint64_t size, uint64_t remote_base);
int unregister_address_region(uint64_t local_base);
pointer_conversion_result_t convert_local_to_remote_pointer(uint64_t local_pointer);
pointer_conversion_result_t convert_remote_to_local_pointer(uint64_t remote_pointer);
bool is_valid_local_pointer(uint64_t pointer);
bool is_valid_remote_pointer(uint64_t pointer);
void print_address_regions(void);

#define POINTER_IS_NULL(ptr) ((ptr) == 0)
#define IS_LOCAL_HEAP_POINTER(ptr) (is_valid_local_pointer(ptr))
#define CONVERT_POINTER_FOR_REMOTE(ptr) (convert_local_to_remote_pointer(ptr))
#define CONVERT_POINTER_FROM_REMOTE(ptr) (convert_remote_to_local_pointer(ptr))

#endif // ADDRESS_TRANSLATION_H