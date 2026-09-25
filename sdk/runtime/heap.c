/* hb_malloc() and friends over HB_HEAP..HB_HEAP_END (see hb/heap.h).
 *
 * First fit over an address-ordered free list; free() merges with both neighbours, so the heap
 * never fragments into more free blocks than there are live allocations + 1. Each block carries
 * an 8-byte header (its size, including the header) and user pointers are 8-byte aligned.
 * The heap lives in this app's bss-zeroed state, so every launch starts with one empty heap.
 */
#include "hb/abi.h"
#include "hb/heap.h"

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

struct blk {
    uint32_t size;              /* bytes, header included, multiple of 8 */
    struct blk *next;           /* free blocks only: next free block, by address */
};

#define HDR         8u
#define MIN_BLOCK   16u

static struct blk *g_free;
static bool g_init;

static void init(void)
{
    g_free = (struct blk *)HB_HEAP;
    g_free->size = HB_HEAP_END - HB_HEAP;
    g_free->next = 0;
    g_init = true;
}

static bool in_heap(const void *p)
{
    return (uint32_t)p >= HB_HEAP + HDR && (uint32_t)p < HB_HEAP_END && ((uint32_t)p & 7) == 0;
}

void *hb_malloc(size_t n)
{
    if (!g_init)
        init();
    if (n == 0 || n > HB_HEAP_END - HB_HEAP)
        return 0;
    uint32_t need = (n + HDR + 7) & ~7u;
    for (struct blk **link = &g_free; *link; link = &(*link)->next) {
        struct blk *b = *link;
        if (b->size < need)
            continue;
        if (b->size - need >= MIN_BLOCK) {          /* split: the tail stays free */
            struct blk *rest = (struct blk *)((char *)b + need);
            rest->size = b->size - need;
            rest->next = b->next;
            *link = rest;
            b->size = need;
        } else {
            *link = b->next;
        }
        return (char *)b + HDR;
    }
    return 0;
}

void hb_free(void *p)
{
    if (!p || !g_init || !in_heap(p))
        return;
    struct blk *b = (struct blk *)((char *)p - HDR);
    struct blk *prev = 0, *next = g_free;
    while (next && next < b) {
        prev = next;
        next = next->next;
    }
    if (next == b)
        return;                                     /* already free */
    b->next = next;
    if (next && (char *)b + b->size == (char *)next) {
        b->size += next->size;
        b->next = next->next;
    }
    if (prev && (char *)prev + prev->size == (char *)b) {
        prev->size += b->size;
        prev->next = b->next;
    } else if (prev) {
        prev->next = b;
    } else {
        g_free = b;
    }
}

void *hb_calloc(size_t count, size_t size)
{
    if (size && count > (HB_HEAP_END - HB_HEAP) / size)
        return 0;
    void *p = hb_malloc(count * size);
    if (p)
        memset(p, 0, count * size);
    return p;
}

void *hb_realloc(void *p, size_t n)
{
    if (!p)
        return hb_malloc(n);
    if (!in_heap(p))
        return 0;
    if (n == 0) {
        hb_free(p);
        return 0;
    }
    uint32_t have = ((struct blk *)((char *)p - HDR))->size - HDR;
    if (n <= have)
        return p;
    void *q = hb_malloc(n);
    if (q) {
        memcpy(q, p, have);
        hb_free(p);
    }
    return q;
}

void hb_heap_stats(size_t *free_bytes, size_t *largest)
{
    if (!g_init)
        init();
    size_t total = 0, big = 0;
    for (struct blk *b = g_free; b; b = b->next) {
        total += b->size - HDR;
        if (b->size - HDR > big)
            big = b->size - HDR;
    }
    if (free_bytes)
        *free_bytes = total;
    if (largest)
        *largest = big;
}
