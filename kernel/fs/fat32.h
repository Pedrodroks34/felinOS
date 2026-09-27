#ifndef FELINOS_FAT32_H
#define FELINOS_FAT32_H

#include <stdint.h>

/* FAT32 BPB (BIOS Parameter Block) - first sector of the partition */
struct fat32_bpb {
    uint8_t  jump[3];           /* 0x00: Jump instruction */
    uint8_t  oem[8];            /* 0x03: OEM name */
    uint16_t bytes_per_sector;  /* 0x0B: Bytes per sector */
    uint8_t  sectors_per_cluster; /* 0x0D: Sectors per cluster */
    uint16_t reserved_sectors;  /* 0x0E: Reserved sectors (FAT32 >= 32) */
    uint8_t  num_fats;          /* 0x10: Number of FATs */
    uint16_t root_entries;      /* 0x11: Root entries (0 for FAT32) */
    uint16_t total_sectors_16;  /* 0x13: Total sectors (0 for FAT32) */
    uint8_t  media_type;        /* 0x15: Media descriptor */
    uint16_t sectors_per_fat_16; /* 0x16: Sectors per FAT (0 for FAT32) */
    uint16_t sectors_per_track; /* 0x18: Sectors per track */
    uint16_t num_heads;         /* 0x1A: Number of heads */
    uint32_t hidden_sectors;    /* 0x1C: Hidden sectors */
    uint32_t total_sectors_32;  /* 0x20: Total sectors */
    /* FAT32 extended fields */
    uint32_t sectors_per_fat_32; /* 0x24: Sectors per FAT */
    uint16_t ext_flags;         /* 0x28: Extended flags */
    uint16_t fs_version;        /* 0x2A: Filesystem version */
    uint32_t root_cluster;      /* 0x2C: Root directory cluster */
    uint16_t fs_info_sector;    /* 0x30: FSInfo sector */
    uint16_t backup_boot_sector; /* 0x32: Backup boot sector */
    uint8_t  reserved[12];      /* 0x34: Reserved */
    uint8_t  drive_num;         /* 0x40: Drive number */
    uint8_t  reserved1;         /* 0x41: Reserved */
    uint8_t  boot_signature;    /* 0x42: Boot signature (0x29) */
    uint32_t volume_id;         /* 0x43: Volume serial number */
    uint8_t  volume_label[11];  /* 0x47: Volume label */
    uint8_t  fs_type[8];        /* 0x52: Filesystem type */
} __attribute__((packed));

#define FAT32_MAGIC          0xAA55
#define FAT32_BOOT_SIGNATURE 0x29
#define FAT32_CLUSTER_FREE   0x00000000
#define FAT32_CLUSTER_RESERVED 0x00000001
#define FAT32_CLUSTER_BAD    0x0FFFFFF7
#define FAT32_CLUSTER_EOC    0x0FFFFFF8
#define FAT32_CLUSTER_LAST   0x0FFFFFFF

/* FAT32 directory entry */
struct fat32_dir_entry {
    uint8_t  name[11];          /* 0x00: Short filename */
    uint8_t  attr;              /* 0x0B: Attributes */
    uint8_t  nt_reserved;       /* 0x0C: NT reserved */
    uint8_t  crt_time_tenth;    /* 0x0D: Creation time (tenths of second) */
    uint16_t crt_time;          /* 0x0E: Creation time */
    uint16_t crt_date;          /* 0x10: Creation date */
    uint16_t lst_acc_date;      /* 0x12: Last access date */
    uint16_t fst_clus_hi;       /* 0x14: First cluster (high 16 bits) */
    uint16_t wrt_time;          /* 0x16: Last write time */
    uint16_t wrt_date;          /* 0x18: Last write date */
    uint16_t fst_clus_lo;       /* 0x1A: First cluster (low 16 bits) */
    uint32_t file_size;         /* 0x1C: File size */
} __attribute__((packed));

/* FAT32 attributes */
#define FAT32_ATTR_READ_ONLY  0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LONG_NAME  0x0F

/* FAT32 long filename entry */
struct fat32_lfn_entry {
    uint8_t  order;             /* 0x00: Order */
    uint8_t  name1[10];         /* 0x01: Characters 1-5 (UTF-16) */
    uint8_t  attr;              /* 0x0B: Attributes (0x0F) */
    uint8_t  type;              /* 0x0C: Type (0 for LFN) */
    uint8_t  checksum;          /* 0x0D: Checksum of short name */
    uint8_t  name2[12];         /* 0x0E: Characters 6-11 (UTF-16) */
    uint16_t fst_clus_lo;       /* 0x1A: First cluster (always 0) */
    uint8_t  name3[4];          /* 0x1C: Characters 12-13 (UTF-16) */
} __attribute__((packed));

#define FAT32_LFN_LAST  0x40

/* Internal FAT32 handle */
struct fat32_handle {
    void *dev_opaque;           /* Opaque device pointer (ata_device or ahci_device) */
    int dev_type;               /* 0 = ATA, 1 = AHCI */
    uint32_t partition_start;   /* LBA of partition start */
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t sectors_per_fat;
    uint32_t total_sectors;
    uint32_t root_cluster;
    uint32_t data_start;        /* First data sector */
    uint32_t fat_start;         /* First FAT sector */
    uint8_t *fat_cache;         /* Cached FAT (1 sector) */
    uint32_t fat_cache_cluster;
    int fat_dirty;
};

/* FAT32 file handle */
struct fat32_file {
    struct fat32_handle *h;
    uint32_t dir_cluster;       /* Cluster of the directory */
    uint32_t entry_index;       /* Entry index within directory */
    struct fat32_dir_entry entry;
    uint32_t position;          /* Current file position */
    uint32_t cluster_chain[4096];
    uint32_t chain_len;
};

void fat32_init(void);
int fat32_mount(void *dev, int dev_type, uint32_t partition_lba);
int fat32_read(struct fat32_file *file, void *buffer, uint32_t count, uint32_t *read_bytes);
int fat32_open(struct fat32_handle *h, const char *path, struct fat32_file *file);
int fat32_opendir(struct fat32_handle *h, const char *path, struct fat32_dir_entry *entries, int max_entries, int *count);
int fat32_getinfo(struct fat32_handle *h, const char *path, struct fat32_dir_entry *entry);

/* Writes an MBR with one FAT32 partition covering the whole disk, then
 * formats it. total_sectors is the size of the disk in 512-byte sectors.
 * Refuses a disk that already carries a partition table unless force is set,
 * so a mistyped device is never destroyed silently. */
int fat32_write_mbr(void *dev, int dev_type, uint32_t total_sectors);
int fat32_format(void *dev, int dev_type, uint32_t partition_lba, uint32_t partition_sectors,
                  const char *label, int force);
/* Sector 0 is the MBR, so the volume starts at partition_lba + 1. */
#define FAT32_PARTITION_LBA 1

#endif
