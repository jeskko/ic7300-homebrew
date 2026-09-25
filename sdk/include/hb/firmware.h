/* Stock firmware entry points and data used by the homebrew loader and runtime.
 *
 * Valid for the 1.42 body.bin ONLY (HB_FW_142). Every address here is cited from notes/ and was
 * checked against the Ghidra project; see the note next to each one.
 */
#ifndef HB_FIRMWARE_H
#define HB_FIRMWARE_H

#include <stdint.h>

#define FW_FN(addr, ret, args) ((ret (*) args)(addr))

/* ---- CI-V (notes/kernel-rtos.md, "CI-V reply staging") ---------------------------------- */
#define fw_civ_tx_pump              FW_FN(0x20011384, void, (void))

/* ---- SD-card file RPC wrappers (sdk/api/filesystem.md) --------------------------------
 * Each posts a request to sdcard_file_rpc_dispatch_task; pass the result to fw_rpc_wait,
 * which blocks until it completes and returns 0 on success. NOTE: fw_rpc_wait is unbounded
 * if the RPC ring is full (notes/kernel-rtos.md). */
#define fw_file_open    FW_FN(0x200bc5f4, int, (const char *path, int flags, int *handle, void *scratch36))
#define fw_file_read    FW_FN(0x200bc6a4, int, (int handle, void *dst, uint32_t len, int32_t *actual))
#define fw_file_close   FW_FN(0x200bc64c, int, (int handle, int zero))
#define fw_rpc_wait     FW_FN(0x200214b0, int, (int request, int err_select))
#define FW_RPC_ERR_SELECT 0x46

/* Directory listing, same RPC family (ids 6/9/7). Pattern from the stock "does this file
 * exist" lookup FUN_200221cc: open the directory, then read entries by position cookie --
 * set d.pos = d.next_pos and d.name_len before each read -- until the RPC fails (end of
 * directory). Observed live: kind 1 = regular file, with st_mode-style permissions at +0x10
 * (0x1b6 = 0666) and the size at +0x14; the stock folder browser treats kind 2 as directories
 * (skipping "." and ".."), and kind 3 as a long-name entry followed by the entry it names. */
struct fw_dirent {                      /* 0x34 bytes */
    uint8_t   _0[8];
    char     *name;                     /* in: buffer, out: filled */
    uint16_t  name_len;                 /* in: buffer size (stock uses 0x14) */
    uint8_t   kind;                     /* 1 file, 2 directory, 3 long name */
    uint8_t   _f;
    uint32_t  mode;                     /* +0x10 */
    uint32_t  size;                     /* +0x14 */
    uint8_t   _18[0x14];
    uint32_t  pos;                      /* +0x2c in: entry to read */
    uint32_t  next_pos;                 /* +0x30 out: cookie for the next read */
};
#define FW_DIRENT_FILE  1
#define fw_dir_open     FW_FN(0x200bc2b4, int, (const char *path, int *handle, void *scratch36))
#define fw_dir_read     FW_FN(0x200bc3e4, int, (int handle, struct fw_dirent *d))
#define fw_dir_close    FW_FN(0x200bc30c, int, (int handle))

/* ---- Front panel input (notes/front-panel-report.md) -------------------------------------
 * The RL78 front-panel MCU's 32-byte register file, updated by the SCIF3 RX path on change.
 * +0x13 touch tag (0 = touching, calibrated pixels), +0x14/+0x16 X/Y BE16 (0..479/0..271),
 * +0x0d..+0x11 key bits. The key scanner diffs against the latched shadow copy. */
#define fw_fp_regs              ((volatile uint8_t *)0x203dcab6)
#define fw_fp_latched           ((volatile uint8_t *)0x203dca96)
#define FW_FP_TOUCH_TAG         0x13
#define FW_FP_TOUCH_X           0x14
#define FW_FP_TOUCH_Y           0x16
#define FW_FP_KEYS_FIRST        0x0d
#define FW_FP_KEYS_LAST         0x11
/* Touch, keys, auto-repeat and long-press ticking, called once per main_idle_loop pass. */
#define fw_ui_input_poll_tick   FW_FN(0x2002fca8, void, (void))
/* Queue one 36-sample DX_REC L block (48 kHz int16) for qso_recorder_rx_audio_block; called by
 * ssif0_rx_pump_dx_rec (0x20060614) from the tick ISR. notes/dsp-protocol.md. */
#define fw_ssif_rx0L_ring_push36 FW_FN(0x2005fb28, void, (const int16_t *block))

/* ---- Time ------------------------------------------------------------------------------ */
/* +1 per RTOS tick in rtos_tick_handler (0x20188084); measured 1 kHz. */
#define fw_tick_ms              (*(volatile uint32_t *)0x20390a78)

/* ---- VDC5 channel 0 graphics planes (sdk/runtime/gfx.c; RZ/A1H HW manual ch. 35) -------
 * The firmware draws its UI into GR2 (RGB565 480x272, AB1 DISP_SEL = BLEND); GR0/GR1 are
 * off and GR3 -- the top of the fixed GR0 < GR1 < GR2 < GR3 stack -- is left at LOWER, i.e.
 * transparent. Offsets per plane: */
#define FW_VDC5_GR2             0xfcff7700u
#define FW_VDC5_GR3             0xfcff7780u
#define GR_UPDATE   0x00        /* b0 IBUS_VEN, b4 P_VEN, b8 UPDATE; read 1 until applied */
#define GR_FLM_RD   0x04        /* b0 = read enable */
#define GR_FLM1     0x08
#define GR_FLM2     0x0c        /* framebuffer base */
#define GR_FLM3     0x10        /* [30:16] line stride */
#define GR_FLM4     0x14
#define GR_FLM5     0x18        /* [26:16] lines - 1 */
#define GR_FLM6     0x1c        /* [31:28] format (0 = RGB565), [26:16] width - 1, [12:10] swap */
#define GR_AB1      0x20        /* [1:0] DISP_SEL: 0 back, 1 lower, 2 current, 3 blend */
#define GR_AB2      0x24        /* {VS, VW} */
#define GR_AB3      0x28        /* {HS, HW} */
#define GR_AB4      0x2c
#define GR_AB5      0x30
#define GR_AB6      0x34
#define GR_AB7      0x38
#define FW_VDC5_REG(plane, off) (*(volatile uint32_t *)((plane) + (off)))

/* ---- SET-style list screens (notes/ui-menu.md, "SET-style settings-list engine") -------- */
struct fw_settings_category {           /* g_settings_category_registry entry */
    uint32_t        count;
    const uint32_t *list;               /* entries: type | (val << 16); type 3 = catalog item */
    uint32_t        flags;
};
struct fw_settings_item {               /* g_settings_item_catalog record, 20 bytes */
    void      (*action)(void);          /* type-3 row tapped; row index in fw_list_cursor */
    int       (*query)(uint8_t *out);   /* NULL = always selectable */
    uint32_t    flags;                  /* low byte = render kind, 0 = plain label */
    const char *en;
    const char *jp;
};
struct fw_screen_descriptor {           /* g_screen_descriptor_table, indexed by screen - 0x13 */
    void       *render;
    uint8_t     _4[4];
    const char *title_en;
    const char *row_label_en;           /* how a parent list labels this screen */
    const char *title_jp;
    const char *row_label_jp;
};

#define fw_settings_registry    ((volatile struct fw_settings_category *)0x201993e0)
#define fw_settings_catalog     ((volatile struct fw_settings_item *)0x2018ed48)
#define fw_screen_descriptors   ((volatile struct fw_screen_descriptor *)0x2018fe24)
#define fw_current_screen       (*(volatile uint8_t *)0x203de17f)
#define fw_list_cursor          (*(volatile uint16_t *)0x20390222)
/* Per-category saved list cursor (EEPROM-backed region 2). */
#define fw_saved_cursor(cat)    (*(volatile uint32_t *)(0x203de4cc + 0x288 + (cat) * 4))

#define fw_operating_mode_change_dispatch FW_FN(0x2005807c, void, (unsigned screen))

/* Stock placeholder row: non-selectable, no-op action. */
#define FW_PLACEHOLDER_ACTION   ((void (*)(void))0x20041b48)
#define FW_PLACEHOLDER_QUERY    ((int (*)(uint8_t *))0x20041b4c)
#define FW_ITEM_FLAGS_PLAIN     0x00010700u     /* same as the stock "Format" row */

/* ---- Popup message dialogs (notes/ui-menu.md, "Popup message dialogs") ------------------
 *
 * A dialog is an *item* (index into fw_dialog_items) whose message byte selects a *message
 * record* (index into fw_message_records) holding the text. The item's type byte fixes the
 * layout: 0 = no buttons (progress), 1 = one button, 2 = two buttons, 8 = timed toast. Each
 * language has 9 string slots: 0-5 are text lines, 6/7 the left/right button labels (a
 * one-button dialog uses 7), 8 is always empty. Unused slots point at "" (never NULL).
 *
 * Only one dialog is up at a time; its live state is fw_dialog_state. A button tap calls the
 * matching callback as `int cb(uint8_t *result)` on the UI thread; returning 2 without
 * touching *result is what the stock no-callback path does (the dialog then closes).
 */
struct fw_dialog_item {                 /* 16 bytes */
    uint8_t  type;
    uint8_t  _1;
    uint16_t timeout[2];
    uint8_t  message;                   /* index into fw_message_records */
    uint8_t  _7;
    void    *hook8;
    void   (*on_activate)(void);
};

struct fw_message_record {              /* 0x4c bytes */
    uint32_t    flags;                  /* 0 plain, 1 caution, 2 error */
    const char *en[9];
    const char *jp[9];                  /* Shift-JIS; ASCII is valid Shift-JIS */
};

#define FW_MSG_TEXT_LINES   6
#define FW_MSG_BUTTON_LEFT  6
#define FW_MSG_BUTTON_RIGHT 7           /* also the only button of a one-button dialog */

typedef int (*fw_dialog_cb)(uint8_t *result);

struct fw_dialog_state {
    uint8_t      active_item;           /* 0 = no dialog */
    uint8_t      button;                /* last button index tapped */
    uint16_t     timeout;
    uint8_t      _4;
    uint8_t      flags;
    uint16_t     _6;
    fw_dialog_cb cb_primary;            /* button 0 (OK / YES) */
    fw_dialog_cb cb_secondary;          /* button 1 (NO / CANCEL) */
};

#define fw_dialog_items     ((volatile struct fw_dialog_item *)0x2018b8f0)
#define fw_message_records  ((struct fw_message_record *)0x2032c91c)
#define fw_dialog_state     ((volatile struct fw_dialog_state *)0x2039c584)

#define fw_ui_show_message_dialog \
    FW_FN(0x200198bc, void, (int item, fw_dialog_cb primary, fw_dialog_cb secondary, int flag))

/* Stock one-button dialog "The USB SEND/Keying settings were corrected." [OK] -- shown only
 * after a settings load that had to fix those settings. The runtime borrows it for
 * ui_message_box(), swapping the record's text for the duration. */
#define FW_DIALOG_ITEM_OK       0x66
#define FW_DIALOG_RECORD_OK     0x53

#endif
