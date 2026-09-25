@ IC-7300 SD-card app loader hook -- live-tested 2026-09-25 in qemu-machine, see README.md in
@ this directory for the full test log. Builds on sdk/examples/civ-hello-world/'s proven
@ injection mechanism (same main_idle_loop retarget, same trigger), but instead of a baked-in
@ payload, opens C:\IC-7300\APP.BIN via the same file wrapper functions firmware_update_main
@ itself uses to read its own update container, reads it into a fixed RAM address, and jumps
@ into it -- this is the actual "install an app = drop a file on the SD card, no reflash"
@ mechanism sdk/roadmap.md's reframing promised.
@
@ Confirmed calling convention (real, decompiled, from firmware_update_main's own working
@ file-read code, not guessed -- see notes/kernel-rtos.md if this gets its own writeup there):
@   FUN_200bc5f4(path, flags, &handle_out, scratch36)  -- open.  r0 return: pass to FUN_200214b0.
@   FUN_200bc6a4(handle, dest_buf, len, &actual_out)   -- read.  same check.
@   FUN_200bc64c(handle, 0)                             -- close. same check.
@   FUN_200214b0(ret, 0x46)                             -- wait for RPC completion; r0=0 on success.
@ (FUN_200bc754/cmd 0x13 is seek, not used here -- reading from a freshly-opened handle starts
@ at offset 0 already.) These are 4 of the 26 tiny per-command wrapper functions around
@ file_rpc_post_command that notes/kernel-rtos.md's task catalog already flagged existed but
@ didn't individually document -- open=0xf, read=0x11, seek=0x13, close=0x10, confirmed by
@ reading each wrapper's own hardcoded command-id argument.

    .syntax unified
    .arm
    .section .text

    .equ CIV_TX_PUMP,   0x20011384
    .equ FPBUF,         0x203dcab6

    .equ FILE_OPEN,     0x200bc5f4
    .equ FILE_READ,     0x200bc6a4
    .equ FILE_CLOSE,    0x200bc64c
    .equ RPC_WAIT,      0x200214b0
    .equ RPC_TIMEOUT,   0x46

    .equ APP_LOAD_ADDR, 0x20610000
    .equ APP_MAX_SIZE,  0x8000        @ 32 KB -- comfortably above a first-cut app, well inside
                                       @ confirmed-safe RAM (see sdk/app-loader-design.md)

    .global homebrew_tick

@ ---------------------------------------------------------------------------
homebrew_tick:
    push    {r4, lr}
    bl      CIV_TX_PUMP
    bl      homebrew_check
    pop     {r4, lr}
    bx      lr

@ ---------------------------------------------------------------------------
@ Debounced combo check: XFC (0x0d bit 7) + SPEECH/LOCK (0x0e bit 6). Same as civ-hello-world.
homebrew_check:
    push    {r4, r5, r6, lr}
    ldr     r4, =FPBUF
    ldrb    r0, [r4, #0x0d]
    and     r0, r0, #0x80
    ldrb    r1, [r4, #0x0e]
    and     r1, r1, #0x40
    cmp     r0, #0
    cmpne   r1, #0
    movne   r5, #1
    moveq   r5, #0

    ldr     r6, =debounce_flag
    ldrb    r0, [r6]
    cmp     r5, #0
    beq     hc_release

    cmp     r0, #0
    bne     hc_done
    mov     r0, #1
    strb    r0, [r6]
    bl      load_and_run_app
    b       hc_done

hc_release:
    mov     r0, #0
    strb    r0, [r6]

hc_done:
    pop     {r4, r5, r6, lr}
    bx      lr

@ ---------------------------------------------------------------------------
@ Open, read, close C:\IC-7300\APP.BIN, then call it if anything was read. Fails closed at
@ every step: a missing file, a failed open/read, or a zero-byte read all just return quietly
@ -- live-verified: booting with no APP.BIN on the card produces no frame, no hang, no crash,
@ and normal CI-V operation continues untouched.
load_and_run_app:
    push    {r4-r7, lr}

    bl      try_open
    cmp     r0, #0
    bne     lr_done             @ open failed -- nothing to close, nothing to run

    bl      try_read
    bl      try_close           @ always close what we opened, regardless of read result

    ldr     r1, =sd_read_actual
    ldr     r1, [r1]
    cmp     r1, #0
    beq     lr_done             @ read failed or zero-length -- nothing to run

    ldr     r0, =APP_LOAD_ADDR
    blx     r0                  @ call the loaded app; it returns via bx lr like any function

lr_done:
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
    str     r0, [r4]            @ zero it first so a failed read reads back as "nothing"
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
debounce_flag:
    .byte 0
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
