#!/bin/sh
# Build felinos.iso.
#
# grub-mkrescue is the happy path: it emits both a BIOS and a UEFI boot image.
# The UEFI half formats a FAT image with mtools (mformat), so on a machine that
# ships grub-pc-bin without mtools it aborts with "mformat invocation failed"
# and no ISO at all, even though a BIOS ISO is perfectly buildable there.
#
# So: use grub-mkrescue when the whole toolset is there, fall back to a
# BIOS-only ISO assembled from grub-mkimage + xorriso, and when neither path can
# run, name the missing package instead of letting the tool report it opaquely.
#
# usage: tools/mkiso.sh <gato.bin> <grub.cfg> <out.iso>

set -e

BIN=$1
CFG=$2
ISO=$3

STAGE=iso
ETORITO=$STAGE/boot/grub/eltorito.img

# Where the host keeps the BIOS GRUB modules. grub-mkrescue takes -d for this
# too, but only the fallback needs to know the path.
BIOS_DIR=${GRUB_BIOS_DIR:-/usr/lib/grub/i386-pc}

# Enough to read the multiboot config and hand over to the kernel. 'multiboot'
# is the one that matters; the rest is the minimum GRUB wants to be usable.
BIOS_MODULES="biosdisk iso9660 normal configfile echo multiboot test terminal serial"

have() { command -v "$1" >/dev/null 2>&1; }

# Does this GRUB installation actually ship the BIOS modules we would need?
# Some distros have i386-pc present but empty, and grub-mkimage then fails on
# the first module name rather than saying which directory is wrong.
bios_modules_present() {
    [ -d "$BIOS_DIR" ] || return 1
    for m in biosdisk iso9660 normal; do
        [ -f "$BIOS_DIR/$m.mod" ] || [ -f "$BIOS_DIR/$m.img" ] || return 1
    done
    return 0
}

stage() {
    mkdir -p "$STAGE/boot/grub"
    cp "$BIN" "$STAGE/boot/gato.bin"
    cp "$CFG" "$STAGE/boot/grub/grub.cfg"
    # A stale zero-length eltorito.img from an earlier failed run is what makes
    # xorriso report "Boot image file is empty" instead of doing the work.
    rm -f "$ETORITO"
}

report_missing() {
    echo "mkiso: cannot build $ISO on this machine." >&2
    echo >&2
    if ! have xorriso; then
        echo "  missing: xorriso          (Debian/Ubuntu: xorriso, Arch: libisoburn)" >&2
    fi
    if ! have grub-mkrescue; then
        echo "  missing: grub-mkrescue    (Debian/Ubuntu: grub-common grub-pc-bin," >&2
        echo "                           Arch: grub, Arch: extra/grub-bios)" >&2
    fi
    if ! have mformat; then
        echo "  missing: mformat          (needed by grub-mkrescue for the UEFI image;" >&2
        echo "                           without it only a BIOS ISO can be built)" >&2
    fi
    if ! bios_modules_present; then
        echo "  missing: BIOS GRUB modules in $BIOS_DIR" >&2
        echo "                           (Debian/Ubuntu: grub-pc-bin, Arch: extra/grub-bios)" >&2
        echo "                           override the path with GRUB_BIOS_DIR=..." >&2
    fi
    echo >&2
    echo "The kernel itself does not need any of this: 'make' builds $(basename "$BIN")," >&2
    echo "and 'make run-kernel' boots it with QEMU's -kernel." >&2
    exit 1
}

# grub-mkrescue emits one El Torito entry per platform it has modules for, so an
# ISO built from a GRUB with only efi-64 will not boot under SeaBIOS. Saying so
# is the whole point: 'make run' uses SeaBIOS and would otherwise fail with a
# bare "no bootable device".
announce() {
    if [ "$1" = bios ]; then
        echo "mkiso: wrote $ISO (BIOS boot only; no UEFI image without mtools)"
        return
    fi
    if bios_modules_present; then
        echo "mkiso: wrote $ISO (boots under both BIOS and UEFI firmware)"
    else
        echo "mkiso: wrote $ISO -- UEFI boot only, because there are no BIOS GRUB" >&2
        echo "       modules in $BIOS_DIR." >&2
        echo "       'make run' uses SeaBIOS and will not boot it; use 'make run-efi'," >&2
        echo "       or 'make run-kernel' which needs no bootloader at all." >&2
        echo "       For a BIOS ISO: USE=\"grub_platforms_pc\" emerge sys-boot/grub" >&2
        echo "       (Debian/Ubuntu: apt install grub-pc-bin)." >&2
    fi
}

stage

if have grub-mkrescue && have mformat; then
    echo "mkiso: building ISO via grub-mkrescue"
    grub-mkrescue -o "$ISO" "$STAGE"
    announce both
    exit 0
fi

if have grub-mkimage && have xorriso && bios_modules_present; then
    # BIOS only. Everything grub-mkrescue does for the BIOS half, minus the EFI
    # image that needs mtools.
    echo "mkiso: BIOS-only ISO via grub-mkimage + xorriso (mtools absent, so no UEFI boot)"
    grub-mkimage -O i386-pc-eltorito -p /boot/grub -o "$ETORITO" $BIOS_MODULES

    if [ ! -s "$ETORITO" ]; then
        echo "mkiso: grub-mkimage produced an empty boot image" >&2
        exit 1
    fi

    xorriso -as mkisofs -graft-points -o "$ISO" \
        -b boot/grub/eltorito.img \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        "$STAGE"

    rm -f "$ETORITO"
    announce bios
    exit 0
fi

report_missing
