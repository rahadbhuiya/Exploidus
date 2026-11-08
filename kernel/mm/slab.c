#include "slab.h"
#include "pmm.h"
#include "../sync/sync.h"
#include "../drivers/serial.h"
#include <string.h>

#define NUM_GENERAL_CACHES 7
static const uint32_t g_slab_sizes[NUM_GENERAL_CACHES] = {
    32, 64, 128, 256, 512, 1024, 2048
};

static kmem_cache_t g_general_caches[NUM_GENERAL_CACHES];
static spinlock_t   g_slab_lock;
static bool         g_slab_initialized = false;

/*
 * List helpers
 */
static void list_add(slab_page_t **head, slab_page_t *page)
{
    page->next = *head;
    page->prev = NULL;
    if (*head) {
        (*head)->prev = page;
    }
    *head = page;
}

static void list_remove(slab_page_t **head, slab_page_t *page)
{
    if (page->prev) {
        page->prev->next = page->next;
    } else if (*head == page) {
        *head = page->next;
    }
    if (page->next) {
        page->next->prev = page->prev;
    }
    page->next = NULL;
    page->prev = NULL;
}

/*
 * Initialize a new physical page as a slab
 */
static slab_page_t *slab_page_create(kmem_cache_t *cache)
{
    uint64_t phys = pmm_alloc(ZONE_GREEN);
    if (!phys) return NULL;

    slab_page_t *sp = (slab_page_t *)(uintptr_t)phys;
    memset(sp, 0, sizeof(slab_page_t));
    sp->magic = SLAB_MAGIC;
    sp->cache = cache;

    /* Object offset starts right after the header, aligned to 16 bytes */
    uintptr_t data_start = (uintptr_t)phys + sizeof(slab_page_t);
    data_start = (data_start + 15) & ~15ULL;

    uint32_t usable_bytes = 4096 - (uint32_t)(data_start - (uintptr_t)phys);
    sp->total = usable_bytes / cache->obj_size;
    sp->inuse = 0;

    /* Build the freelist */
    uint8_t *curr = (uint8_t *)data_start;
    sp->freelist = curr;

    for (uint32_t i = 0; i < sp->total - 1; i++) {
        uint8_t *next = curr + cache->obj_size;
        *(void **)curr = (void *)next;
        curr = next;
    }
    *(void **)curr = NULL;

    return sp;
}

void slab_init(void)
{
    spin_init(&g_slab_lock);

    for (int i = 0; i < NUM_GENERAL_CACHES; i++) {
        g_general_caches[i].name = "slab-general";
        g_general_caches[i].obj_size = g_slab_sizes[i];
        g_general_caches[i].partial = NULL;
        g_general_caches[i].full = NULL;
        g_general_caches[i].empty = NULL;
    }

    g_slab_initialized = true;
    serial_print("[SLAB] Initialized 7 general object caches (32B-2048B)\n");
}

kmem_cache_t *kmem_cache_create(const char *name, size_t size)
{
    (void)name;
    /* Find existing matching size or return general cache for efficiency */
    for (int i = 0; i < NUM_GENERAL_CACHES; i++) {
        if (g_slab_sizes[i] >= size) {
            return &g_general_caches[i];
        }
    }
    return NULL;
}

void *kmem_cache_alloc(kmem_cache_t *cache)
{
    if (!cache) return NULL;

    uint64_t irq_flags = spin_lock_irqsave(&g_slab_lock);

    slab_page_t *sp = cache->partial;
    if (!sp) {
        if (cache->empty) {
            sp = cache->empty;
            list_remove(&cache->empty, sp);
            list_add(&cache->partial, sp);
        } else {
            sp = slab_page_create(cache);
            if (!sp) {
                spin_unlock_irqrestore(&g_slab_lock, irq_flags);
                return NULL;
            }
            list_add(&cache->partial, sp);
        }
    }

    void *obj = sp->freelist;
    sp->freelist = *(void **)obj;
    sp->inuse++;

    if (sp->inuse == sp->total) {
        list_remove(&cache->partial, sp);
        list_add(&cache->full, sp);
    }

    spin_unlock_irqrestore(&g_slab_lock, irq_flags);
    return obj;
}

void kmem_cache_free(kmem_cache_t *cache, void *ptr)
{
    if (!ptr) return;

    slab_page_t *sp = (slab_page_t *)((uintptr_t)ptr & ~0xFFFULL);
    if (sp->magic != SLAB_MAGIC) {
        return;
    }

    uint64_t irq_flags = spin_lock_irqsave(&g_slab_lock);

    *(void **)ptr = sp->freelist;
    sp->freelist = ptr;
    sp->inuse--;

    if (sp->inuse == sp->total - 1) {
        /* Was full, now partial */
        list_remove(&cache->full, sp);
        list_add(&cache->partial, sp);
    } else if (sp->inuse == 0) {
        /* Was partial, now completely empty */
        list_remove(&cache->partial, sp);
        list_add(&cache->empty, sp);
    }

    spin_unlock_irqrestore(&g_slab_lock, irq_flags);
}

void *slab_alloc(size_t size)
{
    if (!g_slab_initialized || size > 2048) {
        return NULL;
    }

    for (int i = 0; i < NUM_GENERAL_CACHES; i++) {
        if (g_slab_sizes[i] >= size) {
            return kmem_cache_alloc(&g_general_caches[i]);
        }
    }

    return NULL;
}

bool is_slab_ptr(void *ptr)
{
    if (!ptr || !g_slab_initialized) return false;
    slab_page_t *sp = (slab_page_t *)((uintptr_t)ptr & ~0xFFFULL);
    return (sp->magic == SLAB_MAGIC);
}

bool slab_free(void *ptr)
{
    if (!is_slab_ptr(ptr)) return false;
    slab_page_t *sp = (slab_page_t *)((uintptr_t)ptr & ~0xFFFULL);
    kmem_cache_free(sp->cache, ptr);
    return true;
}
