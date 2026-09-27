#include "drivers/acpi.h"
#include "console.h"
#include "io.h"
#include "vmm.h"
#include "lib/string.h"

#define ACPI_MAX_TABLE_SIZE (16u * 1024u * 1024u)

#define FADT_FLAG_RESET_REG_SUP (1u << 10)

#define PM1_SCI_EN      0x0001u
#define PM1_SLP_TYP_SHIFT 10
#define PM1_SLP_CLEAR   0x3C00u
#define PM1_SLP_EN      0x2000u

struct acpi_rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct acpi_fadt {
    struct acpi_header hdr;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint8_t cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved1;
    uint32_t flags;
    struct acpi_gas reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    struct acpi_gas x_pm1a_evt_blk;
    struct acpi_gas x_pm1b_evt_blk;
    struct acpi_gas x_pm1a_cnt_blk;
    struct acpi_gas x_pm1b_cnt_blk;
} __attribute__((packed));

_Static_assert(sizeof(struct acpi_header) == 36, "ACPI header is 36 bytes");
_Static_assert(__builtin_offsetof(struct acpi_fadt, pm1a_cnt_blk) == 64, "FADT PM1a_CNT_BLK");
_Static_assert(__builtin_offsetof(struct acpi_fadt, flags) == 112, "FADT flags");
_Static_assert(__builtin_offsetof(struct acpi_fadt, reset_reg) == 116, "FADT RESET_REG");
_Static_assert(__builtin_offsetof(struct acpi_fadt, x_dsdt) == 140, "FADT X_DSDT");
_Static_assert(__builtin_offsetof(struct acpi_fadt, x_pm1b_cnt_blk) == 184, "FADT X_PM1b_CNT_BLK");

#define FADT_HAS(fadt_len, field) \
    ((fadt_len) >= __builtin_offsetof(struct acpi_fadt, field) + sizeof(((struct acpi_fadt *)0)->field))

static struct acpi_info info;
static struct acpi_rsdp rsdp;
static struct acpi_table_ref tables[ACPI_MAX_TABLES];
static int table_total;

static void delay_us(uint32_t us) {
    while (us--) {
        io_wait();
    }
}

static const void *map(uint32_t phys, uint32_t len) {
    if (!len || phys + len < phys) {
        return NULL;
    }
    return vmm_map_physical(phys, len, VM_READ, "acpi");
}

static void unmap(const void *ptr) {
    if (ptr) {
        vmm_free((void *)ptr);
    }
}

static int checksum_ok(const void *data, uint32_t len) {
    const uint8_t *b = data;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + b[i]);
    }
    return sum == 0;
}

static const struct acpi_header *map_table(uint32_t phys) {
    const struct acpi_header *h = map(phys, sizeof(struct acpi_header));
    if (!h) {
        return NULL;
    }
    uint32_t len = h->length;
    unmap(h);
    if (len < sizeof(struct acpi_header) || len > ACPI_MAX_TABLE_SIZE) {
        return NULL;
    }
    return map(phys, len);
}

static uint32_t scan_rsdp(uint32_t phys, uint32_t len) {
    const uint8_t *base = map(phys, len);
    uint32_t found = 0;

    if (!base) {
        return 0;
    }
    for (uint32_t off = 0; off + 20 <= len; off += 16) {
        const struct acpi_rsdp *r = (const struct acpi_rsdp *)(base + off);
        if (memcmp(r->signature, "RSD PTR ", 8) != 0 || !checksum_ok(r, 20)) {
            continue;
        }
        memset(&rsdp, 0, sizeof(rsdp));
        if (r->revision >= 2) {
            uint32_t rlen = r->length;
            if (rlen < sizeof(struct acpi_rsdp) || off + rlen > len || !checksum_ok(r, rlen)) {
                continue;
            }
            memcpy(&rsdp, r, sizeof(rsdp));
        } else {
            memcpy(&rsdp, r, 20);
        }
        found = phys + off;
        break;
    }
    unmap(base);
    return found;
}

static uint32_t find_rsdp(void) {
    uint32_t found = 0;
    const uint16_t *seg = map(0x40E, 2);

    if (seg) {
        uint32_t ebda = (uint32_t)(*seg) << 4;
        unmap(seg);
        if (ebda >= 0x400 && ebda < 0xA0000) {
            found = scan_rsdp(ebda, 1024);
        }
    }
    if (!found) {
        found = scan_rsdp(0xE0000, 0x20000);
    }
    return found;
}

static void add_table(uint32_t phys) {
    const struct acpi_header *h = map(phys, sizeof(struct acpi_header));
    if (!h) {
        return;
    }
    if (table_total < ACPI_MAX_TABLES) {
        struct acpi_table_ref *t = &tables[table_total++];
        memcpy(t->sig, h->signature, 4);
        t->sig[4] = '\0';
        t->revision = h->revision;
        t->phys = phys;
        t->length = h->length;
    }
    unmap(h);
}

static const struct acpi_table_ref *find_table(const char *sig) {
    for (int i = 0; i < table_total; i++) {
        if (memcmp(tables[i].sig, sig, 4) == 0) {
            return &tables[i];
        }
    }
    return NULL;
}

static const struct acpi_header *map_root(int use_xsdt) {
    uint32_t phys;
    const char *sig = use_xsdt ? "XSDT" : "RSDT";

    if (use_xsdt) {
        if (rsdp.revision < 2 || !rsdp.xsdt_addr || (rsdp.xsdt_addr >> 32)) {
            return NULL;
        }
        phys = (uint32_t)rsdp.xsdt_addr;
    } else {
        phys = rsdp.rsdt_addr;
        if (!phys) {
            return NULL;
        }
    }

    const struct acpi_header *root = map_table(phys);
    if (root && (memcmp(root->signature, sig, 4) != 0 || !checksum_ok(root, root->length))) {
        unmap(root);
        root = NULL;
    }
    if (root) {
        info.xsdt = use_xsdt;
        info.root_phys = phys;
    }
    return root;
}

static int aml_int(const uint8_t *aml, uint32_t len, uint32_t *p, uint8_t *out) {
    if (*p >= len) {
        return 0;
    }
    uint8_t op = aml[*p];
    if (op == 0x00 || op == 0x01) {
        *out = op;
        *p += 1;
        return 1;
    }
    uint32_t width = (op == 0x0A) ? 1 : (op == 0x0B) ? 2 : (op == 0x0C) ? 4 : 0;
    if (!width || *p + width >= len) {
        return 0;
    }
    *out = aml[*p + 1];
    *p += 1 + width;
    return 1;
}

static int parse_s5(const uint8_t *aml, uint32_t len) {
    for (uint32_t i = 0; i + 5 < len; i++) {
        if (memcmp(aml + i, "_S5_", 4) != 0) {
            continue;
        }
        int named = (i >= 1 && aml[i - 1] == 0x08) ||
                    (i >= 2 && aml[i - 1] == '\\' && aml[i - 2] == 0x08);
        if (!named || aml[i + 4] != 0x12) {
            continue;
        }

        uint32_t p = i + 5;
        if (p >= len) {
            continue;
        }
        p += (uint32_t)(aml[p] >> 6) + 1;
        p += 1;

        uint8_t typa, typb;
        if (!aml_int(aml, len, &p, &typa) || !aml_int(aml, len, &p, &typb)) {
            continue;
        }
        info.slp_typa = typa & 7;
        info.slp_typb = typb & 7;
        return 1;
    }
    return 0;
}

static int find_s5(const struct acpi_table_ref *t) {
    const struct acpi_header *h = map_table(t->phys);
    int ok = 0;

    if (!h) {
        return 0;
    }
    if (!checksum_ok(h, h->length)) {
        klog("acpi: %s checksum mismatch, using it anyway", t->sig);
    }
    ok = parse_s5((const uint8_t *)h + sizeof(struct acpi_header),
                  h->length - sizeof(struct acpi_header));
    unmap(h);
    if (ok) {
        memcpy(info.s5_from, t->sig, 5);
    }
    return ok;
}

static int gas_io_port(const struct acpi_gas *g, uint32_t *port) {
    if (g->space == ACPI_SPACE_IO && g->address && g->address < 0x10000) {
        *port = (uint32_t)g->address;
        return 1;
    }
    return 0;
}

static void parse_fadt(uint32_t phys) {
    const struct acpi_header *h = map_table(phys);
    if (!h) {
        return;
    }

    uint32_t len = h->length;
    const struct acpi_fadt *f = (const struct acpi_fadt *)h;
    uint32_t dsdt = 0;

    if (!checksum_ok(h, len)) {
        klog("acpi: FADT checksum mismatch, using it anyway");
    }
    if (!FADT_HAS(len, pm1b_cnt_blk)) {
        klog("acpi: FADT too short (%u bytes)", len);
        unmap(h);
        return;
    }

    info.sci_int = f->sci_int;
    info.smi_cmd = f->smi_cmd;
    info.acpi_enable_val = f->acpi_enable;
    info.acpi_disable_val = f->acpi_disable;
    info.pm1a_evt = f->pm1a_evt_blk;
    info.pm1b_evt = f->pm1b_evt_blk;
    info.pm1a_cnt = f->pm1a_cnt_blk;
    info.pm1b_cnt = f->pm1b_cnt_blk;
    dsdt = f->dsdt;

    if (FADT_HAS(len, flags)) {
        info.fadt_flags = f->flags;
    }
    if (FADT_HAS(len, x_dsdt) && f->x_dsdt && !(f->x_dsdt >> 32)) {
        dsdt = (uint32_t)f->x_dsdt;
    }

    uint32_t port;
    if (FADT_HAS(len, x_pm1a_cnt_blk) && gas_io_port(&f->x_pm1a_cnt_blk, &port)) {
        info.pm1a_cnt = port;
    }
    if (FADT_HAS(len, x_pm1b_cnt_blk) && gas_io_port(&f->x_pm1b_cnt_blk, &port)) {
        info.pm1b_cnt = port;
    }
    if (FADT_HAS(len, x_pm1a_evt_blk) && gas_io_port(&f->x_pm1a_evt_blk, &port)) {
        info.pm1a_evt = port;
    }
    if (FADT_HAS(len, x_pm1b_evt_blk) && gas_io_port(&f->x_pm1b_evt_blk, &port)) {
        info.pm1b_evt = port;
    }

    if (FADT_HAS(len, reset_value) && (info.fadt_flags & FADT_FLAG_RESET_REG_SUP)) {
        const struct acpi_gas *g = &f->reset_reg;
        uint64_t a = g->address;
        int valid = 0;
        if (g->space == ACPI_SPACE_IO) {
            valid = a && a < 0x10000;
        } else if (g->space == ACPI_SPACE_MEMORY) {
            valid = a && !(a >> 32);
        } else if (g->space == ACPI_SPACE_PCI) {
            valid = ((a >> 32) & 0xFFFF) < 32 && ((a >> 16) & 0xFFFF) < 8 && (a & 0xFFFF) < 256;
        }
        if (valid) {
            info.reset_reg = *g;
            info.reset_value = f->reset_value;
            info.reset_ok = 1;
        }
    }
    unmap(h);

    info.fadt_ok = info.pm1a_cnt != 0;

    if (dsdt) {
        add_table(dsdt);
    }
    const struct acpi_table_ref *d = find_table("DSDT");
    if (d && d->phys == dsdt && find_s5(d)) {
        info.s5_ok = 1;
        return;
    }
    for (int i = 0; i < table_total && !info.s5_ok; i++) {
        if (memcmp(tables[i].sig, "SSDT", 4) == 0 && find_s5(&tables[i])) {
            info.s5_ok = 1;
        }
    }
}

void acpi_init(void) {
    memset(&info, 0, sizeof(info));
    table_total = 0;

    uint32_t phys = find_rsdp();
    if (!phys) {
        klog("acpi: no RSDP found, using legacy power ports");
        return;
    }

    info.present = 1;
    info.rsdp_phys = phys;
    info.rsdp_revision = rsdp.revision;
    memcpy(info.oem, rsdp.oem_id, 6);
    info.oem[6] = '\0';
    for (int i = 5; i >= 0 && info.oem[i] == ' '; i--) {
        info.oem[i] = '\0';
    }
    klog("acpi: RSDP at 0x%08x, revision %u, OEM %s", phys, (uint32_t)info.rsdp_revision, info.oem);

    const struct acpi_header *root = map_root(1);
    if (!root) {
        root = map_root(0);
    }
    if (!root) {
        klog("acpi: no valid RSDT/XSDT");
        return;
    }

    uint32_t esize = info.xsdt ? 8 : 4;
    uint32_t count = (root->length - sizeof(struct acpi_header)) / esize;
    const uint8_t *entries = (const uint8_t *)root + sizeof(struct acpi_header);
    uint32_t fadt = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t addr = 0;
        if (info.xsdt) {
            memcpy(&addr, entries + i * 8, 8);
        } else {
            uint32_t a32;
            memcpy(&a32, entries + i * 4, 4);
            addr = a32;
        }
        if (!addr || (addr >> 32)) {
            continue;
        }
        add_table((uint32_t)addr);
        if (table_total > 0 && memcmp(tables[table_total - 1].sig, "FACP", 4) == 0) {
            fadt = (uint32_t)addr;
        }
    }
    unmap(root);
    klog("acpi: %s at 0x%08x, %u tables", info.xsdt ? "XSDT" : "RSDT", info.root_phys,
         (uint32_t)table_total);

    if (!fadt) {
        klog("acpi: no FADT");
        return;
    }
    parse_fadt(fadt);

    if (info.fadt_ok) {
        klog("acpi: PM1a_CNT 0x%x PM1b_CNT 0x%x SMI_CMD 0x%x SCI_EN %u", info.pm1a_cnt,
             info.pm1b_cnt, info.smi_cmd, (uint32_t)acpi_sci_enabled());
    }
    if (info.s5_ok) {
        klog("acpi: _S5_ in %s, SLP_TYP %u/%u", info.s5_from, (uint32_t)info.slp_typa,
             (uint32_t)info.slp_typb);
    } else {
        klog("acpi: no _S5_ object found");
    }
    if (info.reset_ok) {
        klog("acpi: reset register space %u address 0x%x value 0x%02x",
             (uint32_t)info.reset_reg.space, (uint32_t)info.reset_reg.address,
             (uint32_t)info.reset_value);
    }
}

const struct acpi_info *acpi_get_info(void) {
    return &info;
}

int acpi_table_count(void) {
    return table_total;
}

const struct acpi_table_ref *acpi_get_table(int index) {
    if (index < 0 || index >= table_total) {
        return NULL;
    }
    return &tables[index];
}

int acpi_can_power_off(void) {
    return info.present && info.fadt_ok && info.s5_ok;
}

int acpi_can_reset(void) {
    return info.present && info.reset_ok;
}

int acpi_sci_enabled(void) {
    if (!info.fadt_ok) {
        return 0;
    }
    return (inw((uint16_t)info.pm1a_cnt) & PM1_SCI_EN) != 0;
}

int acpi_enable(void) {
    if (!info.fadt_ok) {
        return -1;
    }
    if (acpi_sci_enabled()) {
        return 0;
    }
    if (!info.smi_cmd || !info.acpi_enable_val) {
        return -1;
    }
    outb((uint16_t)info.smi_cmd, info.acpi_enable_val);
    for (int i = 0; i < 3000; i++) {
        if (acpi_sci_enabled()) {
            return 0;
        }
        delay_us(1000);
    }
    return -1;
}

void acpi_power_off(void) {
    if (!acpi_can_power_off()) {
        return;
    }
    cli();
    acpi_enable();

    if (info.pm1a_evt) {
        outw((uint16_t)info.pm1a_evt, 0xFFFF);
    }
    if (info.pm1b_evt) {
        outw((uint16_t)info.pm1b_evt, 0xFFFF);
    }

    uint16_t a = (uint16_t)((inw((uint16_t)info.pm1a_cnt) & ~PM1_SLP_CLEAR) |
                            ((uint16_t)info.slp_typa << PM1_SLP_TYP_SHIFT));
    uint16_t b = 0;
    outw((uint16_t)info.pm1a_cnt, a);
    if (info.pm1b_cnt) {
        b = (uint16_t)((inw((uint16_t)info.pm1b_cnt) & ~PM1_SLP_CLEAR) |
                       ((uint16_t)info.slp_typb << PM1_SLP_TYP_SHIFT));
        outw((uint16_t)info.pm1b_cnt, b);
    }

    outw((uint16_t)info.pm1a_cnt, (uint16_t)(a | PM1_SLP_EN));
    if (info.pm1b_cnt) {
        outw((uint16_t)info.pm1b_cnt, (uint16_t)(b | PM1_SLP_EN));
    }
    delay_us(1000000);
}

void acpi_reset(void) {
    if (!acpi_can_reset()) {
        return;
    }
    cli();

    uint64_t addr = info.reset_reg.address;
    uint8_t value = info.reset_value;

    switch (info.reset_reg.space) {
    case ACPI_SPACE_IO:
        outb((uint16_t)addr, value);
        break;
    case ACPI_SPACE_PCI: {
        uint32_t dev = (uint32_t)(addr >> 32) & 0xFFFF;
        uint32_t fn = (uint32_t)(addr >> 16) & 0xFFFF;
        uint32_t reg = (uint32_t)addr & 0xFFFF;
        outl(0xCF8, 0x80000000u | (dev << 11) | (fn << 8) | (reg & 0xFCu));
        outb((uint16_t)(0xCFC + (reg & 3)), value);
        break;
    }
    case ACPI_SPACE_MEMORY: {
        volatile uint8_t *p = vmm_map_physical((uint32_t)addr, 1,
                                               VM_READ | VM_WRITE | VM_UNCACHED, "acpi-reset");
        if (p) {
            *p = value;
        }
        break;
    }
    default:
        return;
    }
    delay_us(500000);
}
