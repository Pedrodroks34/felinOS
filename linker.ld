ENTRY(_start)

SECTIONS
{
    /* Low memory the application processors execute from. INIT resets an AP
     * to the real-mode reset vector, so the code has to sit below 1 MiB in
     * plain physical memory, and the 16-bit entry point has to be linkable
     * to a known address rather than copied in at runtime. The kernel's own
     * identity mapping already covers both pages. */
    . = 0x7000;
    .ap_handover ALIGN(8) : {
        *(.ap_handover)
    }

    . = 0x8000;
    .ap_tramp ALIGN(16) : {
        *(.ap_tramp)
    }

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
