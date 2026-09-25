/* Homebrew loader <-> app ABI (v1).
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
 * Apps don't normally touch any of this directly -- sdk/runtime/ builds the header and drives
 * idle_hook for them (see hb/app.h).
 */
#ifndef HB_ABI_H
#define HB_ABI_H

#define HB_ABI_VERSION      2u      /* v2: + input_grab (v1 apps must be rebuilt) */
#define HB_APP_MAGIC        0x31304248u     /* "HB01" */

/* RAM the app image (code + data + bss + stack) must fit in. Sits inside the region confirmed
 * safe from the runtime allocator by marker tests (sdk/app-loader-design.md). */
#define HB_APP_REGION       0x20610000u
#define HB_APP_REGION_SIZE  0x00020000u     /* 128 KB */

/* Two 480x272 RGB565 framebuffers for sdk/runtime/gfx.c, in the same region: all zero in a
 * live RAM map after boot, between points marker tests showed survive a full boot. */
#define HB_FB0              0x20640000u
#define HB_FB1              0x20680000u

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
};

typedef void (*hb_app_entry_fn)(struct hb_loader_api *api);
#endif

#endif
