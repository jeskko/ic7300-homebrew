/* Dynamic memory for apps: a 448 KB heap (HB_HEAP..HB_HEAP_END, hb/abi.h).
 *
 * The usual malloc family, with hb_ names. Every launch starts with an empty heap, and nothing
 * needs freeing before main() returns. Pointers are 8-byte aligned. hb_malloc returns NULL when
 * no free block is big enough; hb_free ignores NULL and pointers it didn't hand out.
 *
 * Bigger data can also simply be a static array: the app image itself may be up to 1 MB.
 */
#ifndef HB_HEAP_H
#define HB_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *hb_malloc(size_t n);
void *hb_calloc(size_t count, size_t size);
void *hb_realloc(void *p, size_t n);
void hb_free(void *p);

/* Total free bytes, and the largest single allocation that would currently succeed. */
void hb_heap_stats(size_t *free_bytes, size_t *largest);

#endif
