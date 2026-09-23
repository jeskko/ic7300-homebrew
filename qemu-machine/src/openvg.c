/*
 * RZ/A1H "Renesas Graphics Processor for OpenVG(TM)" (0xE8100000, bus-matrix
 * slave SLV5) -- minimal completion-interrupt stub, in the same permissive
 * spirit as rspi2.c / scif.c / riic.c.
 *
 * 2026-09-21 (icom-main-idle-loop-not-reached thread). Why this exists, and
 * how the need was established, end to end:
 *
 *   cold_boot_mode_dispatch (body.bin 0x2002b1c8) runs cold_boot_hw_init once
 *   -- which a GDB breakpoint walk (tools/trace_cold_boot_hw_init_walk.py)
 *   confirmed now completes and RETURNS -- then loops
 *     while (system_mode_request_dispatch(), !shutdown)
 *         ... main_idle_loop() ...
 *   A second walk (tools/walk_call_sites.py) showed system_mode_request_dispatch
 *   (0x2002a6b8) itself never returns: it stalls in FUN_200375e4 (0x200375e4),
 *     while (*0x2039064c != 0) rtos_wait_obj1_forever();
 *   the consumer half of a handshake its immediate predecessor call,
 *   FUN_200378cc(0) (0x200378cc), just posted:
 *     *0x2039064c = 1; FUN_2007edc8();   // wake ui_graphics_lifecycle_task
 *   Only ui_graphics_lifecycle_task's own main loop (0x2007f03c / 0x2007f058)
 *   ever writes 0x2039064c back to 0 -- and that task never reaches its main
 *   loop: it is itself stuck, four levels down, in
 *     ui_graphics_lifecycle_task (0x2007ef5c)
 *       -> graphics_stack_startup_egl_openvg (0x20079240), the "vgStartUp" step
 *         -> slv5_periph_connect_disconnect_handler (0x201009d2)
 *           -> slv5_periph_configure (0x2014ee46)
 *             -> FUN_20153ec2(ctx+0x10, 1, 0)  ==  wait-event-flag, pattern 1,
 *                                                  timeout TMO_FEVR
 *   (each link confirmed live by its own breakpoint walk, not inferred).
 *
 *   That event flag is set from exactly one place: FUN_2014e9f8 (0x2014e9f9,
 *   Thumb), the graphics processor's interrupt handler. slv5_periph_configure
 *   registers it via FUN_2007ec74 on GIC interrupt IDs 0x82-0x85 with priority
 *   0x30 and unmasks all four. The RZ/A1H hardware manual's Table 7.3 (List of
 *   Interrupt IDs) confirms IDs 130-133 are exactly "OpenVG(TM) graphics
 *   processor" INT0/INT1/INT2/INT3, level-triggered -- an exact match, read
 *   from the manual, not derived.
 *
 *   The ISR reads the interrupt-status register at base+0x4018, masks it with
 *   the software-held enable mask (ctx+0x24, set to 0x74f right before the
 *   wait and also written to base+0x4014), acknowledges by writing the status
 *   back to base+0x401c, and then routes bits to three event flags. Status
 *   bit 0 is the one that reaches the flag the stalled wait is parked on:
 *     uVar8 = (status & 1) | current_flag;
 *     if (uVar8 != current_flag) (*(ctx+0xc4))(uVar8);   // set_flg
 *   so a single base+0x4018 bit-0 assertion, delivered on GIC 130, is all the
 *   hardware fidelity this boot path actually needs.
 *
 *   Before this device existed, 0xE8100000-0xE813FFFF was covered only by
 *   rz_a1h.c's plain-RAM "io-e8100000" catch-all: base+0x4018 always read 0,
 *   no interrupt source existed at all, so the wait could never be satisfied
 *   and the ENTIRE boot deadlocked one call short of main_idle_loop.
 *
 * Register map (offsets from 0xE8104000; derived from the driver's own use,
 * NOT from the manual -- Renesas' section 44 is a one-page feature summary
 * with no register descriptions at all, the IP being third-party):
 *   +0x00  command FIFO write port (FUN_20150122 pushes a command list here)
 *   +0x04  written 0 during bring-up (reset/control)
 *   +0x10  read once into the driver context, never validated (ID/version)
 *   +0x14  interrupt enable        (written 0x74f)
 *   +0x18  interrupt status        (read by the ISR)
 *   +0x1c  interrupt clear         (ISR writes the status back to ack)
 *   +0x24  read-modify-write, bit 31 set during command submission
 * Two further registers at base+0x20064 / base+0x20068 (FIFO watermark
 * thresholds, written 0x1ff / 0xfb) are left to the plain-RAM catch-all --
 * the driver never reads them back.
 *
 * UPDATE 2026-09-23: it now renders -- see "Rendering" and "Path tessellator"
 * below (fills, blits, affine image draws, tessellated path fills incl. vector
 * text). The paragraph below describes the original completion-only contract,
 * which still holds for the interrupt side.
 *
 * What was modeled originally: nothing renders. A write to the command FIFO latches the
 * "operation complete" status bit and, if enabled, raises INT0. The driver's
 * own acknowledge write clears it. That is deliberately the entire contract:
 * this unblocks the bring-up handshake without pretending to be a graphics
 * accelerator. Anything downstream that needs real rendering results (actual
 * EGL surface content, the 480x272 window / 960x552 pixmap blits) will still
 * behave as if the GPU produced nothing -- a known, accepted limit, same
 * philosophy as scif.c's virtual DSP responder.
 *
 * Also deliberately NOT modeled: the FIFO-full / FIFO-below-watermark
 * hysteresis (status bits 0x40 and 0x30). The driver only consults those once
 * its own software free-slot counter (0x20390acc, initialized to 0x1ff) runs
 * below 7, which the short bring-up command lists never approach. Asserting
 * 0x40 would actively mislead the driver into believing the FIFO is full, so
 * this model never sets it.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include <math.h>

#include "rz_a1h.h"
#include "rza1h_debug.h"

OBJECT_DECLARE_SIMPLE_TYPE(RZA1HOpenVGState, RZA1H_OPENVG)

#define VG_NUM_REGS (0x9000 / 4)
#define VG_TESS_SIZE 0x200         /* 0xE8102000..0xE81021FF */
#define VG_COV_SLOTS 4
#define VG_MAX_EDGES 4096

typedef struct VGEdge {
    float x0, y0, x1, y1;
} VGEdge;

/* One tessellated path, keyed by the coverage-buffer address the list set in
 * 0x2078 (the driver ping-pongs two). Real hardware writes its coverage into
 * that buffer; nothing but our own cover draw reads it, so the edges are kept
 * host-side instead. */
typedef struct VGCoverage {
    uint32_t buf;
    int n;
    VGEdge e[VG_MAX_EDGES];
} VGCoverage;   /* register file incl. the 0x8000 program store */

struct RZA1HOpenVGState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion tess_iomem;   /* path tessellator / command-list engine */
    uint32_t tess_reg[VG_TESS_SIZE / 4];
    uint64_t tess_kicks;
    uint32_t tl_reg[0x4000 / 4];    /* registers written by command lists */
    VGCoverage cov[VG_COV_SLOTS];
    uint32_t d8_ops[2];             /* operand words of the pending 0xD8 */
    int d8_left;
    qemu_irq int0;
    uint32_t intsts;
    uint32_t inten;
    uint64_t commands;
    uint8_t regs[RZA1H_OPENVG_SIZE];
    /* Command-stream decoder (see "Rendering" below). */
    uint32_t gpu_reg[VG_NUM_REGS];
    uint32_t pkt_reg;       /* next register a packet data word goes to */
    uint32_t pkt_left;      /* data words still expected for the current packet */
    bool dst_fmt_in_208;    /* last 0x010 packet omitted the format word */
    bool img_xform;         /* 0x80c [tx ty 1.0] written since the last 0xD8 */
    uint64_t ops;
};

#define OPENVG_CMD     0x00
#define OPENVG_CTRL    0x04
#define OPENVG_ID      0x10
#define OPENVG_INTEN   0x14
#define OPENVG_INTSTS  0x18
#define OPENVG_INTCLR  0x1c

/* The two "operation complete" status sources this bring-up path actually
 * waits on, both read straight off the ISR's own bit routing (FUN_2014e9f8):
 *
 *   bit 0  -> (status & 1)     -> event flag ctx+0x10, via the callback at
 *                                 ctx+0xc4. slv5_periph_configure's first
 *                                 wait, FUN_20153ec2(ctx+0x10, 1, 0), is
 *                                 parked on exactly this bit.
 *   bit 1  -> (status & 0x60a) -> event flag ctx+0xc. Its second wait,
 *                                 FUN_20153ec2(ctx+0xc, 2, 0), needs bit 1
 *                                 of that flag, i.e. status bit 1.
 *
 * Both were established the same way: arm the wait's own call site with a
 * breakpoint, confirm the walk stops there, add the bit, confirm the walk
 * moves past it (tools/walk_call_sites.py).
 *
 * Bits 3/9/10 also route to ctx+0xc and bits 2/8 to ctx+0x14, but nothing
 * traced waits on them, and they may well be error indications -- so this
 * model leaves them alone rather than asserting every enabled source. Bit 6
 * (FIFO above high-watermark) is deliberately never asserted; see the file
 * comment. */
#define OPENVG_INT_DONE ((1u << 0) | (1u << 1))

static void rza1h_openvg_update_irq(RZA1HOpenVGState *s)
{
    qemu_set_irq(s->int0, (s->intsts & s->inten) != 0);
}


/*
 * Rendering (2026-09-23). The command FIFO carries a packetized register-write
 * stream, decoded from ~10,600 live-captured words (splash + main screen) --
 * NOT from any manual (R-GPVG 2.6.2 has no public register documentation):
 *
 *   0xA<<28 | (n-1)<<16 | reg   followed by n data words -> gpu_reg[reg/4 ..]
 *   0xDA000000                  execute a 2D op (fill / blit)
 *   0xD8000000                  execute a vector draw (here: always an image)
 *   anything else (0, 0x0800xxxx, 0x09000000, ...) is a single-word no-op here
 *
 * Registers, as far as the captured traffic shows them:
 *   0x010 [addr, stride, fmt]    destination surface (stride may be negative);
 *                                A8 glyph targets get a 2-word packet, and
 *                                their format is then the one in 0x208
 *   0x200 [addr, stride, fmt, ?, (W-1)<<16|(H-1), ...]   source surface/image
 *   0x814 [(W-1)<<16|(H-1), dst x<<16|y, src x<<16|y]    2D op rectangle
 *   0x80c 2D: [fill colour (raw, dst format), rop]; draw: [tx, ty, 1.0] floats
 *   0x21c/0x238  float rows of the 2x2 dst->src matrix
 *   0x0f0 [a, r, g, b] floats    paint colour / image colour multiplier
 *   0x110 2D: bit0 = blit from 0x200 (else fill); draw: bit0 = src-over blend
 * Formats: 3 = RGB565, 0xb = A8, 0x41 = 32-bit ARGB (byte order a guess).
 * Image draws map inversely, src = M * (dst - origin) + t, nearest sampling,
 * origin = the cover rectangle's corner (0xD8 operand 0, from the tessellator)
 * -- checked against the frequency-digit glyphs, whose draws land exactly on
 * the rectangles cleared by the neighbouring fills. The 0x8000 fragment programs,
 * 0x020 buffer pointer and 0x208 format hint are stored but not interpreted.
 */

typedef struct VGSurf {
    uint32_t addr;
    int32_t stride;
    uint32_t fmt;
    int bpp;
} VGSurf;

static int vg_fmt_bpp(uint32_t fmt)
{
    switch (fmt) {
    case 0x03: return 2;
    case 0x0b: return 1;
    case 0x41: return 4;
    default:   return 0;
    }
}

static bool vg_surf(RZA1HOpenVGState *s, uint32_t reg, VGSurf *out)
{
    out->addr = s->gpu_reg[reg / 4];
    out->stride = (int32_t)s->gpu_reg[reg / 4 + 1];
    out->fmt = s->gpu_reg[reg / 4 + 2];
    if (reg == 0x010 && s->dst_fmt_in_208) {
        out->fmt = s->gpu_reg[0x208 / 4];
    }
    out->bpp = vg_fmt_bpp(out->fmt);
    if (!out->bpp) {
        rza1h_debug("openvgop", "unknown surface format %x at reg %x",
                    out->fmt, reg);
        return false;
    }
    return true;
}

static void vg_row_io(const VGSurf *sf, int x, int y, int w, uint8_t *buf,
                      bool write)
{
    hwaddr a = sf->addr + (int64_t)y * sf->stride + (int64_t)x * sf->bpp;

    if (write) {
        address_space_write(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                            buf, w * sf->bpp);
    } else {
        address_space_read(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                           buf, w * sf->bpp);
    }
}

/* c = {a, r, g, b}, each 0..1, not premultiplied. A8 loads as white with alpha
 * so that multiplying by the paint colour gives a tinted glyph. */
static void vg_px_load(const VGSurf *sf, const uint8_t *p, float c[4])
{
    uint32_t v;

    switch (sf->fmt) {
    case 0x03:
        v = p[0] | p[1] << 8;
        c[0] = 1.0f;
        c[1] = ((v >> 11) & 31) / 31.0f;
        c[2] = ((v >> 5) & 63) / 63.0f;
        c[3] = (v & 31) / 31.0f;
        break;
    case 0x0b:
        c[0] = p[0] / 255.0f;
        c[1] = c[2] = c[3] = 1.0f;
        break;
    default:
        v = ldl_le_p(p);
        c[0] = (v >> 24) / 255.0f;
        c[1] = ((v >> 16) & 0xff) / 255.0f;
        c[2] = ((v >> 8) & 0xff) / 255.0f;
        c[3] = (v & 0xff) / 255.0f;
        break;
    }
}

static void vg_px_store(const VGSurf *sf, uint8_t *p, const float c[4])
{
    uint32_t v;
#define Q(x, m) ((uint32_t)((x) < 0 ? 0 : (x) > 1 ? (m) : (x) * (m) + 0.5f))
    switch (sf->fmt) {
    case 0x03:
        v = Q(c[1], 31) << 11 | Q(c[2], 63) << 5 | Q(c[3], 31);
        p[0] = v;
        p[1] = v >> 8;
        break;
    case 0x0b:
        p[0] = Q(c[0], 255);
        break;
    default:
        stl_le_p(p, Q(c[0], 255) << 24 | Q(c[1], 255) << 16 |
                    Q(c[2], 255) << 8 | Q(c[3], 255));
        break;
    }
#undef Q
}

static float vg_f(RZA1HOpenVGState *s, uint32_t reg)
{
    uint32_t v = s->gpu_reg[reg / 4];
    float f;

    memcpy(&f, &v, 4);
    return f;
}

static void vg_exec_2d(RZA1HOpenVGState *s)
{
    VGSurf dst, src;
    uint32_t size = s->gpu_reg[0x814 / 4];
    uint32_t dpos = s->gpu_reg[0x818 / 4];
    uint32_t spos = s->gpu_reg[0x81c / 4];
    int w = (size >> 16) + 1, h = (size & 0xffff) + 1;
    int dx = dpos >> 16, dy = dpos & 0xffff;
    int sx = spos >> 16, sy = spos & 0xffff;
    bool blit = s->gpu_reg[0x110 / 4] & 1;
    g_autofree uint8_t *sbuf = NULL, *dbuf = NULL;

    if (!vg_surf(s, 0x010, &dst) || w > 2048 || h > 2048) {
        return;
    }
    dbuf = g_malloc(w * 4);
    if (!blit) {
        uint32_t colour = s->gpu_reg[0x80c / 4];
        for (int i = 0; i < w; i++) {
            memcpy(dbuf + i * dst.bpp, &colour, dst.bpp);
        }
        for (int y = 0; y < h; y++) {
            vg_row_io(&dst, dx, dy + y, w, dbuf, true);
        }
        rza1h_debug("openvgop", "#%" PRIu64 " fill %dx%d @%d,%d surf %08x "
                    "colour %08x", s->ops, w, h, dx, dy, dst.addr, colour);
        return;
    }
    if (!vg_surf(s, 0x200, &src)) {
        return;
    }
    sbuf = g_malloc(w * 4);
    for (int y = 0; y < h; y++) {
        vg_row_io(&src, sx, sy + y, w, sbuf, false);
        if (src.fmt == dst.fmt) {
            memcpy(dbuf, sbuf, w * dst.bpp);
        } else {
            for (int i = 0; i < w; i++) {
                float c[4];
                vg_px_load(&src, sbuf + i * src.bpp, c);
                vg_px_store(&dst, dbuf + i * dst.bpp, c);
            }
        }
        vg_row_io(&dst, dx, dy + y, w, dbuf, true);
    }
    rza1h_debug("openvgop", "#%" PRIu64 " blit %dx%d %08x@%d,%d -> %08x@%d,%d",
                s->ops, w, h, src.addr, sx, sy, dst.addr, dx, dy);
}

static void vg_cover_path(RZA1HOpenVGState *s);

static void vg_exec_draw(RZA1HOpenVGState *s)
{
    VGSurf dst, img;
    uint32_t isize = s->gpu_reg[0x210 / 4];
    int iw = (isize >> 16) + 1, ih = (isize & 0xffff) + 1;
    float m00 = vg_f(s, 0x21c), m01 = vg_f(s, 0x220);
    float m10 = vg_f(s, 0x238), m11 = vg_f(s, 0x23c);
    float tx = vg_f(s, 0x80c), ty = vg_f(s, 0x810);
    float paint[4] = { vg_f(s, 0x0f0), vg_f(s, 0x0f4), vg_f(s, 0x0f8),
                       vg_f(s, 0x0fc) };
    bool over = s->gpu_reg[0x110 / 4] & 1;
    float det = m00 * m11 - m01 * m10;

    /* Fragment program (0x8000) selects the OpenVG image mode. The 12-word
     * program starting 0x61000092 is VG_DRAW_IMAGE_NORMAL: the image's own
     * colour, A8 = white with alpha, paint ignored (0xf0 is left stale -- the
     * main frequency digits were drawn black on black before this). The
     * 14-word one (0xd00201d2 prefix) is VG_DRAW_IMAGE_MULTIPLY: image x
     * paint (the splash fades ramp 0xf0). Inferred from which draws the
     * driver pairs with a 0xf0 write, not from documentation. */
    if (s->gpu_reg[0x8000 / 4] == 0x61000092) {
        paint[0] = paint[1] = paint[2] = paint[3] = 1.0f;
    }
    g_autofree uint8_t *sbuf = NULL, *dbuf = NULL;

    if (!s->img_xform) {
        /* Image draws always (re)write their [tx, ty, 1.0] into 0x80c right
         * before the kick; path draws never do (and a stale 1.0 is left in
         * 0x814 from the last image draw). So this is a path (vector glyph) draw, whose geometry
         * went to the tessellator (vg_tess_kick). Don't replay the stale
         * image registers. */
        vg_cover_path(s);
        return;
    }
    if (!vg_surf(s, 0x010, &dst) || !vg_surf(s, 0x200, &img) ||
        iw > 2048 || ih > 2048 || fabsf(det) < 1e-6f) {
        return;
    }
    /* The cover rectangle comes from the tessellator (D8 operands); the image
     * transform is relative to its origin: src = M * (dst - origin) + t. */
    int ox = s->d8_ops[0] >> 16, oy = s->d8_ops[0] & 0xffff;
    int bx0 = ox, by0 = oy;
    int bx1 = MIN(abs(dst.stride) / dst.bpp, (int)(s->d8_ops[1] >> 16) + 1);
    int by1 = (s->d8_ops[1] & 0xffff) + 1;
    int bw = bx1 - bx0;
    if (bw <= 0 || by1 <= by0 || bw > 2048 || by1 - by0 > 2048) {
        return;
    }
    dbuf = g_malloc(bw * 4);
    sbuf = g_malloc(iw * 4);
    int nz = 0;
    if (rza1h_debug_enabled("openvgop")) {
        for (int v = 0; v < ih; v++) {
            vg_row_io(&img, 0, v, iw, sbuf, false);
            for (int i = 0; i < iw * img.bpp; i++) {
                nz += sbuf[i] != 0;
            }
        }
    }
    for (int y = by0; y < by1; y++) {
        vg_row_io(&dst, bx0, y, bw, dbuf, false);
        for (int x = bx0; x < bx1; x++) {
            float fx = x - ox, fy = y - oy;
            int u = (int)floorf(m00 * fx + m01 * fy + tx);
            int v = (int)floorf(m10 * fx + m11 * fy + ty);
            float c[4], d[4];
            uint8_t *dp = dbuf + (x - bx0) * dst.bpp;

            if (u < 0 || v < 0 || u >= iw || v >= ih) {
                continue;
            }
            vg_row_io(&img, u, v, 1, sbuf, false);
            vg_px_load(&img, sbuf, c);
            for (int k = 0; k < 4; k++) {
                c[k] *= paint[k];
            }
            if (over) {
                vg_px_load(&dst, dp, d);
                for (int k = 1; k < 4; k++) {
                    d[k] = c[k] * c[0] + d[k] * (1.0f - c[0]);
                }
                d[0] = c[0] + d[0] * (1.0f - c[0]);
                vg_px_store(&dst, dp, d);
            } else {
                /* Replace; A8/565 targets have nowhere to keep alpha, so
                 * store it premultiplied over black. */
                if (dst.fmt != 0x0b) {
                    for (int k = 1; k < 4; k++) {
                        c[k] *= c[0];
                    }
                }
                vg_px_store(&dst, dp, c);
            }
        }
        vg_row_io(&dst, bx0, y, bw, dbuf, true);
    }
    rza1h_debug("openvgop", "#%" PRIu64 " draw %dx%d img %08x fmt %x -> %08x "
                "bbox %d,%d-%d,%d paint %.2f,%.2f,%.2f,%.2f %s nz=%d", s->ops, iw,
                ih, img.addr, img.fmt, dst.addr, bx0, by0, bx1, by1, paint[0],
                paint[1], paint[2], paint[3], over ? "over" : "src", nz);
}

static void vg_consume_word(RZA1HOpenVGState *s, uint32_t w)
{
    if (s->d8_left) {
        s->d8_ops[2 - s->d8_left] = w;
        if (--s->d8_left == 0) {
            vg_exec_draw(s);
            s->img_xform = false;
        }
        return;
    }
    if (s->pkt_left) {
        if (s->pkt_reg / 4 < VG_NUM_REGS) {
            s->gpu_reg[s->pkt_reg / 4] = w;
        }
        s->pkt_reg += 4;
        s->pkt_left--;
        return;
    }
    switch (w >> 24) {
    case 0xa0 ... 0xaf:
        s->pkt_reg = w & 0xffff;
        s->pkt_left = ((w >> 16) & 0xfff) + 1;
        if (s->pkt_reg == 0x010) {
            s->dst_fmt_in_208 = s->pkt_left < 3;
        }
        if (s->pkt_reg == 0x80c && s->pkt_left == 3) {
            s->img_xform = true;
        }
        break;
    case 0xda:
        s->ops++;
        vg_exec_2d(s);
        break;
    case 0xd8:                       /* + 2 operands: bbox min, max-1 */
        s->ops++;
        s->d8_left = 2;
        break;
    default:
        break;
    }
}


/*
 * Path tessellator (0xE8102000). Path fills -- which is every vector-font glyph --
 * never go through the command FIFO: vg_tess_build_cmdlist (0x2015fd1e) writes
 * a command list to RAM, vg_tess_kick_cmdlist (0x2015f9fe) sets +0x070 = list
 * address and +0x000 = 7, vg_tess_read_bbox (0x20161244) reads the result box
 * back from +0x098/+0x09c, then vg_cover_draw_emit pushes the FIFO "cover"
 * draw (0xD8 + 2 operand words). Found 2026-09-23 by static trace; before,
 * this range was an unimplemented-device catch-all and every write vanished.
 */
static void vg_edge(VGCoverage *c, const float m[6], float x0, float y0,
                    float x1, float y1)
{
    if (c->n >= VG_MAX_EDGES) {
        return;
    }
    c->e[c->n++] = (VGEdge) {
        m[0] * x0 + m[1] * y0 + m[2], m[3] * x0 + m[4] * y0 + m[5],
        m[0] * x1 + m[1] * y1 + m[2], m[3] * x1 + m[4] * y1 + m[5],
    };
}

/* Flatten a quadratic (cubic when q3 != NULL) Bezier from (px,py) into 8 lines. */
static void vg_bezier(VGCoverage *c, const float m[6], float px, float py,
                      const float *q1, const float *q2, const float *q3)
{
    float lx = px, ly = py;

    for (int i = 1; i <= 8; i++) {
        float t = i / 8.0f, u = 1 - t, x, y;
        if (q3) {
            x = u * u * u * px + 3 * u * u * t * q1[0] + 3 * u * t * t * q2[0] +
                t * t * t * q3[0];
            y = u * u * u * py + 3 * u * u * t * q1[1] + 3 * u * t * t * q2[1] +
                t * t * t * q3[1];
        } else {
            x = u * u * px + 2 * u * t * q1[0] + t * t * q2[0];
            y = u * u * py + 2 * u * t * q1[1] + t * t * q2[1];
        }
        vg_edge(c, m, lx, ly, x, y);
        lx = x;
        ly = y;
    }
}

static float vg_tl_f(RZA1HOpenVGState *s, uint32_t reg)
{
    float f;

    memcpy(&f, &s->tl_reg[(reg & 0x3fff) / 4], 4);
    return f;
}

/*
 * Execute a tessellator command list. Encoding, decoded from live captures
 * (splash + main screen) and vg_tess_build_cmdlist:
 *   0x19nnRRRR + nn+1 words    register write (0x3110/0x3120 = user->surface
 *                              matrix rows [a b tx]/[c d ty]; 0x2078 = coverage
 *                              buffer; 0x2090 = clip size W<<16|H; 0x2098/
 *                              0x209c = result bbox max/min, reset by the list)
 *   0x580000tt + coords        one path segment, tt = the OpenVG segment
 *                              command (VG_MOVE_TO = 2, LINE_TO = 4, QUAD_TO =
 *                              10, ...; odd = relative), float coordinates
 *   0x59020008 x y w h         rectangle (how images are tessellated)
 *   0x14000001                 end of list
 * Everything else (0x59 0x1b 0x14008000 blocks: glyph/program bookkeeping) is
 * skipped word by word. Fill rule: non-zero (glyph outlines).
 */
static void vg_tess_kick(RZA1HOpenVGState *s)
{
    uint32_t list = s->tess_reg[0x070 / 4];
    uint32_t w[2048];
    VGCoverage *c;
    float m[6], sx = 0, sy = 0, px = 0, py = 0, qx = 0, qy = 0;
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
    int i = 0, n;

    s->tess_kicks++;
    address_space_read(&address_space_memory, list, MEMTXATTRS_UNSPECIFIED,
                       w, sizeof(w));
    for (n = 0; n < 2048 && w[n] != 0x14000001; n++) {
    }
    /* Pass 1: register writes (the coverage buffer and matrix come first but
     * keep it order-independent). */
    for (i = 0; i < n;) {
        uint32_t x = w[i];
        if (x >> 24 == 0x19) {
            int cnt = ((x >> 16) & 0xff) + 1;
            for (int k = 0; k < cnt && i + 1 + k < n; k++) {
                s->tl_reg[(((x & 0xffff) + 4 * k) & 0x3fff) / 4] = w[i + 1 + k];
            }
            i += 1 + cnt;
        } else {
            i++;
        }
    }
    m[0] = vg_tl_f(s, 0x3110); m[1] = vg_tl_f(s, 0x3114); m[2] = vg_tl_f(s, 0x3118);
    m[3] = vg_tl_f(s, 0x3120); m[4] = vg_tl_f(s, 0x3124); m[5] = vg_tl_f(s, 0x3128);

    c = &s->cov[0];
    for (int k = 0; k < VG_COV_SLOTS; k++) {
        if (s->cov[k].buf == s->tl_reg[0x2078 / 4]) {
            c = &s->cov[k];
            break;
        }
        if (s->cov[k].buf == 0) {
            c = &s->cov[k];
        }
    }
    c->buf = s->tl_reg[0x2078 / 4];
    c->n = 0;

    /* Pass 2: geometry. */
    for (i = 0; i < n;) {
        uint32_t x = w[i];
        float a[6];
        int t, na;

        if (x >> 24 == 0x19) {
            i += 2 + ((x >> 16) & 0xff);
            continue;
        }
        if (x == 0x59020008 && i + 4 < n) {
            float r[4];
            memcpy(r, &w[i + 1], 16);
            vg_edge(c, m, r[0], r[1], r[0] + r[2], r[1]);
            vg_edge(c, m, r[0] + r[2], r[1], r[0] + r[2], r[1] + r[3]);
            vg_edge(c, m, r[0] + r[2], r[1] + r[3], r[0], r[1] + r[3]);
            vg_edge(c, m, r[0], r[1] + r[3], r[0], r[1]);
            i += 5;
            continue;
        }
        if (x >> 8 != 0x580000) {
            i++;
            continue;
        }
        t = x & 0xfe;
        na = t == 0 ? 0 : t <= 4 ? 2 : t <= 8 ? 1 : t == 10 ? 4 : t == 12 ? 6 :
             t == 14 ? 2 : t == 16 ? 4 : 5;
        if (i + na >= n) {
            break;
        }
        memcpy(a, &w[i + 1], na * 4);
        if (x & 1) {                     /* relative: offset from current point */
            for (int k = 0; k < na; k++) {
                if (t == 6) {
                    a[k] += px;
                } else if (t == 8) {
                    a[k] += py;
                } else if (t >= 18) {
                    if (k >= 3) {
                        a[k] += k == 3 ? px : py;
                    }
                } else {
                    a[k] += (k & 1) ? py : px;
                }
            }
        }
        switch (t) {
        case 0:                          /* CLOSE_PATH */
            vg_edge(c, m, px, py, sx, sy);
            px = qx = sx; py = qy = sy;
            break;
        case 2:                          /* MOVE_TO (implicitly closes) */
            vg_edge(c, m, px, py, sx, sy);
            sx = px = qx = a[0]; sy = py = qy = a[1];
            break;
        case 4: case 6: case 8:          /* LINE_TO / HLINE_TO / VLINE_TO */
        case 18: case 20: case 22: case 24: {   /* arcs: chord only */
            float nx = t == 8 ? px : t >= 18 ? a[3] : a[0];
            float ny = t == 6 ? py : t == 8 ? a[0] : t >= 18 ? a[4] : a[1];
            vg_edge(c, m, px, py, nx, ny);
            px = qx = nx; py = qy = ny;
            break;
        }
        case 10: case 14: {              /* QUAD_TO / SQUAD_TO */
            float q1[2], q2[2];
            if (t == 10) {
                q1[0] = a[0]; q1[1] = a[1]; q2[0] = a[2]; q2[1] = a[3];
            } else {
                q1[0] = 2 * px - qx; q1[1] = 2 * py - qy; q2[0] = a[0]; q2[1] = a[1];
            }
            vg_bezier(c, m, px, py, q1, q2, NULL);
            qx = q1[0]; qy = q1[1]; px = q2[0]; py = q2[1];
            break;
        }
        case 12: case 16: {              /* CUBIC_TO / SCUBIC_TO */
            float q1[2], q2[2], q3[2];
            if (t == 12) {
                q1[0] = a[0]; q1[1] = a[1]; q2[0] = a[2]; q2[1] = a[3];
                q3[0] = a[4]; q3[1] = a[5];
            } else {
                q1[0] = 2 * px - qx; q1[1] = 2 * py - qy; q2[0] = a[0]; q2[1] = a[1];
                q3[0] = a[2]; q3[1] = a[3];
            }
            vg_bezier(c, m, px, py, q1, q2, q3);
            qx = q2[0]; qy = q2[1]; px = q3[0]; py = q3[1];
            break;
        }
        default:
            break;
        }
        i += 1 + na;
    }
    vg_edge(c, m, px, py, sx, sy);

    for (int k = 0; k < c->n; k++) {
        VGEdge *e = &c->e[k];
        bx0 = MIN(bx0, MIN(e->x0, e->x1)); bx1 = MAX(bx1, MAX(e->x0, e->x1));
        by0 = MIN(by0, MIN(e->y0, e->y1)); by1 = MAX(by1, MAX(e->y0, e->y1));
    }
    uint32_t clip = s->tl_reg[0x2090 / 4];
    int cw = clip >> 16, ch = clip & 0xffff;
    int ix0 = MAX(0, (int)floorf(bx0)), iy0 = MAX(0, (int)floorf(by0));
    int ix1 = MIN(cw, (int)ceilf(bx1)), iy1 = MIN(ch, (int)ceilf(by1));
    if (c->n == 0 || ix1 <= ix0 || iy1 <= iy0) {
        s->tess_reg[0x098 / 4] = 0;
        s->tess_reg[0x09c / 4] = 0x07ff07ff;
    } else {
        s->tess_reg[0x09c / 4] = ix0 << 16 | iy0;
        s->tess_reg[0x098 / 4] = ix1 << 16 | iy1;
    }
    s->tess_reg[0x0fc / 4] = 0;
    rza1h_debug("openvgtess", "kick #%" PRIu64 " list %08x (%d words) buf %08x "
                "edges %d bbox %d,%d-%d,%d", s->tess_kicks, list, n + 1, c->buf,
                c->n, ix0, iy0, ix1, iy1);
}

/* The FIFO cover draw for a path: paint x coverage over [x0,x1]x[y0,y1]. */
static void vg_cover_path(RZA1HOpenVGState *s)
{
    VGSurf dst;
    VGCoverage *c = NULL;
    int x0 = s->d8_ops[0] >> 16, y0 = s->d8_ops[0] & 0xffff;
    int x1 = (s->d8_ops[1] >> 16) + 1, y1 = (s->d8_ops[1] & 0xffff) + 1;
    float paint[4] = { vg_f(s, 0x0f0), vg_f(s, 0x0f4), vg_f(s, 0x0f8),
                       vg_f(s, 0x0fc) };
    g_autofree uint8_t *dbuf = NULL;

    for (int k = 0; k < VG_COV_SLOTS; k++) {
        if (s->cov[k].buf && s->cov[k].buf == s->gpu_reg[0x20 / 4]) {
            c = &s->cov[k];
        }
    }
    if (!c || !vg_surf(s, 0x010, &dst) || x1 <= x0 || y1 <= y0 ||
        x1 - x0 > 2048 || y1 - y0 > 2048) {
        return;
    }
    dbuf = g_malloc((x1 - x0) * 4);
    for (int y = y0; y < y1; y++) {
        vg_row_io(&dst, x0, y, x1 - x0, dbuf, false);
        for (int x = x0; x < x1; x++) {
            int hits = 0;
            for (int sj = 0; sj < 4; sj++) {
                float fy = y + (sj + 0.5f) / 4;
                for (int si = 0; si < 4; si++) {
                    float fx = x + (si + 0.5f) / 4;
                    int wind = 0;
                    for (int k = 0; k < c->n; k++) {
                        VGEdge *e = &c->e[k];
                        if ((e->y0 <= fy) != (e->y1 <= fy)) {
                            float ex = e->x0 + (fy - e->y0) / (e->y1 - e->y0) *
                                       (e->x1 - e->x0);
                            if (ex > fx) {
                                wind += e->y1 > e->y0 ? 1 : -1;
                            }
                        }
                    }
                    hits += wind != 0;
                }
            }
            if (hits) {
                float a = paint[0] * hits / 16.0f, d[4];
                uint8_t *dp = dbuf + (x - x0) * dst.bpp;
                vg_px_load(&dst, dp, d);
                for (int k = 1; k < 4; k++) {
                    d[k] = paint[k] * a + d[k] * (1 - a);
                }
                d[0] = a + d[0] * (1 - a);
                vg_px_store(&dst, dp, d);
            }
        }
        vg_row_io(&dst, x0, y, x1 - x0, dbuf, true);
    }
    rza1h_debug("openvgop", "#%" PRIu64 " cover path %d,%d-%d,%d edges %d paint "
                "%.2f,%.2f,%.2f,%.2f", s->ops, x0, y0, x1, y1, c->n, paint[0],
                paint[1], paint[2], paint[3]);
}

static uint64_t rza1h_openvg_tess_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    RZA1HOpenVGState *s = RZA1H_OPENVG(opaque);

    return s->tess_reg[offset / 4];
}

static void rza1h_openvg_tess_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    RZA1HOpenVGState *s = RZA1H_OPENVG(opaque);

    s->tess_reg[offset / 4] = value;
    if (offset == 0x000 && value == 7) {
        vg_tess_kick(s);
    }
}

static const MemoryRegionOps rza1h_openvg_tess_ops = {
    .read = rza1h_openvg_tess_read,
    .write = rza1h_openvg_tess_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t rza1h_openvg_read(void *opaque, hwaddr offset, unsigned size)
{
    RZA1HOpenVGState *s = RZA1H_OPENVG(opaque);
    uint64_t val = 0;

    switch (offset) {
    case OPENVG_INTSTS:
        return s->intsts;
    case OPENVG_INTEN:
        return s->inten;
    default:
        break;
    }

    memcpy(&val, &s->regs[offset], size);
    return val;
}

static void rza1h_openvg_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    RZA1HOpenVGState *s = RZA1H_OPENVG(opaque);

    switch (offset) {
    case OPENVG_CMD:
        /* Command words are consumed instantly and discarded; completion is
         * reported immediately. See the file comment for why "instantly" is
         * the right fidelity here. */
        s->commands++;
        vg_consume_word(s, (uint32_t)value);
        s->intsts |= OPENVG_INT_DONE;
        rza1h_debug("openvg", "cmd %08x (#%" PRIu64 "), intsts=%08x inten=%08x",
                    (uint32_t)value, s->commands, s->intsts, s->inten);
        rza1h_openvg_update_irq(s);
        return;
    case OPENVG_INTEN:
        s->inten = (uint32_t)value;
        rza1h_debug("openvg", "inten <- %08x", s->inten);
        rza1h_openvg_update_irq(s);
        return;
    case OPENVG_INTCLR:
        s->intsts &= ~(uint32_t)value;
        rza1h_debug("openvg", "ack %08x, intsts=%08x", (uint32_t)value,
                    s->intsts);
        rza1h_openvg_update_irq(s);
        return;
    case OPENVG_INTSTS:
        /* Read-only status; the driver acknowledges through INTCLR. */
        return;
    default:
        break;
    }

    memcpy(&s->regs[offset], &value, size);
}

static const MemoryRegionOps rza1h_openvg_ops = {
    .read = rza1h_openvg_read,
    .write = rza1h_openvg_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rza1h_openvg_reset(DeviceState *dev)
{
    RZA1HOpenVGState *s = RZA1H_OPENVG(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->intsts = 0;
    s->inten = 0;
    s->commands = 0;
    memset(s->gpu_reg, 0, sizeof(s->gpu_reg));
    s->pkt_reg = s->pkt_left = 0;
    s->ops = 0;
    memset(s->tess_reg, 0, sizeof(s->tess_reg));
    memset(s->tl_reg, 0, sizeof(s->tl_reg));
    memset(s->cov, 0, sizeof(s->cov));
    s->tess_kicks = 0;
    s->d8_left = 0;
    rza1h_openvg_update_irq(s);
}

static void rza1h_openvg_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    RZA1HOpenVGState *s = RZA1H_OPENVG(obj);

    memory_region_init_io(&s->iomem, obj, &rza1h_openvg_ops, s,
                          TYPE_RZA1H_OPENVG, RZA1H_OPENVG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    memory_region_init_io(&s->tess_iomem, obj, &rza1h_openvg_tess_ops, s,
                          TYPE_RZA1H_OPENVG "-tess", VG_TESS_SIZE);
    sysbus_init_mmio(sbd, &s->tess_iomem);
    sysbus_init_irq(sbd, &s->int0);
}

static void rza1h_openvg_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, rza1h_openvg_reset);
}

static const TypeInfo rza1h_openvg_info = {
    .name          = TYPE_RZA1H_OPENVG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RZA1HOpenVGState),
    .instance_init = rza1h_openvg_init,
    .class_init    = rza1h_openvg_class_init,
};

static void rza1h_openvg_register_types(void)
{
    type_register_static(&rza1h_openvg_info);
}

type_init(rza1h_openvg_register_types)
