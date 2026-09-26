#include "sh/cmds.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "lib/format.h"
#include "drivers/ata.h"
#include "drivers/pci.h"
#include "bcache.h"

static void print_device_row(struct stream *out, struct ata_device *dev) {
    char size[16];
    if (dev->sectors >= (1u << 23)) {
        snprintf(size, sizeof(size), "%uM", dev->sectors >> 11);
    } else {
        cmd_format_size(dev->sectors * ATA_SECTOR_SIZE, size, sizeof(size));
    }

    st_printf(out, "%-8s %-6s %-10s %-8s %s\n",
              dev->name,
              (dev->type == ATA_TYPE_ATAPI) ? "rom" : "disk",
              (dev->type == ATA_TYPE_ATAPI) ? "-" : size,
              (dev->channel == 0) ? "primary" : "second",
              dev->model);
}

int cmd_lsblk(int argc, char **argv, struct stream *in, struct stream *out) {
    if (ata_device_count() == 0) {
        st_puts(out, "No ATA devices detected.\n");
        return 0;
    }

    st_printf(out, "%-8s %-6s %-10s %-8s %s\n", "NAME", "TYPE", "SIZE", "BUS", "MODEL");

    for (int i = 0; i < ata_device_count(); i++) {
        struct ata_device *dev = ata_get_device(i);
        print_device_row(out, dev);

        if (dev->type != ATA_TYPE_ATA) {
            continue;
        }

        struct mbr_partition parts[4];
        int count = ata_read_partitions(dev, parts, 4);

        for (int p = 0; p < count; p++) {
            char size[16];
            cmd_format_size(parts[p].sectors * ATA_SECTOR_SIZE, size, sizeof(size));
            st_printf(out, "|-%s%u   %-6s %-10s %-8s %s\n",
                      dev->name, (uint32_t)(p + 1), "part", size,
                      parts[p].bootable == 0x80 ? "boot" : "-",
                      ata_partition_type_name(parts[p].type));
        }
    }
    return 0;
}

int cmd_blkid(int argc, char **argv, struct stream *in, struct stream *out) {
    if (ata_device_count() == 0) {
        st_puts(out, "No ATA devices detected.\n");
        return 0;
    }

    for (int i = 0; i < ata_device_count(); i++) {
        struct ata_device *dev = ata_get_device(i);
        st_printf(out, "/dev/%s: MODEL=\"%s\" SERIAL=\"%s\" FIRMWARE=\"%s\" SECTORS=%u TYPE=\"%s\"\n",
                  dev->name, dev->model, dev->serial, dev->firmware, dev->sectors,
                  (dev->type == ATA_TYPE_ATAPI) ? "atapi" : "ata");
    }
    return 0;
}

int cmd_fdisk(int argc, char **argv, struct stream *in, struct stream *out) {
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        if (!cmd_is_flag(argv[i])) {
            target = argv[i];
        }
    }

    for (int i = 0; i < ata_device_count(); i++) {
        struct ata_device *dev = ata_get_device(i);
        if (target && !ata_find(target)) {
            cmd_error(out, "fdisk", target, "no such device");
            return 1;
        }
        if (target && ata_find(target) != dev) {
            continue;
        }
        if (dev->type != ATA_TYPE_ATA) {
            continue;
        }

        char size[16];
        cmd_format_size(dev->sectors * ATA_SECTOR_SIZE, size, sizeof(size));
        st_printf(out, "Disk /dev/%s: %s, %u sectors of %u bytes\n",
                  dev->name, size, dev->sectors, (uint32_t)ATA_SECTOR_SIZE);
        st_printf(out, "Model: %s\n\n", dev->model);

        struct mbr_partition parts[4];
        int count = ata_read_partitions(dev, parts, 4);

        if (count <= 0) {
            st_puts(out, "No MBR partition table found.\n\n");
            continue;
        }

        st_printf(out, "%-10s %-6s %-10s %-10s %-10s %s\n",
                  "Device", "Boot", "Start", "End", "Sectors", "Type");
        for (int p = 0; p < count; p++) {
            st_printf(out, "/dev/%s%u %-6s %-10u %-10u %-10u %s\n",
                      dev->name, (uint32_t)(p + 1),
                      parts[p].bootable == 0x80 ? "*" : "",
                      parts[p].lba_start,
                      parts[p].lba_start + parts[p].sectors - 1,
                      parts[p].sectors,
                      ata_partition_type_name(parts[p].type));
        }
        st_putc(out, '\n');
    }
    return 0;
}

int cmd_lspci(int argc, char **argv, struct stream *in, struct stream *out) {
    int verbose = cmd_has_flag(argc, argv, "-v");
    int count = pci_device_count();

    if (count == 0) {
        st_puts(out, "No PCI devices detected.\n");
        return 0;
    }

    for (int i = 0; i < count; i++) {
        struct pci_device *dev = pci_get_device(i);
        st_printf(out, "%02x:%02x.%u %s: %s device %04x\n",
                  dev->bus, dev->slot, dev->func,
                  pci_class_name(dev->class_code, dev->subclass),
                  pci_vendor_name(dev->vendor), dev->device);

        if (verbose) {
            st_printf(out, "    vendor %04x  class %02x subclass %02x prog-if %02x rev %02x\n",
                      dev->vendor, dev->class_code, dev->subclass, dev->prog_if, dev->revision);
            st_printf(out, "    header type %02x  interrupt line %u\n",
                      dev->header_type, dev->irq);
        }
    }
    return 0;
}

static const char *arg_value(int argc, char **argv, const char *key) {
    size_t klen = strlen(key);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], key, klen) == 0 && argv[i][klen] == '=') {
            return argv[i] + klen + 1;
        }
    }
    return NULL;
}

int cmd_dd(int argc, char **argv, struct stream *in, struct stream *out) {
    const char *input = arg_value(argc, argv, "if");
    const char *output = arg_value(argc, argv, "of");
    const char *count_s = arg_value(argc, argv, "count");
    const char *skip_s = arg_value(argc, argv, "skip");
    const char *seek_s = arg_value(argc, argv, "seek");

    if (!input || !output) {
        cmd_error(out, "dd", NULL, "usage: dd if=<src> of=<dst> [count=n] [skip=n] [seek=n]");
        return 1;
    }

    uint32_t count = count_s ? (uint32_t)atoi(count_s) : 1;
    uint32_t skip = skip_s ? (uint32_t)atoi(skip_s) : 0;
    uint32_t seek = seek_s ? (uint32_t)atoi(seek_s) : 0;

    if (count == 0 || count > 2048) {
        cmd_error(out, "dd", NULL, "count must be between 1 and 2048 sectors");
        return 1;
    }

    struct vfs_file *src;
    struct vfs_file *dst;
    int r = vfs_open(input, VFS_O_READ, &src);

    if (r < 0) {
        cmd_vfs_error(out, "dd", input, r);
        return 1;
    }
    r = vfs_open(output, VFS_O_WRITE | VFS_O_CREATE | (seek == 0 ? VFS_O_TRUNC : 0), &dst);
    if (r < 0) {
        cmd_vfs_error(out, "dd", output, r);
        vfs_close(src);
        return 1;
    }

    uint32_t bytes = count * ATA_SECTOR_SIZE;
    uint8_t *buffer = (uint8_t *)kzalloc(bytes);
    if (!buffer) {
        cmd_error(out, "dd", NULL, "out of memory");
        vfs_close(src);
        vfs_close(dst);
        return 1;
    }

    vfs_seek(src, (uint64_t)skip * ATA_SECTOR_SIZE);
    vfs_seek(dst, (uint64_t)seek * ATA_SECTOR_SIZE);

    uint32_t total = 0;
    while (total < bytes) {
        int n = vfs_read(src, buffer + total, bytes - total);
        if (n < 0) {
            cmd_vfs_error(out, "dd", input, n);
            r = n;
            break;
        }
        if (n == 0) {
            break;
        }
        total += (uint32_t)n;
    }

    uint32_t done = 0;
    while (r >= 0 && done < total) {
        int n = vfs_write(dst, buffer + done, total - done);
        if (n <= 0) {
            cmd_vfs_error(out, "dd", output, n < 0 ? n : VFS_ENOSPC);
            r = -1;
            break;
        }
        done += (uint32_t)n;
    }

    vfs_close(src);
    vfs_close(dst);
    kfree(buffer);
    if (r < 0) {
        return 1;
    }

    st_printf(stream_console(), "%u sectors in, %u sectors out, %u bytes copied\n",
              count, count, total);
    return 0;
}

int cmd_bcache(int argc, char **argv, struct stream *in, struct stream *out) {
    struct bcache_stats st;

    bcache_get_stats(&st);
    uint32_t total = st.hits + st.misses;
    uint32_t pct = total ? (st.hits * 100u) / total : 0;

    st_printf(out, "lines:      %u (%u KB used of %u KB)\n",
              st.lines, st.valid * 4u, st.lines * 4u);
    st_printf(out, "hits:       %u\n", st.hits);
    st_printf(out, "misses:     %u\n", st.misses);
    st_printf(out, "hit rate:   %u%%\n", pct);
    st_printf(out, "evictions:  %u\n", st.evictions);
    st_printf(out, "bypassed:   %u (not a single 4 KB block)\n", st.bypassed);
    return 0;
}
