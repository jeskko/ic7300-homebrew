/* Host test for sstv_core.c: decode a 12 kHz mono 16-bit .au or .wav file to a PPM.
 *
 *     cc -O2 -Wall -o build/host_test host_test.c sstv_core.c
 *     build/host_test ../../../scratch/samples/SSTV.test.au out.ppm
 *
 * Other sample rates: resample to 12 kHz first (test_host.py does). Feeds the core in 9-sample
 * blocks, the size the app's audio hook delivers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sstv_core.h"

static uint8_t g_img[256][SSTV_MAX_W][3];
static int g_done;

static void on_line(void *ctx, const sstv_mode *m, int y, const uint16_t *px)
{
    (void)ctx;
    for (int x = 0; x < m->w; x++) {
        g_img[y][x][0] = (uint8_t)((px[x] >> 8) & 0xf8);
        g_img[y][x][1] = (uint8_t)((px[x] >> 3) & 0xfc);
        g_img[y][x][2] = (uint8_t)(px[x] << 3);
    }
}

static void on_event(void *ctx, int ev, const sstv_mode *m, int arg)
{
    const sstv_rx *rx = ctx;
    static const char *names[] = { "VIS", "UNKNOWN_VIS", "DONE", "LOST" };
    printf("t=%6.2f s  %s  %s  arg=%d\n", rx->n / 12000.0, names[ev], m ? m->name : "-", arg);
    if (ev == SSTV_EV_DONE || ev == SSTV_EV_LOST)
        g_done = 1;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s IN(.au|.wav, 12 kHz mono 16-bit) OUT.ppm\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)len);
    if (fread(d, 1, (size_t)len, f) != (size_t)len) return 1;
    fclose(f);

    long off, n;
    int be;
    if (!memcmp(d, ".snd", 4)) {
        off = be32(d + 4);
        n = (len - off) / 2;
        be = 1;
        if (be32(d + 12) != 3 || be32(d + 16) != 12000 || be32(d + 20) != 1) {
            fprintf(stderr, "need 16-bit mono 12 kHz\n");
            return 1;
        }
    } else if (!memcmp(d, "RIFF", 4)) {
        off = 12;
        while (off + 8 <= len && memcmp(d + off, "data", 4))
            off += 8 + le32(d + off + 4);
        n = le32(d + off + 4) / 2;
        off += 8;
        be = 0;
    } else {
        fprintf(stderr, "unknown format\n");
        return 1;
    }

    static sstv_rx rx;
    sstv_init(&rx, on_line, on_event, &rx);
    int16_t blk[9];
    for (long i = 0; i + 9 <= n && !g_done; i += 9) {
        for (int k = 0; k < 9; k++) {
            const uint8_t *p = d + off + 2 * (i + k);
            blk[k] = (int16_t)(be ? (p[0] << 8 | p[1]) : (p[1] << 8 | p[0]));
        }
        sstv_feed(&rx, blk, 9);
    }
    FILE *o = fopen(argv[2], "wb");
    fprintf(o, "P6\n%d %d\n255\n", SSTV_MAX_W, 256);
    fwrite(g_img, 1, sizeof g_img, o);
    fclose(o);
    printf("wrote %s, last sync quality %d%%\n", argv[2], rx.last_sync_quality);
    return g_done ? 0 : 3;
}
