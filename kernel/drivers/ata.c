#include "drivers/ata.h"
#include "drivers/pci.h"
#include "lib/string.h"
#include "io.h"
#include "idt.h"
#include "sched.h"
#include "drivers/pit.h"
#include "sync.h"
#include "console.h"

#define ATA_REG_DATA      0
#define ATA_REG_ERROR     1
#define ATA_REG_SECCOUNT  2
#define ATA_REG_LBA0      3
#define ATA_REG_LBA1      4
#define ATA_REG_LBA2      5
#define ATA_REG_HDDEVSEL  6
#define ATA_REG_COMMAND   7
#define ATA_REG_STATUS    7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_READ_PIO_EXT  0x24
#define ATA_CMD_WRITE_PIO_EXT 0x34
#define ATA_CMD_READ_DMA      0xC8
#define ATA_CMD_WRITE_DMA     0xCA
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_CACHE_FLUSH_EXT 0xEA
#define ATA_CMD_IDENTIFY   0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_SET_FEATURES 0xEF

#define ATA_ID_LBA48_SUPPORTED 0x0400
#define ATA_ID_DMA_SUPPORTED   0x0200

#define ATA_DCR_NIEN 0x02
#define ATA_DCR_SRST 0x04

#define ATA_MS_TO_TICKS(ms)     (((ms) * PIT_FREQUENCY + 999u) / 1000u)
#define ATA_IRQ_TIMEOUT_TICKS   ATA_MS_TO_TICKS(5000u)
#define ATA_FLUSH_TIMEOUT_TICKS ATA_MS_TO_TICKS(30000u)

struct ata_channel {
    volatile int irq_fired;
    mutex_t lock;
    int uses_dma;
};

static struct ata_channel channels[2];
static struct ata_device devices[ATA_MAX_DEVICES];
static int device_count;

static struct pci_device *ide_pci_dev;
static uint8_t ide_irq[2];
static uint16_t ide_io_base[2];
static uint16_t ide_ctrl_base[2];
static uint32_t ide_bm_base;

static void ata_delay(struct ata_device *dev) {
    /* 400 ns minimum per ATA spec. On modern hardware a simple I/O wait
     * (or a few pause instructions) is more reliable than reading the
     * control register 4 times, which can be surprisingly slow on some
     * PCI bridges. */
    for (int i = 0; i < 4; i++) {
        io_wait();
    }
}

static int ata_wait_busy(uint16_t io_base) {
    uint32_t start = pit_ticks();
    uint32_t timeout = ATA_MS_TO_TICKS(10000);
    while ((pit_ticks() - start) < timeout) {
        uint8_t status = inb(io_base + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY)) {
            return 0;
        }
    }
    return -1;
}

static int ata_wait_drq(uint16_t io_base) {
    uint32_t start = pit_ticks();
    uint32_t timeout = ATA_MS_TO_TICKS(10000);
    while ((pit_ticks() - start) < timeout) {
        uint8_t status = inb(io_base + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) {
            return -1;
        }
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) {
            return 0;
        }
    }
    return -1;
}

static void ata_channel_irq(int ch) {
    channels[ch].irq_fired = 1;
    sched_wake((const void *)&channels[ch].irq_fired);
}

static void ata_irq_primary(struct regs *r) {
    ata_channel_irq(0);
}

static void ata_irq_secondary(struct regs *r) {
    ata_channel_irq(1);
}

static void ata_set_interrupts(struct ata_device *dev, int enable) {
    outb(dev->ctrl_base, enable ? 0x00 : ATA_DCR_NIEN);
}

static void ata_channel_lock(int ch) {
    mutex_lock(&channels[ch].lock);
    channels[ch].irq_fired = 0;
}

static void ata_channel_unlock(int ch) {
    channels[ch].irq_fired = 0;
    mutex_unlock(&channels[ch].lock);
}

static void ata_soft_reset(struct ata_device *dev) {
    outb(dev->ctrl_base, ATA_DCR_SRST | ATA_DCR_NIEN);
    uint32_t start = pit_ticks();
    while ((pit_ticks() - start) < ATA_MS_TO_TICKS(1)) {
        inb(dev->ctrl_base);
    }
    outb(dev->ctrl_base, ATA_DCR_NIEN);
    start = pit_ticks();
    while ((pit_ticks() - start) < ATA_MS_TO_TICKS(30)) {
        inb(dev->ctrl_base);
    }
    ata_wait_busy(dev->io_base);
    channels[dev->channel].irq_fired = 0;
}

static int ata_wait_irq(int ch, uint32_t timeout_ticks) {
    uint32_t deadline = pit_ticks() + timeout_ticks;
    int ok = 1;
    uint32_t f = irq_save();

    while (!channels[ch].irq_fired) {
        int32_t left = (int32_t)(deadline - pit_ticks());
        if (left <= 0) {
            ok = 0;
            break;
        }
        /* Clear the pending flag BEFORE sleeping, otherwise an interrupt
         * that arrives between the check and the wait is lost. */
        channels[ch].irq_fired = 0;
        irq_restore(f);
        sched_wait_on_timeout((const void *)&channels[ch].irq_fired, "disk", (uint32_t)left);
        f = irq_save();
    }
    irq_restore(f);
    return ok ? 0 : -1;
}

static int ata_wait_ready(struct ata_device *dev, int use_irq) {
    uint16_t io = dev->io_base;

    if (!use_irq) {
        return ata_wait_drq(io);
    }

    int timed_out = ata_wait_irq(dev->channel, ATA_IRQ_TIMEOUT_TICKS) < 0;

    uint8_t status = inb(io + ATA_REG_STATUS);
    if (status & ATA_SR_BSY) {
        if (timed_out || ata_wait_busy(io) < 0) {
            ata_soft_reset(dev);
            return -1;
        }
        status = inb(io + ATA_REG_STATUS);
    }
    if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        return -1;
    }
    if (!(status & ATA_SR_DRQ)) {
        return -1;
    }
    return 0;
}

static int ata_wait_complete(struct ata_device *dev, int use_irq, uint32_t timeout_ticks) {
    uint16_t io = dev->io_base;
    int timed_out = 0;

    if (use_irq) {
        timed_out = ata_wait_irq(dev->channel, timeout_ticks) < 0;
    }

    uint8_t status = inb(io + ATA_REG_STATUS);
    if (status & ATA_SR_BSY) {
        if (timed_out || ata_wait_busy(io) < 0) {
            ata_soft_reset(dev);
            return -1;
        }
        status = inb(io + ATA_REG_STATUS);
    }
    return (status & (ATA_SR_ERR | ATA_SR_DF)) ? -1 : 0;
}

static void copy_ata_string(char *dst, uint16_t *id, int word_offset, int words) {
    int j = 0;
    for (int i = 0; i < words; i++) {
        uint16_t w = id[word_offset + i];
        dst[j++] = (char)(w >> 8);
        dst[j++] = (char)(w & 0xFF);
    }
    dst[j] = '\0';
    while (j > 0 && (dst[j - 1] == ' ' || dst[j - 1] == '\0')) {
        dst[--j] = '\0';
    }
}

static int ata_identify(struct ata_device *dev) {
    uint16_t io = dev->io_base;
    uint16_t id[256];

    outb(io + ATA_REG_HDDEVSEL, (uint8_t)(0xA0 | (dev->slave << 4)));
    ata_delay(dev);

    outb(io + ATA_REG_SECCOUNT, 0);
    outb(io + ATA_REG_LBA0, 0);
    outb(io + ATA_REG_LBA1, 0);
    outb(io + ATA_REG_LBA2, 0);
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay(dev);

    if (inb(io + ATA_REG_STATUS) == 0) {
        return 0;
    }
    if (ata_wait_busy(io) < 0) {
        return 0;
    }

    uint8_t lba1 = inb(io + ATA_REG_LBA1);
    uint8_t lba2 = inb(io + ATA_REG_LBA2);
    uint8_t type = ATA_TYPE_ATA;

    if (lba1 == 0x14 && lba2 == 0xEB) {
        type = ATA_TYPE_ATAPI;
    } else if (lba1 == 0x69 && lba2 == 0x96) {
        type = ATA_TYPE_ATAPI;
    } else if (lba1 != 0 || lba2 != 0) {
        return 0;
    }

    if (type == ATA_TYPE_ATAPI) {
        outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
        ata_delay(dev);
    }

    if (ata_wait_drq(io) < 0) {
        if (type != ATA_TYPE_ATAPI) {
            return 0;
        }
        memset(id, 0, sizeof(id));
    } else {
        insw(io + ATA_REG_DATA, id, 256);
    }

    dev->type = type;
    dev->capabilities = id[49];
    dev->lba48 = (type == ATA_TYPE_ATA && (id[83] & ATA_ID_LBA48_SUPPORTED)) ? 1 : 0;
    dev->dma_supported = (type == ATA_TYPE_ATA && (id[63] & ATA_ID_DMA_SUPPORTED)) ? 1 : 0;

    if (dev->lba48) {
        uint64_t total = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                          ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
        dev->sectors = (total > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)total;
    } else {
        dev->sectors = ((uint32_t)id[61] << 16) | id[60];
    }

    copy_ata_string(dev->serial, id, 10, 10);
    copy_ata_string(dev->firmware, id, 23, 4);
    copy_ata_string(dev->model, id, 27, 20);

    if (dev->model[0] == '\0') {
        strcpy(dev->model, (type == ATA_TYPE_ATAPI) ? "ATAPI device" : "ATA device");
    }
    return 1;
}

static void ata_setup_dma_prdt(struct ata_device *dev, uint32_t lba, uint8_t count, void *buffer, int write) {
    uint32_t *prdt = (uint32_t *)ide_bm_base;
    uint32_t phys_buf = (uint32_t)(uintptr_t)buffer;
    uint32_t byte_count = count * 512;

    prdt[0] = phys_buf;
    prdt[1] = (byte_count & 0xFFFE) | 0x80000000;

    outl(ide_bm_base + 8, 0);
    outl(ide_bm_base + 4, (uint32_t)(uintptr_t)prdt);
}

static void ata_start_dma(struct ata_device *dev, int write) {
    uint8_t cmd = write ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA;
    if (dev->lba48) {
        cmd = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    }
    outb(dev->io_base + ATA_REG_COMMAND, cmd);
    outb(ide_bm_base, 0x01 | (write ? 0x08 : 0x00));
}

static int ata_wait_dma(struct ata_device *dev) {
    uint32_t start = pit_ticks();
    uint32_t timeout = ATA_IRQ_TIMEOUT_TICKS;

    while ((pit_ticks() - start) < timeout) {
        uint8_t status = inb(ide_bm_base + 2);
        if (status & 0x04) {
            outb(ide_bm_base, 0);
            uint8_t drive_status = inb(dev->io_base + ATA_REG_STATUS);
            return (drive_status & (ATA_SR_ERR | ATA_SR_DF)) ? -1 : 0;
        }
        if (channels[dev->channel].irq_fired) {
            channels[dev->channel].irq_fired = 0;
            outb(ide_bm_base, 0);
            uint8_t drive_status = inb(dev->io_base + ATA_REG_STATUS);
            return (drive_status & (ATA_SR_ERR | ATA_SR_DF)) ? -1 : 0;
        }
    }
    outb(ide_bm_base, 0);
    return -1;
}

static int ata_find_pci_controller(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *pci = pci_get_device(i);
        if (!pci) continue;

        if (pci->class_code == 0x01 && pci->subclass == 0x01) {
            ide_pci_dev = pci;
            /* The two channels sit on consecutive legacy IRQs (14 and 15).
             * Giving them the same line let the secondary's
             * irq_install_handler() overwrite the primary's handler, so
             * commands issued on the primary channel never got serviced. */
            ide_irq[0] = pci->irq;
            ide_irq[1] = pci->irq + 1;

            uint32_t bar0 = pci_read_config(pci->bus, pci->slot, pci->func, 0x10);
            uint32_t bar1 = pci_read_config(pci->bus, pci->slot, pci->func, 0x14);
            uint32_t bar2 = pci_read_config(pci->bus, pci->slot, pci->func, 0x18);
            uint32_t bar3 = pci_read_config(pci->bus, pci->slot, pci->func, 0x1C);
            uint32_t bar4 = pci_read_config(pci->bus, pci->slot, pci->func, 0x20);

            if (bar0 == 0 || bar0 == 0xFFFFFFFF) bar0 = 0x1F0;
            if (bar1 == 0 || bar1 == 0xFFFFFFFF) bar1 = 0x3F6;
            if (bar2 == 0 || bar2 == 0xFFFFFFFF) bar2 = 0x170;
            if (bar3 == 0 || bar3 == 0xFFFFFFFF) bar3 = 0x376;
            if (bar4 == 0 || bar4 == 0xFFFFFFFF) bar4 = 0;

            ide_io_base[0] = (uint16_t)(bar0 & 0xFFF0);
            ide_ctrl_base[0] = (uint16_t)(bar1 & 0xFFF0);
            ide_io_base[1] = (uint16_t)(bar2 & 0xFFF0);
            ide_ctrl_base[1] = (uint16_t)(bar3 & 0xFFF0);
            ide_bm_base = bar4 & 0xFFFFFFF0;

            pci_write_config(pci->bus, pci->slot, pci->func, 0x04, 0x07);

            klog("ata: PCI IDE at %02x:%02x.%x, IRQ %u, BMIDE at 0x%08x",
                 pci->bus, pci->slot, pci->func, pci->irq, ide_bm_base);
            return 1;
        }
    }
    return 0;
}

void ata_init(void) {
    device_count = 0;
    memset(devices, 0, sizeof(devices));
    memset(channels, 0, sizeof(channels));
    mutex_init(&channels[0].lock, "ata0", 0);
    mutex_init(&channels[1].lock, "ata1", 0);

    int has_pci = ata_find_pci_controller();

    if (has_pci) {
        irq_install_handler(ide_irq[0], ata_irq_primary);
        irq_install_handler(ide_irq[1], ata_irq_secondary);
    } else {
        ide_io_base[0] = 0x1F0;
        ide_ctrl_base[0] = 0x3F6;
        ide_io_base[1] = 0x170;
        ide_ctrl_base[1] = 0x376;
        ide_bm_base = 0;
        irq_install_handler(14, ata_irq_primary);
        irq_install_handler(15, ata_irq_secondary);
    }

    for (int channel = 0; channel < 2; channel++) {
        for (int slave = 0; slave < 2; slave++) {
            struct ata_device dev;
            memset(&dev, 0, sizeof(dev));
            dev.channel = (uint8_t)channel;
            dev.slave = (uint8_t)slave;
            dev.io_base = ide_io_base[channel];
            dev.ctrl_base = ide_ctrl_base[channel];

            outb(dev.ctrl_base, 0x02);

            if (ata_identify(&dev)) {
                dev.name[0] = 'h';
                dev.name[1] = 'd';
                dev.name[2] = (char)('a' + channel * 2 + slave);
                dev.name[3] = '\0';
                
                if (dev.dma_supported && ide_bm_base) {
                    channels[channel].uses_dma = 1;
                    outb(dev.io_base + ATA_REG_COMMAND, ATA_CMD_SET_FEATURES);
                    outb(dev.io_base + ATA_REG_SECCOUNT, 0x01);
                    ata_wait_busy(dev.io_base);
                    klog("ata: %s: DMA enabled", dev.name);
                }
                
                devices[device_count++] = dev;
            }
        }
    }
}

int ata_device_count(void) {
    return device_count;
}

struct ata_device *ata_get_device(int index) {
    if (index < 0 || index >= device_count) {
        return NULL;
    }
    return &devices[index];
}

struct ata_device *ata_find(const char *name) {
    if (strncmp(name, "/dev/", 5) == 0) {
        name += 5;
    }
    for (int i = 0; i < device_count; i++) {
        if (strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

int ata_read_sectors(struct ata_device *dev, uint32_t lba, uint8_t count, void *buffer) {
    if (!dev || dev->type != ATA_TYPE_ATA) {
        return -1;
    }
    uint16_t io = dev->io_base;
    uint16_t *buf = (uint16_t *)buffer;
    int ch = dev->channel;
    int use_irq = sched_can_sleep();
    int use_dma = channels[ch].uses_dma && ide_bm_base && count > 1;

    ata_channel_lock(ch);

    if (ata_wait_busy(io) < 0) {
        ata_channel_unlock(ch);
        return -1;
    }

    if (use_dma) {
        outb(io + ATA_REG_HDDEVSEL, (uint8_t)(0xE0 | (dev->slave << 4)));
        ata_set_interrupts(dev, use_irq);
        
        if (dev->lba48) {
            outb(io + ATA_REG_SECCOUNT, 0);
            outb(io + ATA_REG_LBA0, (uint8_t)((lba >> 24) & 0xFF));
            outb(io + ATA_REG_LBA1, 0);
            outb(io + ATA_REG_LBA2, 0);
            outb(io + ATA_REG_SECCOUNT, count);
            outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
            outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
            outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
        } else {
            outb(io + ATA_REG_SECCOUNT, count);
            outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
            outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
            outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
        }

        ata_setup_dma_prdt(dev, lba, count, buffer, 0);
        ata_start_dma(dev, 0);
        int result = ata_wait_dma(dev);
        ata_channel_unlock(ch);
        return result;
    }

    if (dev->lba48) {
        outb(io + ATA_REG_HDDEVSEL, (uint8_t)(0xE0 | (dev->slave << 4)));
        ata_set_interrupts(dev, use_irq);
        outb(io + ATA_REG_SECCOUNT, 0);
        outb(io + ATA_REG_LBA0, (uint8_t)((lba >> 24) & 0xFF));
        outb(io + ATA_REG_LBA1, 0);
        outb(io + ATA_REG_LBA2, 0);
        outb(io + ATA_REG_SECCOUNT, count);
        outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
        outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
        outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
        outb(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO_EXT);
    } else {
        outb(io + ATA_REG_HDDEVSEL, (uint8_t)(0xE0 | (dev->slave << 4) | ((lba >> 24) & 0x0F)));
        ata_set_interrupts(dev, use_irq);
        outb(io + ATA_REG_SECCOUNT, count);
        outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
        outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
        outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
        outb(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    }

    int sectors = (count == 0) ? 256 : count;
    int result = 0;
    for (int s = 0; s < sectors; s++) {
        if (ata_wait_ready(dev, use_irq) < 0) {
            result = -1;
            break;
        }
        insw(io + ATA_REG_DATA, buf + s * 256, 256);
        if (!use_irq) {
            ata_delay(dev);
        }
    }

    ata_channel_unlock(ch);
    return result;
}

int ata_write_nf(struct ata_device *dev, uint32_t lba, uint8_t count, const void *buffer) {
    if (!dev || dev->type != ATA_TYPE_ATA) {
        return -1;
    }
    uint16_t io = dev->io_base;
    const uint16_t *buf = (const uint16_t *)buffer;
    int ch = dev->channel;
    int use_irq = sched_can_sleep();
    int use_dma = channels[ch].uses_dma && ide_bm_base && count > 1;

    ata_channel_lock(ch);

    if (ata_wait_busy(io) < 0) {
        ata_channel_unlock(ch);
        return -1;
    }

    if (use_dma) {
        outb(io + ATA_REG_HDDEVSEL, (uint8_t)(0xE0 | (dev->slave << 4)));
        ata_set_interrupts(dev, use_irq);
        
        if (dev->lba48) {
            outb(io + ATA_REG_SECCOUNT, 0);
            outb(io + ATA_REG_LBA0, (uint8_t)((lba >> 24) & 0xFF));
            outb(io + ATA_REG_LBA1, 0);
            outb(io + ATA_REG_LBA2, 0);
            outb(io + ATA_REG_SECCOUNT, count);
            outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
            outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
            outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
        } else {
            outb(io + ATA_REG_SECCOUNT, count);
            outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
            outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
            outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
        }

        ata_setup_dma_prdt(dev, lba, count, (void *)buffer, 1);
        ata_start_dma(dev, 1);
        int result = ata_wait_dma(dev);
        ata_channel_unlock(ch);
        return result;
    }

    if (dev->lba48) {
        outb(io + ATA_REG_HDDEVSEL, (uint8_t)(0xE0 | (dev->slave << 4)));
        ata_set_interrupts(dev, use_irq);
        outb(io + ATA_REG_SECCOUNT, 0);
        outb(io + ATA_REG_LBA0, (uint8_t)((lba >> 24) & 0xFF));
        outb(io + ATA_REG_LBA1, 0);
        outb(io + ATA_REG_LBA2, 0);
        outb(io + ATA_REG_SECCOUNT, count);
        outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
        outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
        outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
        outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO_EXT);
    } else {
        outb(io + ATA_REG_HDDEVSEL, (uint8_t)(0xE0 | (dev->slave << 4) | ((lba >> 24) & 0x0F)));
        ata_set_interrupts(dev, use_irq);
        outb(io + ATA_REG_SECCOUNT, count);
        outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
        outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
        outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
        outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
    }

    int sectors = (count == 0) ? 256 : count;
    int result = 0;
    for (int s = 0; s < sectors; s++) {
        int ready = (s == 0) ? ata_wait_drq(io) : ata_wait_ready(dev, use_irq);
        if (ready < 0) {
            result = -1;
            break;
        }
        for (int i = 0; i < 256; i++) {
            outw(io + ATA_REG_DATA, buf[s * 256 + i]);
        }
        if (!use_irq) {
            ata_delay(dev);
        }
    }

    if (result == 0 && ata_wait_complete(dev, use_irq, ATA_IRQ_TIMEOUT_TICKS) < 0) {
        result = -1;
    }

    ata_channel_unlock(ch);
    return result;
}

int ata_flush(struct ata_device *dev) {
    if (!dev || dev->type != ATA_TYPE_ATA) {
        return -1;
    }
    uint16_t io = dev->io_base;
    int ch = dev->channel;
    int use_irq = sched_can_sleep();
    int result;

    ata_channel_lock(ch);

    if (ata_wait_busy(io) < 0) {
        ata_channel_unlock(ch);
        return -1;
    }

    outb(io + ATA_REG_HDDEVSEL, (uint8_t)(0xE0 | (dev->slave << 4)));
    ata_delay(dev);
    if (ata_wait_busy(io) < 0) {
        ata_channel_unlock(ch);
        return -1;
    }

    ata_set_interrupts(dev, use_irq);
    outb(io + ATA_REG_COMMAND, dev->lba48 ? ATA_CMD_CACHE_FLUSH_EXT : ATA_CMD_CACHE_FLUSH);
    if (!use_irq) {
        ata_delay(dev);
    }
    result = ata_wait_complete(dev, use_irq, ATA_FLUSH_TIMEOUT_TICKS);

    ata_channel_unlock(ch);
    return result;
}

int ata_write_sectors(struct ata_device *dev, uint32_t lba, uint8_t count, const void *buffer) {
    int r = ata_write_nf(dev, lba, count, buffer);
    if (r == 0) r = ata_flush(dev);
    return r;
}

int ata_read_partitions(struct ata_device *dev, struct mbr_partition *parts, int max) {
    uint8_t sector[ATA_SECTOR_SIZE];

    if (ata_read_sectors(dev, 0, 1, sector) < 0) {
        return -1;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return 0;
    }

    int found = 0;
    for (int i = 0; i < 4 && i < max; i++) {
        uint8_t *entry = &sector[446 + i * 16];
        uint8_t type = entry[4];
        if (type == 0) {
            continue;
        }
        parts[found].bootable = entry[0];
        parts[found].type = type;
        parts[found].lba_start = (uint32_t)entry[8] | ((uint32_t)entry[9] << 8) |
                                 ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
        parts[found].sectors = (uint32_t)entry[12] | ((uint32_t)entry[13] << 8) |
                                ((uint32_t)entry[14] << 16) | ((uint32_t)entry[15] << 24);
        found++;
    }
    return found;
}

const char *ata_partition_type_name(uint8_t type) {
    switch (type) {
        case 0x01: return "FAT12";
        case 0x04:
        case 0x06: return "FAT16";
        case 0x05:
        case 0x0F: return "Extended";
        case 0x07: return "NTFS/exFAT";
        case 0x0B:
        case 0x0C: return "FAT32";
        case 0x82: return "Linux swap";
        case 0x83: return "Linux";
        case 0x8E: return "Linux LVM";
        case 0xA5: return "FreeBSD";
        case 0xAF: return "HFS+";
        case 0xEE: return "GPT protective";
        case 0xEF: return "EFI system";
        case 0x7F: return "FelinOS";
        default: return "Unknown";
    }
}