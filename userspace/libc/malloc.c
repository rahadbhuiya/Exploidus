/*
 * Exploidus minimal malloc
 * Simple free-list allocator using mmap for large regions.
 */
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

#define BLOCK_MAGIC 0xA110CA7E
#define ALIGN       16
#define MIN_BLOCK   32

typedef struct block {
    uint32_t magic;
    uint32_t free;
    size_t   size;      /* usable bytes after header */
    struct block *next; /* next in free list */
} block_t;

static block_t *g_free_list = (block_t *)0;

/* Align size up to ALIGN bytes */
static size_t align_up(size_t n)
{
    return (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
}

/* Get a new region from kernel via mmap */
static block_t *new_region(size_t need)
{
    size_t total = sizeof(block_t) + need;
    /* Round up to 4096 */
    total = (total + 4095) & ~(size_t)4095;
    void *p = (void *)(uintptr_t)mmap(total);
    if (!p) return (block_t *)0;
    block_t *b = (block_t *)p;
    b->magic = BLOCK_MAGIC;
    b->free  = 1;
    b->size  = total - sizeof(block_t);
    b->next  = (block_t *)0;
    return b;
}

void *malloc(size_t n)
{
    if (!n) return (void *)0;
    n = align_up(n < MIN_BLOCK ? MIN_BLOCK : n);

    /* Search free list */
    block_t **pp = &g_free_list;
    while (*pp) {
        block_t *b = *pp;
        if (b->free && b->size >= n) {
            /* Split if large enough */
            if (b->size >= n + sizeof(block_t) + MIN_BLOCK) {
                block_t *rest = (block_t *)((uint8_t *)(b + 1) + n);
                rest->magic = BLOCK_MAGIC;
                rest->free  = 1;
                rest->size  = b->size - n - sizeof(block_t);
                rest->next  = b->next;
                b->next = rest;
                b->size = n;
            }
            b->free = 0;
            *pp = b->next;
            return (void *)(b + 1);
        }
        pp = &b->next;
    }

    /* Nothing in free list — get new region */
    block_t *b = new_region(n);
    if (!b) return (void *)0;
    b->free = 0;
    return (void *)(b + 1);
}

void free(void *ptr)
{
    if (!ptr) return;
    block_t *b = (block_t *)ptr - 1;
    if (b->magic != BLOCK_MAGIC) return; /* corrupt */
    b->free = 1;
    /* Prepend to free list */
    b->next = g_free_list;
    g_free_list = b;
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t n)
{
    if (!ptr) return malloc(n);
    if (!n)   { free(ptr); return (void *)0; }
    block_t *b = (block_t *)ptr - 1;
    if (b->magic != BLOCK_MAGIC) return (void *)0;
    if (b->size >= n) return ptr; /* already big enough */
    void *new = malloc(n);
    if (!new) return (void *)0;
    memcpy(new, ptr, b->size);
    free(ptr);
    return new;
}