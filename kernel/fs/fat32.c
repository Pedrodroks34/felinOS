#include "fs/fat32.h"
#include "drivers/ata.h"
#include "drivers/ahci.h"
#include "lib/string.h"
#include "console.h"
#include "vmm.h"
#include "sync.h"

static struct fat32_handle fat32_handles[4];
static int fat32_handle_count;
static spinlock_t fat32_lock = SPINLOCK_INIT("fat32");

static uint32_t fat32_read_fat(struct fat32_handle *h, uint32_t cluster);
static int fat32_read_cluster(struct fat32_handle *h, uint32_t cluster, void *buffer);
static int fat32_get_next_cluster(struct fat32_handle *h, uint32_t cluster, uint32_t *next);
static int fat32_find_entry(struct fat32_handle *h, uint32_t dir_cluster, const char *name, struct fat32_dir_entry *entry, uint32_t *entry_index);
static int fat32_parse_path(struct fat32_handle *h, const char *path, uint32_t *dir_cluster, const char **filename);

void fat32_init(void) {
    fat32_handle_count = 0;
    memset(fat32_handles, 0, sizeof(fat32_handles));
}

int fat32_mount(void *dev, int dev_type, uint32_t partition_lba) {
    if (fat32_handle_count >= 4) return -1;

    /* The handle table and its FAT cache are shared by every task, so the
     * probe below runs under the driver lock. */
    spin_lock(&fat32_lock);
    if (fat32_handle_count >= 4) {
        spin_unlock(&fat32_lock);
        return -1;
    }
    struct fat32_handle *h = &fat32_handles[fat32_handle_count];
    memset(h, 0, sizeof(*h));

    h->dev_opaque = dev;
    h->dev_type = dev_type;
    h->partition_start = partition_lba;

    /* Read BPB */
    uint8_t bpb_buf[512];
    int result;
    if (dev_type == 0) {
        result = ata_read_sectors((struct ata_device *)dev, partition_lba, 1, bpb_buf);
    } else {
        result = ahci_read_sectors((struct ahci_device *)dev, partition_lba, 1, bpb_buf);
    }
    if (result < 0) { spin_unlock(&fat32_lock); return -1; }

    struct fat32_bpb *bpb = (struct fat32_bpb *)bpb_buf;

    /* Validate BPB */
    if (bpb->bytes_per_sector != 512) { spin_unlock(&fat32_lock); return -1; }
    if (bpb->num_fats == 0) { spin_unlock(&fat32_lock); return -1; }
    if (bpb->total_sectors_32 == 0) { spin_unlock(&fat32_lock); return -1; }
    if (bpb->sectors_per_fat_32 == 0) { spin_unlock(&fat32_lock); return -1; }

    h->bytes_per_sector = bpb->bytes_per_sector;
    h->sectors_per_cluster = bpb->sectors_per_cluster;
    h->reserved_sectors = bpb->reserved_sectors;
    h->num_fats = bpb->num_fats;
    h->sectors_per_fat = bpb->sectors_per_fat_32;
    h->total_sectors = bpb->total_sectors_32;
    h->root_cluster = bpb->root_cluster;

    /* Calculate FAT and data start */
    h->fat_start = partition_lba + h->reserved_sectors;
    h->data_start = h->fat_start + (h->num_fats * h->sectors_per_fat);

    /* Allocate FAT cache */
    h->fat_cache = vmm_alloc(512, VM_READ | VM_WRITE, "fat32-cache");
    if (!h->fat_cache) { spin_unlock(&fat32_lock); return -1; }
    h->fat_cache_cluster = 0xFFFFFFFF;
    h->fat_dirty = 0;

    fat32_handle_count++;
    spin_unlock(&fat32_lock);
    klog("fat32: mounted on %s, partition at LBA %u, %u sectors", 
         dev_type == 0 ? ((struct ata_device *)dev)->name : ((struct ahci_device *)dev)->model,
         partition_lba, h->total_sectors);

    return 0;
}

static uint32_t fat32_read_fat(struct fat32_handle *h, uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = h->fat_start + (fat_offset / h->bytes_per_sector);
    uint32_t entry_offset = fat_offset % h->bytes_per_sector;

    /* Check cache */
    if (h->fat_cache_cluster != fat_sector) {
        /* Write back dirty cache */
        if (h->fat_dirty) {
            if (h->dev_type == 0) {
                ata_write_sectors((struct ata_device *)h->dev_opaque, h->fat_cache_cluster, 1, h->fat_cache);
            } else {
                ahci_write_sectors((struct ahci_device *)h->dev_opaque, h->fat_cache_cluster, 1, h->fat_cache);
            }
            h->fat_dirty = 0;
        }

        /* Read new sector */
        if (h->dev_type == 0) {
            ata_read_sectors((struct ata_device *)h->dev_opaque, fat_sector, 1, h->fat_cache);
        } else {
            ahci_read_sectors((struct ahci_device *)h->dev_opaque, fat_sector, 1, h->fat_cache);
        }
        h->fat_cache_cluster = fat_sector;
    }

    uint32_t *fat_entry = (uint32_t *)(h->fat_cache + entry_offset);
    return *fat_entry & 0x0FFFFFFF;
}

static int fat32_get_next_cluster(struct fat32_handle *h, uint32_t cluster, uint32_t *next) {
    *next = fat32_read_fat(h, cluster);
    return 0;
}

static int fat32_read_cluster(struct fat32_handle *h, uint32_t cluster, void *buffer) {
    uint32_t first_sector = h->data_start + (cluster - 2) * h->sectors_per_cluster;
    uint32_t sectors = h->sectors_per_cluster;

    if (h->dev_type == 0) {
        return ata_read_sectors((struct ata_device *)h->dev_opaque, first_sector, sectors, buffer);
    } else {
        return ahci_read_sectors((struct ahci_device *)h->dev_opaque, first_sector, sectors, buffer);
    }
}

static int fat32_parse_path(struct fat32_handle *h, const char *path, uint32_t *dir_cluster, const char **filename) {
    *dir_cluster = h->root_cluster;

    /* Skip leading slash */
    while (*path == '/') path++;

    /* Find last component */
    const char *last_slash = strrchr(path, '/');
    if (last_slash) {
        /* Navigate to directory */
        uint32_t current = h->root_cluster;
        const char *p = path;
        while (p < last_slash) {
            /* Find next slash */
            const char *slash = strchr(p, '/');
            if (!slash || slash > last_slash) slash = last_slash;

            /* Extract component */
            char component[64];
            uint32_t len = slash - p;
            if (len >= sizeof(component)) return -1;
            memcpy(component, p, len);
            component[len] = '\0';

            /* Find entry */
            struct fat32_dir_entry entry;
            uint32_t entry_index;
            if (fat32_find_entry(h, current, component, &entry, &entry_index) < 0) return -1;
            if (!(entry.attr & FAT32_ATTR_DIRECTORY)) return -1;
            current = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;

            p = slash + 1;
        }
        *dir_cluster = current;
        *filename = last_slash + 1;
    } else {
        *filename = path;
    }

    return 0;
}

static int fat32_find_entry(struct fat32_handle *h, uint32_t dir_cluster, const char *name, struct fat32_dir_entry *entry, uint32_t *entry_index) {
    uint8_t cluster_buf[512 * 8];  /* Max 8 sectors per cluster */
    uint32_t cluster = dir_cluster;
    uint32_t index = 0;

    while (1) {
        /* Read cluster */
        if (fat32_read_cluster(h, cluster, cluster_buf) < 0) return -1;

        /* Search entries */
        uint32_t entries_per_cluster = (h->sectors_per_cluster * h->bytes_per_sector) / sizeof(struct fat32_dir_entry);
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            struct fat32_dir_entry *e = (struct fat32_dir_entry *)(cluster_buf + i * sizeof(struct fat32_dir_entry));

            if (e->name[0] == 0x00) return -1;  /* End of directory */
            if (e->name[0] == 0xE5) continue;    /* Deleted */
            if (e->attr == FAT32_ATTR_LONG_NAME) continue;  /* LFN entry */

            /* Compare name (case-insensitive) */
            char entry_name[12];
            memcpy(entry_name, e->name, 11);
            entry_name[11] = '\0';

            /* Pad with spaces */
            for (int j = 0; j < 11; j++) {
                if (entry_name[j] == ' ') entry_name[j] = '\0';
            }

            if (strcasecmp(entry_name, name) == 0) {
                *entry = *e;
                *entry_index = index + i;
                return 0;
            }
        }

        /* Next cluster */
        uint32_t next;
        if (fat32_get_next_cluster(h, cluster, &next) < 0) return -1;
        if (next >= FAT32_CLUSTER_EOC) return -1;
        cluster = next;
        index += entries_per_cluster;
    }

    return -1;
}

int fat32_open(struct fat32_handle *h, const char *path, struct fat32_file *file) {
    if (!h || !path || !file) return -1;

    uint32_t dir_cluster;
    const char *filename;
    if (fat32_parse_path(h, path, &dir_cluster, &filename) < 0) return -1;

    struct fat32_dir_entry entry;
    uint32_t entry_index;
    if (fat32_find_entry(h, dir_cluster, filename, &entry, &entry_index) < 0) return -1;

    memset(file, 0, sizeof(*file));
    file->h = h;
    file->dir_cluster = dir_cluster;
    file->entry_index = entry_index;
    file->entry = entry;
    file->position = 0;

    /* Build cluster chain */
    uint32_t cluster = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
    file->chain_len = 0;
    while (cluster < FAT32_CLUSTER_EOC && file->chain_len < 4096) {
        file->cluster_chain[file->chain_len++] = cluster;
        uint32_t next;
        if (fat32_get_next_cluster(h, cluster, &next) < 0) break;
        cluster = next;
    }

    return 0;
}

int fat32_read(struct fat32_file *file, void *buffer, uint32_t count, uint32_t *read_bytes) {
    if (!file || !buffer || !read_bytes) return -1;

    struct fat32_handle *h = file->h;
    uint8_t *buf = (uint8_t *)buffer;
    uint32_t total_read = 0;
    uint32_t file_size = file->entry.file_size;

    while (count > 0 && file->position < file_size) {
        uint32_t cluster_index = file->position / (h->sectors_per_cluster * h->bytes_per_sector);
        uint32_t offset_in_cluster = file->position % (h->sectors_per_cluster * h->bytes_per_sector);

        if (cluster_index >= file->chain_len) break;

        uint32_t cluster = file->cluster_chain[cluster_index];
        uint8_t cluster_buf[512 * 8];
        if (fat32_read_cluster(h, cluster, cluster_buf) < 0) break;

        uint32_t to_copy = count;
        uint32_t remaining_in_cluster = (h->sectors_per_cluster * h->bytes_per_sector) - offset_in_cluster;
        if (to_copy > remaining_in_cluster) to_copy = remaining_in_cluster;
        if (to_copy > file_size - file->position) to_copy = file_size - file->position;

        memcpy(buf + total_read, cluster_buf + offset_in_cluster, to_copy);
        file->position += to_copy;
        total_read += to_copy;
        count -= to_copy;
    }

    *read_bytes = total_read;
    return 0;
}

int fat32_opendir(struct fat32_handle *h, const char *path, struct fat32_dir_entry *entries, int max_entries, int *count) {
    if (!h || !path || !entries || !count) return -1;

    uint32_t dir_cluster;
    const char *filename;
    if (fat32_parse_path(h, path, &dir_cluster, &filename) < 0) return -1;

    /* If path is a file, use its parent directory */
    struct fat32_dir_entry entry;
    uint32_t entry_index;
    if (fat32_find_entry(h, dir_cluster, filename, &entry, &entry_index) == 0) {
        if (entry.attr & FAT32_ATTR_DIRECTORY) {
            dir_cluster = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
        } else {
            return -1;
        }
    }

    /* Read directory */
    uint8_t cluster_buf[512 * 8];
    uint32_t cluster = dir_cluster;
    int found = 0;

    while (found < max_entries) {
        if (fat32_read_cluster(h, cluster, cluster_buf) < 0) break;

        uint32_t entries_per_cluster = (h->sectors_per_cluster * h->bytes_per_sector) / sizeof(struct fat32_dir_entry);
        for (uint32_t i = 0; i < entries_per_cluster && found < max_entries; i++) {
            struct fat32_dir_entry *e = (struct fat32_dir_entry *)(cluster_buf + i * sizeof(struct fat32_dir_entry));

            if (e->name[0] == 0x00) goto done;
            if (e->name[0] == 0xE5) continue;
            if (e->attr == FAT32_ATTR_LONG_NAME) continue;
            if (e->name[0] == '.') continue;  /* Skip . and .. */

            entries[found++] = *e;
        }

        uint32_t next;
        if (fat32_get_next_cluster(h, cluster, &next) < 0) break;
        if (next >= FAT32_CLUSTER_EOC) break;
        cluster = next;
    }

done:
    *count = found;
    return 0;
}

int fat32_getinfo(struct fat32_handle *h, const char *path, struct fat32_dir_entry *entry) {
    if (!h || !path || !entry) return -1;

    uint32_t dir_cluster;
    const char *filename;
    if (fat32_parse_path(h, path, &dir_cluster, &filename) < 0) return -1;

    uint32_t entry_index;
    return fat32_find_entry(h, dir_cluster, filename, entry, &entry_index);
}

/* ---------- volume creation ---------- */

/* Both drivers move whole sectors, so the FAT and the metadata blocks are
 * built in a 512-byte staging buffer and flushed one sector at a time. */
static int fat32_dev_write(void *dev, int dev_type, uint32_t lba, uint8_t count, const void *buf) {
    if (dev_type == 0) {
        return ata_write_sectors((struct ata_device *)dev, lba, count, buf);
    }
    return ahci_write_sectors((struct ahci_device *)dev, lba, count, buf);
}

static int fat32_dev_read(void *dev, int dev_type, uint32_t lba, uint8_t count, void *buf) {
    if (dev_type == 0) {
        return ata_read_sectors((struct ata_device *)dev, lba, count, buf);
    }
    return ahci_read_sectors((struct ahci_device *)dev, lba, count, buf);
}

int fat32_write_mbr(void *dev, int dev_type, uint32_t total_sectors) {
    if (total_sectors < 64) return -1;

    uint8_t sec[512];
    memset(sec, 0, sizeof(sec));

    /* One partition of type 0x0C (FAT32 LBA) starting at sector 34, which
     * leaves room for the MBR and a 33-sector gap. */
    const uint32_t start = 34;
    if (total_sectors <= start + 4) return -1;

    uint8_t *p = sec + 446;
    p[0] = 0x00;                       /* not bootable on its own */
    p[1] = 0x00; p[2] = 0x02; p[3] = 0x00;   /* CHS start, LBA-only disk */
    p[4] = 0x0C;                       /* FAT32 LBA */
    p[5] = 0xFF; p[6] = 0xFF; p[7] = 0xFF;   /* CHS end: unknown */
    p[8]  = (uint8_t)(start);
    p[9]  = (uint8_t)(start >> 8);
    p[10] = (uint8_t)(start >> 16);
    p[11] = (uint8_t)(start >> 24);
    p[12] = (uint8_t)(total_sectors - start);
    p[13] = (uint8_t)((total_sectors - start) >> 8);
    p[14] = (uint8_t)((total_sectors - start) >> 16);
    p[15] = (uint8_t)((total_sectors - start) >> 24);

    sec[510] = 0x55;
    sec[511] = 0xAA;
    return fat32_dev_write(dev, dev_type, 0, 1, sec);
}

int fat32_format(void *dev, int dev_type, uint32_t partition_lba, uint32_t partition_sectors,
                 const char *label, int force) {
    if (!dev || partition_sectors < 64) return -1;

    /* Refuse to touch a disk that already looks like it has a partition
     * table, a boot signature or any data in the first MiB. */
    if (!force) {
        uint8_t first[512];
        if (fat32_dev_read(dev, dev_type, 0, 1, first) == 0) {
            if (first[510] == 0x55 && first[511] == 0xAA) {
                klog("fat32: %s already has a partition table, use -f to override",
                     dev_type == 0 ? ((struct ata_device *)dev)->name
                                   : ((struct ahci_device *)dev)->model);
                return -2;
            }
        }
    }

    /* Layout for a FAT32 volume: 1 boot sector, 2 FSInfo sectors, 1 backup
     * boot sector, then 32 reserved sectors, 2 FATs and the data area. One
     * sector per cluster keeps the geometry simple and the arithmetic exact
     * for volumes of any size. */
    const uint32_t reserved = 32;
    const uint32_t num_fats = 2;
    const uint32_t spc = 1;

    uint32_t fat_sectors = 1;
    for (int attempt = 0; attempt < 24; attempt++) {
        /* entries needed = data clusters + 2 (reserved), 4 bytes each */
        uint32_t meta = reserved + num_fats * fat_sectors;
        if (partition_sectors <= meta) return -1;
        uint32_t data_clusters = (partition_sectors - meta) / spc;
        uint32_t needed = ((data_clusters + 2) * 4 + 511) / 512;
        if (needed <= fat_sectors) {
            break;
        }
        fat_sectors = needed;
    }

    uint32_t fat_start = partition_lba + reserved;
    uint32_t data_start = fat_start + num_fats * fat_sectors;
    uint32_t data_clusters = (partition_lba + partition_sectors - data_start) / spc;
    if (data_clusters < 1) return -1;

    uint8_t sec[512];
    memset(sec, 0, sizeof(sec));

    /* --- boot sector --- */
    struct fat32_bpb *bpb = (struct fat32_bpb *)sec;
    bpb->jump[0] = 0xEB; bpb->jump[1] = 0x58; bpb->jump[2] = 0x90;
    memcpy(bpb->oem, "MSWIN4.1", 8);
    bpb->bytes_per_sector = 512;
    bpb->sectors_per_cluster = (uint8_t)spc;
    bpb->reserved_sectors = (uint16_t)reserved;
    bpb->num_fats = (uint8_t)num_fats;
    bpb->root_entries = 0;
    bpb->total_sectors_16 = 0;
    bpb->media_type = 0xF8;
    bpb->sectors_per_fat_16 = 0;
    bpb->sectors_per_track = 63;
    bpb->num_heads = 255;
    bpb->hidden_sectors = partition_lba;
    bpb->total_sectors_32 = partition_sectors;
    bpb->sectors_per_fat_32 = fat_sectors;
    bpb->ext_flags = 0;
    bpb->fs_version = 0;
    bpb->root_cluster = 2;
    bpb->fs_info_sector = 1;
    bpb->backup_boot_sector = 6;
    bpb->drive_num = 0x80;
    bpb->boot_signature = FAT32_BOOT_SIGNATURE;
    bpb->volume_id = 0x4741544Fu;   /* "GATO" */
    char lbl[11];
    memset(lbl, ' ', sizeof(lbl));
    if (label) {
        for (int i = 0; i < 11 && label[i]; i++) {
            char c = label[i];
            lbl[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        }
    }
    memcpy(bpb->volume_label, lbl, 11);
    memcpy(bpb->fs_type, "FAT32   ", 8);
    sec[510] = 0x55;
    sec[511] = 0xAA;

    if (fat32_dev_write(dev, dev_type, partition_lba, 1, sec) < 0) return -1;
    /* Backup boot sector at the sector named in the BPB. */
    if (fat32_dev_write(dev, dev_type, partition_lba + bpb->backup_boot_sector, 1, sec) < 0) return -1;

    /* --- FSInfo ---
     * Lead signature at 0, struct signature at 484, free cluster count at
     * 488, next free cluster at 492, trail signature at 508. */
    memset(sec, 0, sizeof(sec));
    sec[0] = 'R'; sec[1] = 'R'; sec[2] = 'a'; sec[3] = 'N';
    sec[484] = 'F'; sec[485] = 'S'; sec[486] = 'I'; sec[487] = 'n';
    sec[488] = 'F'; sec[489] = 'S'; sec[490] = 'I'; sec[491] = 'n';
    sec[492] = 'f'; sec[493] = 'a'; sec[494] = 't'; sec[495] = 'i';
    uint32_t *fsi = (uint32_t *)(sec + 488);
    fsi[0] = data_clusters - 1;   /* free cluster count */
    fsi[1] = 3;                   /* next free: cluster 2 is the root */
    sec[508] = 0x00; sec[509] = 0x00; sec[510] = 0x55; sec[511] = 0xAA;
    if (fat32_dev_write(dev, dev_type, partition_lba + 1, 1, sec) < 0) return -1;
    if (fat32_dev_write(dev, dev_type, partition_lba + 7, 1, sec) < 0) return -1;

    /* --- FATs: entry 0 carries the media type and the EOC marker --- */
    memset(sec, 0, sizeof(sec));
    sec[0] = 0xF8;
    sec[1] = 0xFF;
    sec[2] = 0xFF;
    sec[3] = 0xFF;
    /* cluster 1 is reserved by the spec */
    for (uint32_t f = 0; f < num_fats; f++) {
        if (fat32_dev_write(dev, dev_type, fat_start + f * fat_sectors, 1, sec) < 0) return -1;
    }

    /* --- root directory: one cluster, holding ".", ".." and the volume label --- */
    memset(sec, 0, sizeof(sec));
    struct fat32_dir_entry *de = (struct fat32_dir_entry *)sec;
    memset(de[0].name, ' ', 11);
    de[0].name[0] = '.';
    de[0].attr = FAT32_ATTR_DIRECTORY;
    de[0].fst_clus_hi = 0;
    de[0].fst_clus_lo = 0;

    de[1] = de[0];
    de[1].name[1] = '.';
    de[1].fst_clus_lo = 0;   /* ".." points at the root itself here */

    memset(de[2].name, ' ', 11);
    memcpy(de[2].name, lbl, 11);
    de[2].attr = FAT32_ATTR_VOLUME_ID;
    de[2].fst_clus_lo = 0;

    if (fat32_dev_write(dev, dev_type, data_start, 1, sec) < 0) return -1;

    klog("fat32: formatted %u sectors at LBA %u, %u sectors per FAT, %u data clusters",
         partition_sectors, partition_lba, fat_sectors, data_clusters);
    return 0;
}
