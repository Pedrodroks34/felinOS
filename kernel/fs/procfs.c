#include <stdarg.h>
#include "fs/vfs.h"
#include "drivers/cpu.h"
#include "drivers/pit.h"
#include "lib/format.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include "swap.h"
#include "system.h"
#include "vmm.h"

enum {
    P_MEMINFO,
    P_UPTIME,
    P_VERSION,
    P_CPUINFO,
    P_MOUNTS,
    P_FILESYSTEMS
};

enum {
    PK_NONE,
    PK_ROOT,
    PK_FILE,
    PK_PIDDIR,
    PK_STATUS
};

static const struct {
    const char *name;
    int id;
} files[] = {
    { "meminfo", P_MEMINFO },
    { "uptime", P_UPTIME },
    { "version", P_VERSION },
    { "cpuinfo", P_CPUINFO },
    { "mounts", P_MOUNTS },
    { "filesystems", P_FILESYSTEMS },
};

#define FILE_COUNT ((int)(sizeof(files) / sizeof(files[0])))

struct pbuf {
    char *data;
    uint32_t len;
    uint32_t cap;
    int failed;
};

static void pb_emit(void *ctx, char c) {
    struct pbuf *b = (struct pbuf *)ctx;

    if (b->failed) {
        return;
    }
    if (b->len + 2 > b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 512;
        char *grown = (char *)krealloc(b->data, cap);
        if (!grown) {
            b->failed = 1;
            return;
        }
        b->data = grown;
        b->cap = cap;
    }
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static void pb_printf(struct pbuf *b, const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    fmt_vprint(pb_emit, b, fmt, args);
    va_end(args);
}

static int is_number(const char *s) {
    if (!*s) {
        return 0;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
    }
    return 1;
}

static int task_visible(int pid) {
    struct task *t = task_find(pid);

    return t && t->state != TASK_NEW && t->state != TASK_DYING;
}

static int parse_path(const char *path, int *id, int *pid) {
    if (strcmp(path, "/") == 0) {
        return PK_ROOT;
    }

    const char *p = path + 1;
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    char part[16];

    if (n == 0 || n >= sizeof(part)) {
        return PK_NONE;
    }
    memcpy(part, p, n);
    part[n] = '\0';

    if (is_number(part)) {
        *pid = atoi(part);
        if (!task_visible(*pid)) {
            return PK_NONE;
        }
        if (!slash) {
            return PK_PIDDIR;
        }
        return strcmp(slash + 1, "status") == 0 ? PK_STATUS : PK_NONE;
    }
    if (slash) {
        return PK_NONE;
    }
    for (int i = 0; i < FILE_COUNT; i++) {
        if (strcmp(files[i].name, part) == 0) {
            *id = files[i].id;
            return PK_FILE;
        }
    }
    return PK_NONE;
}

static void gen_meminfo(struct pbuf *b) {
    uint32_t total, used, largest, blocks;
    struct vmm_stats vm;

    heap_stats(&total, &used, &largest, &blocks);
    vmm_get_stats(&vm);

    pb_printf(b, "MemTotal:       %u kB\n", system_total_kb());
    pb_printf(b, "MemFree:        %u kB\n", pmm_free_frames() * (PMM_FRAME_SIZE / 1024u));
    pb_printf(b, "MemUsed:        %u kB\n", pmm_used_frames() * (PMM_FRAME_SIZE / 1024u));
    pb_printf(b, "KernelImage:    %u kB\n", (system_kernel_end() - system_kernel_start()) / 1024u);
    pb_printf(b, "HeapTotal:      %u kB\n", total / 1024u);
    pb_printf(b, "HeapUsed:       %u kB\n", used / 1024u);
    pb_printf(b, "HeapFree:       %u kB\n", (total - used) / 1024u);
    pb_printf(b, "HeapBlocks:     %u\n", blocks);
    pb_printf(b, "HeapLargest:    %u kB\n", largest / 1024u);
    pb_printf(b, "FramesTotal:    %u\n", pmm_total_frames());
    pb_printf(b, "FramesUsed:     %u\n", pmm_used_frames());
    pb_printf(b, "FramesFree:     %u\n", pmm_free_frames());
    pb_printf(b, "VmSize:         %u kB\n", vm.virtual_pages * (PAGE_SIZE / 1024u));
    pb_printf(b, "VmResident:     %u kB\n", vm.resident_pages * (PAGE_SIZE / 1024u));
    pb_printf(b, "VmSwapped:      %u kB\n", vm.swapped_pages * (PAGE_SIZE / 1024u));
    if (swap_active()) {
        pb_printf(b, "SwapTotal:      %u kB\n", swap_total_slots() * (PAGE_SIZE / 1024u));
        pb_printf(b, "SwapUsed:       %u kB\n", swap_used_slots() * (PAGE_SIZE / 1024u));
        pb_printf(b, "SwapFree:       %u kB\n", swap_free_slots() * (PAGE_SIZE / 1024u));
    } else {
        pb_printf(b, "SwapTotal:      0 kB\n");
        pb_printf(b, "SwapUsed:       0 kB\n");
        pb_printf(b, "SwapFree:       0 kB\n");
    }
    pb_printf(b, "PageFaults:     %u\n", vm.page_faults);
    pb_printf(b, "DemandFaults:   %u\n", vm.demand_faults);
    pb_printf(b, "CowFaults:      %u\n", vm.cow_faults);
    pb_printf(b, "SwapFaults:     %u\n", vm.swap_faults);
}

static void gen_uptime(struct pbuf *b) {
    struct sched_stats s;
    uint32_t ticks = pit_ticks();

    sched_get_stats(&s);
    pb_printf(b, "%u.%02u %u.%02u\n",
              ticks / PIT_FREQUENCY, (ticks % PIT_FREQUENCY) * 100u / PIT_FREQUENCY,
              s.idle_ticks / PIT_FREQUENCY, (s.idle_ticks % PIT_FREQUENCY) * 100u / PIT_FREQUENCY);
}

static void gen_version(struct pbuf *b) {
    pb_printf(b, "%s version %s (%s %s, %s)\n", KERNEL_NAME, KERNEL_VERSION,
              FELINOS_NAME, FELINOS_VERSION, KERNEL_ARCH);
}

static void gen_cpuinfo(struct pbuf *b) {
    char vendor[16];
    char brand[52];

    cpu_vendor(vendor);
    pb_printf(b, "vendor_id\t: %s\n", vendor);
    if (cpu_brand(brand)) {
        const char *p = brand;
        while (*p == ' ') {
            p++;
        }
        pb_printf(b, "model name\t: %s\n", p);
    }
    pb_printf(b, "cpu family\t: %u\n", cpu_family());
    pb_printf(b, "model\t\t: %u\n", cpu_model());
    pb_printf(b, "stepping\t: %u\n", cpu_stepping());
    pb_printf(b, "flags\t\t:");
    for (int i = 0; cpu_feature_list(i); i++) {
        if (cpu_feature_present(i)) {
            pb_printf(b, " %s", cpu_feature_list(i));
        }
    }
    pb_printf(b, "\n");
}

static void gen_mounts(struct pbuf *b) {
    struct vfs_mount_info mi;

    for (int i = 0; vfs_mount_info(i, &mi) == 0; i++) {
        pb_printf(b, "%s %s %s rw 0 0\n", mi.source, mi.path, mi.type);
    }
}

static void gen_filesystems(struct pbuf *b) {
    for (int i = 0; i < vfs_fs_count(); i++) {
        const char *name = vfs_fs_name(i);
        pb_printf(b, "%s\t%s\n", strcmp(name, "gatofs") == 0 ? "" : "nodev", name);
    }
}

static int gen_status(struct pbuf *b, int pid) {
    struct task_info *info = (struct task_info *)kmalloc(sizeof(struct task_info) * SCHED_MAX_TASKS);
    int found = 0;

    if (!info) {
        return VFS_ENOMEM;
    }
    int n = sched_snapshot(info, SCHED_MAX_TASKS);
    int self = sched_current()->pid;
    for (int i = 0; i < n; i++) {
        struct task_info *t = &info[i];
        if (t->pid != pid) {
            continue;
        }
        const char *state = (t->pid == self) ? "running" : sched_state_name(t->state);
        if (t->state == TASK_RUNNING && t->pid != self) {
            state = "ready";
        }
        pb_printf(b, "Name:\t%s\n", t->name);
        pb_printf(b, "State:\t%s\n", state);
        pb_printf(b, "Pid:\t%d\n", t->pid);
        pb_printf(b, "PPid:\t%d\n", t->ppid);
        pb_printf(b, "Kind:\t%s\n", t->user ? "user" : "kernel");
        pb_printf(b, "Nice:\t%d\n", t->nice);
        pb_printf(b, "Wait:\t%s\n", t->wait_reason ? t->wait_reason : "-");
        pb_printf(b, "CpuTicks:\t%u\n", t->cpu_ticks);
        pb_printf(b, "LifeTicks:\t%u\n", t->lifetime_ticks);
        pb_printf(b, "Switches:\t%u\n", t->switches);
        found = 1;
        break;
    }
    kfree(info);
    return found ? VFS_OK : VFS_ENOENT;
}

static uint32_t proc_mtime(void) {
    return system_boot_unix() + pit_uptime_seconds();
}

static int proc_stat(struct vfs_mount *m, const char *path, struct vfs_stat *st) {
    int id = 0;
    int pid = 0;
    int kind = parse_path(path, &id, &pid);

    if (kind == PK_NONE) {
        return VFS_ENOENT;
    }
    st->mtime = proc_mtime();
    if (kind == PK_ROOT || kind == PK_PIDDIR) {
        st->type = VFS_DIR;
        st->mode = 0555;
        st->ino = (kind == PK_ROOT) ? 1u : 100u + (uint32_t)pid;
        return VFS_OK;
    }
    st->type = VFS_FILE;
    st->mode = 0444;
    st->ino = (kind == PK_FILE) ? 2u + (uint32_t)id : 1000u + (uint32_t)pid;
    return VFS_OK;
}

static int proc_open(struct vfs_mount *m, const char *path, int flags, struct vfs_file *f) {
    int id = 0;
    int pid = 0;
    int kind = parse_path(path, &id, &pid);

    if (kind == PK_NONE) {
        return (flags & VFS_O_CREATE) ? VFS_EPERM : VFS_ENOENT;
    }
    if (kind == PK_ROOT || kind == PK_PIDDIR) {
        return VFS_EISDIR;
    }
    if (flags & VFS_O_WRITE) {
        return VFS_EPERM;
    }

    struct pbuf *b = (struct pbuf *)kzalloc(sizeof(struct pbuf));
    if (!b) {
        return VFS_ENOMEM;
    }
    int r = VFS_OK;
    if (kind == PK_STATUS) {
        r = gen_status(b, pid);
    } else {
        switch (id) {
        case P_MEMINFO:     gen_meminfo(b); break;
        case P_UPTIME:      gen_uptime(b); break;
        case P_VERSION:     gen_version(b); break;
        case P_CPUINFO:     gen_cpuinfo(b); break;
        case P_MOUNTS:      gen_mounts(b); break;
        case P_FILESYSTEMS: gen_filesystems(b); break;
        }
    }
    if (r == VFS_OK && b->failed) {
        r = VFS_ENOMEM;
    }
    if (r < 0) {
        kfree(b->data);
        kfree(b);
        return r;
    }
    f->priv = b;
    f->pos = 0;
    return VFS_OK;
}

static void proc_close(struct vfs_file *f) {
    struct pbuf *b = (struct pbuf *)f->priv;

    kfree(b->data);
    kfree(b);
}

static int proc_read(struct vfs_file *f, void *buf, uint32_t len) {
    struct pbuf *b = (struct pbuf *)f->priv;

    if (f->pos >= b->len) {
        return 0;
    }
    uint32_t avail = b->len - (uint32_t)f->pos;
    if (len > avail) {
        len = avail;
    }
    memcpy(buf, b->data + f->pos, len);
    f->pos += len;
    return (int)len;
}

static int proc_size(struct vfs_file *f, uint64_t *out) {
    *out = ((struct pbuf *)f->priv)->len;
    return VFS_OK;
}

static int proc_readdir(struct vfs_mount *m, const char *path, vfs_dir_cb cb, void *ctx) {
    int id = 0;
    int pid = 0;
    int kind = parse_path(path, &id, &pid);

    if (kind == PK_NONE) {
        return VFS_ENOENT;
    }
    if (kind == PK_FILE || kind == PK_STATUS) {
        return VFS_ENOTDIR;
    }
    if (kind == PK_PIDDIR) {
        cb("status", VFS_FILE, ctx);
        return VFS_OK;
    }

    for (int i = 0; i < FILE_COUNT; i++) {
        if (cb(files[i].name, VFS_FILE, ctx)) {
            return VFS_OK;
        }
    }
    struct task_info *info = (struct task_info *)kmalloc(sizeof(struct task_info) * SCHED_MAX_TASKS);
    if (!info) {
        return VFS_ENOMEM;
    }
    int n = sched_snapshot(info, SCHED_MAX_TASKS);
    for (int i = 0; i < n; i++) {
        char name[16];
        snprintf(name, sizeof(name), "%d", info[i].pid);
        if (cb(name, VFS_DIR, ctx)) {
            break;
        }
    }
    kfree(info);
    return VFS_OK;
}

const struct fs_ops procfs_ops = {
    .name = "procfs",
    .stat = proc_stat,
    .open = proc_open,
    .close = proc_close,
    .read = proc_read,
    .readdir = proc_readdir,
    .size = proc_size,
};
