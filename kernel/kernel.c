#include <stdint.h>
#include "system.h"
#include "console.h"
#include "gdt.h"
#include "idt.h"
#include "multiboot.h"
#include "io.h"
#include "pmm.h"
#include "paging.h"
#include "vmm.h"
#include "swap.h"
#include "mm/writeback.h"
#include "mm/ksm.h"
#include "mm/zswap.h"
#include "mm/numa.h"
#include "mm/cgroup.h"
#include "mm/huge.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "drivers/vga.h"
#include "drivers/fb.h"
#include "drivers/serial.h"
#include "drivers/pic.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "drivers/keyboard.h"
#include "drivers/ata.h"
#include "drivers/ahci.h"
#include "drivers/apic.h"
#include "drivers/font.h"
#include "drivers/pci.h"
#include "drivers/acpi.h"
#include "drivers/cpu.h"
#include "fs/vfs.h"
#include "fs/gatofs.h"
#include "fs/fat32.h"
#include "lib/format.h"
#include "sh/shell.h"
#include "user.h"
#include "sched.h"
#include "drivers/input.h"
#include "drivers/e1000.h"
#include "drivers/rtl8139.h"
#include "drivers/rtl8169.h"
#include "bcache.h"
#include "net/netbuf.h"
#include "net/netif.h"
#include "net/arp.h"
#include "net/ip.h"
#include "net/udp.h"
#include "net/tcp.h"
#include "net/socket.h"

extern void ap_startup(void);

static void banner(void) {
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("\n%s %s", FELINOS_NAME, FELINOS_VERSION);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("  -  %s kernel %s (%s)\n\n", KERNEL_NAME, KERNEL_VERSION, KERNEL_ARCH);
}

static void shell_thread(void *arg) {
    for (;;) {
        shell_run();
    }
}

static void net_maint_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep_ms(100);
        arp_age_tick();
        tcp_timer_tick();
    }
}

static int net_init(void) {
    netbuf_pool_init();
    arp_init();
    ip_init();
    udp_init();
    tcp_init();
    socket_init();

    int has_nic = (e1000_init() == 0 || rtl8169_init() == 0 || rtl8139_init() == 0);
    if (has_nic) {
        netif_start_rx_thread();
        kthread_create("netmaint", net_maint_thread, NULL);
    }
    return has_nic;
}

static void boot_step(const char *name, const char *detail) {
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    kprintf("  [");
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("ok");
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    kprintf("] ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("%-22s %s\n", name, detail);
    klog("%s: %s", name, detail);
}

void kernel_main(uint32_t magic, struct multiboot_info *mbi) {
    char detail[96];

    gdt_install();
    console_init();
    banner();

    boot_step("global descriptors", "flat 64-bit code and data segments");

    idt_install();
    pic_remap();
    boot_step("interrupt table", "32 exceptions, 16 hardware IRQs");

    system_init((magic == MULTIBOOT_BOOTLOADER_MAGIC) ? mbi : NULL);
    snprintf(detail, sizeof(detail), "%u KB reported by the bootloader", system_total_kb());
    boot_step("memory map", detail);

    uint32_t mem_top = 0x100000u + system_mem_upper_kb() * 1024u;

    pmm_init(mem_top);
    sched_reserve_shell_stack();
    snprintf(detail, sizeof(detail), "%u frames (%u MB), %u free",
             pmm_total_frames(),
             (pmm_total_frames() * PMM_FRAME_SIZE) / (1024u * 1024u),
             pmm_free_frames());
    boot_step("physical memory", detail);

    paging_init(mem_top);
    snprintf(detail, sizeof(detail), "%u MB mapped, %u tables, directory at 0x%08x",
             paging_mapped_mb(), paging_table_count(), paging_directory_phys());
    boot_step("paging", detail);

    swap_init();
    vmm_init();
    snprintf(detail, sizeof(detail), "demand paging, copy-on-write, %u regions",
             (uint32_t)vmm_region_count(vmm_kernel_space()));
    boot_step("virtual memory", detail);

    heap_init(vmm_heap_base(), vmm_heap_size());
    snprintf(detail, sizeof(detail), "%u KB demand-paged heap at 0x%08x",
             vmm_heap_size() / 1024u, vmm_heap_base());
    boot_step("kernel heap", detail);

    /* The APIC maps its own MMIO windows through the VMM, so it can only be
     * brought up once paging, the VMM and the heap exist. Nothing between the
     * interrupt table and here touches it, and nothing after here needs it
     * before it runs. */
    apic_init();
    if (apic_is_present()) {
        snprintf(detail, sizeof(detail), "local APIC at 0x%08x, %u CPU(s)",
                 apic_get_base(), smp_cpu_count());
    } else {
        snprintf(detail, sizeof(detail), "not present, using PIC");
    }
    boot_step("apic", detail);

    pit_init();
    boot_step("system timer", "PIT channel 0 at 100 Hz");

    sched_init();
    snprintf(detail, sizeof(detail), "preemptive round-robin, %u ms time slices",
             sched_quantum() * (1000u / PIT_FREQUENCY));
    boot_step("scheduler", detail);

    keyboard_init();
    input_init();
    boot_step("keyboard", "PS/2 set 1, shift, ctrl and caps lock");

    fb_init();
    boot_step("framebuffer", "320x200 256-color at 0xA0000, inactive (VGA text still active)");

    font_init();
    boot_step("font renderer", "8x16 bitmap font for framebuffer");

    if (serial_available()) {
        boot_step("serial console", "COM1 at 115200 baud, input and output");
    }

    struct rtc_time now;
    rtc_read(&now);
    snprintf(detail, sizeof(detail), "%04u-%02u-%02u %02u:%02u:%02u",
             now.year, now.month, now.day, now.hour, now.minute, now.second);
    boot_step("real time clock", detail);

    char vendor[16];
    cpu_vendor(vendor);
    snprintf(detail, sizeof(detail), "%s family %u model %u",
             vendor, cpu_family(), cpu_model());
    boot_step("processor", detail);

    pci_scan();
    snprintf(detail, sizeof(detail), "%d devices on the bus", pci_device_count());
    boot_step("pci bus", detail);

    if (net_init()) {
        struct netif *nif = netif_default();
        char mac[18];
        mac_to_str(nif->mac, mac);
        snprintf(detail, sizeof(detail), "%s driver, mac %s, run 'dhcp' or 'ifconfig' to configure",
                 nif->driver_name, mac);
    } else {
        snprintf(detail, sizeof(detail), "no supported NIC found");
    }
    boot_step("network", detail);

    acpi_init();
    {
        const struct acpi_info *ai = acpi_get_info();
        if (acpi_can_power_off()) {
            snprintf(detail, sizeof(detail), "rev %u, %u tables, S5 via PM1a 0x%x, reset %s",
                     (uint32_t)ai->rsdp_revision, (uint32_t)acpi_table_count(), ai->pm1a_cnt,
                     acpi_can_reset() ? "register" : "legacy");
        } else if (ai->present) {
            snprintf(detail, sizeof(detail), "rev %u, %u tables, no S5 (legacy power ports)",
                     (uint32_t)ai->rsdp_revision, (uint32_t)acpi_table_count());
        } else {
            snprintf(detail, sizeof(detail), "not found, legacy power ports");
        }
        boot_step("acpi", detail);
    }

    smp_init();
    smp_boot_aps();

    bcache_init();
    boot_step("buffer cache", "4 MB, 1024 lines of 4 KB over the ATA layer");

    ata_init();
    if (ata_device_count() > 0) {
        struct ata_device *first = ata_get_device(0);
        snprintf(detail, sizeof(detail), "%d device(s), first is %s",
                 ata_device_count(), first->model);
    } else {
        snprintf(detail, sizeof(detail), "no drives attached");
    }
    boot_step("ata controller", detail);

    ahci_init();
    if (ahci_device_count() > 0) {
        struct ahci_device *first = ahci_get_device(0);
        snprintf(detail, sizeof(detail), "%d SATA device(s), first is %s",
                 ahci_device_count(), first->model);
    } else {
        snprintf(detail, sizeof(detail), "no SATA devices");
    }
    boot_step("ahci controller", detail);

    fat32_init();
    boot_step("fat32", "FAT32 reader ready for host file exchange");

    if (swap_autostart() == SWAP_OK) {
        snprintf(detail, sizeof(detail), "%u MB on %s, %u slots",
                 (swap_total_slots() * PAGE_SIZE) / (1024u * 1024u),
                 swap_device_name(), swap_total_slots());
    } else {
        snprintf(detail, sizeof(detail), "no swap area (mkswap <disk>)");
    }
    boot_step("swap", detail);

    vfs_init();
    writeback_init();
    ksm_init();
    ksm_start_thread();
    zswap_init();
    numa_init();
    cgroup_mem_init();
    huge_init();
    huge_reserve_2mb(64);  /* Reserve 128MB for huge pages */

    int fresh_volume = 0;
    int populated = 0;
    if (gatofs_probe() != 0 && gatofs_autoformat() == 0) {
        fresh_volume = 1;
    }
    if (!(gatofs_mounted() && vfs_mount("gatofs", NULL, "/") == 0)) {
        vfs_mount("ramfs", NULL, "/");
    }
    if (vfs_root_is_empty()) {
        vfs_populate_defaults();
        populated = 1;
    }
    vfs_mkpath("/dev");
    vfs_mkpath("/proc");
    vfs_mkpath("/tmp");
    vfs_mount("devfs", NULL, "/dev");
    vfs_mount("procfs", NULL, "/proc");
    vfs_mount("ramfs", NULL, "/tmp");

    for (int i = 0; i < ata_device_count(); i++) {
        struct ata_device *dev = ata_get_device(i);
        if (!dev) continue;
        struct mbr_partition parts[4];
        int np = ata_read_partitions(dev, parts, 4);
        for (int p = 0; p < np; p++) {
            if (parts[p].type == 0x0B || parts[p].type == 0x0C) {
                char mnt[16];
                snprintf(mnt, sizeof(mnt), "/mnt/fat%d", i * 4 + p);
                vfs_mkpath(mnt);
                struct fat32_handle *fh = kmalloc(sizeof(struct fat32_handle));
                if (fh && fat32_mount(fh, 0, parts[p].lba_start) == 0) {
                    klog("fat32: mounted %s partition %d on %s", dev->name, p, mnt);
                }
            }
        }
    }

    for (int i = 0; i < ahci_device_count(); i++) {
        struct ahci_device *dev = ahci_get_device(i);
        if (!dev) continue;
        uint8_t sector[512];
        if (ahci_read_sectors(dev, 0, 1, sector) == 0 &&
            sector[510] == 0x55 && sector[511] == 0xAA) {
            for (int p = 0; p < 4; p++) {
                uint8_t *entry = &sector[446 + p * 16];
                uint8_t type = entry[4];
                if (type == 0x0B || type == 0x0C) {
                    uint32_t lba_start = (uint32_t)entry[8] | ((uint32_t)entry[9] << 8) |
                                         ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
                    char mnt[16];
                    snprintf(mnt, sizeof(mnt), "/mnt/fat%d", i * 4 + p);
                    vfs_mkpath(mnt);
                    struct fat32_handle *fh = kmalloc(sizeof(struct fat32_handle));
                    if (fh && fat32_mount(fh, 1, lba_start) == 0) {
                        klog("fat32: mounted %s partition %d on %s", dev->model, p, mnt);
                    }
                }
            }
        }
    }

    struct vfs_mount_info root_mount;
    vfs_mount_info(0, &root_mount);
    snprintf(detail, sizeof(detail), "/ is %s (%s), /dev, /proc and /tmp mounted",
             root_mount.type, root_mount.source);
    boot_step("virtual filesystem", detail);

    if (gatofs_mounted()) {
        struct gatofs_info vi;
        gatofs_info(&vi);
        snprintf(detail, sizeof(detail), "%s on %s, %u MB%s", vi.label, vi.dev,
                 vi.total_blocks >> 8, fresh_volume ? " (formatted now)" : "");
    } else {
        snprintf(detail, sizeof(detail), "no volume (gatofs format <disk>)");
    }
    boot_step("gatofs", detail);

    user_init(populated);
    boot_step("user mode", "ring 3, per-process address spaces, int 0x80 syscalls, ELF loader");

    shell_init();
    boot_step("shell", "vsh with pipes, redirection and history");

    kprintf("\n");
    sched_run(shell_thread);
}
