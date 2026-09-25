@ IC-7300 "Homebrew Apps" real menu row -- live-tested 2026-09-25, see README.md in this
@ directory for the full test log (including screenshots of the real menu tree). Triggers the
@ same SD-card app load as sdk/examples/sd-card-app/, but from a genuine tap on a visible menu
@ item instead of a hidden key combo -- this is the actual "Homebrew Apps" menu button.
@
@ Mechanism (see notes/ui-menu.md's "SET-style settings-list engine" section, traced 2026-09-25):
@ every SET-tree list screen (SD CARD among them) is driven by a per-category item list indexing
@ a shared catalog, g_settings_item_catalog (0x2018ed48, 20-byte records
@ {action, query, flags, en, jp}). Tapping a type-3 row calls settings_list_activate_row, which
@ tail-calls catalog[val].action with NO arguments -- exactly the calling convention this file's
@ own entry point (homebrew_menu_action) uses.
@
@ Two DATA-only patches to the existing image (no instruction patched, unlike the other two
@ examples' main_idle_loop retarget):
@   - g_settings_category_registry[0x18] (SD CARD's own entry, 0x20199500): count 8->9, list
@     pointer retargeted to sd_menu_list_v2 below.
@   - one new 20-byte catalog record, in the confirmed-unused padding right after the registry
@     table (0x20199758-0x201998cc): {action=homebrew_menu_action, query=0 (always selectable),
@     flags=0x00010700 (copied from the real "Format" row, plain-label kind), en=jp=menu_label}.
@
@ homebrew_menu_action itself is exactly sd-card-app's load_and_run_app logic (open/read/close
@ C:\IC-7300\APP.BIN via the same confirmed wrapper functions, call it if anything was read) --
@ sd-card-app's own app_main.s file works completely unmodified as the loaded APP.BIN here too.
@
@ CORRECTIONS from an adversarial review pass, 2026-09-25 (see sdk/examples/sd-card-app/
@ loader_hook.s's own header comment for the full writeup -- the same four corrections apply
@ here, since this file started as a copy of that one):
@ 1. FIXED: the same failed-read-executes-garbage bug (checked try_read's RPC_WAIT result and
@    `sd_read_actual`'s sign, not just non-zero).
@ 2. FIXED: the same cache-maintenance gap before jumping into freshly-loaded code.
@ 3. NOT fixed, documented: `RPC_WAIT`'s unbounded wait -- see loader_hook.s.
@ 4. FIXED: the same `push {lr}`-only stack-alignment issue in try_open/try_read/try_close --
@    and the same second-order mistake loader_hook.s's own header comment now documents in
@    detail (an initial `push/pop {r0, lr}` silently destroyed the RPC result it needed to
@    return, caught live by an emulator regression test; fixed to `{r1, lr}`).
@
@ One more correction specific to this file: `sd_menu_list_v2` and `menu_label` below are
@ assembled here for convenience, but build.py copies their actual byte content into the
@ firmware's own read-only image (the same confirmed-unused padding gap the new catalog record
@ lives in) rather than pointing the registry/catalog at these symbols' own linked (appended-RAM)
@ addresses. Reasoning: an adversarial review pass flagged that leaving the SD CARD menu's own
@ item list and label in the less-certain appended-RAM region means *that entire menu* --
@ including the real Firmware Update recovery row -- would render garbage if that RAM were ever
@ found to be unsafe the same way earlier addresses in this region were. Baking the list/label
@ into the image itself removes that risk for everything except this one new row's own action
@ pointer, which still has to live in appended code.
@
@ Still open, not addressed by this pass (see sdk/app-loader-design.md): no SD-card-ready /
@ voice-recorder-busy gate before running (every sibling SD-menu row checks both), no size/
@ magic/checksum validation of APP.BIN's own content, and this row's catalog index (0x881) is
@ read as a truncated u8 by one consumer (FUN_2003fd6c) -- harmless in the 1.42 image checked
@ (0x81 doesn't collide with anything), but worth re-checking against any other firmware version.

    .syntax unified
    .arm
    .section .text

    .equ FILE_OPEN,     0x200bc5f4
    .equ FILE_READ,     0x200bc6a4
    .equ FILE_CLOSE,    0x200bc64c
    .equ RPC_WAIT,      0x200214b0
    .equ RPC_TIMEOUT,   0x46

    .equ APP_LOAD_ADDR, 0x20610000
    .equ APP_MAX_SIZE,  0x8000

    .global homebrew_menu_action

@ ---------------------------------------------------------------------------
@ Called with no arguments (a plain `bx` tail-call from settings_list_activate_row's type-3
@ case) and expected to just return -- no different from any other menu row's action function.
@ Live-verified: tapping the real "Homebrew Apps" row loads APP.BIN, runs it, and the menu
@ screen stays exactly where it was (no navigation side effect) since this action never calls
@ operating_mode_change_dispatch the way a screen-switching row would.
homebrew_menu_action:
    push    {r4-r7, lr}

    bl      try_open
    cmp     r0, #0
    bne     action_done

    bl      try_read
    mov     r5, r0              @ save try_read's own RPC_WAIT result across try_close
    bl      try_close

    cmp     r5, #0
    bne     action_done         @ the read RPC itself failed

    ldr     r1, =sd_read_actual
    ldr     r1, [r1]
    cmp     r1, #0
    ble     action_done         @ signed: a negative errno or a zero-byte read -- nothing to run
    cmp     r1, #APP_MAX_SIZE
    bgt     action_done         @ never trust a read result past our own buffer either

    mov     r6, r1              @ length actually read, saved across the call below
    ldr     r0, =APP_LOAD_ADDR
    mov     r1, r6
    bl      cache_flush_range   @ make instruction fetch see the bytes we just wrote

    ldr     r0, =APP_LOAD_ADDR
    blx     r0

action_done:
    pop     {r4-r7, lr}
    bx      lr

@ ---------------------------------------------------------------------------
try_open:
    push    {r1, lr}
    ldr     r0, =app_path
    mov     r1, #0
    ldr     r2, =sd_handle
    ldr     r3, =sd_open_scratch
    bl      FILE_OPEN
    mov     r1, #RPC_TIMEOUT
    bl      RPC_WAIT
    pop     {r1, lr}
    bx      lr

try_read:
    push    {r1, lr}
    ldr     r4, =sd_read_actual
    mov     r0, #0
    str     r0, [r4]
    ldr     r0, =sd_handle
    ldr     r0, [r0]
    ldr     r1, =APP_LOAD_ADDR
    ldr     r2, =APP_MAX_SIZE
    mov     r3, r4
    bl      FILE_READ
    mov     r1, #RPC_TIMEOUT
    bl      RPC_WAIT
    pop     {r1, lr}
    bx      lr

try_close:
    push    {r1, lr}
    ldr     r0, =sd_handle
    ldr     r0, [r0]
    mov     r1, #0
    bl      FILE_CLOSE
    mov     r1, #RPC_TIMEOUT
    bl      RPC_WAIT
    pop     {r1, lr}
    bx      lr

@ ---------------------------------------------------------------------------
@ cache_flush_range(r0=start address, r1=length) -- see sd-card-app/loader_hook.s's own copy
@ of this routine for the full explanation. Kept as a separate copy here rather than shared
@ across the two independently-linked example binaries.
cache_flush_range:
    add     r1, r0, r1
    bic     r0, r0, #0x1f
cfr_loop:
    mcr     p15, 0, r0, c7, c11, 1
    mcr     p15, 0, r0, c7, c5, 1
    add     r0, r0, #32
    cmp     r0, r1
    blo     cfr_loop
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 6
    dsb
    isb
    bx      lr

    .ltorg

    .align 2
sd_handle:
    .word 0
sd_read_actual:
    .word 0
    .align 2
sd_open_scratch:
    .space 36
    .align 2
app_path:
    .asciz "C:\\IC-7300\\APP.BIN"

    .align 2
@ Content only -- build.py copies these bytes into the firmware's own image (see the file
@ header comment) rather than using their linked addresses here. Keep both exactly 36 and
@ <=16 bytes respectively if either is ever edited, to match the ROM gap build.py reserves.
    .global menu_label
menu_label:
    .asciz "Homebrew Apps"

    .align 2
    .global sd_menu_list_v2
sd_menu_list_v2:
    .word (3 | (9    << 16))   @ Load Setting
    .word (3 | (8    << 16))   @ Save Setting
    .word (2 | (0x145 << 16))  @ Save Form
    .word (1 | (0x34 << 16))   @ SD Card Info
    .word (3 | (0x14 << 16))   @ Screen Capture View
    .word (3 | (0x12 << 16))   @ Firmware Update
    .word (3 | (0x16 << 16))   @ Format
    .word (3 | (0x17 << 16))   @ Unmount
    .word (3 | (0x881 << 16))  @ Homebrew Apps -- NEW

    .align 2
