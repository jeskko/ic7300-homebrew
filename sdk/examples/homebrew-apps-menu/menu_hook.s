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
@     pointer retargeted to sd_menu_list_v2 below (the original 8 entries plus one new one).
@   - one new 20-byte catalog record, in the confirmed-unused padding right after the registry
@     table (0x20199758-0x201998cc): {action=homebrew_menu_action, query=0 (always selectable),
@     flags=0x00010700 (copied from the real "Format" row, plain-label kind), en=jp=menu_label}.
@
@ homebrew_menu_action itself is exactly sd-card-app's load_and_run_app logic (open/read/close
@ C:\IC-7300\APP.BIN via the same confirmed wrapper functions, call it if anything was read) --
@ sd-card-app's own app_main.s file works completely unmodified as the loaded APP.BIN here too.

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
    bl      try_close

    ldr     r1, =sd_read_actual
    ldr     r1, [r1]
    cmp     r1, #0
    beq     action_done

    ldr     r0, =APP_LOAD_ADDR
    blx     r0

action_done:
    pop     {r4-r7, lr}
    bx      lr

@ ---------------------------------------------------------------------------
try_open:
    push    {lr}
    ldr     r0, =app_path
    mov     r1, #0
    ldr     r2, =sd_handle
    ldr     r3, =sd_open_scratch
    bl      FILE_OPEN
    mov     r1, #RPC_TIMEOUT
    bl      RPC_WAIT
    pop     {lr}
    bx      lr

try_read:
    push    {lr}
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
    pop     {lr}
    bx      lr

try_close:
    push    {lr}
    ldr     r0, =sd_handle
    ldr     r0, [r0]
    mov     r1, #0
    bl      FILE_CLOSE
    mov     r1, #RPC_TIMEOUT
    bl      RPC_WAIT
    pop     {lr}
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
menu_label:
    .asciz "Homebrew Apps"

    .align 2
@ The SD CARD menu's real item list, plus one new entry. Original 8 read directly from the live
@ image at 0x201990bc (Load Setting, Save Setting, Save Form, SD Card Info, Screen Capture View,
@ Firmware Update, Format, Unmount) -- unchanged, in the same order -- plus a 9th entry pointing
@ at the new catalog record this build patches in (index 0x881, see build.py). Each entry is
@ {u8 type, u8 pad, u16 val} packed as one LE word: type | (val << 16).
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
