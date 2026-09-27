ENTRY(_start)

/* Two LOAD segments rather than one RWX blob, so the linker does not warn
 * about a writable text segment and so the program headers describe the
 * image honestly. The kernel's ELF loader currently maps the whole image
 * read+write+execute and does not apply these per-segment permissions yet,
 * which is why putting .data on its own page costs nothing today; it is what
 * would let a future loader make .text read-only. */
SECTIONS
{
    . = 0x40000000;

    .text : {
        *(.text*)
        *(.rodata*)
    }

    . = ALIGN(0x1000);

    .data : {
        *(.data*)
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
