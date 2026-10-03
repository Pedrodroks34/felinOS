ENTRY(boot32_entry)

SECTIONS
{
    /* Low memory the application processors execute from. INIT resets an AP
     * to the real-mode reset vector, so the code has to sit below 1 MiB in
     * plain physical memory, and the 16-bit entry point has to be linkable
     * to a known address rather than copied in at runtime. The kernel's own
     * identity mapping already covers both pages.
     *
     * Note the Multiboot1 header (.multiboot) is NOT placed here but inside
     * .text at 1M below, so it lands well past the first 8192 bytes of the
     * file and is not a spec-valid Multiboot1 header. That is deliberate:
     * QEMU/SeaBIOS's -kernel loader ('make run-kernel', 'make test',
     * 'make run-smp') tries Multiboot1 before the Xen PVH note and picks
     * /rom@genroms/multiboot_dma.bin as soon as the header validates,
     * which then never reaches boot32_entry. Keeping it invalid makes
     * SeaBIOS use /rom@genroms/pvh.bin, as the PVH note below intends. */
    . = 0x7000;
    .ap_handover ALIGN(8) : {
        *(.ap_handover)
    }

    . = 0x8000;
    .ap_tramp ALIGN(16) : {
        *(.ap_tramp)
    }

    /* Xen PVH note, so QEMU's -kernel loader can boot the kernel with no
     * bootloader in the middle. It has to be a PT_NOTE segment, which is why
     * it is placed explicitly here instead of falling into the /DISCARD/ rule
     * for .note* below. The value in it is the absolute 32-bit entry address;
     * see boot.s. */
    .note.Xen ALIGN(4) : {
        *(.note.Xen)
    }

    . = 1M;

    kernel_start = .;

    .text ALIGN(4K) : {
        text_start = .;
        *(.multiboot)
        /* 32-bit protected-mode entry (PVH/multiboot). Must live up here
         * with the rest of the kernel, NOT in low memory: QEMU loads ELF
         * segments below 1 MiB as ROM blobs into RAM, and SeaBIOS then
         * reuses 0x7000-0x9000 as stack/scratch before pvh.bin jumps to the
         * entry point, so an entry placed there runs zeroed memory (#UD,
         * triple fault). The AP trampoline pages above are only needed
         * much later. */
        *(.boot32)
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
