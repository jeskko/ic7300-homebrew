@ The actual "app" -- live-tested 2026-09-25. This is what gets built into APP.BIN and dropped
@ on the SD card at C:\IC-7300\APP.BIN; loader_hook.s loads it fresh from the card at runtime
@ and calls it. Entered as a plain function call (blx) at its own fixed load address; returns
@ via bx lr like any ordinary function. No install step beyond copying the file -- this is the
@ whole point: iterate on this file, drop it back on the card, no firmware reflash needed.
@
@ Emits one CI-V frame with payload "SDAPP" (distinct from civ-hello-world's baked-in
@ "HOMEBREW", so a successful test unambiguously proves the frame came from the loaded file --
@ that string exists nowhere in the firmware image itself, only in this separate file).

    .syntax unified
    .arm
    .section .text

    .equ RXBUF,         0x20396ad4
    .equ RXBUF_REPLY,   0x20396b3a
    .equ RXBUF_READY,   0x20396b9e
    .equ DRV,           0x20390039
    .equ DRV_POS,       0x2039003b
    .equ OWN_CIV_ADDR,  0x203de525

    .global app_main

app_main:
    push    {r4, r5, r6, lr}

    ldr     r4, =RXBUF
    ldrb    r0, [r4]
    cmp     r0, #0
    bne     app_done

    ldr     r4, =RXBUF_READY
    ldrb    r0, [r4]
    cmp     r0, #0
    bne     app_done

    ldr     r4, =DRV
    ldrb    r0, [r4]
    and     r1, r0, #0x78
    cmp     r1, #0
    bne     app_done

    ldr     r5, =DRV_POS
    ldrb    r1, [r5]
    cmp     r1, #0
    bne     app_done

    ldr     r6, =RXBUF_REPLY
    mov     r1, #0xE0
    strb    r1, [r6], #1

    ldr     r1, =OWN_CIV_ADDR
    ldrb    r1, [r1]
    strb    r1, [r6], #1

    mov     r1, #0x51           @ distinct command byte from civ-hello-world's 0x50
    strb    r1, [r6], #1

    ldr     r1, =app_payload
    ldr     r2, =app_payload_end
copy_loop:
    cmp     r1, r2
    bge     copy_done
    ldrb    r3, [r1], #1
    strb    r3, [r6], #1
    b       copy_loop
copy_done:
    mov     r1, #0xFD
    strb    r1, [r6]

    ldr     r4, =RXBUF_READY
    mov     r1, #1
    strb    r1, [r4]

    ldr     r4, =DRV
    ldrb    r0, [r4]
    orr     r0, r0, #0x40
    strb    r0, [r4]

app_done:
    pop     {r4, r5, r6, lr}
    bx      lr

    .ltorg

    .align 2
app_payload:
    .ascii "SDAPP"
app_payload_end:

    .align 2
