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
    if (result < 0) return -1;

    struct fat32_bpb *bpb = (struct fat32_bpb *)bpb_buf;

    /* Validate BPB */
    if (bpb->bytes_per_sector != 512) return -1;
    if (bpb->num_fats == 0) return -1;
    if (bpb->total_sectors_32 == 0) return -1;
    if (bpb->sectors_per_fat_32 == 0) return -1;

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
    if (!h->fat_cache) return -1;
    h->fat_cache_cluster = 0xFFFFFFFF;
    h->fat_dirty = 0;

    fat32_handle_count++;
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
