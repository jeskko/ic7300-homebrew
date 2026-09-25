/* Loader test app (sdk/loader/test_emu.py --big): a ~770 KB image and a heap workout.
 *
 * The image holds a 768 KB constant blob with markers at both ends, so a load that stopped short
 * (or read one file RPC's worth only) is caught. Then the heap: fill and verify four 80 KB
 * blocks, free the middle two and check they coalesce, check an oversize request fails, and
 * that everything comes back once all is freed. Shows "BIG OK" or "BIG FAIL <step>".
 */
#include "hb/abi.h"
#include "hb/app.h"
#include "hb/heap.h"

#define BLOB_SIZE   (768 * 1024)

const uint8_t g_blob[BLOB_SIZE] = {
    0x42, 0x49, 0x47,                   /* "BIG" */
    [BLOB_SIZE / 2] = 0x5a,
    [BLOB_SIZE - 1] = 0xa5,
};
int g_fail_step;                        /* 0 = passed; read by the test */

static bool in_heap(const void *p, size_t n)
{
    return (uint32_t)p >= HB_HEAP && (uint32_t)p + n <= HB_HEAP_END;
}

static int run(void)
{
    /* 1-2: the whole image arrived. */
    const volatile uint8_t *b = g_blob;
    if (b[0] != 0x42 || b[1] != 0x49 || b[2] != 0x47 || b[BLOB_SIZE / 2] != 0x5a ||
        b[BLOB_SIZE - 1] != 0xa5)
        return 1;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < BLOB_SIZE; i++)
        sum += b[i];
    if (sum != 0x42 + 0x49 + 0x47 + 0x5a + 0xa5)
        return 2;

    /* 3: fresh heap is (almost) all free. */
    size_t free0, big0;
    hb_heap_stats(&free0, &big0);
    if (free0 != big0 || free0 < HB_HEAP_END - HB_HEAP - 16)
        return 3;

    /* 4-5: four 80 KB blocks, inside the heap, non-overlapping, each holding its pattern. */
    enum { N = 4, SZ = 80 * 1024 };
    uint8_t *blk[N];
    for (int i = 0; i < N; i++) {
        blk[i] = hb_malloc(SZ);
        if (!blk[i] || !in_heap(blk[i], SZ) || ((uint32_t)blk[i] & 7))
            return 4;
        for (int j = 0; j < SZ; j++)
            blk[i][j] = (uint8_t)(i * 37 + j);
    }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < SZ; j++)
            if (blk[i][j] != (uint8_t)(i * 37 + j))
                return 5;

    /* 6: an oversize request fails cleanly. */
    if (hb_malloc(HB_HEAP_END - HB_HEAP) != 0)
        return 6;

    /* 7: freeing the middle two coalesces them; a 160 KB block fits exactly there. */
    hb_free(blk[1]);
    hb_free(blk[2]);
    uint8_t *mid = hb_malloc(2 * SZ);
    if (mid != blk[1])
        return 7;

    /* 8: realloc grows by moving and keeps the contents. */
    uint8_t *r = hb_realloc(blk[3], SZ + 4096);
    if (!r || r[SZ - 1] != (uint8_t)(3 * 37 + SZ - 1) || r[0] != (uint8_t)(3 * 37))
        return 8;

    /* 9: once everything is freed, the heap is one block again. */
    hb_free(mid);
    hb_free(r);
    hb_free(blk[0]);
    size_t free1, big1;
    hb_heap_stats(&free1, &big1);
    if (free1 != free0 || big1 != big0)
        return 9;
    return 0;
}

int main(void)
{
    g_fail_step = run();
    if (g_fail_step == 0) {
        ui_message_box("BIG OK");
    } else {
        char msg[] = "BIG FAIL 0";
        msg[9] = (char)('0' + g_fail_step);
        ui_message_box(msg);
    }
    return 0;
}
