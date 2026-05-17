#pragma once
#include <stdint.h>

/*  HEAP API  */
void  kmalloc_init(uint64_t heap_start, uint64_t heap_size);

void *kmalloc(uint64_t size);
void *kzalloc(uint64_t size);
void  kfree(void *ptr);

/*  OPTIONAL DEBUG HOOKS 
 * Useful for catching silent corruption early
 */
#ifdef DEBUG_HEAP
void kmalloc_dump(void);
uint64_t kmalloc_total_used(void);
#endif