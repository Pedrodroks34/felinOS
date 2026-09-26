#ifndef FELINOS_ATA_H
#define FELINOS_ATA_H

#include <stdint.h>

#define ATA_MAX_DEVICES 4
#define ATA_SECTOR_SIZE 512

#define ATA_TYPE_NONE  0
#define ATA_TYPE_ATA   1
#define ATA_TYPE_ATAPI 2

struct ata_device {
    uint8_t type;
    uint8_t channel;
    uint8_t slave;
    uint16_t io_base;
    uint16_t ctrl_base;
    uint16_t capabilities;
    uint8_t lba48;
    uint32_t sectors;
    char name[8];
    char model[41];
    char serial[21];
    char firmware[9];
};

struct mbr_partition {
    uint8_t bootable;
    uint8_t type;
    uint32_t lba_start;
    uint32_t sectors;
};

void ata_init(void);
int ata_device_count(void);
struct ata_device *ata_get_device(int index);
struct ata_device *ata_find(const char *name);
int ata_read_sectors(struct ata_device *dev, uint32_t lba, uint8_t count, void *buffer);
int ata_write_sectors(struct ata_device *dev, uint32_t lba, uint8_t count, const void *buffer);
int ata_write_nf(struct ata_device *dev, uint32_t lba, uint8_t count, const void *buffer);
int ata_flush(struct ata_device *dev);
int ata_read_partitions(struct ata_device *dev, struct mbr_partition *parts, int max);
const char *ata_partition_type_name(uint8_t type);

#endif
