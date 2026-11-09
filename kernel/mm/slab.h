#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SLAB_MAGIC 0x51ABCAFE51ABCAFEULL

typedef struct kmem_cache kmem_cache_t;

typedef struct slab_page {
    uint64_t          magic;       /* SLAB_MAGIC */
    kmem_cache_t     *cache;       /* Owner cache */
    void             *freelist;    /* Head of free objects in this page */
    uint32_t          inuse;       /* Allocated objects count */
    uint32_t          total;       /* Total object capacity */
    struct slab_page *next;
    struct slab_page *prev;
} slab_page_t;

struct kmem_cache {
    const char   *name;
    uint32_t      obj_size;
    uint32_t      objs_per_slab;
    slab_page_t  *partial;
    slab_page_t  *full;
    slab_page_t  *empty;
};

/* Core SLAB interfaces */
void          slab_init(void);
kmem_cache_t *kmem_cache_create(const char *name, size_t size);
void         *kmem_cache_alloc(kmem_cache_t *cache);
void          kmem_cache_free(kmem_cache_t *cache, void *ptr);

/* Generic size-based slab allocation (32B to 2048B) */
void         *slab_alloc(size_t size);
bool          slab_free(void *ptr);
bool          is_slab_ptr(void *ptr);
