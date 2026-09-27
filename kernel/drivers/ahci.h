#ifndef FELINOS_AHCI_H
#define FELINOS_AHCI_H

#include <stdint.h>

#define AHCI_MAX_PORTS 32
#define AHCI_MAX_CMDS  32
#define AHCI_MAX_PRDT  8

/* HBA Generic Host Control registers (offset from ABAR) */
#define AHCI_CAP        0x00
#define AHCI_GHC        0x04
#define AHCI_IS         0x08
#define AHCI_PI         0x0C
#define AHCI_VS         0x10
#define AHCI_CCC_CTL    0x14
#define AHCI_CCC_PORTS  0x18
#define AHCI_EM_LOC     0x1C
#define AHCI_EM_CTL     0x20
#define AHCI_CAP2       0x24
#define AHCI_BOHC       0x28

/* GHC bits */
#define AHCI_GHC_AE     (1u << 31)
#define AHCI_GHC_IE     (1u << 1)
#define AHCI_GHC_HR     (1u << 0)

/* Port registers (offset from port base) */
#define AHCI_PORT_CLB   0x00
#define AHCI_PORT_CLBU  0x04
#define AHCI_PORT_FB    0x08
#define AHCI_PORT_FBU   0x0C
#define AHCI_PORT_IS    0x10
#define AHCI_PORT_IE    0x14
#define AHCI_PORT_CMD   0x18
#define AHCI_PORT_TFD   0x20
#define AHCI_PORT_SIG   0x24
#define AHCI_PORT_SSTS  0x28
#define AHCI_PORT_SCTL  0x2C
#define AHCI_PORT_SERR  0x30
#define AHCI_PORT_SACT  0x34
#define AHCI_PORT_CI    0x38
#define AHCI_PORT_SNTF  0x3C
#define AHCI_PORT_FBS   0x40

/* Port CMD bits */
#define AHCI_CMD_ST     (1u << 0)
#define AHCI_CMD_SUD    (1u << 1)
#define AHCI_CMD_POD    (1u << 2)
#define AHCI_CMD_CLO    (1u << 3)
#define AHCI_CMD_FRE    (1u << 4)
#define AHCI_CMD_FR     (1u << 14)
#define AHCI_CMD_CR     (1u << 15)
#define AHCI_CMD_ICC    (1u << 28)

/* Port IS bits */
#define AHCI_IS_DHRS    (1u << 0)   /* Device to Host Register FIS */
#define AHCI_IS_PSS     (1u << 1)   /* PIO Setup FIS */
#define AHCI_IS_DSS     (1u << 2)   /* DMA Setup FIS */
#define AHCI_IS_SDBS    (1u << 3)   /* Set Device Bits FIS */
#define AHCI_IS_UFS     (1u << 4)   /* Unknown FIS */
#define AHCI_IS_DPS     (1u << 5)   /* Descriptor Processed */
#define AHCI_IS_PCS     (1u << 6)   /* Port Connect Change */
#define AHCI_IS_DMPS    (1u << 7)   /* Device Mechanical Presence */
#define AHCI_IS_PRCS    (1u << 22)  /* Phy Ready Change */
#define AHCI_IS_IPMS    (1u << 23)  /* Incorrect Port Multiplier */
#define AHCI_IS_OFS     (1u << 24)  /* Overflow */
#define AHCI_IS_INFS    (1u << 26)  /* Interface Non-fatal Error */
#define AHCI_IS_IFS     (1u << 27)  /* Interface Fatal Error */
#define AHCI_IS_HBDS    (1u << 28)  /* Host Bus Data Error */
#define AHCI_IS_HBFS    (1u << 29)  /* Host Bus Fatal Error */
#define AHCI_IS_TFES    (1u << 30)  /* Task File Error */
#define AHCI_IS_CPDS    (1u << 31)  /* Cold Port Detect */

/* Task File Data bits */
#define AHCI_TFD_STS_BSY (1u << 7)
#define AHCI_TFD_STS_DRQ (1u << 3)
#define AHCI_TFD_STS_ERR (1u << 0)

/* Signature values */
#define AHCI_SIG_SATA   0x00000101u
#define AHCI_SIG_SATAPI 0xEB140101u
#define AHCI_SIG_SEMB   0xC33C0101u
#define AHCI_SIG_PM     0x96690101u

/* FIS types */
#define AHCI_FIS_TYPE_REG_H2D  0x27
#define AHCI_FIS_TYPE_REG_D2H  0x34
#define AHCI_FIS_TYPE_DMA_ACT   0x39
#define AHCI_FIS_TYPE_DMA_SETUP 0x41
#define AHCI_FIS_TYPE_DATA      0x46
#define AHCI_FIS_TYPE_BIST      0x58
#define AHCI_FIS_TYPE_PIO_SETUP  0x5F
#define AHCI_FIS_TYPE_DEV_BITS  0xA1

/* Command header flags */
#define AHCI_CMD_HEADER_C   (1u << 10)  /* Clear BSY upon R_OK */
#define AHCI_CMD_HEADER_B   (1u << 9)   /* BIST */
#define AHCI_CMD_HEADER_R   (1u << 8)   /* Reset */
#define AHCI_CMD_HEADER_P   (1u << 7)   /* Prefetchable */
#define AHCI_CMD_HEADER_W   (1u << 6)   /* Write */
#define AHCI_CMD_HEADER_A   (1u << 5)   /* ATAPI */
#define AHCI_CMD_HEADER_PMP (1u << 12)  /* Port Multiplier Port */

/* FIS structures */
struct ahci_fis {
    uint8_t fis_type;
    uint8_t pmport_c;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2, device;
    uint8_t lba3, lba4, lba5, featureh;
    uint8_t countl, counth;
    uint8_t icc;
    uint8_t control;
    uint8_t rsv[4];
} __attribute__((packed));

/* PRDT entry */
struct ahci_prdt_entry {
    uint32_t dba;       /* Data Base Address */
    uint32_t dbau;      /* Data Base Address Upper 32-bits */
    uint32_t reserved;
    uint32_t dbc;       /* Data Byte Count (0-based, minus 1) */
};

/* Command table */
struct ahci_cmd_table {
    uint8_t cfis[64];   /* Command FIS */
    uint8_t acmd[16];   /* ATAPI command */
    uint8_t reserved[48];
    struct ahci_prdt_entry prdt[AHCI_MAX_PRDT];
};

/* Command header */
struct ahci_cmd_header {
    uint16_t flags;     /* Flags */
    uint16_t prdtl;     /* PRDT length in entries */
    uint32_t prdbc;     /* Physical Region Descriptor Byte Count */
    uint32_t ctba;      /* Command Table Descriptor Base Address */
    uint32_t ctbau;     /* Command Table Descriptor Base Address Upper */
    uint32_t reserved[4];
};

/* HBA memory registers */
struct ahci_hba_mem {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_ports;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint32_t reserved[53];
    uint32_t vendor[24];
};

/* Device info */
struct ahci_device {
    int used;
    int port_num;
    int present;
    int atapi;
    uint32_t signature;
    uint64_t sectors;
    uint32_t sector_size;
    char model[41];
    char serial[21];
    char firmware[9];
    uint16_t pci_bus, pci_slot, pci_func;
};

void ahci_init(void);
int ahci_device_count(void);
struct ahci_device *ahci_get_device(int index);
struct ahci_device *ahci_find(const char *name);
int ahci_read_sectors(struct ahci_device *dev, uint64_t lba, uint32_t count, void *buffer);
int ahci_write_sectors(struct ahci_device *dev, uint64_t lba, uint32_t count, const void *buffer);
int ahci_identify(struct ahci_device *dev);

#endif
