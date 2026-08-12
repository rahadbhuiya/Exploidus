#define DEBUG_HEAP
#include "mm/kmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define HEAP_SIZE (1 * 1024 * 1024)  /* 1 MiB test heap */

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while (0)

static uint8_t *g_heap_mem;

static void fresh_heap(void)
{
    free(g_heap_mem);
    g_heap_mem = malloc(HEAP_SIZE + 4096);
    /* 16-byte align, matching real pmm page alignment */
    uintptr_t aligned = ((uintptr_t)g_heap_mem + 15) & ~(uintptr_t)15;
    kmalloc_init(aligned, HEAP_SIZE);
}

/* Test 1: basic alloc/free roundtrip */
static void test_basic(void)
{
    printf("[TEST] basic alloc/free\n");
    fresh_heap();
    void *p = kmalloc(64);
    CHECK(p != NULL, "kmalloc(64) should succeed on fresh heap");
    memset(p, 0xAA, 64);
    kfree(p);
    void *p2 = kmalloc(64);
    CHECK(p2 == p, "freed block should be reused (no leak/drift)");
    kfree(p2);
}

/* Test 2: the integer-overflow bug we just fixed */
static void test_overflow_guard(void)
{
    printf("[TEST] overflow guard (size near UINT64_MAX)\n");
    fresh_heap();
    void *p1 = kmalloc((uint64_t)-8);       /* would wrap ALIGN_UP to ~0 */
    CHECK(p1 == NULL, "huge size near UINT64_MAX must return NULL, not a tiny buffer");

    void *p2 = kmalloc((uint64_t)-1);
    CHECK(p2 == NULL, "SIZE_MAX must return NULL");

    void *p3 = kmalloc(HEAP_SIZE * 2);      /* > heap, but no wraparound */
    CHECK(p3 == NULL, "size larger than heap (no overflow) must also return NULL");

    void *p4 = kmalloc(64);                 /* heap should still work normally after */
    CHECK(p4 != NULL, "heap must remain usable after rejected oversized requests");
    kfree(p4);
}

/* Test 3: OOM behavior -- exhaust the heap, confirm clean NULL, no crash */
static void test_oom(void)
{
    printf("[TEST] OOM exhaustion\n");
    fresh_heap();
    void *ptrs[100000];
    int n = 0;
    while (n < 100000) {
        void *p = kmalloc(64);
        if (!p) break;
        ptrs[n++] = p;
    }
    CHECK(n > 0, "should have allocated at least something before OOM");
    void *fail = kmalloc(64);
    CHECK(fail == NULL, "allocating past OOM must return NULL, not crash/corrupt");
    printf("  (allocated %d blocks of 64B before OOM, heap=%dKB)\n", n, HEAP_SIZE / 1024);

    /* free everything back, heap should fully recover */
    for (int i = 0; i < n; i++) kfree(ptrs[i]);
    void *p = kmalloc(HEAP_SIZE - 64);  /* should get back ~the whole heap as one block */
    CHECK(p != NULL, "heap should fully recover (coalesce back to ~one big block) after freeing everything");
    if (p) kfree(p);
}

/* Test 4: double-free safety */
static void test_double_free(void)
{
    printf("[TEST] double-free safety\n");
    fresh_heap();
    void *p = kmalloc(64);
    CHECK(p != NULL, "alloc should succeed");
    kfree(p);
    kfree(p);  /* double free -- must not corrupt the heap */
    void *p2 = kmalloc(64);
    CHECK(p2 != NULL, "heap must still be usable after a double-free");
    kfree(p2);
}

/* Test 5: coalescing actually merges adjacent free blocks */
static void test_coalescing(void)
{
    printf("[TEST] coalescing\n");
    fresh_heap();
    void *a = kmalloc(256);
    void *b = kmalloc(256);
    void *c = kmalloc(256);
    CHECK(a && b && c, "three allocs should succeed");
    kfree(a);
    kfree(b);
    kfree(c);
    /* After freeing all three (and prior splits), a request close to
     * the full heap size should succeed if coalescing actually
     * merged everything back into one block. */
    void *big = kmalloc(HEAP_SIZE - 512);
    CHECK(big != NULL, "coalesced free blocks should satisfy a near-full-heap request");
    if (big) kfree(big);
}

/* Test 6: fragmentation -- known limitation of first-fit w/ adjacent-only
 * coalescing. This documents current behavior rather than asserting an
 * ideal outcome. */
static void test_fragmentation(void)
{
    printf("[TEST] fragmentation pattern (documents current first-fit limitation)\n");
    fresh_heap();
    void *ptrs[5000];
    int n = 0;
    /* Fill the ENTIRE heap with small blocks (must reach OOM, not
     * just allocate a handful -- otherwise a large leftover tail
     * block masks the fragmentation this test is trying to create). */
    while (n < 5000) {
        void *p = kmalloc(256);
        if (!p) break;
        ptrs[n++] = p;
    }
    CHECK(n > 100, "should have filled heap with many small blocks");

    /* Free every OTHER block -> checkerboard fragmentation. Total
     * free bytes is ~half the heap, but no single free block is
     * bigger than ~256B, since freed blocks are separated by
     * still-used ones and adjacent-only coalescing can't merge
     * across a used block. */
    for (int i = 0; i < n; i += 2) kfree(ptrs[i]);

    uint64_t total_free_approx = (uint64_t)(n / 2) * 256;
    void *big = kmalloc(4096); /* well within total free bytes, but NOT contiguous */
    printf("  (%d blocks allocated, ~%llu bytes free but scattered; "
           "requesting 4096B contiguous: %s)\n",
           n, (unsigned long long)total_free_approx,
           big ? "SUCCEEDED (unexpected -- allocator found a way)" :
                 "FAILED as expected -- first-fit can't bridge non-adjacent free blocks");
    CHECK(big == NULL,
        "documents known limitation: first-fit + adjacent-only coalescing "
        "cannot satisfy a request larger than the biggest single free run, "
        "even when total free memory is sufficient");
    if (big) kfree(big);

    /* clean up remaining odd blocks */
    for (int i = 1; i < n; i += 2) kfree(ptrs[i]);
}

/* Test 7: kfree() on garbage / out-of-heap / NULL pointers must not crash */
static void test_kfree_safety(void)
{
    printf("[TEST] kfree() misuse safety\n");
    fresh_heap();
    kfree(NULL);                          /* must be a no-op */
    int stack_var;
    kfree(&stack_var);                    /* out-of-heap pointer, must be ignored */
    kfree((void *)0x1);                   /* clearly bogus pointer */
    void *p = kmalloc(64);
    CHECK(p != NULL, "heap still usable after feeding kfree() garbage pointers");
    kfree(p);
}

int main(void)
{
    printf("=== kmalloc.c stress test (host build) ===\n\n");
    test_basic();
    test_overflow_guard();
    test_oom();
    test_double_free();
    test_coalescing();
    test_fragmentation();
    test_kfree_safety();

    printf("\n=== RESULTS: %d passed, %d failed ===\n", g_pass, g_fail);
    free(g_heap_mem);
    return g_fail ? 1 : 0;
}