#include "drivers/rtl8139.h"
#include "drivers/rtl.h"
#include "drivers/pci.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "sync.h"
#include "lib/string.h"
#include "net/netif.h"
#include "net/netbuf.h"

#define RTL8139_IDR0     0x00u
#define RTL8139_MAR0     0x08u
#define RTL8139_TSD0     0x10u
#define RTL8139_TSAD0    0x20u
#define RTL8139_RBSTART  0x30u
#define RTL8139_CR       0x37u
#define RTL8139_CAPR     0x38u
#define RTL8139_IMR      0x3Cu
#define RTL8139_ISR      0x3Eu
#define RTL8139_TCR      0x40u
#define RTL8139_RCR      0x44u
#define RTL8139_CONFIG1  0x52u
#define RTL8139_MSR      0x58u

#define RTL8139_CR_BUFE  0x01u
#define RTL8139_CR_TE    0x04u
#define RTL8139_CR_RE    0x08u
#define RTL8139_CR_RST   0x10u

#define RTL8139_ISR_ROK    0x0001u
#define RTL8139_ISR_RER    0x0002u
#define RTL8139_ISR_RXOVW  0x0010u
#define RTL8139_ISR_FOVW   0x0040u

#define RTL8139_IMR_RX     (RTL8139_ISR_ROK | RTL8139_ISR_RER | RTL8139_ISR_RXOVW | RTL8139_ISR_FOVW)

#define RTL8139_RX_ROK     0x0001u

#define RTL8139_TSD_OWN    0x00002000u
#define RTL8139_TSD_THRESH (8u << 16)

#define RTL8139_RCR_VALUE  0x0000E78Eu
#define RTL8139_TCR_VALUE  0x03000700u

#define RTL8139_MSR_LINKB  0x04u

#define RTL8139_RX_RING    8192u
#define RTL8139_RX_ALLOC   (RTL8139_RX_RING + 16u + 1536u)
#define RTL8139_RX_MAXLEN  1536u
#define RTL8139_RX_MINLEN  18u
#define RTL8139_TX_SLOTS   4u
#define RTL8139_TX_BUF     2048u
#define RTL8139_TX_MAX     1792u
#define RTL8139_ETH_MIN    60u

static struct rtl_regs hw;
static uint8_t *rx_buf;
static uint32_t rx_phys;
static uint32_t rx_off;
static uint8_t *tx_buf;
static uint32_t tx_phys;
static uint32_t tx_cur;
static uint8_t tx_busy[RTL8139_TX_SLOTS];
static uint8_t nic_mac[6];
static int nic_present;
static spinlock_t tx_lock = SPINLOCK_INIT("rtl8139-tx");

static void rtl8139_rx_program(void) {
    rtl_wr32(&hw, RTL8139_RBSTART, rx_phys);
    rtl_wr32(&hw, RTL8139_RCR, RTL8139_RCR_VALUE);
    rtl_wr16(&hw, RTL8139_CAPR, (uint16_t)(0u - 16u));
    rx_off = 0;
}

static void rtl8139_rx_reset(void) {
    rtl_wr8(&hw, RTL8139_CR, RTL8139_CR_TE);
    rtl8139_rx_program();
    rtl_wr8(&hw, RTL8139_CR, RTL8139_CR_TE | RTL8139_CR_RE);
}

static void rtl8139_rx_poll(void) {
    while (!(rtl_rd8(&hw, RTL8139_CR) & RTL8139_CR_BUFE)) {
        uint32_t hdr = *(volatile uint32_t *)(rx_buf + rx_off);
        uint32_t status = hdr & 0xFFFFu;
        uint32_t len = hdr >> 16;

        if (!(status & RTL8139_RX_ROK) || len < RTL8139_RX_MINLEN || len > RTL8139_RX_MAXLEN + 4u) {
            struct netif *nif = netif_default();
            if (nif) {
                nif->rx_errors++;
            }
            rtl8139_rx_reset();
            return;
        }

        uint32_t payload = len - 4u;
        struct netbuf *nb = netbuf_alloc_raw();
        if (nb && payload <= NETBUF_CAP) {
            memcpy(nb->storage, rx_buf + rx_off + 4u, payload);
            nb->len = (uint16_t)payload;
            netif_rx_enqueue(nb);
        } else if (nb) {
            netbuf_free(nb);
        }

        rx_off = (rx_off + len + 4u + 3u) & ~3u;
        if (rx_off >= RTL8139_RX_RING) {
            rx_off -= RTL8139_RX_RING;
        }
        rtl_wr16(&hw, RTL8139_CAPR, (uint16_t)(rx_off - 16u));
    }
}

static void rtl8139_irq_handler(struct regs *r) {
    (void)r;
    uint16_t isr = rtl_rd16(&hw, RTL8139_ISR);

    if (isr == 0 || isr == 0xFFFFu) {
        return;
    }
    rtl_wr16(&hw, RTL8139_ISR, isr);
    if (isr & RTL8139_IMR_RX) {
        rtl8139_rx_poll();
    }
}

static int rtl8139_transmit(struct netbuf *nb) {
    uint32_t len = nb->len;

    if (len > RTL8139_TX_MAX) {
        return -1;
    }

    uint32_t f = spin_lock_irqsave(&tx_lock);
    uint32_t slot = tx_cur;

    if (tx_busy[slot] && !(rtl_rd32(&hw, RTL8139_TSD0 + slot * 4u) & RTL8139_TSD_OWN)) {
        spin_unlock_irqrestore(&tx_lock, f);
        return -1;
    }

    uint8_t *dst = tx_buf + slot * RTL8139_TX_BUF;
    memcpy(dst, nb->data, len);
    if (len < RTL8139_ETH_MIN) {
        memset(dst + len, 0, RTL8139_ETH_MIN - len);
        len = RTL8139_ETH_MIN;
    }

    rtl_wr32(&hw, RTL8139_TSAD0 + slot * 4u, tx_phys + slot * RTL8139_TX_BUF);
    rtl_wr32(&hw, RTL8139_TSD0 + slot * 4u, len | RTL8139_TSD_THRESH);
    tx_busy[slot] = 1;
    tx_cur = (slot + 1u) % RTL8139_TX_SLOTS;
    spin_unlock_irqrestore(&tx_lock, f);
    return 0;
}

int rtl8139_present(void) {
    return nic_present;
}

const char *rtl8139_link_status(void) {
    if (!nic_present) {
        return "no device";
    }
    return (rtl_rd8(&hw, RTL8139_MSR) & RTL8139_MSR_LINKB) ? "down" : "up";
}

int rtl8139_init(void) {
    static const uint16_t known_ids[] = { 0x8139, 0x8138 };
    struct pci_device *dev = pci_find(RTL_VENDOR_ID, known_ids, (int)(sizeof(known_ids) / sizeof(known_ids[0])));

    if (!dev) {
        return -1;
    }
    rtl_enable_bus_master(dev);
    if (rtl_map(dev, &hw, 0) != 0) {
        return -1;
    }

    rtl_wr8(&hw, RTL8139_CONFIG1, 0);
    rtl_wr8(&hw, RTL8139_CR, RTL8139_CR_RST);
    int spins = 1000000;
    while ((rtl_rd8(&hw, RTL8139_CR) & RTL8139_CR_RST) && spins-- > 0) {
    }
    if (rtl_rd8(&hw, RTL8139_CR) & RTL8139_CR_RST) {
        return -1;
    }

    for (int i = 0; i < 6; i++) {
        nic_mac[i] = rtl_rd8(&hw, RTL8139_IDR0 + (uint32_t)i);
    }

    uint32_t rx_frames = (RTL8139_RX_ALLOC + PMM_FRAME_SIZE - 1u) / PMM_FRAME_SIZE;
    rx_phys = pmm_alloc_contiguous(rx_frames);
    if (!rx_phys) {
        return -1;
    }
    rx_buf = (uint8_t *)vmm_map_physical(rx_phys, rx_frames * PMM_FRAME_SIZE,
                                         VM_READ | VM_WRITE, "rtl8139-rxb");
    if (!rx_buf) {
        return -1;
    }
    memset(rx_buf, 0, rx_frames * PMM_FRAME_SIZE);

    uint32_t tx_frames = (RTL8139_TX_SLOTS * RTL8139_TX_BUF + PMM_FRAME_SIZE - 1u) / PMM_FRAME_SIZE;
    tx_phys = pmm_alloc_contiguous(tx_frames);
    if (!tx_phys) {
        return -1;
    }
    tx_buf = (uint8_t *)vmm_map_physical(tx_phys, tx_frames * PMM_FRAME_SIZE,
                                         VM_READ | VM_WRITE, "rtl8139-txb");
    if (!tx_buf) {
        return -1;
    }
    memset(tx_buf, 0, tx_frames * PMM_FRAME_SIZE);
    tx_cur = 0;
    memset(tx_busy, 0, sizeof(tx_busy));

    rtl_wr32(&hw, RTL8139_MAR0, 0xFFFFFFFFu);
    rtl_wr32(&hw, RTL8139_MAR0 + 4u, 0xFFFFFFFFu);
    rtl_wr16(&hw, RTL8139_IMR, 0);
    rtl_wr16(&hw, RTL8139_ISR, 0xFFFFu);
    rtl8139_rx_program();
    rtl_wr8(&hw, RTL8139_CR, RTL8139_CR_TE | RTL8139_CR_RE);
    rtl_wr32(&hw, RTL8139_TCR, RTL8139_TCR_VALUE);

    nic_present = 1;
    netif_register(nic_mac, "rtl8139", rtl8139_transmit, rtl8139_link_status);

    if (rtl_irq_usable(dev)) {
        irq_install_handler(dev->irq, rtl8139_irq_handler);
        rtl_wr16(&hw, RTL8139_IMR, RTL8139_IMR_RX);
    } else {
        rtl_start_poll(rtl8139_rx_poll);
    }
    return 0;
}
