#include "kmalloc.h"
#include <string.h>

#define ALIGN_UP(x, a)  (((x) + (a) - 1) & ~((a) - 1))
#define MIN_ALLOC       16
#define HEAP_MAGIC      0xE7F00D1DEADBEEFULL

typedef struct block_header {
    uint64_t magic;
    uint64_t size;
    uint8_t  free;
    uint8_t  _pad[7];
    struct block_header *next;
} block_header_t;

#define HEADER_SIZE sizeof(block_header_t)

static block_header_t *g_heap_head = NULL;
static uint64_t g_heap_start = 0;
static uint64_t g_heap_end   = 0;

/*  INIT  */

void kmalloc_init(uint64_t heap_start, uint64_t heap_size)
{
    g_heap_start = heap_start;
    g_heap_end   = heap_start + heap_size;

    g_heap_head = (block_header_t *)(uintptr_t)heap_start;

    g_heap_head->magic = HEAP_MAGIC;
    g_heap_head->size  = heap_size - HEADER_SIZE;
    g_heap_head->free  = 1;
    g_heap_head->next  = NULL;
}

/*  VALIDATION  */

static inline int in_heap(void *p)
{
    return ((uint64_t)p >= g_heap_start &&
            (uint64_t)p <  g_heap_end);
}

static inline int valid_hdr(block_header_t *h)
{
    return h &&
           in_heap(h) &&
           h->magic == HEAP_MAGIC &&
           h->size > 0;   /* FIX: prevents zero-size corruption loops */
}

/*  ALLOC  */

void *kmalloc(uint64_t size)
{
    if (!size || !g_heap_head)
        return NULL;

    size = ALIGN_UP(size, 16);
    if (size < MIN_ALLOC)
        size = MIN_ALLOC;

    block_header_t *cur = g_heap_head;

    while (cur) {

        if (!valid_hdr(cur))
            return NULL;

        if (cur->free && cur->size >= size) {

            uint64_t remaining = cur->size - size;

            /* FIX 1: strict split safety */
            if (remaining >= HEADER_SIZE + MIN_ALLOC) {

                block_header_t *split =
                    (block_header_t *)((uint8_t *)cur + HEADER_SIZE + size);

                if (!in_heap(split))
                    return NULL;

                split->magic = HEAP_MAGIC;
                split->size  = remaining - HEADER_SIZE;
                split->free  = 1;
                split->next  = cur->next;

                cur->next = split;
                cur->size = size;
            }

            cur->free = 0;

            /* FIX 2: return cleaned pointer */
            void *ret = (void *)((uint8_t *)cur + HEADER_SIZE);
            return ret;
        }

        cur = cur->next;
    }

    return NULL;
}

/*  CALLOC  */

void *kzalloc(uint64_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

/*  FREE  */

void kfree(void *ptr)
{
    if (!ptr)
        return;

    if (!in_heap(ptr))
        return;

    block_header_t *hdr =
        (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);

    if (!valid_hdr(hdr))
        return;

    if (hdr->free)
        return;

    hdr->free = 1;

    /* FIX 3: safer coalescing (no infinite restart loop) */
    block_header_t *cur = g_heap_head;

    while (cur && cur->next) {

        if (!valid_hdr(cur) || !valid_hdr(cur->next))
            break;

        if (cur->free && cur->next->free) {

            cur->size += HEADER_SIZE + cur->next->size;
            cur->next = cur->next->next;

            continue; /* keep scanning current */
        }

        cur = cur->next;
    }
}