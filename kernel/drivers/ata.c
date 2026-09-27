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
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_CACHE_FLUSH_EXT 0xEA
#define ATA_CMD_IDENTIFY   0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1

#define ATA_ID_LBA48_SUPPORTED 0x0400

#define ATA_DCR_NIEN 0x02
#define ATA_DCR_SRST 0x04

/* How long a task may block waiting for the drive's interrupt before the
   command is declared dead. Per-sector PIO transfers finish in microseconds
   to milliseconds, so this only ever fires when an IRQ was lost or the drive
   stopped responding. A cache flush can legitimately take much longer. */
#define ATA_MS_TO_TICKS(ms)     (((ms) * PIT_FREQUENCY + 999u) / 1000u)
#define ATA_IRQ_TIMEOUT_TICKS   ATA_MS_TO_TICKS(5000u)
#define ATA_FLUSH_TIMEOUT_TICKS ATA_MS_TO_TICKS(30000u)

/*
 * One state block per ATA channel (0 = primary, 1 = secondary).
 * `irq_fired` is set by the IRQ handler and consumed by whichever task is
 * waiting on the channel; `busy` serialises access to the channel itself,
 * since the hardware can only have one command in flight at a time and,
 * unlike the old pure-polling driver, a task can now block (and let another
 * task run) in the middle of a transfer.
 */
struct ata_channel {
    volatile int irq_fired;
    mutex_t lock;
};

static struct ata_channel channels[2];
static struct ata_device devices[ATA_MAX_DEVICES];
static int device_count;

/* PCI IDE controller info */
static struct pci_device *ide_controller;
static uint8_t ide_irq_primary;
static uint8_t ide_irq_secondary;
static uint16_t ide_io_bases[2] = { 0x1F0, 0x170 };
static uint16_t ide_ctrl_bases[2] = { 0x3F6, 0x376 };

static void ata_delay(struct ata_device *dev) {
    for (int i = 0; i < 4; i++) {
        inb(dev->ctrl_base);
    }
}

/* Time-based timeout using PIT ticks instead of loop counter */
static int ata_wait_busy(uint16_t io_base) {
    uint32_t start = pit_ticks();
    uint32_t timeout = ATA_MS_TO_TICKS(10000);  /* 10 second timeout */
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
    uint32_t timeout = ATA_MS_TO_TICKS(10000);  /* 10 second timeout */
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

/*
 * IRQ handlers for the primary and secondary channels. All they
 * do is record that an interrupt happened and wake whoever is blocked on
 * it; the actual status/error handling happens in the waiting task, once
 * it is scheduled back in, by reading the regular Status register (which
 * is also what acknowledges the interrupt at the drive).
 */
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

/* nIEN only takes effect for the currently selected drive, so this must be
   called right after a Drive/Head select and before the command byte. */
static void ata_set_interrupts(struct ata_device *dev, int enable) {
    outb(dev->ctrl_base, enable ? 0x00 : ATA_DCR_NIEN);
}

/*
 * Only one command can be in flight per channel at a time. The old fully
 * polling driver never yielded the CPU mid-transfer, so two tasks could
 * never actually be "inside" ata_read/write_sectors concurrently. Now that
 * a task blocks (and lets others run) while waiting for an IRQ, a second
 * caller on the same channel has to wait its turn instead of racing the
 * first one on the hardware registers.
 */
static void ata_channel_lock(int ch) {
    /* The mutex is FIFO and makes its holder unkillable until it lets go: a
       SIGINT/SIGKILL arriving mid-transfer is recorded and acted on after
       ata_channel_unlock(), so the channel can never be left locked by a task
       that dies inside a wait. The wait is bounded by the timeout in
       ata_wait_irq(), so this cannot make a task unkillable for long. */
    mutex_lock(&channels[ch].lock);
    channels[ch].irq_fired = 0;
}

static void ata_channel_unlock(int ch) {
    channels[ch].irq_fired = 0;
    mutex_unlock(&channels[ch].lock);
}

/*
 * Pulse SRST on the channel to abort whatever the drive is stuck doing.
 * Used only after an IRQ wait timed out with the drive still BSY, so the
 * channel is left in a known-idle state for the next command instead of
 * inheriting a wedged one. This resets both devices on the channel; the
 * driver keeps no per-device configuration that a reset would lose.
 */
static void ata_soft_reset(struct ata_device *dev) {
    outb(dev->ctrl_base, ATA_DCR_SRST | ATA_DCR_NIEN);
    /* Hold SRST for >= 5us (use PIT for accurate timing) */
    uint32_t start = pit_ticks();
    while ((pit_ticks() - start) < ATA_MS_TO_TICKS(1)) {
        inb(dev->ctrl_base);
    }
    outb(dev->ctrl_base, ATA_DCR_NIEN);
    /* Give the drive ~2ms before polling BSY (real hardware may need up to 30ms) */
    start = pit_ticks();
    while ((pit_ticks() - start) < ATA_MS_TO_TICKS(30)) {
        inb(dev->ctrl_base);
    }
    ata_wait_busy(dev->io_base);
    channels[dev->channel].irq_fired = 0;
}

/*
 * Block until the channel's IRQ has fired (and consume it), or until
 * `timeout_ticks` have elapsed. Returns 0 if the IRQ was seen, -1 on timeout.
 * The timeout is what guarantees the caller -- who holds the channel lock --
 * always comes back and releases it, even if IRQ14/15 never arrives.
 */
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
        sched_wait_on_timeout((const void *)&channels[ch].irq_fired, "disk", (uint32_t)left);
    }
    channels[ch].irq_fired = 0;
    irq_restore(f);
    return ok ? 0 : -1;
}

/*
 * Wait for the drive to signal "data ready" (read) or "ready for more
 * data" (write, sector 2 onward). With no scheduler to hand the CPU to
 * (early boot, before sched_run) this falls back to the exact original
 * busy-poll so boot-time reads are unaffected. Once tasks are running, the
 * caller blocks via the scheduler and an interrupt wakes it back up,
 * leaving the CPU free for other tasks while the drive works.
 */
static int ata_wait_ready(struct ata_device *dev, int use_irq) {
    uint16_t io = dev->io_base;

    if (!use_irq) {
        return ata_wait_drq(io);
    }

    int timed_out = ata_wait_irq(dev->channel, ATA_IRQ_TIMEOUT_TICKS) < 0;

    uint8_t status = inb(io + ATA_REG_STATUS); /* also acks the IRQ at the drive */
    if (status & ATA_SR_BSY) {
        /* Either the IRQ was lost and the drive is genuinely hung, or this
           was a stale/early wakeup. Only give the latter a short poll. */
        if (timed_out || ata_wait_busy(io) < 0) {
            ata_soft_reset(dev);
            return -1;
        }
        status = inb(io + ATA_REG_STATUS);
    }
    /* A timeout with BSY clear means the IRQ was lost but the drive did
       finish; carry on and let the status bits decide. */
    if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        return -1;
    }
    if (!(status & ATA_SR_DRQ)) {
        return -1;
    }
    return 0;
}

/*
 * Wait for a command that ends without a data phase (the last sector of a
 * PIO write, CACHE FLUSH) to actually finish: BSY clear and no error. Per the
 * ATA PIO protocol the drive raises one more interrupt for this, after the
 * final sector has been transferred, so it has to be consumed here rather
 * than left pending (where it would look like a stale wakeup to the next
 * command) or raced against with a new command byte.
 */
static int ata_wait_complete(struct ata_device *dev, int use_irq, uint32_t timeout_ticks) {
    uint16_t io = dev->io_base;
    int timed_out = 0;

    if (use_irq) {
        timed_out = ata_wait_irq(dev->channel, timeout_ticks) < 0;
    }

    uint8_t status = inb(io + ATA_REG_STATUS); /* also acks the IRQ at the drive */
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

/* Find IDE controller via PCI and get its I/O ports and IRQ */
static int ata_find_pci_controller(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *pci = pci_get_device(i);
        if (!pci) continue;

        /* Class 0x01 = Mass Storage, Subclass 0x01 = IDE */
        if (pci->class_code == 0x01 && pci->subclass == 0x01) {
            ide_controller = pci;
            ide_irq_primary = pci->irq;
            ide_irq_secondary = pci->irq;

            /* Read BAR0-BAR3 for I/O ports */
            uint32_t bar0 = pci_read_config(pci->bus, pci->slot, pci->func, 0x10);
            uint32_t bar1 = pci_read_config(pci->bus, pci->slot, pci->func, 0x14);
            uint32_t bar2 = pci_read_config(pci->bus, pci->slot, pci->func, 0x18);
            uint32_t bar3 = pci_read_config(pci->bus, pci->slot, pci->func, 0x1C);

            /* If BARs are 0, use legacy defaults */
            if (bar0 == 0 || bar0 == 0xFFFFFFFF) bar0 = 0x1F0;
            if (bar1 == 0 || bar1 == 0xFFFFFFFF) bar1 = 0x3F6;
            if (bar2 == 0 || bar2 == 0xFFFFFFFF) bar2 = 0x170;
            if (bar3 == 0 || bar3 == 0xFFFFFFFF) bar3 = 0x376;

            ide_io_bases[0] = (uint16_t)(bar0 & 0xFFF0);
            ide_ctrl_bases[0] = (uint16_t)(bar1 & 0xFFF0);
            ide_io_bases[1] = (uint16_t)(bar2 & 0xFFF0);
            ide_ctrl_bases[1] = (uint16_t)(bar3 & 0xFFF0);

            klog("ata: PCI IDE controller at %02x:%02x.%x, IRQ %u, ports 0x%04x/0x%04x",
                 pci->bus, pci->slot, pci->func, pci->irq, ide_io_bases[0], ide_io_bases[1]);

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

    /* Try to find PCI IDE controller first */
    if (ata_find_pci_controller()) {
        /* Use PCI-discovered ports and IRQ */
        irq_install_handler(ide_irq_primary, ata_irq_primary);
        irq_install_handler(ide_irq_secondary, ata_irq_secondary);
    } else {
        /* Fall back to legacy ISA ports */
        irq_install_handler(14, ata_irq_primary);
        irq_install_handler(15, ata_irq_secondary);
    }

    for (int channel = 0; channel < 2; channel++) {
        for (int slave = 0; slave < 2; slave++) {
            struct ata_device dev;
            memset(&dev, 0, sizeof(dev));
            dev.channel = (uint8_t)channel;
            dev.slave = (uint8_t)slave;
            dev.io_base = ide_io_bases[channel];
            dev.ctrl_base = ide_ctrl_bases[channel];

            outb(dev.ctrl_base, 0x02);

            if (ata_identify(&dev)) {
                dev.name[0] = 'h';
                dev.name[1] = 'd';
                dev.name[2] = (char)('a' + channel * 2 + slave);
                dev.name[3] = '\0';
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

    ata_channel_lock(ch);

    if (ata_wait_busy(io) < 0) {
        ata_channel_unlock(ch);
        return -1;
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

    /* READ SECTORS raises DRQ (and, once interrupts are enabled, an IRQ)
       for every sector in the request, including the first. */
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

    ata_channel_lock(ch);

    if (ata_wait_busy(io) < 0) {
        ata_channel_unlock(ch);
        return -1;
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
        /* WRITE SECTORS never raises an IRQ for the first sector -- the
           drive is only ready once BSY clears and DRQ sets on its own,
           which the host has to poll for. From the second sector on, the
           IRQ means "previous sector consumed, send the next one". */
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

    /* The drive is still busy committing the last sector (and will raise one
       more IRQ when done). Don't hand the channel to anyone, or issue FLUSH,
       until that has happened. */
    if (result == 0 && ata_wait_complete(dev, use_irq, ATA_IRQ_TIMEOUT_TICKS) < 0) {
        result = -1;
    }

    ata_channel_unlock(ch);
    return result;
}

/*
 * Flush the drive's write cache. Takes the channel lock like any other
 * command (another task may be mid-transfer on this channel or on the other
 * drive), re-selects `dev` (the last command on the channel may have been for
 * its sibling), and only writes the command byte once the drive is idle.
 * Returns 0 on success, -1 on error or timeout.
 */
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
    if (ata_wait_busy(io) < 0) {   /* selected drive must not be BSY either */
        ata_channel_unlock(ch);
        return -1;
    }

    ata_set_interrupts(dev, use_irq);
    outb(io + ATA_REG_COMMAND, dev->lba48 ? ATA_CMD_CACHE_FLUSH_EXT : ATA_CMD_CACHE_FLUSH);
    if (!use_irq) {
        ata_delay(dev);            /* let BSY assert before polling it */
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
