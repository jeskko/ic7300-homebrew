@ IC-7300 SD-card app loader hook -- live-tested 2026-09-25 in qemu-machine, see README.md in
@ this directory for the full test log. Builds on sdk/examples/civ-hello-world/'s proven
@ injection mechanism (same main_idle_loop retarget, same trigger), but instead of a baked-in
@ payload, opens C:\IC-7300\APP.BIN via the same file wrapper functions firmware_update_main
@ itself uses to read its own update container, reads it into a fixed RAM address, and jumps
@ into it -- this is the actual "install an app = drop a file on the SD card, no reflash"
@ mechanism sdk/roadmap.md's reframing promised.
@
@ Confirmed calling convention (real, decompiled, from firmware_update_main's own working
@ file-read code, not guessed -- see notes/kernel-rtos.md's "SD-card file I/O" section):
@   FUN_200bc5f4(path, flags, &handle_out, scratch36)  -- open.  r0 return: pass to FUN_200214b0.
@   FUN_200bc6a4(handle, dest_buf, len, &actual_out)   -- read.  same check.
@   FUN_200bc64c(handle, 0)                             -- close. same check.
@   FUN_200214b0(ret, 0x46)                             -- wait for the RPC task to finish; r0=0
@                                                           on success.
@ (FUN_200bc754/cmd 0x13 is seek, not used here -- reading from a freshly-opened handle starts
@ at offset 0 already.) These are 4 of the 26 tiny per-command wrapper functions around
@ file_rpc_post_command that notes/kernel-rtos.md's task catalog already flagged existed but
@ didn't individually document -- open=0xf, read=0x11, seek=0x13, close=0x10, confirmed by
@ reading each wrapper's own hardcoded command-id argument.
@
@ CORRECTIONS from an adversarial review pass, 2026-09-25 (all verified against real
@ decompiles/listings, not just re-read from the earlier notes -- see sdk/app-loader-design.md's
@ open items for the ones still outstanding):
@
@ 1. FIXED, was a real bug: on a failed read, the read handler (0x200bb958) writes the
@    *negative error code* to `*actual_out` before returning -- the old check here
@    (`actual != 0`) treated that as "got some bytes" and called into 0x20610000 anyway, on
@    whatever was there (stale data from a previous run, or power-on garbage on real hardware).
@    Fixed below: check try_read's own RPC_WAIT result AND require `0 < actual <= APP_MAX_SIZE`
@    with a *signed* comparison.
@ 2. FIXED, was missing: cache maintenance before jumping into freshly-DMA'd/RPC'd-in code.
@    QEMU's TCG execution doesn't model I-cache/D-cache incoherency, so this was invisible in
@    every emulator test so far -- on real Cortex-A9 silicon, executing code the CPU just wrote
@    via a path that only guarantees D-side visibility (not fetched through the I-side) can run
@    stale cache lines. `cache_flush_range` below does the standard clean-D-to-PoU /
@    invalidate-I-to-PoU / BPIALL / DSB / ISB sequence over the bytes actually read, before the
@    `blx`. Still unverified live (QEMU can't show whether this was needed or whether it now
@    works correctly) -- flagged in sdk/app-loader-design.md's open items for a real-hardware or
@    cache-model check.
@ 3. NOT fixed here, documented instead: `RPC_WAIT` (`0x200214b0`) is not actually a timeout --
@    `0x46` only selects an error-code mapping inside it, and the wait itself blocks forever on
@    a kernel semaphore/event. If the SD-menu task's own RPC ring is ever full,
@    `file_rpc_post_command` returns `2`, which no real request handle will ever equal, so
@    `RPC_WAIT` spins forever waiting for a match that can't happen. Because this hook runs from
@    `main_idle_loop`, that would freeze the whole UI and the CI-V pump, not just this feature --
@    a real, currently-unmitigated risk. See sdk/app-loader-design.md's open items; the deeper
@    fix (running the load from `sd_menu_dispatch_task`'s own context instead, so a hang there
@    stays confined the way the firmware's own equivalent failures already are) is a real
@    redesign, not attempted in this pass.
@ 4. FIXED (twice -- see below): `try_open`/`try_read`/`try_close` used to `push {lr}` (one
@    register, 4 bytes) before calling into real firmware code, breaking 8-byte stack alignment
@    AAPCS expects at a public call boundary -- harmless for this specific callee chain
@    (checked), but a real trap for reuse. Now `push {r1, lr}` / `pop {r1, lr}`.
@
@    **Second correction, same pass**: the first attempt at this used `push {r0, lr}` /
@    `pop {r0, lr}` -- which is wrong, and was caught live (an emulator regression test after
@    this whole pass: the app opened correctly but silently produced no frame). `r0` carries
@    `RPC_WAIT`'s own return value out of these functions; popping it at the end overwrites that
@    result with whatever garbage was in `r0` when the function was first entered, so the
@    caller's success/failure check was reading noise instead of the real result. `r1` is safe
@    padding for the same alignment fix because none of these three functions' callers ever rely
@    on a return value coming back in `r1`.
@
@ Still open, not addressed by this pass (see sdk/app-loader-design.md): no SD-card-ready /
@ voice-recorder-busy gate before running (every sibling SD-menu row checks both; this hook
@ doesn't), and no size/magic/checksum validation of APP.BIN's own content before executing it.

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
@ Open, read, close C:\IC-7300\APP.BIN, then call it only if a real, positive byte count came
@ back. Fails closed at every step: a missing file, a failed open, a failed read, a negative
@ (error) or zero actual-length, or a read larger than the buffer all just return quietly --
@ live-verified for the missing-file case (booting with no APP.BIN produces no frame, no hang,
@ no crash); the failed-read-after-successful-open case is the bug this pass fixed and has not
@ itself been live-triggered (would need a real mid-read I/O failure to exercise).
load_and_run_app:
    push    {r4-r7, lr}

    bl      try_open
    cmp     r0, #0
    bne     lr_done             @ open failed -- nothing to close, nothing to run

    bl      try_read
    mov     r5, r0              @ save try_read's own RPC_WAIT result across try_close
    bl      try_close           @ always close what we opened, regardless of read result

    cmp     r5, #0
    bne     lr_done             @ the read RPC itself failed

    ldr     r1, =sd_read_actual
    ldr     r1, [r1]
    cmp     r1, #0
    ble     lr_done             @ signed: a negative errno or a zero-byte read -- nothing to run
    cmp     r1, #APP_MAX_SIZE
    bgt     lr_done             @ shouldn't happen (FILE_READ is bounded by the length we pass),
                                 @ but never trust a read result past our own buffer either

    mov     r6, r1              @ length actually read, saved across the call below
    ldr     r0, =APP_LOAD_ADDR
    mov     r1, r6
    bl      cache_flush_range   @ make sure the CPU's instruction fetch sees the bytes we just
                                 @ wrote, not stale I-cache content (see the file header comment)

    ldr     r0, =APP_LOAD_ADDR
    blx     r0                  @ call the loaded app; it returns via bx lr like any function

lr_done:
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
    str     r0, [r4]            @ zero it first so a failed post reads back as "nothing"
    ldr     r0, =sd_handle
    ldr     r0, [r0]
    ldr     r1, =APP_LOAD_ADDR
    ldr     r2, =APP_MAX_SIZE
    mov     r3, r4
    bl      FILE_READ
    mov     r1, #RPC_TIMEOUT
    bl      RPC_WAIT
    pop     {r1, lr}            @ discard the saved r0, return RPC_WAIT's own r0 unchanged
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
@ cache_flush_range(r0=start address, r1=length): clean D-cache to PoU and invalidate I-cache
@ to PoU over every 32-byte line touching [addr, addr+length), then invalidate the branch
@ predictor and issue the barriers needed before the CPU can safely fetch instructions from
@ that range. Standard ARMv7-A sequence for "the CPU just wrote code it's about to execute";
@ see the file header comment for why this was added and its current (unverified on real
@ hardware) status.
cache_flush_range:
    add     r1, r0, r1          @ r1 = end address (start + length)
    bic     r0, r0, #0x1f       @ align start down to a 32-byte line
cfr_loop:
    mcr     p15, 0, r0, c7, c11, 1  @ DCCMVAU -- clean D-cache line by MVA to PoU
    mcr     p15, 0, r0, c7, c5, 1   @ ICIMVAU -- invalidate I-cache line by MVA to PoU
    add     r0, r0, #32
    cmp     r0, r1
    blo     cfr_loop
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 6   @ BPIALL -- invalidate branch predictor
    dsb
    isb
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
