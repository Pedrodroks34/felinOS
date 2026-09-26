#ifndef FELINOS_SWAP_H
#define FELINOS_SWAP_H

#include <stdint.h>
#include "drivers/ata.h"

#define SWAP_MAX_SLOTS 262144u
#define SWAP_SLOT_SECTORS 8u
#define SWAP_FIRST_LBA 64u
#define SWAP_LABEL_LEN 16

struct swap_info {
    int active;
    char device[8];
    char label[SWAP_LABEL_LEN];
    uint32_t total_slots;
    uint32_t used_slots;
    uint32_t peak_slots;
    uint32_t reads;
    uint32_t writes;
    uint32_t errors;
};

void swap_init(void);
int swap_format(struct ata_device *dev, uint32_t megabytes, const char *label, int force);
int swap_enable(struct ata_device *dev);
int swap_disable(void);
int swap_autostart(void);
int swap_active(void);
int swap_alloc_slot(uint32_t *slot_out);
void swap_free_slot(uint32_t slot);
int swap_write_page(uint32_t slot, const void *page);
int swap_read_page(uint32_t slot, void *page);
void swap_get_info(struct swap_info *info);
uint32_t swap_total_slots(void);
uint32_t swap_used_slots(void);
uint32_t swap_free_slots(void);
const char *swap_device_name(void);
const char *swap_strerror(int code);

#define SWAP_OK        0
#define SWAP_ENODEV   -1
#define SWAP_EIO      -2
#define SWAP_EINVAL   -3
#define SWAP_ENOSPC   -4
#define SWAP_EBUSY    -5
#define SWAP_EEXIST   -6

#endif
