/* FAT32 volume management.
 *
 * A blank image has no MBR, so the autoumount pass in kernel.c has nothing to
 * find and nothing mounts. That is what this command is for: mkfs.vfat lives
 * on the host, not in the kernel, so the volume is built here instead. */
#include "sh/cmds.h"
#include "drivers/ata.h"
#include "drivers/ahci.h"
#include "fs/fat32.h"
#include "fs/vfs.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "lib/format.h"

#define FAT32_SECTOR 512

/* Every device the kernel can read a volume from, flattened so format and
 * info do not each have to know about both buses. */
struct fat_target {
    void *dev;
    int type;                /* 0 = ATA, 1 = AHCI */
    uint32_t sectors;
    const char *name;
};

static int collect_targets(struct fat_target *out, int max) {
    int n = 0;

    for (int i = 0; i < ata_device_count() && n < max; i++) {
        struct ata_device *dev = ata_get_device(i);
        if (!dev || dev->type != ATA_TYPE_ATA) continue;
        out[n].dev = dev;
        out[n].type = 0;
        out[n].sectors = dev->sectors;
        out[n].name = dev->name;
        n++;
    }
    for (int i = 0; i < ahci_device_count() && n < max; i++) {
        struct ahci_device *dev = ahci_get_device(i);
        if (!dev) continue;
        out[n].dev = dev;
        out[n].type = 1;
        out[n].sectors = dev->sectors;
        out[n].name = dev->model;
        n++;
    }
    return n;
}

static int read_sector(struct fat_target *t, uint32_t lba, uint8_t *buf) {
    if (t->type == 0) {
        return ata_read_sectors((struct ata_device *)t->dev, lba, 1, buf);
    }
    return ahci_read_sectors((struct ahci_device *)t->dev, lba, 1, buf);
}

static struct fat_target *find_target(struct fat_target *list, int n, const char *name) {
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].name, name) == 0) {
            return &list[i];
        }
    }
    return 0;
}

static void print_info(struct stream *out, struct fat_target *t) {
    uint8_t sector[FAT32_SECTOR];
    struct mbr_partition parts[4];
    int count = 0;

    st_printf(out, "%s: %u sectors, %u MiB\n", t->name, t->sectors, t->sectors / 2048);

    if (read_sector(t, 0, sector) != 0) {
        st_puts(out, "  cannot read the first sector\n");
        return;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        st_puts(out, "  no partition table (blank image); run 'fat32 format' first\n");
        return;
    }

    if (t->type == 0) {
        count = ata_read_partitions((struct ata_device *)t->dev, parts, 4);
    } else {
        for (int p = 0; p < 4; p++) {
            const uint8_t *e = &sector[446 + p * 16];
            parts[p].type = e[4];
            parts[p].lba_start = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                                 ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
            parts[p].sectors = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                               ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
            if (parts[p].type) count++;
        }
    }

    int found = 0;
    for (int p = 0; p < count; p++) {
        if (parts[p].type != 0x0B && parts[p].type != 0x0C) continue;
        found++;
        char size[16];
        cmd_format_size64((uint64_t)parts[p].sectors * FAT32_SECTOR, size, sizeof(size));
        st_printf(out, "  partition %d: type 0x%02x, LBA %u, %u sectors, %s\n",
                  p, parts[p].type, parts[p].lba_start, parts[p].sectors, size);
    }
    if (!found) {
        st_puts(out, "  no FAT32 partition\n");
    }
}

static int do_format(struct stream *out, struct fat_target *t, const char *label, int force) {
    if (t->sectors < 64) {
        st_printf(out, "%s: only %u sectors, too small for a FAT32 volume\n", t->name, t->sectors);
        return 1;
    }

    char volume[12];
    if (!label) label = "GATO";
    for (int i = 0; i < 11; i++) {
        char c = label[i];
        if (!c) break;
        volume[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    volume[11] = 0;

    st_printf(out, "Creating a FAT32 volume on %s (%u MiB), label %s%s\n",
              t->name, t->sectors / 2048, volume, force ? ", forced" : "");

    /* The partition starts one MiB in: that leaves room for an MBR and a
     * partition table with a partition that is not 4 KiB aligned. */
    const uint32_t lba = FAT32_PARTITION_LBA;
    if (fat32_write_mbr(t->dev, t->type, t->sectors) != 0) {
        st_puts(out, "  failed to write the master boot record\n");
        return 1;
    }
    st_printf(out, "  wrote the MBR, partition 1 starts at LBA %u\n", lba);

    int r = fat32_format(t->dev, t->type, lba, t->sectors - lba, volume, force);
    if (r == -2) {
        st_puts(out, "  the device already holds data; pass -f to overwrite it\n");
        return 1;
    }
    if (r != 0) {
        st_puts(out, "  failed to write the FAT32 structures\n");
        return 1;
    }
    st_puts(out, "  wrote the boot sectors, both FATs and the root directory\n");
    st_puts(out, "Done. 'mount /mnt/fat0 fat32 hda1' or reboot to mount it.\n");
    return 0;
}

static void usage(struct stream *out) {
    st_puts(out,
            "Usage:\n"
            "  fat32 info [device...]        show the partition table of each device\n"
            "  fat32 format [-f] [-L label] <device>\n"
            "                               write an MBR and a FAT32 volume\n"
            "\n"
            "Devices are named as ATA reports them (hda, hdb) or by AHCI model.\n"
            "-f overwrites a device that already looks like it has a partition\n"
            "table. The volume is not mounted; use mount for that.\n");
}

int cmd_fat32(int argc, char **argv, struct stream *in, struct stream *out) {
    (void)in;

    struct fat_target list[16];
    int n = collect_targets(list, 16);
    if (n == 0) {
        st_puts(out, "No ATA or AHCI devices found.\n");
        return 1;
    }

    const char *sub = argc > 1 ? argv[1] : "info";

    if (strcmp(sub, "info") == 0) {
        int shown = 0;
        for (int i = 2; i < argc; i++) {
            struct fat_target *t = find_target(list, n, argv[i]);
            if (!t) {
                st_printf(out, "fat32: no device named %s\n", argv[i]);
                return 1;
            }
            print_info(out, t);
            shown++;
        }
        if (!shown) {
            for (int i = 0; i < n; i++) {
                print_info(out, &list[i]);
            }
        }
        return 0;
    }

    if (strcmp(sub, "format") == 0) {
        int force = cmd_has_flag(argc, argv, "-f");
        const char *label = 0;
        const char *device = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
                label = argv[++i];
            } else if (!cmd_is_flag(argv[i])) {
                device = argv[i];
            }
        }
        if (!device) {
            usage(out);
            return 1;
        }
        struct fat_target *t = find_target(list, n, device);
        if (!t) {
            st_printf(out, "fat32: no device named %s\n", device);
            return 1;
        }
        return do_format(out, t, label, force);
    }

    if (strcmp(sub, "-h") == 0 || strcmp(sub, "--help") == 0) {
        usage(out);
        return 0;
    }

    st_printf(out, "fat32: unknown subcommand %s\n", sub);
    usage(out);
    return 1;
}
