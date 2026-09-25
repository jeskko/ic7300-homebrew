/* APP.BIN layout: header first, then code/data; bss and the app stack follow, not stored in
 * the file. Everything must fit in HB_APP_REGION (sdk/include/hb/abi.h). */
MEMORY { APP (rwx) : ORIGIN = 0x20610000, LENGTH = 1M }

ENTRY(_hb_start)

SECTIONS
{
    .text : {
        KEEP(*(.hb_header))
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        . = ALIGN(4);
    } > APP

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > APP

    .stack (NOLOAD) : {
        . = ALIGN(8);
        . += 16K;
        __hb_stack_top = .;
    } > APP

    __hb_image_end = .;

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
