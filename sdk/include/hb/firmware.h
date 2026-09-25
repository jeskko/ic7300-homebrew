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
