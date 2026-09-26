ENTRY(_start)

SECTIONS
{
    . = 1M;

    kernel_start = .;

    .text ALIGN(4K) : {
        text_start = .;
        *(.multiboot)
        *(.text)
    }
    . = ALIGN(4K);
    text_end = .;

    .rodata ALIGN(4K) : {
        rodata_start = .;
        *(.rodata)
    }
    . = ALIGN(4K);
    rodata_end = .;

    .data ALIGN(4K) : {
        data_start = .;
        *(.data)
    }

    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss)
    }

    . = ALIGN(4K);
    data_end = .;
    kernel_end = ALIGN(4K);

    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment) }
}
