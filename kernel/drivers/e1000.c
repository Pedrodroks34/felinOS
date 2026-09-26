#include "drivers/e1000.h"
#include "drivers/pci.h"
#include "io.h"
#include "idt.h"
#include "sched.h"
#include "pmm.h"
#include "vmm.h"
#include "lib/string.h"
#include "lib/format.h"
#include "net/netif.h"
#include "net/netbuf.h"

#define E1000_REG_CTRL   0x0000
#define E1000_REG_STATUS 0x0008
#define E1000_REG_EECD   0x0010
#define E1000_REG_EERD   0x0014
#define E1000_REG_ICR    0x00C0
#define E1000_REG_ITR    0x00C4
#define E1000_REG_IMS    0x00D0
#define E1000_REG_IMC    0x00D8
#define E1000_REG_RCTL   0x0100
#define E1000_REG_TCTL   0x0400
#define E1000_REG_TIPG   0x0410
#define E1000_REG_RDBAL  0x2800
#define E1000_REG_RDBAH  0x2804
#define E1000_REG_RDLEN  0x2808
#define E1000_REG_RDH    0x2810
#define E1000_REG_RDT    0x2818
#define E1000_REG_TDBAL  0x3800
#define E1000_REG_TDBAH  0x3804
#define E1000_REG_TDLEN  0x3808
#define E1000_REG_TDH    0x3810
#define E1000_REG_TDT    0x3818
#define E1000_REG_RAL0   0x5400
#define E1000_REG_RAH0   0x5404
#define E1000_REG_MTA    0x5200

#define E1000_CTRL_SLU  0x00000040u
#define E1000_CTRL_ASDE 0x00000020u
#define E1000_CTRL_RST  0x04000000u

#define E1000_RCTL_EN     0x00000002u
#define E1000_RCTL_UPE    0x00000008u
#define E1000_RCTL_MPE    0x00000010u
#define E1000_RCTL_BAM    0x00008000u
#define E1000_RCTL_SECRC  0x04000000u

#define E1000_TCTL_EN    0x00000002u
#define E1000_TCTL_PSP   0x00000008u
#define E1000_TCTL_CT    (0x0Fu << 4)
#define E1000_TCTL_COLD  (0x40u << 12)

#define E1000_ICR_LSC   0x00000004u
#define E1000_ICR_RXT0  0x00000080u
#define E1000_ICR_RXO   0x00000040u

#define E1000_RX_DESC_STATUS_DD 0x01u
#define E1000_TX_CMD_EOP  0x01u
#define E1000_TX_CMD_IFCS 0x02u
#define E1000_TX_CMD_RS   0x08u
#define E1000_TX_STATUS_DD 0x01u

#define NUM_RX_DESC 32
#define NUM_TX_DESC 32
#define BUF_SIZE    2048

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

static volatile uint32_t *regs;
static volatile struct e1000_rx_desc *rx_ring;
static volatile struct e1000_tx_desc *tx_ring;
static uint8_t *rx_bufs;
static uint8_t *tx_bufs;
static uint32_t rx_bufs_phys;
static uint32_t tx_bufs_phys;
static uint32_t tx_tail;
static uint8_t nic_mac[6];
static int nic_present;
static uint8_t nic_irq;

static uint32_t e1000_read(uint32_t offset) {
    return regs[offset / 4];
}

static void e1000_write(uint32_t offset, uint32_t value) {
    regs[offset / 4] = value;
}

static int e1000_eeprom_read(uint8_t addr, uint16_t *out) {
    e1000_write(E1000_REG_EERD, ((uint32_t)addr << 8) | 1u);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = e1000_read(E1000_REG_EERD);
        if (v & 0x10u) {
            *out = (uint16_t)(v >> 16);
            return 0;
        }
    }
    return -1;
}

static void e1000_read_mac(void) {
    uint32_t ral = e1000_read(E1000_REG_RAL0);
    uint32_t rah = e1000_read(E1000_REG_RAH0);

    if (ral != 0 || (rah & 0xFFFF) != 0) {
        nic_mac[0] = (uint8_t)(ral & 0xFF);
        nic_mac[1] = (uint8_t)((ral >> 8) & 0xFF);
        nic_mac[2] = (uint8_t)((ral >> 16) & 0xFF);
        nic_mac[3] = (uint8_t)((ral >> 24) & 0xFF);
        nic_mac[4] = (uint8_t)(rah & 0xFF);
        nic_mac[5] = (uint8_t)((rah >> 8) & 0xFF);
        return;
    }
    uint16_t word;
    for (int i = 0; i < 3; i++) {
        if (e1000_eeprom_read((uint8_t)i, &word) != 0) {
            word = 0;
        }
        nic_mac[i * 2] = (uint8_t)(word & 0xFF);
        nic_mac[i * 2 + 1] = (uint8_t)(word >> 8);
    }
}

static void e1000_setup_rx(void) {
    uint32_t frames = ((uint32_t)NUM_RX_DESC * sizeof(struct e1000_rx_desc) + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    uint32_t ring_phys = pmm_alloc_contiguous(frames);
    rx_ring = (volatile struct e1000_rx_desc *)vmm_map_physical(ring_phys, frames * PMM_FRAME_SIZE,
                                                                 VM_READ | VM_WRITE, "e1000-rxd");
    memset((void *)rx_ring, 0, frames * PMM_FRAME_SIZE);

    uint32_t buf_frames = ((uint32_t)NUM_RX_DESC * BUF_SIZE + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    rx_bufs_phys = pmm_alloc_contiguous(buf_frames);
    rx_bufs = (uint8_t *)vmm_map_physical(rx_bufs_phys, buf_frames * PMM_FRAME_SIZE,
                                          VM_READ | VM_WRITE, "e1000-rxb");

    for (int i = 0; i < NUM_RX_DESC; i++) {
        rx_ring[i].addr = rx_bufs_phys + (uint32_t)i * BUF_SIZE;
        rx_ring[i].status = 0;
    }

    e1000_write(E1000_REG_RDBAL, ring_phys);
    e1000_write(E1000_REG_RDBAH, 0);
    e1000_write(E1000_REG_RDLEN, (uint32_t)NUM_RX_DESC * sizeof(struct e1000_rx_desc));
    e1000_write(E1000_REG_RDH, 0);
    e1000_write(E1000_REG_RDT, NUM_RX_DESC - 1);
    e1000_write(E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
}

static void e1000_setup_tx(void) {
    uint32_t frames = ((uint32_t)NUM_TX_DESC * sizeof(struct e1000_tx_desc) + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    uint32_t ring_phys = pmm_alloc_contiguous(frames);
    tx_ring = (volatile struct e1000_tx_desc *)vmm_map_physical(ring_phys, frames * PMM_FRAME_SIZE,
                                                                 VM_READ | VM_WRITE, "e1000-txd");
    memset((void *)tx_ring, 0, frames * PMM_FRAME_SIZE);

    uint32_t buf_frames = ((uint32_t)NUM_TX_DESC * BUF_SIZE + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    tx_bufs_phys = pmm_alloc_contiguous(buf_frames);
    tx_bufs = (uint8_t *)vmm_map_physical(tx_bufs_phys, buf_frames * PMM_FRAME_SIZE,
                                          VM_READ | VM_WRITE, "e1000-txb");

    for (int i = 0; i < NUM_TX_DESC; i++) {
        tx_ring[i].addr = tx_bufs_phys + (uint32_t)i * BUF_SIZE;
        tx_ring[i].status = E1000_TX_STATUS_DD;
    }
    tx_tail = 0;

    e1000_write(E1000_REG_TDBAL, ring_phys);
    e1000_write(E1000_REG_TDBAH, 0);
    e1000_write(E1000_REG_TDLEN, (uint32_t)NUM_TX_DESC * sizeof(struct e1000_tx_desc));
    e1000_write(E1000_REG_TDH, 0);
    e1000_write(E1000_REG_TDT, 0);
    e1000_write(E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT | E1000_TCTL_COLD);
    e1000_write(E1000_REG_TIPG, 0x0060200Au);
}

static int e1000_transmit(struct netbuf *nb) {
    if (nb->len > BUF_SIZE) {
        return -1;
    }
    if (!(tx_ring[tx_tail].status & E1000_TX_STATUS_DD)) {
        return -1;
    }
    memcpy(tx_bufs + (uint32_t)tx_tail * BUF_SIZE, nb->data, nb->len);
    tx_ring[tx_tail].length = nb->len;
    tx_ring[tx_tail].cmd = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    tx_ring[tx_tail].status = 0;
    tx_tail = (tx_tail + 1) % NUM_TX_DESC;
    e1000_write(E1000_REG_TDT, tx_tail);
    return 0;
}

static void e1000_poll_rx(void) {
    static uint32_t rx_tail = NUM_RX_DESC - 1;
    uint32_t cur = (rx_tail + 1) % NUM_RX_DESC;

    while (rx_ring[cur].status & E1000_RX_DESC_STATUS_DD) {
        uint16_t len = rx_ring[cur].length;
        struct netbuf *nb = netbuf_alloc_raw();
        if (nb && len <= NETBUF_CAP) {
            memcpy(nb->storage, rx_bufs + (uint32_t)cur * BUF_SIZE, len);
            nb->len = len;
            netif_rx_enqueue(nb);
        } else if (nb) {
            netbuf_free(nb);
        }
        rx_ring[cur].status = 0;
        rx_tail = cur;
        e1000_write(E1000_REG_RDT, rx_tail);
        cur = (rx_tail + 1) % NUM_RX_DESC;
    }
}

static void e1000_irq_handler(struct regs *r) {
    (void)r;
    uint32_t icr = e1000_read(E1000_REG_ICR);

    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXO)) {
        e1000_poll_rx();
    }
}

int e1000_present(void) {
    return nic_present;
}

const char *e1000_link_status(void) {
    if (!nic_present) {
        return "no device";
    }
    return (e1000_read(E1000_REG_STATUS) & 0x02u) ? "up" : "down";
}

int e1000_init(void) {
    static const uint16_t known_ids[] = {
        0x100E, 0x1004, 0x100F, 0x1010, 0x1011, 0x1012, 0x1013, 0x1015,
        0x1016, 0x1017, 0x1018, 0x1019, 0x101A, 0x101D, 0x101E, 0x1026,
        0x1027, 0x1028, 0x105E, 0x105F, 0x1060
    };
    struct pci_device *dev = pci_find(0x8086, known_ids, (int)(sizeof(known_ids) / sizeof(known_ids[0])));

    if (!dev) {
        return -1;
    }
    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func, 0x04);
    pci_write_config(dev->bus, dev->slot, dev->func, 0x04, cmd | 0x0006u);

    uint32_t bar0 = pci_read_config(dev->bus, dev->slot, dev->func, 0x10) & ~0xFu;
    regs = (volatile uint32_t *)vmm_map_physical(bar0, 0x20000u, VM_READ | VM_WRITE | VM_UNCACHED, "e1000-mmio");
    if (!regs) {
        return -1;
    }

    e1000_write(E1000_REG_IMC, 0xFFFFFFFFu);
    e1000_write(E1000_REG_CTRL, e1000_read(E1000_REG_CTRL) | E1000_CTRL_RST);
    for (volatile int i = 0; i < 1000000; i++) {
    }
    e1000_write(E1000_REG_IMC, 0xFFFFFFFFu);
    (void)e1000_read(E1000_REG_ICR);

    e1000_write(E1000_REG_CTRL, e1000_read(E1000_REG_CTRL) | E1000_CTRL_SLU | E1000_CTRL_ASDE);

    for (int i = 0; i < 128; i++) {
        e1000_write(E1000_REG_MTA + (uint32_t)i * 4, 0);
    }

    e1000_read_mac();
    e1000_setup_rx();
    e1000_setup_tx();

    nic_irq = dev->irq;
    irq_install_handler(nic_irq, e1000_irq_handler);
    e1000_write(E1000_REG_IMS, E1000_ICR_LSC | E1000_ICR_RXT0 | E1000_ICR_RXO);

    nic_present = 1;
    netif_register(nic_mac, "e1000", e1000_transmit);
    return 0;
}
