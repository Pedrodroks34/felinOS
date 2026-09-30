#include "drivers/rtl8169.h"
#include "drivers/rtl.h"
#include "drivers/pci.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "sync.h"
#include "lib/string.h"
#include "net/netif.h"
#include "net/netbuf.h"

#define RTL8169_IDR0       0x00u
#define RTL8169_MAR0       0x08u
#define RTL8169_TNPDS      0x20u
#define RTL8169_CR         0x37u
#define RTL8169_TPPOLL     0x38u
#define RTL8169_IMR        0x3Cu
#define RTL8169_ISR        0x3Eu
#define RTL8169_TCR        0x40u
#define RTL8169_RCR        0x44u
#define RTL8169_CFG9346    0x50u
#define RTL8169_PHYAR      0x60u
#define RTL8169_PHYSTATUS  0x6Cu
#define RTL8169_RMS        0xDAu
#define RTL8169_RDSAR      0xE4u
#define RTL8169_MTPS       0xECu

#define RTL8169_CR_TE      0x04u
#define RTL8169_CR_RE      0x08u
#define RTL8169_CR_RST     0x10u

#define RTL8169_CFG_UNLOCK 0xC0u
#define RTL8169_CFG_LOCK   0x00u

#define RTL8169_TPPOLL_NPQ 0x40u

#define RTL8169_ISR_ROK    0x0001u
#define RTL8169_ISR_RER    0x0002u
#define RTL8169_ISR_RDU    0x0010u
#define RTL8169_ISR_LINK   0x0020u
#define RTL8169_ISR_FOVW   0x0040u

#define RTL8169_IMR_RX     (RTL8169_ISR_ROK | RTL8169_ISR_RER | RTL8169_ISR_RDU | RTL8169_ISR_FOVW)
#define RTL8169_IMR_ALL    (RTL8169_IMR_RX | RTL8169_ISR_LINK)

#define RTL8169_RCR_VALUE  0x0000E70Eu
#define RTL8169_TCR_VALUE  0x03000700u

#define RTL8169_PHY_BUSY   0x80000000u
#define RTL8169_PHY_BMCR   0u
#define RTL8169_BMCR_PDOWN 0x0800u
#define RTL8169_BMCR_ANEN  0x1000u
#define RTL8169_BMCR_ANRST 0x0200u

#define RTL8169_LINK_UP    0x02u

#define RTL8169_DESC_OWN   0x80000000u
#define RTL8169_DESC_EOR   0x40000000u
#define RTL8169_DESC_FS    0x20000000u
#define RTL8169_DESC_LS    0x10000000u
#define RTL8169_DESC_RES   0x00200000u
#define RTL8169_DESC_LEN   0x00003FFFu

#define RTL8169_RING       32u
#define RTL8169_BUF        2048u
#define RTL8169_RX_MAX     1536u
#define RTL8169_MTPS_VALUE 0x3Bu
#define RTL8169_ETH_MIN    60u
#define RTL8169_TX_MAX     1792u

struct rtl8169_desc {
    volatile uint32_t opts1;
    volatile uint32_t opts2;
    volatile uint32_t addr_lo;
    volatile uint32_t addr_hi;
};

static struct rtl_regs hw;
static struct rtl8169_desc *rx_ring;
static struct rtl8169_desc *tx_ring;
static uint8_t *rx_bufs;
static uint8_t *tx_bufs;
static uint32_t rx_ring_phys;
static uint32_t tx_ring_phys;
static uint32_t rx_bufs_phys;
static uint32_t tx_bufs_phys;
static uint32_t rx_cur;
static uint32_t tx_cur;
static uint8_t nic_mac[6];
static int nic_present;
static spinlock_t tx_lock = SPINLOCK_INIT("rtl8169-tx");

static inline void rtl8169_barrier(void) {
    __asm__ volatile ("" : : : "memory");
}

static uint16_t rtl8169_phy_read(uint8_t reg) {
    rtl_wr32(&hw, RTL8169_PHYAR, (uint32_t)reg << 16);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rtl_rd32(&hw, RTL8169_PHYAR);
        if (v & RTL8169_PHY_BUSY) {
            return (uint16_t)(v & 0xFFFFu);
        }
    }
    return 0xFFFFu;
}

static void rtl8169_phy_write(uint8_t reg, uint16_t value) {
    rtl_wr32(&hw, RTL8169_PHYAR, RTL8169_PHY_BUSY | ((uint32_t)reg << 16) | value);
    for (int i = 0; i < 100000; i++) {
        if (!(rtl_rd32(&hw, RTL8169_PHYAR) & RTL8169_PHY_BUSY)) {
            return;
        }
    }
}

static void rtl8169_phy_start(void) {
    uint16_t bmcr = rtl8169_phy_read(RTL8169_PHY_BMCR);

    if (bmcr == 0xFFFFu) {
        return;
    }
    bmcr = (uint16_t)((bmcr & ~RTL8169_BMCR_PDOWN) | RTL8169_BMCR_ANEN | RTL8169_BMCR_ANRST);
    rtl8169_phy_write(RTL8169_PHY_BMCR, bmcr);
}

static void rtl8169_rx_arm(uint32_t i) {
    uint32_t flags = RTL8169_DESC_OWN | RTL8169_BUF;

    if (i == RTL8169_RING - 1u) {
        flags |= RTL8169_DESC_EOR;
    }
    rx_ring[i].opts2 = 0;
    rtl8169_barrier();
    rx_ring[i].opts1 = flags;
}

static void rtl8169_rx_poll(void) {
    for (;;) {
        struct rtl8169_desc *d = &rx_ring[rx_cur];
        uint32_t opts1 = d->opts1;

        if (opts1 & RTL8169_DESC_OWN) {
            break;
        }
        rtl8169_barrier();

        uint32_t len = opts1 & RTL8169_DESC_LEN;
        int good = !(opts1 & RTL8169_DESC_RES) &&
                   (opts1 & (RTL8169_DESC_FS | RTL8169_DESC_LS)) == (RTL8169_DESC_FS | RTL8169_DESC_LS) &&
                   len > 4u && (len - 4u) <= NETBUF_CAP;

        if (good) {
            struct netbuf *nb = netbuf_alloc_raw();
            if (nb) {
                memcpy(nb->storage, rx_bufs + rx_cur * RTL8169_BUF, len - 4u);
                nb->len = (uint16_t)(len - 4u);
                netif_rx_enqueue(nb);
            }
        } else {
            struct netif *nif = netif_default();
            if (nif) {
                nif->rx_errors++;
            }
        }

        rtl8169_rx_arm(rx_cur);
        rx_cur = (rx_cur + 1u) % RTL8169_RING;
    }
}

static void rtl8169_irq_handler(struct regs *r) {
    (void)r;
    uint16_t isr = rtl_rd16(&hw, RTL8169_ISR);

    if (isr == 0 || isr == 0xFFFFu) {
        return;
    }
    rtl_wr16(&hw, RTL8169_ISR, isr);
    if (isr & RTL8169_IMR_RX) {
        rtl8169_rx_poll();
    }
}

static int rtl8169_transmit(struct netbuf *nb) {
    uint32_t len = nb->len;

    if (len > RTL8169_TX_MAX) {
        return -1;
    }

    uint32_t f = spin_lock_irqsave(&tx_lock);
    uint32_t slot = tx_cur;
    struct rtl8169_desc *d = &tx_ring[slot];

    if (d->opts1 & RTL8169_DESC_OWN) {
        spin_unlock_irqrestore(&tx_lock, f);
        return -1;
    }

    uint8_t *dst = tx_bufs + slot * RTL8169_BUF;
    memcpy(dst, nb->data, len);
    if (len < RTL8169_ETH_MIN) {
        memset(dst + len, 0, RTL8169_ETH_MIN - len);
        len = RTL8169_ETH_MIN;
    }

    uint32_t flags = RTL8169_DESC_OWN | RTL8169_DESC_FS | RTL8169_DESC_LS | len;
    if (slot == RTL8169_RING - 1u) {
        flags |= RTL8169_DESC_EOR;
    }
    d->opts2 = 0;
    rtl8169_barrier();
    d->opts1 = flags;
    rtl8169_barrier();

    tx_cur = (slot + 1u) % RTL8169_RING;
    rtl_wr8(&hw, RTL8169_TPPOLL, RTL8169_TPPOLL_NPQ);
    spin_unlock_irqrestore(&tx_lock, f);
    return 0;
}

int rtl8169_present(void) {
    return nic_present;
}

const char *rtl8169_link_status(void) {
    if (!nic_present) {
        return "no device";
    }
    return (rtl_rd8(&hw, RTL8169_PHYSTATUS) & RTL8169_LINK_UP) ? "up" : "down";
}

static int rtl8169_alloc_rings(void) {
    uint32_t ring_bytes = RTL8169_RING * (uint32_t)sizeof(struct rtl8169_desc);
    uint32_t ring_frames = (ring_bytes + PMM_FRAME_SIZE - 1u) / PMM_FRAME_SIZE;
    uint32_t buf_frames = (RTL8169_RING * RTL8169_BUF + PMM_FRAME_SIZE - 1u) / PMM_FRAME_SIZE;

    rx_ring_phys = pmm_alloc_contiguous(ring_frames);
    tx_ring_phys = pmm_alloc_contiguous(ring_frames);
    rx_bufs_phys = pmm_alloc_contiguous(buf_frames);
    tx_bufs_phys = pmm_alloc_contiguous(buf_frames);
    if (!rx_ring_phys || !tx_ring_phys || !rx_bufs_phys || !tx_bufs_phys) {
        return -1;
    }

    rx_ring = (struct rtl8169_desc *)vmm_map_physical(rx_ring_phys, ring_frames * PMM_FRAME_SIZE,
                                                      VM_READ | VM_WRITE | VM_UNCACHED, "rtl8169-rxd");
    tx_ring = (struct rtl8169_desc *)vmm_map_physical(tx_ring_phys, ring_frames * PMM_FRAME_SIZE,
                                                      VM_READ | VM_WRITE | VM_UNCACHED, "rtl8169-txd");
    rx_bufs = (uint8_t *)vmm_map_physical(rx_bufs_phys, buf_frames * PMM_FRAME_SIZE,
                                          VM_READ | VM_WRITE, "rtl8169-rxb");
    tx_bufs = (uint8_t *)vmm_map_physical(tx_bufs_phys, buf_frames * PMM_FRAME_SIZE,
                                          VM_READ | VM_WRITE, "rtl8169-txb");
    if (!rx_ring || !tx_ring || !rx_bufs || !tx_bufs) {
        return -1;
    }

    memset((void *)rx_ring, 0, ring_frames * PMM_FRAME_SIZE);
    memset((void *)tx_ring, 0, ring_frames * PMM_FRAME_SIZE);
    memset(rx_bufs, 0, buf_frames * PMM_FRAME_SIZE);
    memset(tx_bufs, 0, buf_frames * PMM_FRAME_SIZE);

    for (uint32_t i = 0; i < RTL8169_RING; i++) {
        rx_ring[i].addr_lo = rx_bufs_phys + i * RTL8169_BUF;
        rx_ring[i].addr_hi = 0;
        rtl8169_rx_arm(i);
        tx_ring[i].addr_lo = tx_bufs_phys + i * RTL8169_BUF;
        tx_ring[i].addr_hi = 0;
        tx_ring[i].opts1 = (i == RTL8169_RING - 1u) ? RTL8169_DESC_EOR : 0;
    }
    rx_cur = 0;
    tx_cur = 0;
    return 0;
}

int rtl8169_init(void) {
    static const uint16_t known_ids[] = { 0x8168, 0x8169, 0x8167, 0x8161, 0x8136 };
    struct pci_device *dev = pci_find(RTL_VENDOR_ID, known_ids, (int)(sizeof(known_ids) / sizeof(known_ids[0])));

    if (!dev) {
        return -1;
    }
    rtl_enable_bus_master(dev);
    if (rtl_map(dev, &hw, 1) != 0) {
        return -1;
    }

    rtl_wr16(&hw, RTL8169_IMR, 0);
    rtl_wr8(&hw, RTL8169_CR, RTL8169_CR_RST);
    int spins = 1000000;
    while ((rtl_rd8(&hw, RTL8169_CR) & RTL8169_CR_RST) && spins-- > 0) {
    }
    if (rtl_rd8(&hw, RTL8169_CR) & RTL8169_CR_RST) {
        return -1;
    }
    rtl_wr16(&hw, RTL8169_IMR, 0);
    rtl_wr16(&hw, RTL8169_ISR, 0xFFFFu);

    for (int i = 0; i < 6; i++) {
        nic_mac[i] = rtl_rd8(&hw, RTL8169_IDR0 + (uint32_t)i);
    }

    if (rtl8169_alloc_rings() != 0) {
        return -1;
    }

    rtl_wr8(&hw, RTL8169_CFG9346, RTL8169_CFG_UNLOCK);
    rtl_wr16(&hw, RTL8169_RMS, RTL8169_RX_MAX);
    if (dev->device != 0x8169 && dev->device != 0x8167) {
        rtl_wr8(&hw, RTL8169_MTPS, RTL8169_MTPS_VALUE);
    }

    rtl_wr32(&hw, RTL8169_TNPDS + 4u, 0);
    rtl_wr32(&hw, RTL8169_TNPDS, tx_ring_phys);
    rtl_wr32(&hw, RTL8169_RDSAR + 4u, 0);
    rtl_wr32(&hw, RTL8169_RDSAR, rx_ring_phys);

    rtl_wr8(&hw, RTL8169_CR, RTL8169_CR_TE | RTL8169_CR_RE);
    rtl_wr32(&hw, RTL8169_TCR, RTL8169_TCR_VALUE);
    rtl_wr32(&hw, RTL8169_RCR, RTL8169_RCR_VALUE);
    rtl_wr32(&hw, RTL8169_MAR0, 0xFFFFFFFFu);
    rtl_wr32(&hw, RTL8169_MAR0 + 4u, 0xFFFFFFFFu);
    rtl_wr8(&hw, RTL8169_CFG9346, RTL8169_CFG_LOCK);

    rtl8169_phy_start();

    const char *name = "rtl8169";
    if (dev->device == 0x8168 || dev->device == 0x8161) {
        name = "rtl8168";
    } else if (dev->device == 0x8136) {
        name = "rtl8101";
    }

    nic_present = 1;
    netif_register(nic_mac, name, rtl8169_transmit, rtl8169_link_status);

    if (rtl_irq_usable(dev)) {
        irq_install_handler(dev->irq, rtl8169_irq_handler);
        rtl_wr16(&hw, RTL8169_IMR, RTL8169_IMR_ALL);
    } else {
        rtl_start_poll(rtl8169_rx_poll);
    }
    return 0;
}
