#include "drivers/pci.h"
#include "lib/string.h"
#include "io.h"
#include "sync.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define PCI_MAX_DEVICES    64

static spinlock_t pci_lock = SPINLOCK_INIT("pci-config");
static struct pci_device devices[PCI_MAX_DEVICES];
static int device_count;

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) |
                                  (offset & 0xFC) | 0x80000000u);
    uint32_t f = spin_lock_irqsave(&pci_lock);
    outl(PCI_CONFIG_ADDRESS, address);
    uint32_t v = inl(PCI_CONFIG_DATA);
    spin_unlock_irqrestore(&pci_lock, f);
    return v;
}

void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) |
                                  (offset & 0xFC) | 0x80000000u);
    uint32_t f = spin_lock_irqsave(&pci_lock);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
    spin_unlock_irqrestore(&pci_lock, f);
}

struct pci_device *pci_find(uint16_t vendor, const uint16_t *wanted, int count) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].vendor != vendor) {
            continue;
        }
        for (int j = 0; j < count; j++) {
            if (devices[i].device == wanted[j]) {
                return &devices[i];
            }
        }
    }
    return NULL;
}

static void pci_probe(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t id = pci_read_config(bus, slot, func, 0x00);
    uint16_t vendor = (uint16_t)(id & 0xFFFF);

    if (vendor == 0xFFFF || device_count >= PCI_MAX_DEVICES) {
        return;
    }

    uint32_t class_reg = pci_read_config(bus, slot, func, 0x08);
    uint32_t header_reg = pci_read_config(bus, slot, func, 0x0C);
    uint32_t irq_reg = pci_read_config(bus, slot, func, 0x3C);

    struct pci_device *dev = &devices[device_count++];
    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    dev->vendor = vendor;
    dev->device = (uint16_t)(id >> 16);
    dev->revision = (uint8_t)(class_reg & 0xFF);
    dev->prog_if = (uint8_t)((class_reg >> 8) & 0xFF);
    dev->subclass = (uint8_t)((class_reg >> 16) & 0xFF);
    dev->class_code = (uint8_t)((class_reg >> 24) & 0xFF);
    dev->header_type = (uint8_t)((header_reg >> 16) & 0xFF);
    dev->irq = (uint8_t)(irq_reg & 0xFF);
}

void pci_scan(void) {
    device_count = 0;

    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read_config((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) {
                continue;
            }
            pci_probe((uint8_t)bus, (uint8_t)slot, 0);

            uint32_t header = pci_read_config((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            if ((header >> 16) & 0x80) {
                for (int func = 1; func < 8; func++) {
                    uint32_t fid = pci_read_config((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x00);
                    if ((fid & 0xFFFF) != 0xFFFF) {
                        pci_probe((uint8_t)bus, (uint8_t)slot, (uint8_t)func);
                    }
                }
            }
        }
    }
}

int pci_device_count(void) {
    return device_count;
}

struct pci_device *pci_get_device(int index) {
    if (index < 0 || index >= device_count) {
        return NULL;
    }
    return &devices[index];
}

const char *pci_vendor_name(uint16_t vendor) {
    switch (vendor) {
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "ATI/AMD";
        case 0x1234: return "QEMU";
        case 0x1AF4: return "Red Hat/Virtio";
        case 0x15AD: return "VMware";
        case 0x80EE: return "VirtualBox";
        case 0x10EC: return "Realtek";
        case 0x1106: return "VIA";
        case 0x1B36: return "Red Hat";
        case 0x106B: return "Apple";
        default: return "Unknown vendor";
    }
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
        case 0x00: return "Unclassified device";
        case 0x01:
            switch (subclass) {
                case 0x00: return "SCSI storage controller";
                case 0x01: return "IDE interface";
                case 0x05: return "ATA controller";
                case 0x06: return "SATA controller";
                case 0x08: return "NVMe controller";
                default: return "Mass storage controller";
            }
        case 0x02:
            return (subclass == 0x00) ? "Ethernet controller" : "Network controller";
        case 0x03:
            return (subclass == 0x00) ? "VGA compatible controller" : "Display controller";
        case 0x04: return "Multimedia controller";
        case 0x05: return "Memory controller";
        case 0x06:
            switch (subclass) {
                case 0x00: return "Host bridge";
                case 0x01: return "ISA bridge";
                case 0x04: return "PCI bridge";
                default: return "Bridge device";
            }
        case 0x07: return "Communication controller";
        case 0x08: return "System peripheral";
        case 0x09: return "Input device controller";
        case 0x0A: return "Docking station";
        case 0x0B: return "Processor";
        case 0x0C:
            switch (subclass) {
                case 0x03: return "USB controller";
                case 0x05: return "SMBus controller";
                default: return "Serial bus controller";
            }
        case 0x0D: return "Wireless controller";
        case 0x0E: return "Intelligent controller";
        case 0x0F: return "Satellite controller";
        case 0x10: return "Encryption controller";
        case 0x11: return "Signal processing controller";
        default: return "Unknown class";
    }
}
