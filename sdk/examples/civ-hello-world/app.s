@ IC-7300 custom-code proof of concept: a main_idle_loop hook that emits one
@ CI-V frame on a front-panel key combo, then resumes normal radio operation.
@
@ Working, live-tested (2026-09-25) in qemu-machine, see README.md in this
@ directory for the full test log and how to reproduce it. This is the first
@ real, running artifact for sdk/roadmap.md's Phase 2/3 -- everything before
@ this was design and static analysis; this is where custom code actually
@ executes on (emulated) IC-7300 firmware for the first time.
@
@ Mechanism: retargets the existing `bl civ_tx_pump` call site inside
@ main_idle_loop (0x20052f64) to homebrew_tick below, which calls civ_tx_pump
@ itself first (preserving the original behaviour exactly) and then checks
@ for a debounced front-panel key combo (XFC + SPEECH/LOCK held together). On
@ a fresh press it stages one CI-V frame using the exact same rxbuf/drv
@ convention a real CI-V command handler uses (see notes/kernel-rtos.md's
@ "CI-V reply staging" section), then returns -- main_idle_loop never sees
@ any difference from an ordinary tick.
@
@ Linked at a fixed address, 0x20600000 -- NOT the end of body.bin's own
@ static image (0x20395b18), despite that looking like the obvious "free
@ space" choice. See sdk/app-loader-design.md's "Where appended code
@ actually has to live" section: the region starting right after the static
@ image is a runtime memory pool that gets progressively consumed as the
@ system boots (confirmed live: code placed at 0x20395b18, and even at
@ 0x20500000, reliably gets overwritten within seconds of boot). 0x20600000
@ was confirmed empirically to survive a full boot to steady state.

    .syntax unified
    .arm
    .section .text

    .equ CIV_TX_PUMP,   0x20011384   @ existing firmware function, unchanged
    .equ RXBUF,         0x20396ad4   @ rxbuf, fixed address
    .equ RXBUF_REPLY,   0x20396b3a   @ rxbuf + 0x66: reply payload staging area
    .equ RXBUF_READY,   0x20396b9e   @ rxbuf + 0xca: reply-ready flag
    .equ DRV,           0x20390039  @ CI-V driver state byte
    .equ DRV_POS,       0x2039003b  @ drv + 2: rx/tx byte position
    .equ OWN_CIV_ADDR,  0x203de525  @ radio's own configured CI-V address
    .equ FPBUF,         0x203dcab6  @ g_scif3_rx_status_buffer (front-panel report)

    .global homebrew_tick

@ ---------------------------------------------------------------------------
homebrew_tick:
    push    {r4, lr}
    bl      CIV_TX_PUMP             @ unchanged original behaviour
    bl      homebrew_check
    pop     {r4, lr}
    bx      lr

@ ---------------------------------------------------------------------------
@ Debounced combo check: XFC (0x0d bit 7) + SPEECH/LOCK (0x0e bit 6) held
@ together. Fires homebrew_emit_civ once per press (not once per tick).
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
    bne     hc_done                 @ already armed since last press, no-op
    mov     r0, #1
    strb    r0, [r6]
    bl      homebrew_emit_civ
    b       hc_done

hc_release:
    mov     r0, #0
    strb    r0, [r6]

hc_done:
    pop     {r4, r5, r6, lr}
    bx      lr

@ ---------------------------------------------------------------------------
@ Stage one CI-V frame [to][from][cmd]"HOMEBREW"[FD] the same way a real
@ handler does, then arm the pump. Fails closed: any precondition not met,
@ just return without touching anything (best-effort, never blocks).
homebrew_emit_civ:
    push    {r4, r5, r6, lr}
    @ NOTE: no IRQ masking here (cpsid/cpsie faulted as undefined-instruction when tried --
    @ this hook's execution context doesn't tolerate the dedicated CPS instruction, confirmed
    @ live via GDB/QMP: PSR showed und32 mode parked in the firmware's own "can't emulate this
    @ undefined instruction" trap loop at 0x200b9674). Best-effort instead: a real IRQ landing
    @ mid-write here can drop or corrupt this one attempt, never worse than that -- acceptable
    @ for a fail-closed, best-effort unsolicited frame. See sdk/app-loader-design.md.

    ldr     r4, =RXBUF
    ldrb    r0, [r4]
    cmp     r0, #0
    bne     hec_abort

    ldr     r4, =RXBUF_READY
    ldrb    r0, [r4]
    cmp     r0, #0
    bne     hec_abort

    ldr     r4, =DRV
    ldrb    r0, [r4]
    and     r1, r0, #0x78
    cmp     r1, #0
    bne     hec_abort

    ldr     r5, =DRV_POS
    ldrb    r1, [r5]
    cmp     r1, #0
    bne     hec_abort

    ldr     r6, =RXBUF_REPLY
    mov     r1, #0xE0
    strb    r1, [r6], #1

    ldr     r1, =OWN_CIV_ADDR
    ldrb    r1, [r1]
    strb    r1, [r6], #1

    mov     r1, #0x50
    strb    r1, [r6], #1

    ldr     r1, =civ_payload
    ldr     r2, =civ_payload_end
hec_copy:
    cmp     r1, r2
    bge     hec_copy_done
    ldrb    r3, [r1], #1
    strb    r3, [r6], #1
    b       hec_copy
hec_copy_done:
    mov     r1, #0xFD
    strb    r1, [r6]

    ldr     r4, =RXBUF_READY
    mov     r1, #1
    strb    r1, [r4]

    ldr     r4, =DRV
    ldrb    r0, [r4]
    orr     r0, r0, #0x40
    strb    r0, [r4]

hec_abort:
    pop     {r4, r5, r6, lr}
    bx      lr

    .ltorg

    .align 2
debounce_flag:
    .byte 0

    .align 2
civ_payload:
    .ascii "HOMEBREW"
civ_payload_end:

    .align 2
