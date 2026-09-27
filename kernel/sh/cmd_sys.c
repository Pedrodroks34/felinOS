#include "sh/cmds.h"
#include "system.h"
#include "console.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "lib/format.h"
#include "drivers/vga.h"
#include "drivers/rtc.h"
#include "drivers/pit.h"
#include "drivers/cpu.h"
#include "drivers/power.h"
#include "drivers/acpi.h"
#include "drivers/speaker.h"
#include "drivers/ata.h"
#include "fs/gatofs.h"
#include "pmm.h"
#include "paging.h"
#include "vmm.h"
#include "swap.h"

extern void simple_compile(void);

int cmd_help(int argc, char **argv, struct stream *in, struct stream *out) {
    const struct command *table = shell_command_table();
    int count = shell_command_count();

    if (argc > 1) {
        const struct command *cmd = shell_find_command(argv[1]);
        if (!cmd) {
            cmd_error(out, "help", argv[1], "no such command");
            return 1;
        }
        st_printf(out, "%s - %s\nusage: %s\n", cmd->name, cmd->summary, cmd->usage);
        return 0;
    }

    st_printf(out, "FelinOS shell, %d commands available.\n", count);
    st_puts(out, "Use 'man <command>' for details. Pipes, redirection, && and || work.\n\n");

    for (int i = 0; i < count; i++) {
        st_printf(out, "  %-10s %s\n", table[i].name, table[i].summary);
    }
    return 0;
}

int cmd_man(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        cmd_error(out, "man", NULL, "what manual page do you want?");
        return 1;
    }

    const struct command *cmd = shell_find_command(argv[1]);
    if (!cmd) {
        cmd_error(out, "man", argv[1], "no manual entry");
        return 1;
    }

    st_printf(out, "NAME\n    %s - %s\n\n", cmd->name, cmd->summary);
    st_printf(out, "SYNOPSIS\n    %s\n\n", cmd->usage);
    st_printf(out, "DESCRIPTION\n    Part of the FelinOS base command set, served by the Gato kernel.\n");
    st_puts(out, "    Output can be piped with | and redirected with > and >>.\n");
    return 0;
}

int cmd_uname(int argc, char **argv, struct stream *in, struct stream *out) {
    int all = cmd_has_flag(argc, argv, "-a");
    int release = cmd_has_flag(argc, argv, "-r");
    int machine = cmd_has_flag(argc, argv, "-m");
    int nodename = cmd_has_flag(argc, argv, "-n");

    if (all) {
        const char *host = shell_getenv("HOSTNAME");
        st_printf(out, "%s %s %s %s %s\n", KERNEL_NAME, host ? host : "felinos",
                  KERNEL_VERSION, KERNEL_ARCH, FELINOS_NAME);
        return 0;
    }
    if (release) {
        st_printf(out, "%s\n", KERNEL_VERSION);
        return 0;
    }
    if (machine) {
        st_printf(out, "%s\n", KERNEL_ARCH);
        return 0;
    }
    if (nodename) {
        const char *host = shell_getenv("HOSTNAME");
        st_printf(out, "%s\n", host ? host : "felinos");
        return 0;
    }
    st_printf(out, "%s\n", KERNEL_NAME);
    return 0;
}

int cmd_uptime(int argc, char **argv, struct stream *in, struct stream *out) {
    uint32_t seconds = pit_uptime_seconds();
    uint32_t hours = seconds / 3600;
    uint32_t minutes = (seconds % 3600) / 60;
    struct rtc_time now;
    rtc_read(&now);

    st_printf(out, "%02u:%02u:%02u up %u hours, %u minutes, %u seconds, %u ticks\n",
              now.hour, now.minute, now.second, hours, minutes, seconds % 60, pit_ticks());
    return 0;
}

int cmd_date(int argc, char **argv, struct stream *in, struct stream *out) {
    struct rtc_time t;
    rtc_read(&t);

    int wd = day_of_week(t.year, t.month, t.day);
    st_printf(out, "%s %s %u %02u:%02u:%02u UTC %u\n",
              weekday_name(wd), month_name(t.month), t.day,
              t.hour, t.minute, t.second, t.year);
    return 0;
}

int cmd_cal(int argc, char **argv, struct stream *in, struct stream *out) {
    struct rtc_time now;
    rtc_read(&now);

    int month = now.month;
    int year = now.year;

    if (argc == 2) {
        year = atoi(argv[1]);
        month = 0;
    } else if (argc >= 3) {
        month = atoi(argv[1]);
        year = atoi(argv[2]);
    }

    int first = (month == 0) ? 1 : month;
    int last = (month == 0) ? 12 : month;

    for (int m = first; m <= last; m++) {
        char header[32];
        snprintf(header, sizeof(header), "%s %u", month_name(m), (uint32_t)year);
        int pad = (20 - (int)strlen(header)) / 2;
        for (int i = 0; i < pad; i++) {
            st_putc(out, ' ');
        }
        st_printf(out, "%s\n", header);
        st_puts(out, "Su Mo Tu We Th Fr Sa\n");

        int start = day_of_week(year, m, 1);
        int days = days_in_month(year, m);

        for (int i = 0; i < start; i++) {
            st_puts(out, "   ");
        }
        for (int d = 1; d <= days; d++) {
            int today = (d == now.day && m == now.month && year == now.year);
            if (today) {
                st_printf(out, "%2u*", (uint32_t)d);
            } else {
                st_printf(out, "%2u ", (uint32_t)d);
            }
            if ((start + d) % 7 == 0) {
                st_putc(out, '\n');
            }
        }
        if ((start + days) % 7 != 0) {
            st_putc(out, '\n');
        }
        if (m != last) {
            st_putc(out, '\n');
        }
    }
    return 0;
}

int cmd_free(int argc, char **argv, struct stream *in, struct stream *out) {
    int human = cmd_has_flag(argc, argv, "-h");
    uint32_t total, used, largest, blocks;
    struct vmm_stats vm;
    heap_stats(&total, &used, &largest, &blocks);
    vmm_get_stats(&vm);

    uint32_t ram = system_total_kb() * 1024u;
    uint32_t kernel = system_kernel_end() - system_kernel_start();

    uint32_t pf_total = pmm_total_frames();
    uint32_t pf_used = pmm_used_frames();
    uint32_t pf_free = pmm_free_frames();

    if (human) {
        char a[16], b[16], c[16], d[16], e[16], f[16], g[16], h[16];
        cmd_format_size(ram, a, sizeof(a));
        cmd_format_size(kernel, b, sizeof(b));
        cmd_format_size(total, c, sizeof(c));
        cmd_format_size(used, d, sizeof(d));
        cmd_format_size(total - used, e, sizeof(e));
        cmd_format_size(pf_total * PMM_FRAME_SIZE, f, sizeof(f));
        cmd_format_size(pf_used * PMM_FRAME_SIZE, g, sizeof(g));
        cmd_format_size(pf_free * PMM_FRAME_SIZE, h, sizeof(h));
        st_printf(out, "%-12s %10s\n", "RAM total:", a);
        st_printf(out, "%-12s %10s\n", "Kernel:", b);
        st_printf(out, "%-12s %10s\n", "Heap:", c);
        st_printf(out, "%-12s %10s\n", "Heap used:", d);
        st_printf(out, "%-12s %10s\n", "Heap free:", e);
        st_printf(out, "%-12s %10s\n", "Frames:", f);
        st_printf(out, "%-12s %10s\n", "Frames used:", g);
        st_printf(out, "%-12s %10s\n", "Frames free:", h);
        char i[16], j[16], k[16];
        cmd_format_size(vm.virtual_pages * PAGE_SIZE, i, sizeof(i));
        cmd_format_size(vm.resident_pages * PAGE_SIZE, j, sizeof(j));
        cmd_format_size(vm.swapped_pages * PAGE_SIZE, k, sizeof(k));
        st_printf(out, "%-12s %10s\n", "Virtual:", i);
        st_printf(out, "%-12s %10s\n", "Resident:", j);
        st_printf(out, "%-12s %10s\n", "Swapped:", k);
        if (swap_active()) {
            char l[16];
            cmd_format_size(swap_total_slots() * PAGE_SIZE, l, sizeof(l));
            st_printf(out, "%-12s %10s  on %s\n", "Swap:", l, swap_device_name());
        }
    } else {
        st_printf(out, "%-12s %10s %10s %10s\n", "", "total", "used", "free");
        st_printf(out, "%-12s %10u %10u %10u\n", "Memory:", ram, kernel + used, ram - kernel - used);
        st_printf(out, "%-12s %10u %10u %10u\n", "Heap:", total, used, total - used);
        st_printf(out, "%-12s %10u %10u %10u\n", "Frames:", pf_total, pf_used, pf_free);
        st_printf(out, "%-12s %10u %10u %10u\n", "Virtual:",
                  vm.virtual_pages * PAGE_SIZE, vm.resident_pages * PAGE_SIZE,
                  vm.swapped_pages * PAGE_SIZE);
        if (swap_active()) {
            st_printf(out, "%-12s %10u %10u %10u\n", "Swap:",
                      swap_total_slots() * PAGE_SIZE, swap_used_slots() * PAGE_SIZE,
                      swap_free_slots() * PAGE_SIZE);
        }
    }
    st_printf(out, "\n%u heap blocks, largest free block %u bytes.\n", blocks, largest);
    st_printf(out, "%u page faults served (%u demand, %u copy-on-write, %u from swap).\n",
              vm.page_faults, vm.demand_faults, vm.cow_faults, vm.swap_faults);
    return 0;
}

int cmd_lsmem(int argc, char **argv, struct stream *in, struct stream *out) {
    struct multiboot_info *mbi = system_multiboot();

    st_printf(out, "%-12s %-12s %-12s %s\n", "Start", "End", "Size", "Type");
    st_printf(out, "%-12s %-12s %-12u %s\n", "0x00000000", "0x000A0000",
              system_mem_lower_kb() * 1024u, "usable (low)");

    if (mbi && (mbi->flags & (1 << 6)) && mbi->mmap_length) {
        uint32_t addr = mbi->mmap_addr;
        uint32_t end = addr + mbi->mmap_length;

        while (addr < end) {
            struct multiboot_mmap_entry *entry = (struct multiboot_mmap_entry *)addr;
            uint32_t base = (uint32_t)entry->addr;
            uint32_t length = (uint32_t)entry->len;
            st_printf(out, "0x%08x   0x%08x   %-12u %s\n", base, base + length, length,
                      (entry->type == 1) ? "usable" : "reserved");
            addr += entry->size + 4;
        }
    } else {
        st_printf(out, "0x%08x   0x%08x   %-12u %s\n", 0x100000u,
                  0x100000u + system_mem_upper_kb() * 1024u,
                  system_mem_upper_kb() * 1024u, "usable (high)");
    }

    st_printf(out, "\nKernel image: 0x%08x - 0x%08x\n",
              system_kernel_start(), system_kernel_end());
    st_printf(out, "Kernel heap:  0x%08x - 0x%08x (virtual, demand paged)\n",
              vmm_heap_base(), vmm_heap_base() + vmm_heap_size());
    st_printf(out, "Vmalloc area: 0x%08x - 0x%08x\n", VMM_VMALLOC_BASE, VMM_VMALLOC_END);
    st_printf(out, "Mmio area:    0x%08x - 0x%08x\n", VMM_MMIO_BASE, VMM_MMIO_END);
    st_printf(out, "Page tables:  0x%08x - 0xffffffff (reserved)\n", VMM_SELFMAP_BASE);
    st_printf(out, "Page directory: 0x%08x, %u MB identity-mapped, %u tables\n",
              paging_directory_phys(), paging_mapped_mb(), paging_table_count());
    st_printf(out, "Frames: %u total, %u used, %u free (4 KB each)\n",
              pmm_total_frames(), pmm_used_frames(), pmm_free_frames());
    if (swap_active()) {
        st_printf(out, "Swap: %u of %u slots used on %s\n",
                  swap_used_slots(), swap_total_slots(), swap_device_name());
    }
    return 0;
}

int cmd_dmesg(int argc, char **argv, struct stream *in, struct stream *out) {
    if (cmd_has_flag(argc, argv, "-c")) {
        klog_clear();
        return 0;
    }
    st_puts(out, klog_buffer());
    return 0;
}

int cmd_whoami(int argc, char **argv, struct stream *in, struct stream *out) {
    const char *user = shell_getenv("USER");
    st_printf(out, "%s\n", user ? user : "root");
    return 0;
}

int cmd_credits(int argc, char **argv, struct stream *in, struct stream *out) {
    st_printf(out,
        "Thanks to these people, the system exists and keeps improving every day\n"
        "Builderman\n"
        "Rtk\n"
        "Two times\n"
        "Telamon\n"
        "Samuel\n"
        "Rafael\n"
        "ShyLily\n");
    return 0;
}

int cmd_hostname(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc > 1) {
        shell_setenv("HOSTNAME", argv[1]);
        char line[SHELL_LINE_MAX];
        snprintf(line, sizeof(line), "%s\n", argv[1]);
        vfs_save("/etc/hostname", line, (uint32_t)strlen(line));
        return 0;
    }
    const char *host = shell_getenv("HOSTNAME");
    st_printf(out, "%s\n", host ? host : "felinos");
    return 0;
}

int cmd_clear(int argc, char **argv, struct stream *in, struct stream *out) {
    console_clear();
    return 0;
}

int cmd_color(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        st_puts(out, "colors: 0 black, 1 blue, 2 green, 3 cyan, 4 red, 5 magenta,\n");
        st_puts(out, "        6 brown, 7 light grey, 8 dark grey, 9 light blue,\n");
        st_puts(out, "        10 light green, 11 light cyan, 12 light red,\n");
        st_puts(out, "        13 light magenta, 14 yellow, 15 white\n");
        return 0;
    }

    uint8_t fg = (uint8_t)(atoi(argv[1]) & 0x0F);
    uint8_t bg = (argc > 2) ? (uint8_t)(atoi(argv[2]) & 0x0F) : 0;
    vga_set_color(fg, bg);
    return 0;
}

int cmd_history(int argc, char **argv, struct stream *in, struct stream *out) {
    if (cmd_has_flag(argc, argv, "-c")) {
        shell_history_clear();
        return 0;
    }
    for (int i = 0; i < shell_history_count(); i++) {
        st_printf(out, "%5u  %s\n", (uint32_t)(i + 1), shell_history_get(i));
    }
    return 0;
}

int cmd_alias(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        for (int i = 0; i < shell_alias_count(); i++) {
            const char *name = shell_alias_name(i);
            if (name) {
                st_printf(out, "alias %s='%s'\n", name, shell_alias_value(i));
            }
        }
        return 0;
    }

    char *equals = strchr(argv[1], '=');
    if (!equals) {
        const char *value = shell_get_alias(argv[1]);
        if (!value) {
            cmd_error(out, "alias", argv[1], "not found");
            return 1;
        }
        st_printf(out, "alias %s='%s'\n", argv[1], value);
        return 0;
    }

    *equals = '\0';
    shell_set_alias(argv[1], equals + 1);
    return 0;
}

int cmd_unalias(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        cmd_error(out, "unalias", NULL, "missing name");
        return 1;
    }
    if (shell_remove_alias(argv[1]) < 0) {
        cmd_error(out, "unalias", argv[1], "not found");
        return 1;
    }
    return 0;
}

int cmd_env(int argc, char **argv, struct stream *in, struct stream *out) {
    for (int i = 0; i < shell_env_count(); i++) {
        const char *name = shell_env_name(i);
        if (name) {
            st_printf(out, "%s=%s\n", name, shell_env_value(i));
        }
    }
    return 0;
}

int cmd_export(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        return cmd_env(argc, argv, in, out);
    }

    char *equals = strchr(argv[1], '=');
    if (equals) {
        *equals = '\0';
        shell_setenv(argv[1], equals + 1);
        return 0;
    }
    if (argc >= 3) {
        shell_setenv(argv[1], argv[2]);
        return 0;
    }

    cmd_error(out, "export", NULL, "usage: export NAME=value");
    return 1;
}

int cmd_unset(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        cmd_error(out, "unset", NULL, "missing name");
        return 1;
    }
    shell_unsetenv(argv[1]);
    return 0;
}

int cmd_which(int argc, char **argv, struct stream *in, struct stream *out) {
    int status = 0;

    for (int i = 1; i < argc; i++) {
        const struct command *cmd = shell_find_command(argv[i]);
        if (cmd) {
            st_printf(out, "%s: shell builtin\n", argv[i]);
        } else if (shell_get_alias(argv[i])) {
            st_printf(out, "%s: aliased to '%s'\n", argv[i], shell_get_alias(argv[i]));
        } else {
            st_printf(out, "%s: not found\n", argv[i]);
            status = 1;
        }
    }
    return status;
}

int cmd_exit(int argc, char **argv, struct stream *in, struct stream *out) {
    shell_request_exit();
    return 0;
}

int cmd_true(int argc, char **argv, struct stream *in, struct stream *out) {
    return 0;
}

int cmd_false(int argc, char **argv, struct stream *in, struct stream *out) {
    return 1;
}

int cmd_test(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc == 3) {
        struct vfs_stat st;
        int found = vfs_stat(argv[2], &st) == 0;
        if (strcmp(argv[1], "-e") == 0) {
            return found ? 0 : 1;
        }
        if (strcmp(argv[1], "-f") == 0) {
            return (found && st.type == VFS_FILE) ? 0 : 1;
        }
        if (strcmp(argv[1], "-d") == 0) {
            return (found && st.type == VFS_DIR) ? 0 : 1;
        }
        if (strcmp(argv[1], "-s") == 0) {
            return (found && st.size > 0) ? 0 : 1;
        }
        if (strcmp(argv[1], "-z") == 0) {
            return (argv[2][0] == '\0') ? 0 : 1;
        }
        if (strcmp(argv[1], "-n") == 0) {
            return (argv[2][0] != '\0') ? 0 : 1;
        }
    }

    if (argc == 4) {
        if (strcmp(argv[2], "=") == 0) {
            return strcmp(argv[1], argv[3]) == 0 ? 0 : 1;
        }
        if (strcmp(argv[2], "!=") == 0) {
            return strcmp(argv[1], argv[3]) != 0 ? 0 : 1;
        }

        int a = atoi(argv[1]);
        int b = atoi(argv[3]);
        if (strcmp(argv[2], "-eq") == 0) {
            return a == b ? 0 : 1;
        }
        if (strcmp(argv[2], "-ne") == 0) {
            return a != b ? 0 : 1;
        }
        if (strcmp(argv[2], "-lt") == 0) {
            return a < b ? 0 : 1;
        }
        if (strcmp(argv[2], "-le") == 0) {
            return a <= b ? 0 : 1;
        }
        if (strcmp(argv[2], "-gt") == 0) {
            return a > b ? 0 : 1;
        }
        if (strcmp(argv[2], "-ge") == 0) {
            return a >= b ? 0 : 1;
        }
    }

    cmd_error(out, "test", NULL, "unsupported expression");
    return 2;
}

int cmd_expr(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 4) {
        cmd_error(out, "expr", NULL, "usage: expr <a> <op> <b>");
        return 1;
    }

    int a = atoi(argv[1]);
    int b = atoi(argv[3]);
    const char *op = argv[2];

    if (strcmp(op, "+") == 0) {
        st_printf(out, "%d\n", a + b);
    } else if (strcmp(op, "-") == 0) {
        st_printf(out, "%d\n", a - b);
    } else if (strcmp(op, "*") == 0 || strcmp(op, "x") == 0) {
        st_printf(out, "%d\n", a * b);
    } else if (strcmp(op, "/") == 0) {
        if (b == 0) {
            cmd_error(out, "expr", NULL, "division by zero");
            return 1;
        }
        st_printf(out, "%d\n", a / b);
    } else if (strcmp(op, "%") == 0) {
        if (b == 0) {
            cmd_error(out, "expr", NULL, "division by zero");
            return 1;
        }
        st_printf(out, "%d\n", a % b);
    } else {
        cmd_error(out, "expr", op, "unknown operator");
        return 1;
    }
    return 0;
}

int cmd_seq(int argc, char **argv, struct stream *in, struct stream *out) {
    int first = 1;
    int step = 1;
    int last = 0;

    if (argc == 2) {
        last = atoi(argv[1]);
    } else if (argc == 3) {
        first = atoi(argv[1]);
        last = atoi(argv[2]);
    } else if (argc >= 4) {
        first = atoi(argv[1]);
        step = atoi(argv[2]);
        last = atoi(argv[3]);
    } else {
        cmd_error(out, "seq", NULL, "usage: seq [first] [step] <last>");
        return 1;
    }

    if (step == 0) {
        cmd_error(out, "seq", NULL, "step cannot be zero");
        return 1;
    }

    if (step > 0) {
        for (int i = first; i <= last; i += step) {
            st_printf(out, "%d\n", i);
        }
    } else {
        for (int i = first; i >= last; i += step) {
            st_printf(out, "%d\n", i);
        }
    }
    return 0;
}

int cmd_yes(int argc, char **argv, struct stream *in, struct stream *out) {
    int count = 1000;
    int start = 1;

    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        count = atoi(argv[2]);
        start = 3;
    }
    if (count <= 0 || count > 100000) {
        count = 1000;
    }

    char joined[256];
    joined[0] = '\0';
    for (int i = start; i < argc; i++) {
        if (i > start) {
            strlcpy(joined + strlen(joined), " ", sizeof(joined) - strlen(joined));
        }
        strlcpy(joined + strlen(joined), argv[i], sizeof(joined) - strlen(joined));
    }
    const char *text = (joined[0] != '\0') ? joined : "y";
    for (int i = 0; i < count; i++) {
        st_printf(out, "%s\n", text);
    }
    return 0;
}

int cmd_basename(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        cmd_error(out, "basename", NULL, "missing operand");
        return 1;
    }

    char buf[VFS_PATH_MAX];
    strlcpy(buf, argv[1], sizeof(buf));
    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/') {
        buf[--len] = '\0';
    }

    char *slash = strrchr(buf, '/');
    st_printf(out, "%s\n", slash ? slash + 1 : buf);
    return 0;
}

int cmd_dirname(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        cmd_error(out, "dirname", NULL, "missing operand");
        return 1;
    }

    char buf[VFS_PATH_MAX];
    strlcpy(buf, argv[1], sizeof(buf));
    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/') {
        buf[--len] = '\0';
    }

    char *slash = strrchr(buf, '/');
    if (!slash) {
        st_puts(out, ".\n");
        return 0;
    }
    if (slash == buf) {
        st_puts(out, "/\n");
        return 0;
    }
    *slash = '\0';
    st_printf(out, "%s\n", buf);
    return 0;
}

int cmd_sleep(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        cmd_error(out, "sleep", NULL, "missing duration");
        return 1;
    }
    int seconds = atoi(argv[1]);
    if (seconds < 0 || seconds > 3600) {
        cmd_error(out, "sleep", NULL, "duration out of range");
        return 1;
    }
    sleep_ms((uint32_t)seconds * 1000u);
    return 0;
}

int cmd_beep(int argc, char **argv, struct stream *in, struct stream *out) {
    uint32_t frequency = (argc > 1) ? (uint32_t)atoi(argv[1]) : 880;
    uint32_t duration = (argc > 2) ? (uint32_t)atoi(argv[2]) : 200;

    if (frequency < 20 || frequency > 20000) {
        frequency = 880;
    }
    if (duration > 5000) {
        duration = 5000;
    }
    speaker_beep(frequency, duration);
    return 0;
}

int cmd_time(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        cmd_error(out, "time", NULL, "missing command");
        return 1;
    }

    char line[SHELL_LINE_MAX];
    line[0] = '\0';
    for (int i = 1; i < argc; i++) {
        strlcpy(line + strlen(line), argv[i], SHELL_LINE_MAX - strlen(line));
        if (i + 1 < argc) {
            strlcpy(line + strlen(line), " ", SHELL_LINE_MAX - strlen(line));
        }
    }

    uint32_t start = pit_uptime_ms();
    int status = shell_run_line(line);
    uint32_t elapsed = pit_uptime_ms() - start;

    st_printf(stream_console(), "\nreal %u.%03us  status %d\n",
              elapsed / 1000u, elapsed % 1000u, status);
    return status;
}

int cmd_sync(int argc, char **argv, struct stream *in, struct stream *out) {
    vfs_sync();
    return 0;
}

int cmd_lscpu(int argc, char **argv, struct stream *in, struct stream *out) {
    char vendor[16];
    char brand[52];

    cpu_vendor(vendor);
    st_printf(out, "%-16s %s\n", "Architecture:", KERNEL_ARCH);
    st_printf(out, "%-16s %s\n", "Vendor ID:", vendor);

    if (cpu_brand(brand)) {
        const char *p = brand;
        while (*p == ' ') {
            p++;
        }
        st_printf(out, "%-16s %s\n", "Model name:", p);
    }

    st_printf(out, "%-16s %u\n", "CPU family:", cpu_family());
    st_printf(out, "%-16s %u\n", "Model:", cpu_model());
    st_printf(out, "%-16s %u\n", "Stepping:", cpu_stepping());
    st_printf(out, "%-16s %s\n", "Mode:", "64-bit long");
    st_printf(out, "%-16s %u\n", "CPUs:", 1u);
    st_printf(out, "%-16s ", "Flags:");

    int column = 17;
    for (int i = 0; cpu_feature_list(i); i++) {
        if (!cpu_feature_present(i)) {
            continue;
        }
        const char *name = cpu_feature_list(i);
        int len = (int)strlen(name) + 1;
        if (column + len > 78) {
            st_puts(out, "\n                 ");
            column = 17;
        }
        st_printf(out, "%s ", name);
        column += len;
    }
    st_putc(out, '\n');
    return 0;
}

int cmd_acpi(int argc, char **argv, struct stream *in, struct stream *out) {
    const struct acpi_info *a = acpi_get_info();

    if (!a->present) {
        st_puts(out, "ACPI not found: power off and reset use the legacy ports.\n");
        return 1;
    }

    st_printf(out, "%-16s 0x%08x, revision %u, OEM '%s'\n", "RSDP:", a->rsdp_phys,
              (uint32_t)a->rsdp_revision, a->oem);
    st_printf(out, "%-16s 0x%08x (%s)\n", "Root table:", a->root_phys, a->xsdt ? "XSDT" : "RSDT");
    st_printf(out, "%-16s %d\n", "Tables:", acpi_table_count());
    for (int i = 0; i < acpi_table_count(); i++) {
        const struct acpi_table_ref *t = acpi_get_table(i);
        st_printf(out, "  %-6s 0x%08x %8u bytes  rev %u\n", t->sig, t->phys, t->length,
                  (uint32_t)t->revision);
    }

    if (!a->fadt_ok) {
        st_puts(out, "FADT:            missing or unusable\n");
        return 1;
    }
    st_printf(out, "%-16s %u\n", "SCI IRQ:", (uint32_t)a->sci_int);
    st_printf(out, "%-16s %s\n", "ACPI mode:", acpi_sci_enabled() ? "enabled (SCI_EN)" : "legacy");
    st_printf(out, "%-16s 0x%x (enable 0x%02x, disable 0x%02x)\n", "SMI command:", a->smi_cmd,
              (uint32_t)a->acpi_enable_val, (uint32_t)a->acpi_disable_val);
    st_printf(out, "%-16s 0x%x / 0x%x\n", "PM1a/PM1b CNT:", a->pm1a_cnt, a->pm1b_cnt);
    if (a->s5_ok) {
        st_printf(out, "%-16s SLP_TYP %u/%u (from %s)\n", "Power off (S5):",
                  (uint32_t)a->slp_typa, (uint32_t)a->slp_typb, a->s5_from);
    } else {
        st_printf(out, "%-16s not available, legacy ports\n", "Power off (S5):");
    }
    if (a->reset_ok) {
        static const char *const spaces[] = { "memory", "I/O port", "PCI config" };
        st_printf(out, "%-16s %s 0x%x, value 0x%02x\n", "Reset register:",
                  spaces[a->reset_reg.space], (uint32_t)a->reset_reg.address,
                  (uint32_t)a->reset_value);
    } else {
        st_printf(out, "%-16s not available, keyboard controller / 0xCF9\n", "Reset register:");
    }
    return 0;
}

int cmd_reboot(int argc, char **argv, struct stream *in, struct stream *out) {
    st_puts(stream_console(), "Restarting the machine...\n");
    sleep_ms(300);
    power_reboot();
    return 0;
}

int cmd_poweroff(int argc, char **argv, struct stream *in, struct stream *out) {
    st_puts(stream_console(), "Powering off...\n");
    sleep_ms(300);
    power_off();
    return 0;
}

int cmd_halt(int argc, char **argv, struct stream *in, struct stream *out) {
    st_puts(stream_console(), "System halted.\n");
    power_halt();
    return 0;
}

int cmd_simplecc(int argc, char **argv, struct stream *in, struct stream *out) {
    (void)argc; (void)argv; (void)in; (void)out;
    simple_compile();
    return 0;
}
