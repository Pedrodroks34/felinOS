#ifndef FELINOS_ACPI_H
#define FELINOS_ACPI_H

#include <stdint.h>

#define ACPI_MAX_TABLES 64

#define ACPI_SPACE_MEMORY 0
#define ACPI_SPACE_IO     1
#define ACPI_SPACE_PCI    2

struct acpi_gas {
    uint8_t space;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed));

struct acpi_table_ref {
    char sig[5];
    uint8_t revision;
    uint32_t phys;
    uint32_t length;
};

struct acpi_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_info {
    int present;
    uint8_t rsdp_revision;
    char oem[7];
    uint32_t rsdp_phys;
    int xsdt;
    uint32_t root_phys;

    int fadt_ok;
    uint32_t fadt_flags;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable_val;
    uint8_t acpi_disable_val;
    uint32_t pm1a_evt;
    uint32_t pm1b_evt;
    uint32_t pm1a_cnt;
    uint32_t pm1b_cnt;

    int s5_ok;
    uint8_t slp_typa;
    uint8_t slp_typb;
    char s5_from[5];

    int reset_ok;
    struct acpi_gas reset_reg;
    uint8_t reset_value;
};

void acpi_init(void);
const struct acpi_info *acpi_get_info(void);
int acpi_table_count(void);
const struct acpi_table_ref *acpi_get_table(int index);

int acpi_can_power_off(void);
int acpi_can_reset(void);
int acpi_sci_enabled(void);
int acpi_enable(void);

/* Both return only when the request had no effect. */
void acpi_power_off(void);
void acpi_reset(void);

#endif
