/* The loader is appended to body.bin at 0x20600000. Everything, bss included, is emitted into
 * the flat image so it arrives zeroed -- nothing initializes RAM past the appended bytes. */
SECTIONS
{
    . = 0x20600000;
    .text : {
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4);
    }
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
