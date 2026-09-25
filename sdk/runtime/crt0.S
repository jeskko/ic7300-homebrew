@ APP.BIN header + entry point. The loader checks the header, then calls _hb_start(api) on the
@ UI thread. We zero .bss and hand over to the C runtime (runtime.c), which runs main() as a
@ coroutine on the app's own stack.

    .syntax unified
    .arm

#include "hb/abi.h"

    .section .hb_header, "a"
    .word   HB_APP_MAGIC
    .word   HB_ABI_VERSION
    .word   _hb_start
    .word   __hb_image_end

    .text
    .global _hb_start
_hb_start:
    ldr     r1, =__bss_start
    ldr     r2, =__bss_end
    mov     r3, #0
1:  cmp     r1, r2
    strlo   r3, [r1], #4
    blo     1b
    b       hb_runtime_start        @ r0 = api, still intact; returns straight to the loader

@ void hb_coro_switch(struct hb_coro *from, struct hb_coro *to)
@ Saves callee-saved state on the current stack, stores sp in *from, loads sp from *to and
@ restores. Ten words (r12 as padding) keep sp 8-byte aligned. No VFP state: apps are built
@ soft-float and only ever switch from their own code, never from inside a firmware call.
    .global hb_coro_switch
hb_coro_switch:
    push    {r4-r12, lr}
    str     sp, [r0]
    ldr     sp, [r1]
    pop     {r4-r12, lr}
    bx      lr
