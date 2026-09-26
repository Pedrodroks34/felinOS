#include "swap.h"
#include "paging.h"
#include "lib/string.h"
#include "vmm.h"

#define SWAP_MAGIC "GATOSWAP"
#define SWAP_VERSION 1u

struct swap_header {
    char magic[8];
    uint32_t version;
    uint32_t page_size;
    uint32_t slots;
    uint32_t first_lba;
    char label[SWAP_LABEL_LEN];
    uint8_t pad[ATA_SECTOR_SIZE - 8 - 16 - SWAP_LABEL_LEN];
};

static int swap_format_l(struct ata_device *dev, uint32_t megabytes, const char *label, int force);
static int swap_enable_l(struct ata_device *dev);
static int swap_disable_l(void);
static int swap_alloc_slot_l(uint32_t *slot_out);
static void swap_free_slot_l(uint32_t slot);
static void swap_get_info_l(struct swap_info *info);

static struct ata_device *swap_dev;
static uint32_t slot_bitmap[SWAP_MAX_SLOTS / 32];
static uint32_t total_slots;
static uint32_t used_slots;
static uint32_t peak_slots;
static uint32_t search_from;
static uint32_t read_count;
static uint32_t write_count;
static uint32_t error_count;
static char swap_label[SWAP_LABEL_LEN];

static inline void slot_set(uint32_t slot) {
    slot_bitmap[slot / 32] |= (1u << (slot % 32));
}

static inline void slot_clear(uint32_t slot) {
    slot_bitmap[slot / 32] &= ~(1u << (slot % 32));
}

static inline int slot_test(uint32_t slot) {
    return (slot_bitmap[slot / 32] >> (slot % 32)) & 1u;
}

void swap_init(void) {
    swap_dev = NULL;
    total_slots = 0;
    used_slots = 0;
    peak_slots = 0;
    search_from = 0;
    read_count = 0;
    write_count = 0;
    error_count = 0;
    swap_label[0] = '\0';
    memset(slot_bitmap, 0, sizeof(slot_bitmap));
}

static int area_is_blank(struct ata_device *dev) {
    static uint8_t buf[ATA_SECTOR_SIZE];

    for (uint32_t lba = 0; lba < 32; lba++) {
        if (ata_read_sectors(dev, lba, 1, buf) != 0) {
            return 0;
        }
        for (uint32_t i = 0; i < ATA_SECTOR_SIZE; i++) {
            if (buf[i]) {
                return 0;
            }
        }
    }
    return 1;
}

static int read_header(struct ata_device *dev, struct swap_header *hdr) {
    if (ata_read_sectors(dev, 0, 1, hdr) != 0) {
        return SWAP_EIO;
    }
    if (memcmp(hdr->magic, SWAP_MAGIC, 8) != 0) {
        return SWAP_EINVAL;
    }
    if (hdr->version != SWAP_VERSION || hdr->page_size != PAGE_SIZE) {
        return SWAP_EINVAL;
    }
    if (hdr->slots == 0 || hdr->slots > SWAP_MAX_SLOTS) {
        return SWAP_EINVAL;
    }
    return SWAP_OK;
}

static int swap_format_l(struct ata_device *dev, uint32_t megabytes, const char *label, int force) {
    static struct swap_header hdr;

    if (!dev || dev->type != ATA_TYPE_ATA || dev->sectors <= SWAP_FIRST_LBA) {
        return SWAP_ENODEV;
    }
    if (swap_dev == dev) {
        return SWAP_EBUSY;
    }

    struct swap_header probe;
    int existing = read_header(dev, &probe);
    if (!force && existing != SWAP_OK && !area_is_blank(dev)) {
        return SWAP_EEXIST;
    }

    uint32_t capacity = (dev->sectors - SWAP_FIRST_LBA) / SWAP_SLOT_SECTORS;
    uint32_t slots = capacity;
    if (megabytes) {
        uint32_t wanted = megabytes * (1024u * 1024u / PAGE_SIZE);
        if (wanted < slots) {
            slots = wanted;
        }
    }
    if (slots > SWAP_MAX_SLOTS) {
        slots = SWAP_MAX_SLOTS;
    }
    if (slots == 0) {
        return SWAP_ENOSPC;
    }

    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, SWAP_MAGIC, 8);
    hdr.version = SWAP_VERSION;
    hdr.page_size = PAGE_SIZE;
    hdr.slots = slots;
    hdr.first_lba = SWAP_FIRST_LBA;
    if (label && label[0]) {
        strncpy(hdr.label, label, SWAP_LABEL_LEN - 1);
    } else {
        strncpy(hdr.label, "gatoswap", SWAP_LABEL_LEN - 1);
    }

    if (ata_write_sectors(dev, 0, 1, &hdr) != 0) {
        return SWAP_EIO;
    }
    ata_flush(dev);
    return (int)slots;
}

static int swap_enable_l(struct ata_device *dev) {
    static struct swap_header hdr;

    if (!dev || dev->type != ATA_TYPE_ATA) {
        return SWAP_ENODEV;
    }
    if (swap_dev) {
        return SWAP_EBUSY;
    }
    int rc = read_header(dev, &hdr);
    if (rc != SWAP_OK) {
        return rc;
    }
    if (hdr.first_lba + hdr.slots * SWAP_SLOT_SECTORS > dev->sectors) {
        return SWAP_EINVAL;
    }

    memset(slot_bitmap, 0, sizeof(slot_bitmap));
    total_slots = hdr.slots;
    used_slots = 0;
    peak_slots = 0;
    search_from = 0;
    read_count = 0;
    write_count = 0;
    error_count = 0;
    memcpy(swap_label, hdr.label, SWAP_LABEL_LEN);
    swap_label[SWAP_LABEL_LEN - 1] = '\0';
    swap_dev = dev;
    return SWAP_OK;
}

static int swap_disable_l(void) {
    if (!swap_dev) {
        return SWAP_ENODEV;
    }
    if (used_slots) {
        return SWAP_EBUSY;
    }
    swap_dev = NULL;
    total_slots = 0;
    search_from = 0;
    swap_label[0] = '\0';
    return SWAP_OK;
}

int swap_autostart(void) {
    static struct swap_header hdr;

    if (swap_dev) {
        return SWAP_EBUSY;
    }
    for (int i = 0; i < ata_device_count(); i++) {
        struct ata_device *dev = ata_get_device(i);
        if (!dev || dev->type != ATA_TYPE_ATA) {
            continue;
        }
        if (read_header(dev, &hdr) == SWAP_OK) {
            return swap_enable(dev);
        }
    }
    return SWAP_ENODEV;
}

int swap_active(void) {
    return swap_dev != NULL;
}

static int swap_alloc_slot_l(uint32_t *slot_out) {
    if (!swap_dev) {
        return SWAP_ENODEV;
    }
    for (uint32_t i = 0; i < total_slots; i++) {
        uint32_t slot = search_from + i;
        if (slot >= total_slots) {
            slot -= total_slots;
        }
        if (!slot_test(slot)) {
            slot_set(slot);
            used_slots++;
            if (used_slots > peak_slots) {
                peak_slots = used_slots;
            }
            search_from = slot + 1;
            if (search_from >= total_slots) {
                search_from = 0;
            }
            if (slot_out) {
                *slot_out = slot;
            }
            return SWAP_OK;
        }
    }
    return SWAP_ENOSPC;
}

static void swap_free_slot_l(uint32_t slot) {
    if (!swap_dev || slot >= total_slots || !slot_test(slot)) {
        return;
    }
    slot_clear(slot);
    used_slots--;
}

int swap_write_page(uint32_t slot, const void *page) {
    if (!swap_dev) {
        return SWAP_ENODEV;
    }
    if (slot >= total_slots) {
        return SWAP_EINVAL;
    }
    uint32_t lba = SWAP_FIRST_LBA + slot * SWAP_SLOT_SECTORS;
    if (ata_write_nf(swap_dev, lba, SWAP_SLOT_SECTORS, page) != 0) {
        error_count++;
        return SWAP_EIO;
    }
    write_count++;
    return SWAP_OK;
}

int swap_read_page(uint32_t slot, void *page) {
    if (!swap_dev) {
        return SWAP_ENODEV;
    }
    if (slot >= total_slots) {
        return SWAP_EINVAL;
    }
    uint32_t lba = SWAP_FIRST_LBA + slot * SWAP_SLOT_SECTORS;
    if (ata_read_sectors(swap_dev, lba, SWAP_SLOT_SECTORS, page) != 0) {
        error_count++;
        return SWAP_EIO;
    }
    read_count++;
    return SWAP_OK;
}

static void swap_get_info_l(struct swap_info *info) {
    if (!info) {
        return;
    }
    memset(info, 0, sizeof(*info));
    info->active = swap_dev ? 1 : 0;
    if (swap_dev) {
        strncpy(info->device, swap_dev->name, sizeof(info->device) - 1);
    }
    strncpy(info->label, swap_label, SWAP_LABEL_LEN - 1);
    info->total_slots = total_slots;
    info->used_slots = used_slots;
    info->peak_slots = peak_slots;
    info->reads = read_count;
    info->writes = write_count;
    info->errors = error_count;
}

uint32_t swap_total_slots(void) {
    return total_slots;
}

uint32_t swap_used_slots(void) {
    return used_slots;
}

uint32_t swap_free_slots(void) {
    return total_slots - used_slots;
}

const char *swap_device_name(void) {
    return swap_dev ? swap_dev->name : "none";
}

const char *swap_strerror(int code) {
    switch (code) {
        case SWAP_OK: return "ok";
        case SWAP_ENODEV: return "no swap device";
        case SWAP_EIO: return "disk I/O error";
        case SWAP_EINVAL: return "invalid swap header";
        case SWAP_ENOSPC: return "swap area full";
        case SWAP_EBUSY: return "swap device busy";
        case SWAP_EEXIST: return "device holds data, use -f to overwrite";
        default: return "unknown error";
    }
}

/* Swap state is guarded by the (recursive) VMM mutex, the same one the
   reclaimer and the fault handler already hold when they call in. */
int swap_format(struct ata_device *dev, uint32_t megabytes, const char *label, int force) {
    vmm_lock();
    int r = swap_format_l(dev, megabytes, label, force);
    vmm_unlock();
    return r;
}

int swap_enable(struct ata_device *dev) {
    vmm_lock();
    int r = swap_enable_l(dev);
    vmm_unlock();
    return r;
}

int swap_disable(void) {
    vmm_lock();
    int r = swap_disable_l();
    vmm_unlock();
    return r;
}

int swap_alloc_slot(uint32_t *slot_out) {
    vmm_lock();
    int r = swap_alloc_slot_l(slot_out);
    vmm_unlock();
    return r;
}

void swap_free_slot(uint32_t slot) {
    vmm_lock();
    swap_free_slot_l(slot);
    vmm_unlock();
}

void swap_get_info(struct swap_info *info) {
    vmm_lock();
    swap_get_info_l(info);
    vmm_unlock();
}

