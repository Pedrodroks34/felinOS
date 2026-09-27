#include "drivers/ahci.h"
#include "drivers/pci.h"
#include "drivers/pit.h"
#include "lib/string.h"
#include "io.h"
#include "vmm.h"
#include "console.h"
#include "sync.h"
#include "sched.h"
#include "idt.h"

#define AHCI_BAR5_OFFSET 0x24
#define AHCI_PCI_CLASS 0x01
#define AHCI_PCI_SUBCLASS 0x06

static struct ahci_device ahci_devices[AHCI_MAX_PORTS];
static int ahci_dev_count;
static volatile struct ahci_hba_mem *hba;
static spinlock_t ahci_lock = SPINLOCK_INIT("ahci");

static uint8_t ahci_irq;
static uint8_t ahci_port_irq[AHCI_MAX_PORTS];

struct ahci_cmd_header *cmd_headers[AHCI_MAX_PORTS];
struct ahci_cmd_table *cmd_tables[AHCI_MAX_PORTS];
struct ahci_fis *fis_areas[AHCI_MAX_PORTS];

static void ahci_port_rebase(int port_num);
static int ahci_port_start(volatile uint32_t *port_base, int port_num);
static int ahci_port_stop(volatile uint32_t *port_base);
static int ahci_port_wait_idle(volatile uint32_t *port_base, uint32_t timeout_ms);
static int ahci_port_find_cmd_slot(volatile uint32_t *port_base);
static int ahci_port_transfer(volatile uint32_t *port_base, int port_num, uint64_t lba, uint32_t count, void *buffer, int write);

static void ahci_irq_handler(struct regs *r) {
    if (!hba) return;
    uint32_t is = hba->is;
    hba->is = is;

    for (int p = 0; p < AHCI_MAX_PORTS; p++) {
        if (is & (1u << p)) {
            volatile uint32_t *port_base = (volatile uint32_t *)((uintptr_t)hba + 0x100 + p * 0x80);
            uint32_t port_is = port_base[AHCI_PORT_IS / 4];
            port_base[AHCI_PORT_IS / 4] = port_is;
            if (port_is & AHCI_IS_DHRS) {
                sched_wake((const void *)&port_base[AHCI_PORT_CI / 4]);
            }
        }
    }
}

void ahci_init(void) {
    ahci_dev_count = 0;
    memset(ahci_devices, 0, sizeof(ahci_devices));
    memset(ahci_port_irq, 0, sizeof(ahci_port_irq));

    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *pci = pci_get_device(i);
        if (!pci) continue;
        if (pci->class_code != AHCI_PCI_CLASS || pci->subclass != AHCI_PCI_SUBCLASS) continue;

        uint32_t bar5 = pci_read_config(pci->bus, pci->slot, pci->func, AHCI_BAR5_OFFSET);
        if (!bar5 || bar5 == 0xFFFFFFFF) continue;

        hba = (volatile struct ahci_hba_mem *)vmm_map_physical(bar5 & 0xFFFFFFF0, 0x1100, VM_READ | VM_WRITE | VM_UNCACHED, "ahci-hba");
        if (!hba) {
            klog("ahci: failed to map HBA memory at 0x%08x", bar5 & 0xFFFFFFF0);
            continue;
        }

        klog("ahci: HBA at 0x%08x, version %x, %u ports", bar5 & 0xFFFFFFF0, hba->vs, hba->pi);

        hba->ghc |= AHCI_GHC_AE;

        hba->ghc |= AHCI_GHC_HR;
        for (int t = 0; t < 1000; t++) {
            if (!(hba->ghc & AHCI_GHC_HR)) break;
            sleep_ms(1);
        }
        if (hba->ghc & AHCI_GHC_HR) {
            klog("ahci: HBA reset timeout");
            continue;
        }

        ahci_irq = pci->irq;
        irq_install_handler(ahci_irq, ahci_irq_handler);
        hba->ghc |= AHCI_GHC_IE;

        uint32_t pi = hba->pi;
        for (int p = 0; p < AHCI_MAX_PORTS; p++) {
            if (!(pi & (1u << p))) continue;

            volatile uint32_t *port_base = (volatile uint32_t *)((uintptr_t)hba + 0x100 + p * 0x80);

            uint32_t ssts = port_base[AHCI_PORT_SSTS / 4];
            uint8_t det = ssts & 0x0F;
            uint8_t ipm = (ssts >> 8) & 0x0F;

            if (det != 0x03 || ipm != 0x01) continue;

            if (ahci_port_start(port_base, p) < 0) continue;

            struct ahci_device *dev = &ahci_devices[ahci_dev_count];
            memset(dev, 0, sizeof(*dev));
            dev->used = 1;
            dev->port_num = p;
            dev->present = 1;
            dev->pci_bus = pci->bus;
            dev->pci_slot = pci->slot;
            dev->pci_func = pci->func;

            if (ahci_identify(dev) < 0) {
                ahci_port_stop(port_base);
                continue;
            }

            dev->model[0] = 's';
            dev->model[1] = 'd';
            dev->model[2] = 'a' + ahci_dev_count;
            dev->model[3] = '\0';

            ahci_dev_count++;
            klog("ahci: port %d: %s, %lu sectors", p, dev->model, dev->sectors);
        }

        break;
    }

    if (ahci_dev_count == 0) {
        klog("ahci: no SATA devices found");
    }
}

static int ahci_port_start(volatile uint32_t *port_base, int port_num) {
    ahci_port_stop(port_base);

    if (ahci_port_wait_idle(port_base, 1000) < 0) return -1;

    void *cmd_list = vmm_alloc(0x400, VM_READ | VM_WRITE | VM_UNCACHED, "ahci-cmdlist");
    void *fis_area = vmm_alloc(0x100, VM_READ | VM_WRITE | VM_UNCACHED, "ahci-fis");
    if (!cmd_list || !fis_area) {
        if (cmd_list) vmm_free(cmd_list);
        if (fis_area) vmm_free(fis_area);
        return -1;
    }

    cmd_headers[port_num] = (struct ahci_cmd_header *)cmd_list;
    fis_areas[port_num] = (struct ahci_fis *)fis_area;

    uint64_t cmd_list_phys = (uint64_t)(uintptr_t)cmd_list;
    uint64_t fis_phys = (uint64_t)(uintptr_t)fis_area;

    port_base[AHCI_PORT_CLB / 4] = (uint32_t)cmd_list_phys;
    port_base[AHCI_PORT_CLBU / 4] = (uint32_t)(cmd_list_phys >> 32);
    port_base[AHCI_PORT_FB / 4] = (uint32_t)fis_phys;
    port_base[AHCI_PORT_FBU / 4] = (uint32_t)(fis_phys >> 32);

    port_base[AHCI_PORT_SERR / 4] = 0xFFFFFFFF;

    port_base[AHCI_PORT_CMD / 4] |= AHCI_CMD_FRE;

    for (int t = 0; t < 100; t++) {
        if (port_base[AHCI_PORT_CMD / 4] & AHCI_CMD_FR) break;
        sleep_ms(1);
    }
    if (!(port_base[AHCI_PORT_CMD / 4] & AHCI_CMD_FR)) return -1;

    port_base[AHCI_PORT_CMD / 4] |= AHCI_CMD_ST;

    for (int t = 0; t < 100; t++) {
        uint32_t tfd = port_base[AHCI_PORT_TFD / 4];
        if (!(tfd & AHCI_TFD_STS_BSY) && !(tfd & AHCI_TFD_STS_DRQ)) break;
        sleep_ms(1);
    }

    void *cmd_tables_mem = vmm_alloc(0x1000, VM_READ | VM_WRITE | VM_UNCACHED, "ahci-cmdtables");
    if (!cmd_tables_mem) return -1;
    cmd_tables[port_num] = (struct ahci_cmd_table *)cmd_tables_mem;

    for (int s = 0; s < AHCI_MAX_CMDS; s++) {
        uint64_t ctba = (uint64_t)(uintptr_t)&cmd_tables[port_num][s];
        cmd_headers[port_num][s].ctba = (uint32_t)ctba;
        cmd_headers[port_num][s].ctbau = (uint32_t)(ctba >> 32);
    }

    return 0;
}

static int ahci_port_stop(volatile uint32_t *port_base) {
    port_base[AHCI_PORT_CMD / 4] &= ~(AHCI_CMD_ST | AHCI_CMD_FRE);

    for (int t = 0; t < 100; t++) {
        uint32_t cmd = port_base[AHCI_PORT_CMD / 4];
        if (!(cmd & AHCI_CMD_CR) && !(cmd & AHCI_CMD_FR)) break;
        sleep_ms(1);
    }

    return 0;
}

static int ahci_port_wait_idle(volatile uint32_t *port_base, uint32_t timeout_ms) {
    uint32_t start = pit_ticks();
    while ((pit_ticks() - start) < timeout_ms * PIT_FREQUENCY / 1000) {
        uint32_t cmd = port_base[AHCI_PORT_CMD / 4];
        if (!(cmd & AHCI_CMD_CR) && !(cmd & AHCI_CMD_FR)) return 0;
        sleep_ms(1);
    }
    return -1;
}

static int ahci_port_find_cmd_slot(volatile uint32_t *port_base) {
    uint32_t ci = port_base[AHCI_PORT_CI / 4];
    uint32_t sact = port_base[AHCI_PORT_SACT / 4];
    uint32_t slots = ci | sact;

    for (int i = 0; i < AHCI_MAX_CMDS; i++) {
        if (!(slots & (1u << i))) return i;
    }
    return -1;
}

static int ahci_port_transfer(volatile uint32_t *port_base, int port_num, uint64_t lba, uint32_t count, void *buffer, int write) {
    int slot = ahci_port_find_cmd_slot(port_base);
    if (slot < 0) return -1;

    struct ahci_cmd_header *header = &cmd_headers[port_num][slot];
    struct ahci_cmd_table *table = &cmd_tables[port_num][slot];

    memset(table, 0, sizeof(*table));
    uint8_t *fis = table->cfis;
    fis[0] = AHCI_FIS_TYPE_REG_H2D;
    fis[1] = 0x80;
    fis[2] = write ? 0x35 : 0x25;
    fis[3] = 0;

    fis[4] = (uint8_t)(lba & 0xFF);
    fis[5] = (uint8_t)((lba >> 8) & 0xFF);
    fis[6] = (uint8_t)((lba >> 16) & 0xFF);
    fis[7] = 0x40;
    fis[8] = (uint8_t)((lba >> 24) & 0xFF);
    fis[9] = (uint8_t)((lba >> 32) & 0xFF);
    fis[10] = (uint8_t)((lba >> 40) & 0xFF);
    fis[11] = 0;

    fis[12] = (uint8_t)(count & 0xFF);
    fis[13] = (uint8_t)((count >> 8) & 0xFF);

    fis[14] = 0;
    fis[15] = 0;

    header->flags = (write ? AHCI_CMD_HEADER_W : 0) | AHCI_CMD_HEADER_C;
    header->prdtl = 1;
    header->prdbc = 0;

    uint64_t buf_phys = (uint64_t)(uintptr_t)buffer;
    table->prdt[0].dba = (uint32_t)buf_phys;
    table->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    table->prdt[0].dbc = (count * 512) - 1;

    uint32_t f = spin_lock_irqsave(&ahci_lock);
    port_base[AHCI_PORT_CI / 4] = (1u << slot);
    spin_unlock_irqrestore(&ahci_lock, f);

    uint32_t start = pit_ticks();
    while ((pit_ticks() - start) < 30000 * PIT_FREQUENCY / 1000) {
        if (!(port_base[AHCI_PORT_CI / 4] & (1u << slot))) break;
        sleep_ms(1);
    }

    int result = 0;
    if (port_base[AHCI_PORT_CI / 4] & (1u << slot)) {
        result = -1;
    }

    uint32_t is = port_base[AHCI_PORT_IS / 4];
    if (is & (AHCI_IS_TFES | AHCI_IS_HBFS | AHCI_IS_HBDS | AHCI_IS_IFS | AHCI_IS_OFS)) {
        result = -1;
    }

    return result;
}

int ahci_identify(struct ahci_device *dev) {
    volatile uint32_t *port_base = (volatile uint32_t *)((uintptr_t)hba + 0x100 + dev->port_num * 0x80);

    int slot = ahci_port_find_cmd_slot(port_base);
    if (slot < 0) return -1;

    struct ahci_cmd_header *header = &cmd_headers[dev->port_num][slot];
    struct ahci_cmd_table *table = &cmd_tables[dev->port_num][slot];

    uint16_t *id_buf = vmm_alloc(512, VM_READ | VM_WRITE, "ahci-identify");
    if (!id_buf) return -1;

    memset(table, 0, sizeof(*table));
    uint8_t *fis = table->cfis;
    fis[0] = AHCI_FIS_TYPE_REG_H2D;
    fis[1] = 0x80;
    fis[2] = 0xEC;

    header->flags = AHCI_CMD_HEADER_C;
    header->prdtl = 1;
    header->prdbc = 0;

    uint64_t buf_phys = (uint64_t)(uintptr_t)id_buf;
    table->prdt[0].dba = (uint32_t)buf_phys;
    table->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    table->prdt[0].dbc = 511;

    uint32_t f = spin_lock_irqsave(&ahci_lock);
    port_base[AHCI_PORT_CI / 4] = (1u << slot);
    spin_unlock_irqrestore(&ahci_lock, f);

    uint32_t start = pit_ticks();
    while ((pit_ticks() - start) < 5000 * PIT_FREQUENCY / 1000) {
        if (!(port_base[AHCI_PORT_CI / 4] & (1u << slot))) break;
        sleep_ms(1);
    }

    if (port_base[AHCI_PORT_CI / 4] & (1u << slot)) {
        vmm_free(id_buf);
        return -1;
    }

    dev->signature = port_base[AHCI_PORT_SIG / 4];
    dev->atapi = (dev->signature == AHCI_SIG_SATAPI) ? 1 : 0;

    if (dev->atapi) {
        vmm_free(id_buf);
        return -1;
    }

    uint64_t total = (uint64_t)id_buf[100] | ((uint64_t)id_buf[101] << 16) |
                     ((uint64_t)id_buf[102] << 32) | ((uint64_t)id_buf[103] << 48);
    dev->sectors = total;

    int j = 0;
    for (int i = 0; i < 20; i++) {
        uint16_t w = id_buf[27 + i];
        dev->model[j++] = (char)(w >> 8);
        dev->model[j++] = (char)(w & 0xFF);
    }
    dev->model[j] = '\0';
    while (j > 0 && (dev->model[j - 1] == ' ' || dev->model[j - 1] == '\0')) {
        dev->model[--j] = '\0';
    }

    j = 0;
    for (int i = 0; i < 10; i++) {
        uint16_t w = id_buf[10 + i];
        dev->serial[j++] = (char)(w >> 8);
        dev->serial[j++] = (char)(w & 0xFF);
    }
    dev->serial[j] = '\0';
    while (j > 0 && (dev->serial[j - 1] == ' ' || dev->serial[j - 1] == '\0')) {
        dev->serial[--j] = '\0';
    }

    j = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t w = id_buf[23 + i];
        dev->firmware[j++] = (char)(w >> 8);
        dev->firmware[j++] = (char)(w & 0xFF);
    }
    dev->firmware[j] = '\0';
    while (j > 0 && (dev->firmware[j - 1] == ' ' || dev->firmware[j - 1] == '\0')) {
        dev->firmware[--j] = '\0';
    }

    if (dev->model[0] == '\0') {
        strcpy(dev->model, "SATA device");
    }

    vmm_free(id_buf);
    return 0;
}

int ahci_device_count(void) {
    return ahci_dev_count;
}

struct ahci_device *ahci_get_device(int index) {
    if (index < 0 || index >= ahci_dev_count) return NULL;
    return &ahci_devices[index];
}

struct ahci_device *ahci_find(const char *name) {
    if (strncmp(name, "/dev/", 5) == 0) name += 5;
    for (int i = 0; i < ahci_dev_count; i++) {
        if (strcmp(ahci_devices[i].model, name) == 0) {
            return &ahci_devices[i];
        }
    }
    return NULL;
}

int ahci_read_sectors(struct ahci_device *dev, uint64_t lba, uint32_t count, void *buffer) {
    if (!dev || !dev->present || dev->atapi) return -1;
    if (lba + count > dev->sectors) return -1;

    volatile uint32_t *port_base = (volatile uint32_t *)((uintptr_t)hba + 0x100 + dev->port_num * 0x80);
    return ahci_port_transfer(port_base, dev->port_num, lba, count, buffer, 0);
}

int ahci_write_sectors(struct ahci_device *dev, uint64_t lba, uint32_t count, const void *buffer) {
    if (!dev || !dev->present || dev->atapi) return -1;
    if (lba + count > dev->sectors) return -1;

    volatile uint32_t *port_base = (volatile uint32_t *)((uintptr_t)hba + 0x100 + dev->port_num * 0x80);
    return ahci_port_transfer(port_base, dev->port_num, lba, count, (void *)buffer, 1);
}