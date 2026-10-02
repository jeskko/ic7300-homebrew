/* Homebrew loader <-> app ABI (v4).
 *
 * The loader (sdk/loader/, flashed once as part of a modified body.bin) reads APP.BIN from the
 * SD card into HB_APP_REGION, checks its header, and calls header->entry(api) on the UI thread
 * (main_idle_loop's context -- the same thread that renders the screen and processes touches).
 *
 * An app that needs to keep running after entry() returns sets api->idle_hook: the loader then
 * calls it once per main_idle_loop pass until the app clears it again. While idle_hook is set the
 * app counts as resident, and the loader refuses to load another APP.BIN over it.
 *
 * An app that wants the touchscreen and keys to itself sets api->input_grab: the loader then
 * skips the firmware's own touch/key handling (ui_input_poll_tick) while keeping its key
 * shadow in sync, so nothing the app sees reaches the screen underneath. The dials are not
 * grabbed.
 *
 * An app that wants the receiver's audio sets api->audio_hook (v4): the loader calls it with
 * every 36-sample block of DX_REC L -- the demodulated RX audio at 48 kHz, before AF gain
 * (notes/dsp-protocol.md) -- right after the firmware has queued the same block for its own
 * reader. It is called from the 250 us system-tick ISR (ssif0_rx_pump_dx_rec), 1333 times a
 * second: copy the samples and return; no firmware calls, no floating point, no waiting. The
 * loader clears it once the app is gone. hb/audio.h wraps it.
 *
 * Apps don't normally touch any of this directly -- sdk/runtime/ builds the header and drives
 * idle_hook for them (see hb/app.h).
 */
#ifndef HB_ABI_H
#define HB_ABI_H

#define HB_ABI_VERSION      4u      /* v4: audio_hook; v3: 1 MB region, framebuffers moved, heap */
#define HB_APP_MAGIC        0x31304248u     /* "HB01" */

/* The homebrew RAM map (v3). 0x20600000-0x207fffff is ordinary cached, executable RAM that
 * the firmware never uses: its own RAM ends with the mode stacks at 0x205dcf60, and a marker
 * sweep plus a static trace found nothing of it above that (notes/memory-map.md). The loader
 * itself sits at 0x20600000.
 *
 *   0x20610000  HB_APP_REGION   1 MB: the app image (code + data + bss + stack)
 *   0x20710000  HB_FB0          two 480x272 RGB565 framebuffers for sdk/runtime/gfx.c
 *   0x20750000  HB_FB1
 *   0x20790000  HB_HEAP         448 KB: hb_malloc() (sdk/runtime/heap.c)
 *   0x20800000  HB_HEAP_END     (above: execute-never, then firmware page tables and GPU RAM)
 */
#define HB_APP_REGION       0x20610000u
#define HB_APP_REGION_SIZE  0x00100000u     /* 1 MB */
#define HB_FB0              0x20710000u
#define HB_FB1              0x20750000u     /* each 480*272*2 = 0x3fc00 bytes */
#define HB_HEAP             0x20790000u
#define HB_HEAP_END         0x20800000u

/* Firmware build the loader was built for -- the runtime refuses to run against any other,
 * since hb/firmware.h's addresses are only valid for this one. */
#define HB_FW_142           0x0142u

#ifndef __ASSEMBLER__
#include <stdint.h>

struct hb_app_header {          /* first 16 bytes of APP.BIN, at HB_APP_REGION */
    uint32_t magic;             /* HB_APP_MAGIC */
    uint32_t abi_version;       /* HB_ABI_VERSION the app was built against */
    uint32_t entry;             /* void entry(struct hb_loader_api *) -- absolute address */
    uint32_t image_end;         /* end of the app's RAM footprint (incl. bss + stack) */
};

struct hb_loader_api {
    uint32_t abi_version;               /* HB_ABI_VERSION */
    uint32_t fw_build;                  /* HB_FW_142 */
    void (*volatile idle_hook)(void);   /* app-owned: called every main_idle_loop pass if set */
    volatile uint32_t input_grab;       /* app-owned: nonzero = firmware ignores touch/keys */
    /* app-owned (v4): called from the tick ISR with each 36-sample 48 kHz RX-audio block */
    void (*volatile audio_hook)(const int16_t *samples, uint32_t n);
};

typedef void (*hb_app_entry_fn)(struct hb_loader_api *api);
#endif

#endif
