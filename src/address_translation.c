#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#include "include/address_translation.h"

#define LOG(...) fprintf(stderr, __VA_ARGS__)

static address_translator_t global_translator = {0};

int address_translator_init(void) {
    if (global_translator.initialized) {
        return 0;
    }
    
    pthread_mutex_init(&global_translator.mutex, NULL);
    global_translator.regions = NULL;
    global_translator.initialized = true;
    
    // local heap
    global_translator.local_heap_base = 0x7f0000000000ULL;
    global_translator.local_heap_end = 0x7fffffffffffffffULL;
    
    // remote heap
    global_translator.remote_heap_base = 0x2000000000ULL;  
    global_translator.remote_heap_end = 0x3fffffffffULL;
    
    LOG("Address Translator initialized:\n");
    LOG("  Local heap:  0x%lx - 0x%lx\n", 
        global_translator.local_heap_base, global_translator.local_heap_end);
    LOG("  Remote heap: 0x%lx - 0x%lx\n", 
        global_translator.remote_heap_base, global_translator.remote_heap_end);
    
    return 0;
}

void address_translator_cleanup(void) {
    if (!global_translator.initialized) {
        return;
    }
    
    pthread_mutex_lock(&global_translator.mutex);
    
    address_region_t* current = global_translator.regions;
    while (current != NULL) {
        address_region_t* next = current->next;
        free(current);
        current = next;
    }
    
    global_translator.regions = NULL;
    global_translator.initialized = false;
    
    pthread_mutex_unlock(&global_translator.mutex);
    pthread_mutex_destroy(&global_translator.mutex);
    
    LOG("Address Translator cleaned up\n");
}

int register_address_region(uint64_t local_base, uint64_t size, uint64_t remote_base) {
    if (!global_translator.initialized) {
        return -ENOTCONN;
    }
    
    pthread_mutex_lock(&global_translator.mutex);
    
    address_region_t* current = global_translator.regions;
    while (current != NULL) {
        if (current->local_base == local_base) {
            
            current->local_end = local_base + size;
            current->remote_base = remote_base;
            current->size = size;
            current->active = true;
            pthread_mutex_unlock(&global_translator.mutex);
            LOG("Updated address region: local 0x%lx -> remote 0x%lx (size: 0x%lx)\n",
                local_base, remote_base, size);
            return 0;
        }
        current = current->next;
    }
    
    address_region_t* new_region = malloc(sizeof(address_region_t));
    if (new_region == NULL) {
        pthread_mutex_unlock(&global_translator.mutex);
        return -ENOMEM;
    }
    
    new_region->local_base = local_base;
    new_region->local_end = local_base + size;
    new_region->remote_base = remote_base;
    new_region->size = size;
    new_region->active = true;
    new_region->next = global_translator.regions;
    global_translator.regions = new_region;
    
    pthread_mutex_unlock(&global_translator.mutex);
    
    LOG("Registered address region: local 0x%lx -> remote 0x%lx (size: 0x%lx)\n",
        local_base, remote_base, size);
    
    return 0;
}

int unregister_address_region(uint64_t local_base) {
    if (!global_translator.initialized) {
        return -ENOTCONN;
    }
    
    pthread_mutex_lock(&global_translator.mutex);
    
    address_region_t* current = global_translator.regions;
    address_region_t* prev = NULL;
    
    while (current != NULL) {
        if (current->local_base == local_base) {
            if (prev == NULL) {
                global_translator.regions = current->next;
            } else {
                prev->next = current->next;
            }
            free(current);
            pthread_mutex_unlock(&global_translator.mutex);
            LOG("Unregistered address region: local 0x%lx\n", local_base);
            return 0;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&global_translator.mutex);
    return -ENOENT;
}

pointer_conversion_result_t convert_local_to_remote_pointer(uint64_t local_pointer) {
    pointer_conversion_result_t result = {0, false, NULL};
    
    if (local_pointer == 0) {
        result.converted_pointer = 0;
        result.valid = true;
        return result;
    }
    
    if (!global_translator.initialized) {
        return result;
    }
    
    pthread_mutex_lock(&global_translator.mutex);
    
    address_region_t* current = global_translator.regions;
    while (current != NULL) {
        if (current->active && 
            local_pointer >= current->local_base && 
            local_pointer < current->local_end) {
            
            uint64_t offset = local_pointer - current->local_base;
            result.converted_pointer = current->remote_base + offset;
            result.valid = true;
            result.region = current;
            
            LOG("Converted pointer: 0x%lx -> 0x%lx (offset: 0x%lx)\n",
                local_pointer, result.converted_pointer, offset);
            break;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&global_translator.mutex);
    
    if (!result.valid) {
        LOG("Failed to convert local pointer 0x%lx: no matching region\n", local_pointer);
    }
    
    return result;
}

pointer_conversion_result_t convert_remote_to_local_pointer(uint64_t remote_pointer) {
    pointer_conversion_result_t result = {0, false, NULL};
    
    if (remote_pointer == 0) {
        result.converted_pointer = 0;
        result.valid = true;
        return result;
    }
    
    if (!global_translator.initialized) {
        return result;
    }
    
    pthread_mutex_lock(&global_translator.mutex);
    
    address_region_t* current = global_translator.regions;
    while (current != NULL) {
        if (current->active && 
            remote_pointer >= current->remote_base && 
            remote_pointer < (current->remote_base + current->size)) {
            
            uint64_t offset = remote_pointer - current->remote_base;
            result.converted_pointer = current->local_base + offset;
            result.valid = true;
            result.region = current;
            
            LOG("Converted remote pointer: 0x%lx -> 0x%lx (offset: 0x%lx)\n",
                remote_pointer, result.converted_pointer, offset);
            break;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&global_translator.mutex);
    
    if (!result.valid) {
        LOG("Failed to convert remote pointer 0x%lx: no matching region\n", remote_pointer);
    }
    
    return result;
}

bool is_valid_local_pointer(uint64_t pointer) {
    if (pointer == 0) return true;
    if (!global_translator.initialized) return false;
    
    return (pointer >= global_translator.local_heap_base && 
            pointer < global_translator.local_heap_end);
}

bool is_valid_remote_pointer(uint64_t pointer) {
    if (pointer == 0) return true;
    if (!global_translator.initialized) return false;
    
    return (pointer >= global_translator.remote_heap_base && 
            pointer < global_translator.remote_heap_end);
}

void print_address_regions(void) {
    if (!global_translator.initialized) {
        LOG("Address Translator not initialized\n");
        return;
    }
    
    pthread_mutex_lock(&global_translator.mutex);
    
    LOG("Address Translation Regions:\n");
    address_region_t* current = global_translator.regions;
    int count = 0;
    
    while (current != NULL) {
        LOG("  Region %d: Local 0x%lx-0x%lx -> Remote 0x%lx-0x%lx (size: 0x%lx, active: %s)\n",
            count++, current->local_base, current->local_end,
            current->remote_base, current->remote_base + current->size,
            current->size, current->active ? "yes" : "no");
        current = current->next;
    }
    
    if (count == 0) {
        LOG("  No regions registered\n");
    }
    
    pthread_mutex_unlock(&global_translator.mutex);
}