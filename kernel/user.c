#include <stdint.h>
#include "user.h"
#include "syscall.h"
#include "sched.h"
#include "gdt.h"
#include "vmm.h"
#include "io.h"
#include "console.h"
#include "sh/shell.h"
#include "sh/cmds.h"
#include "drivers/input.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "lib/string.h"
#include "lib/format.h"
#include "lib/heap.h"
#include "net/socket.h"

extern void user_jump(uint64_t entry, uint64_t user_sp, int is32) __attribute__((noreturn));
extern void fork_trampoline(struct regs *r) __attribute__((noreturn));

#define MAX_FDS 16
#define USER_KSTACK 16384u
#define SPAWN_MAX_ARGS 8
#define SPAWN_ARG_LEN 128
#define IO_CHUNK 4096u

struct elf_hdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf_ph {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

struct elf64_hdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_ph {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct seg {
    uint64_t vaddr, offset, filesz, memsz;
};

struct uproc {
    struct vfs_file *fds[MAX_FDS];
    int is32;        /* 32-bit (compatibility mode) program */
    uint32_t entry;
    uint32_t esp;
    uint32_t brk;
    uint32_t line_len;
    uint32_t line_pos;
    char linebuf[256];
};

static struct uproc uprocs[SCHED_MAX_TASKS];

static struct uproc *cur_proc(void) {
    return (struct uproc *)sched_current()->priv;
}

/* ---------- embedded programs ---------- */

#define PROG_DECL(n) extern const uint8_t prog_##n##_start[], prog_##n##_end[];
PROG_DECL(hello) PROG_DECL(cat) PROG_DECL(crash) PROG_DECL(spin) PROG_DECL(count) PROG_DECL(launch) PROG_DECL(hello32)

static const struct { const char *name; const uint8_t *s, *e; } progs[] = {
    { "hello", prog_hello_start, prog_hello_end },
    { "cat",   prog_cat_start,   prog_cat_end },
    { "crash", prog_crash_start, prog_crash_end },
    { "spin",  prog_spin_start,  prog_spin_end },
    { "count", prog_count_start, prog_count_end },
    { "launch", prog_launch_start, prog_launch_end },
    { "hello32", prog_hello32_start, prog_hello32_end },
};

static int same_contents(const char *path, const uint8_t *data, uint32_t len) {
    void *buf;
    uint32_t size;

    if (vfs_load(path, &buf, &size) < 0) {
        return 0;
    }
    int same = size == len && memcmp(buf, data, len) == 0;
    kfree(buf);
    return same;
}

static void user_task_exit(struct task *t) {
    struct uproc *p = (struct uproc *)t->priv;

    if (!p) {
        return;
    }
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i]) {
            vfs_close(p->fds[i]);
            p->fds[i] = NULL;
        }
    }
}

void user_init(int install_missing) {
    sched_set_exit_hook(user_task_exit);
    if (install_missing) {
        vfs_mkpath("/bin");
    }
    for (uint32_t i = 0; i < sizeof(progs) / sizeof(progs[0]); i++) {
        char path[32];
        struct vfs_stat st;
        uint32_t len = (uint32_t)(progs[i].e - progs[i].s);
        strcpy(path, "/bin/");
        strcat(path, progs[i].name);
        int exists = vfs_stat(path, &st) == 0 && st.type == VFS_FILE;
        if (!exists && !install_missing) {
            continue;
        }
        if (exists && st.size == len && same_contents(path, progs[i].s, len)) {
            continue;
        }
        vfs_save(path, progs[i].s, len);
    }
}

/* ---------- user memory validation ---------- */

static int uptr_ok(uint32_t p, uint32_t len) {
    if (p < USER_BASE || p + len < p || p + len > USER_STACK_TOP) {
        return 0;
    }
    if (!len) {
        return 1;
    }
    struct vm_space *sp = vmm_current_space();
    for (uint32_t a = p & PAGE_MASK; a < p + len; a += PAGE_SIZE) {
        struct vm_region *rg = vmm_find_region(sp, a);
        if (!rg || !(rg->flags & VM_USER)) {
            return 0;
        }
    }
    return 1;
}

static int ustr_copy(char *dst, uint32_t src, uint32_t max) {
    for (uint32_t i = 0; i < max; i++) {
        if (!uptr_ok(src + i, 1)) {
            return -1;
        }
        dst[i] = *(const char *)(src + i);
        if (!dst[i]) {
            return 0;
        }
    }
    return -1;
}

/* ---------- ELF loading ---------- */

int user_is_elf(const char *path) {
    struct vfs_stat st;
    struct vfs_file *f;
    uint8_t head[sizeof(struct elf_hdr)];

    if (!path || vfs_stat(path, &st) < 0 || st.type != VFS_FILE || st.size < sizeof(struct elf_hdr)) {
        return 0;
    }
    if (vfs_open(path, VFS_O_READ, &f) < 0) {
        return 0;
    }
    int n = vfs_read(f, head, sizeof(head));
    vfs_close(f);
    return n == (int)sizeof(head) && head[0] == 0x7F && head[1] == 'E' &&
           head[2] == 'L' && head[3] == 'F';
}

static int load_elf(const uint8_t *d, uint32_t size, uint32_t *entry, int *is32) {
    struct seg segs[8];
    int n = 0;
    uint64_t ent;

    if (size < sizeof(struct elf_hdr) || d[0] != 0x7F || d[1] != 'E' ||
        d[2] != 'L' || d[3] != 'F' || d[5] != 1) {
        return -1;
    }
    if (d[4] == 1) {
        const struct elf_hdr *h = (const struct elf_hdr *)d;
        if (h->type != 2 || h->machine != 3 || h->phentsize != sizeof(struct elf_ph) ||
            h->phnum == 0 || h->phnum > 8 ||
            (uint64_t)h->phoff + (uint64_t)h->phnum * sizeof(struct elf_ph) > size) {
            return -1;
        }
        const struct elf_ph *ph = (const struct elf_ph *)(d + h->phoff);
        for (int i = 0; i < h->phnum; i++) {
            if (ph[i].type == 1) {
                segs[n++] = (struct seg){ ph[i].vaddr, ph[i].offset, ph[i].filesz, ph[i].memsz };
            }
        }
        ent = h->entry;
        *is32 = 1;
    } else if (d[4] == 2 && size >= sizeof(struct elf64_hdr)) {
        const struct elf64_hdr *h = (const struct elf64_hdr *)d;
        if (h->type != 2 || h->machine != 62 || h->phentsize != sizeof(struct elf64_ph) ||
            h->phnum == 0 || h->phnum > 8 || h->phoff > size ||
            h->phoff + (uint64_t)h->phnum * sizeof(struct elf64_ph) > size) {
            return -1;
        }
        const struct elf64_ph *ph = (const struct elf64_ph *)(d + h->phoff);
        for (int i = 0; i < h->phnum; i++) {
            if (ph[i].type == 1) {
                segs[n++] = (struct seg){ ph[i].vaddr, ph[i].offset, ph[i].filesz, ph[i].memsz };
            }
        }
        ent = h->entry;
        *is32 = 0;
    } else {
        return -1;
    }

    uint64_t lo = ~0ull, hi = 0;
    for (int i = 0; i < n; i++) {
        if (segs[i].vaddr < USER_BASE || segs[i].memsz < segs[i].filesz ||
            segs[i].vaddr + segs[i].memsz > USER_HEAP_BASE ||
            segs[i].offset + segs[i].filesz > size) {
            return -1;
        }
        if (segs[i].vaddr < lo) lo = segs[i].vaddr;
        if (segs[i].vaddr + segs[i].memsz > hi) hi = segs[i].vaddr + segs[i].memsz;
    }
    if (n == 0 || lo >= hi || ent < lo || ent >= hi) {
        return -1;
    }
    uint32_t base = (uint32_t)lo & PAGE_MASK;
    uint32_t span = (uint32_t)hi - base;
    if (!vmm_alloc_at(base, span, VM_READ | VM_WRITE | VM_EXEC | VM_USER, "user-image")) {
        return -1;
    }
    memset((void *)(uintptr_t)base, 0, (span + PAGE_SIZE - 1) & PAGE_MASK);
    for (int i = 0; i < n; i++) {
        if (segs[i].filesz) {
            memcpy((void *)(uintptr_t)segs[i].vaddr, d + segs[i].offset, (uint32_t)segs[i].filesz);
        }
    }
    *entry = (uint32_t)ent;
    return 0;
}

/* ---------- process lifecycle ---------- */

static uint32_t build_stack(int argc, char **argv, int is32) {
    uint32_t psz = is32 ? 4 : 8;
    uint32_t sp = USER_STACK_TOP - 16;
    uint32_t ptrs[SHELL_MAX_ARGS + 1];

    if (argc > SHELL_MAX_ARGS) {
        argc = SHELL_MAX_ARGS;
    }
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t l = (uint32_t)strlen(argv[i]) + 1;
        if (l > 256) l = 256;
        sp -= l;
        memcpy((void *)(uintptr_t)sp, argv[i], l - 1);
        ((char *)(uintptr_t)sp)[l - 1] = 0;
        ptrs[i] = sp;
    }
    ptrs[argc] = 0;
    sp &= ~15u;
    sp -= (uint32_t)(argc + 2) * psz;
    sp &= ~15u;
    if (is32) {
        uint32_t *w = (uint32_t *)(uintptr_t)sp;
        w[0] = (uint32_t)argc;
        for (int i = 0; i <= argc; i++) w[1 + i] = ptrs[i];
    } else {
        uint64_t *w = (uint64_t *)(uintptr_t)sp;
        w[0] = (uint64_t)argc;
        for (int i = 0; i <= argc; i++) w[1 + i] = ptrs[i];
    }
    return sp;
}

static void user_task_main(void *arg) {
    struct uproc *p = (struct uproc *)arg;

    cli();
    user_jump(p->entry, p->esp, p->is32);
}

struct task *user_spawn_io(const char *path, int argc, char **argv,
                            struct vfs_file *fin, struct vfs_file *fout) {
    void *img;
    uint32_t imgsize;

    if (vfs_load(path, &img, &imgsize) < 0) {
        return NULL;
    }
    const char *name = vfs_basename(path);
    struct vm_space *prev = vmm_current_space();
    struct vm_space *space = vmm_space_create(name);

    if (!space) {
        kfree(img);
        return NULL;
    }
    struct task *t = task_new(name, user_task_main, NULL, USER_KSTACK, space, 1);
    if (!t) {
        vmm_space_destroy(space);
        kfree(img);
        return NULL;
    }
    struct uproc *p = &uprocs[task_slot(t)];
    memset(p, 0, sizeof(*p));
    t->priv = p;
    t->arg = p;

    uint32_t entry = 0;
    int ok;

    vmm_space_switch(space);
    ok = load_elf((const uint8_t *)img, imgsize, &entry, &p->is32) == 0 &&
         vmm_alloc_at(USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE,
                      VM_READ | VM_WRITE | VM_USER | VM_DEMAND, "user-stack") &&
         vmm_alloc_at(USER_HEAP_BASE, USER_HEAP_MAX,
                      VM_READ | VM_WRITE | VM_USER | VM_DEMAND, "user-heap");
    if (ok) {
        p->esp = build_stack(argc, argv, p->is32);
    }
    vmm_space_switch(prev);
    kfree(img);
    if (!ok) {
        if (fin) {
            vfs_close(fin);
        }
        if (fout) {
            vfs_close(fout);
        }
        task_discard(t);
        return NULL;
    }
    p->entry = entry;
    p->brk = USER_HEAP_BASE;
    if (fin) {
        p->fds[0] = fin;
    }
    if (fout) {
        p->fds[1] = fout;
    }
    task_start(t);
    return t;
}

struct task *user_spawn(const char *path, int argc, char **argv) {
    return user_spawn_io(path, argc, argv, NULL, NULL);
}

void user_fault(struct regs *r, const char *name, uint32_t cr2) {
    kprintf("\n[pid %d] killed: %s (int %u) at rip=%08x%08x", sched_current()->pid, name,
            (uint32_t)r->int_no, (uint32_t)(r->rip >> 32), (uint32_t)r->rip);
    if (r->int_no == 14) {
        kprintf(", address=%08x", cr2);
    }
    kprintf("\n");
    task_exit(128 + (int)r->int_no);
}

/* ---------- pipes ---------- */

#define PIPE_SIZE 4096

struct pipe {
    uint8_t buf[PIPE_SIZE];
    uint32_t tail, count;
    int readers, writers;
};

static int pipe_read(struct vfs_file *f, void *buf, uint32_t len) {
    struct pipe *pi = (struct pipe *)f->priv;
    uint8_t *out = (uint8_t *)buf;

    for (;;) {
        uint32_t fl = irq_save();
        uint32_t n = 0;
        while (n < len && pi->count > 0) {
            out[n++] = pi->buf[pi->tail];
            pi->tail = (pi->tail + 1) % PIPE_SIZE;
            pi->count--;
        }
        if (n > 0) {
            irq_restore(fl);
            sched_wake(pi);
            return (int)n;
        }
        if (pi->writers == 0) {
            irq_restore(fl);
            return 0;
        }
        sched_wait_on(pi, "pipe");
        irq_restore(fl);
    }
}

static int pipe_write(struct vfs_file *f, const void *buf, uint32_t len) {
    struct pipe *pi = (struct pipe *)f->priv;
    const uint8_t *in = (const uint8_t *)buf;

    for (;;) {
        uint32_t fl = irq_save();
        if (pi->readers == 0) {
            irq_restore(fl);
            task_signal(sched_current()->pid, SIGPIPE);
            return -1;
        }
        uint32_t n = 0;
        while (n < len && pi->count < PIPE_SIZE) {
            pi->buf[(pi->tail + pi->count) % PIPE_SIZE] = in[n++];
            pi->count++;
        }
        if (n > 0) {
            irq_restore(fl);
            sched_wake(pi);
            return (int)n;
        }
        sched_wait_on(pi, "pipe");
        irq_restore(fl);
    }
}

static void pipe_close(struct vfs_file *f) {
    struct pipe *pi = (struct pipe *)f->priv;
    uint32_t fl = irq_save();

    if (f->flags & VFS_O_READ) {
        pi->readers--;
    } else {
        pi->writers--;
    }
    int last = pi->readers <= 0 && pi->writers <= 0;
    irq_restore(fl);
    if (last) {
        f->priv = NULL;
        kfree(pi);
        return;
    }
    sched_wake(pi);
}

static const struct fs_ops pipe_ops = {
    .name = "pipe",
    .read = pipe_read,
    .write = pipe_write,
    .close = pipe_close,
};

static struct vfs_mount pipe_mount = {
    .ops = &pipe_ops,
};

static int kpipe_create(struct vfs_file **out_r, struct vfs_file **out_w) {
    struct pipe *pi = (struct pipe *)kmalloc(sizeof(struct pipe));
    struct vfs_file *rfile = (struct vfs_file *)kmalloc(sizeof(struct vfs_file));
    struct vfs_file *wfile = (struct vfs_file *)kmalloc(sizeof(struct vfs_file));

    if (!pi || !rfile || !wfile) {
        kfree(pi);
        kfree(rfile);
        kfree(wfile);
        return -1;
    }
    memset(pi, 0, sizeof(*pi));
    pi->readers = 1;
    pi->writers = 1;
    rfile->mnt = &pipe_mount;
    rfile->priv = pi;
    rfile->pos = 0;
    rfile->flags = VFS_O_READ;
    rfile->refs = 1;
    wfile->mnt = &pipe_mount;
    wfile->priv = pi;
    wfile->pos = 0;
    wfile->flags = VFS_O_WRITE;
    wfile->refs = 1;
    *out_r = rfile;
    *out_w = wfile;
    return 0;
}

int user_pipe_create(struct vfs_file **out_r, struct vfs_file **out_w) {
    return kpipe_create(out_r, out_w);
}

/* ---------- argv marshalling ---------- */

static int copy_argv(uint32_t uargv, int is32, char args[][SPAWN_ARG_LEN], char **argv, int *out_argc) {
    int argc = 0;
    uint32_t psz = is32 ? 4 : 8;

    if (uargv) {
        while (argc < SPAWN_MAX_ARGS) {
            uint32_t slot = uargv + (uint32_t)argc * psz;
            if (!uptr_ok(slot, psz)) {
                return -1;
            }
            uint64_t ptr = psz == 4 ? *(const uint32_t *)(uintptr_t)slot : *(const uint64_t *)(uintptr_t)slot;
            if (ptr > 0xFFFFFFFFull) {
                return -1;
            }
            if (!ptr) {
                break;
            }
            if (ustr_copy(args[argc], (uint32_t)ptr, SPAWN_ARG_LEN) < 0) {
                return -1;
            }
            argv[argc] = args[argc];
            argc++;
        }
    }
    argv[argc] = NULL;
    *out_argc = argc;
    return 0;
}

/* ---------- syscalls ---------- */

/* Syscall ABI: 32-bit programs use ebx/ecx/edx, 64-bit programs rdi/rsi/rdx
 * (number in eax/rax, int 0x80). Pointers are user addresses below 4 GiB. */
static uint32_t arg1(struct regs *r) { return (uint32_t)(cur_proc()->is32 ? r->rbx : r->rdi); }
static uint32_t arg2(struct regs *r) { return (uint32_t)(cur_proc()->is32 ? r->rcx : r->rsi); }
static uint32_t arg3(struct regs *r) { return (uint32_t)r->rdx; }

static int sys_write(struct regs *r) {
    struct uproc *p = cur_proc();
    int fd = (int)arg1(r);
    uint32_t buf = arg2(r), len = arg3(r);

    if (!uptr_ok(buf, len)) {
        return -1;
    }
    if (fd < 0 || fd >= MAX_FDS) {
        return -1;
    }
    if (!p->fds[fd]) {
        if (fd != 1 && fd != 2) {
            return -1;
        }
        console_write_n((const char *)(uintptr_t)buf, len);
        return (int)len;
    }
    uint8_t *kbuf = (uint8_t *)kmalloc(IO_CHUNK);
    if (!kbuf) {
        return -1;
    }
    uint32_t done = 0;
    int failed = 0;
    while (done < len) {
        uint32_t chunk = len - done < IO_CHUNK ? len - done : IO_CHUNK;
        memcpy(kbuf, (const void *)(uintptr_t)(buf + done), chunk);
        int n = vfs_write(p->fds[fd], kbuf, chunk);
        if (n < 0) {
            failed = 1;
            break;
        }
        done += (uint32_t)n;
        if ((uint32_t)n < chunk) {
            break;
        }
    }
    kfree(kbuf);
    return (done == 0 && failed) ? -1 : (int)done;
}

static int sys_read(struct regs *r) {
    struct uproc *p = cur_proc();
    int fd = (int)arg1(r);
    uint32_t buf = arg2(r), len = arg3(r);

    if (!uptr_ok(buf, len)) {
        return -1;
    }
    if (fd < 0 || fd >= MAX_FDS) {
        return -1;
    }
    if (fd == 0 && !p->fds[0]) {
        if (!sched_is_foreground(sched_current())) {
            return 0;
        }
        if (p->line_pos >= p->line_len) {
            p->line_len = p->line_pos = 0;
            for (;;) {
                int k = input_getkey();
                if (k == '\n') {
                    console_putchar('\n');
                    p->linebuf[p->line_len++] = '\n';
                    break;
                }
                if (k == '\b') {
                    if (p->line_len) {
                        p->line_len--;
                        console_write("\b \b");
                    }
                } else if (k >= 32 && k < 127 && p->line_len < sizeof(p->linebuf) - 1) {
                    p->linebuf[p->line_len++] = (char)k;
                    console_putchar((char)k);
                }
            }
        }
        uint32_t n = 0;
        while (n < len && p->line_pos < p->line_len) {
            ((char *)buf)[n++] = p->linebuf[p->line_pos++];
        }
        return (int)n;
    }
    if (!p->fds[fd]) {
        return -1;
    }
    uint8_t *kbuf = (uint8_t *)kmalloc(IO_CHUNK);
    if (!kbuf) {
        return -1;
    }
    uint32_t done = 0;
    int failed = 0;
    while (done < len) {
        uint32_t chunk = len - done < IO_CHUNK ? len - done : IO_CHUNK;
        int n = vfs_read(p->fds[fd], kbuf, chunk);
        if (n < 0) {
            failed = 1;
            break;
        }
        if (n == 0) {
            break;
        }
        memcpy((void *)(uintptr_t)(buf + done), kbuf, (uint32_t)n);
        done += (uint32_t)n;
        if ((uint32_t)n < chunk) {
            break;
        }
    }
    kfree(kbuf);
    return (done == 0 && failed) ? -1 : (int)done;
}

static int vfs_flags_from_open(int flags) {
    int vf;

    switch (flags & 3) {
    case O_RDONLY: vf = VFS_O_READ; break;
    case O_WRONLY: vf = VFS_O_WRITE; break;
    default:       vf = VFS_O_READ | VFS_O_WRITE; break;
    }
    if (flags & O_CREAT) {
        vf |= VFS_O_CREATE;
    }
    if (flags & O_TRUNC) {
        vf |= VFS_O_TRUNC;
    }
    if (flags & O_APPEND) {
        vf |= VFS_O_APPEND;
    }
    return vf;
}

static int sys_open(struct regs *r) {
    struct uproc *p = cur_proc();
    char path[VFS_PATH_MAX];
    struct vfs_file *f;
    int slot = -1;

    if (ustr_copy(path, arg1(r), sizeof(path)) < 0) {
        return -1;
    }
    for (int i = 3; i < MAX_FDS; i++) {
        if (!p->fds[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }
    if (vfs_open(path, vfs_flags_from_open((int)arg2(r)), &f) < 0) {
        return -1;
    }
    p->fds[slot] = f;
    return slot;
}

static int find_program(const char *name, char *path, size_t size) {
    if (strchr(name, '/')) {
        if (vfs_normalize(name, path, size) < 0) {
            return 0;
        }
    } else {
        if (strlen(name) > 200) {
            return 0;
        }
        snprintf(path, size, "/bin/%s", name);
    }
    return user_is_elf(path);
}

static int sys_spawn(struct regs *r) {
    char path[VFS_PATH_MAX];
    char args[SPAWN_MAX_ARGS][SPAWN_ARG_LEN];
    char *argv[SPAWN_MAX_ARGS + 1];
    int argc = 0;

    if (ustr_copy(path, arg1(r), sizeof(path)) < 0) {
        return -1;
    }
    if (arg2(r)) {
        uint32_t psz = cur_proc()->is32 ? 4 : 8;
        while (argc < SPAWN_MAX_ARGS) {
            uint32_t slot = arg2(r) + (uint32_t)argc * psz;
            if (!uptr_ok(slot, psz)) {
                return -1;
            }
            uint64_t ptr = psz == 4 ? *(const uint32_t *)(uintptr_t)slot : *(const uint64_t *)(uintptr_t)slot;
            if (ptr > 0xFFFFFFFFull) {
                return -1;
            }
            if (!ptr) {
                break;
            }
            if (ustr_copy(args[argc], (uint32_t)ptr, SPAWN_ARG_LEN) < 0) {
                return -1;
            }
            argv[argc] = args[argc];
            argc++;
        }
    }
    if (argc == 0) {
        strlcpy(args[0], path, SPAWN_ARG_LEN);
        argv[0] = args[0];
        argc = 1;
    }
    argv[argc] = NULL;

    char image[VFS_PATH_MAX];
    if (!find_program(path, image, sizeof(image))) {
        return -1;
    }
    struct task *t = user_spawn(image, argc, argv);
    return t ? t->pid : -1;
}

static int sys_wait(struct regs *r) {
    int code = 0;
    int pid = task_wait((int)arg1(r), &code);

    if (pid > 0 && arg2(r) && uptr_ok(arg2(r), 4)) {
        *(int *)(uintptr_t)arg2(r) = code;
    }
    return pid;
}

static int sys_lseek(struct regs *r) {
    struct uproc *p = cur_proc();
    int fd = (int)arg1(r);
    int32_t off = (int32_t)arg2(r);
    int whence = (int)arg3(r);

    if (fd < 0 || fd >= MAX_FDS || !p->fds[fd]) {
        return -1;
    }
    struct vfs_file *f = p->fds[fd];
    int64_t pos;

    if (whence == SEEK_SET) {
        pos = off;
    } else if (whence == SEEK_CUR) {
        pos = (int64_t)f->pos + off;
    } else if (whence == SEEK_END) {
        uint64_t size;
        if (vfs_size(f, &size) < 0) {
            return -1;
        }
        pos = (int64_t)size + off;
    } else {
        return -1;
    }
    if (pos < 0) {
        return -1;
    }
    vfs_seek(f, (uint64_t)pos);
    return (int)pos;
}

static int sys_stat(struct regs *r) {
    char path[VFS_PATH_MAX];
    struct vfs_stat st;
    uint32_t ubuf = arg2(r);

    if (ustr_copy(path, arg1(r), sizeof(path)) < 0 || !uptr_ok(ubuf, sizeof(st))) {
        return -1;
    }
    if (vfs_stat(path, &st) < 0) {
        return -1;
    }
    memcpy((void *)(uintptr_t)ubuf, &st, sizeof(st));
    return 0;
}

static int sys_readdir(struct regs *r) {
    char path[VFS_PATH_MAX];
    struct vfs_dirent *list;
    int count;
    uint32_t idx = arg2(r);
    uint32_t uout = arg3(r);

    if (ustr_copy(path, arg1(r), sizeof(path)) < 0 || !uptr_ok(uout, sizeof(struct vfs_dirent))) {
        return -1;
    }
    if (vfs_list(path, &list, &count) < 0) {
        return -1;
    }
    int ret = ((int)idx >= count) ? 0 : 1;
    if (ret) {
        memcpy((void *)(uintptr_t)uout, &list[idx], sizeof(struct vfs_dirent));
    }
    kfree(list);
    return ret;
}

static int sys_mkdir(struct regs *r) {
    char path[VFS_PATH_MAX];

    if (ustr_copy(path, arg1(r), sizeof(path)) < 0) {
        return -1;
    }
    return vfs_mkdir(path) < 0 ? -1 : 0;
}

static int sys_unlink(struct regs *r) {
    char path[VFS_PATH_MAX];

    if (ustr_copy(path, arg1(r), sizeof(path)) < 0) {
        return -1;
    }
    return vfs_unlink(path) < 0 ? -1 : 0;
}

static int sys_rename(struct regs *r) {
    char from[VFS_PATH_MAX], to[VFS_PATH_MAX];

    if (ustr_copy(from, arg1(r), sizeof(from)) < 0 || ustr_copy(to, arg2(r), sizeof(to)) < 0) {
        return -1;
    }
    return vfs_rename(from, to) < 0 ? -1 : 0;
}

static int sys_chdir(struct regs *r) {
    char path[VFS_PATH_MAX];

    if (ustr_copy(path, arg1(r), sizeof(path)) < 0) {
        return -1;
    }
    return vfs_chdir(path) < 0 ? -1 : 0;
}

static int sys_getcwd(struct regs *r) {
    uint32_t ubuf = arg1(r);
    uint32_t size = arg2(r);
    const char *cwd = vfs_getcwd();
    uint32_t len = (uint32_t)strlen(cwd) + 1;

    if (len > size || !uptr_ok(ubuf, len)) {
        return -1;
    }
    memcpy((void *)(uintptr_t)ubuf, cwd, len);
    return (int)(len - 1);
}

static int std_materialize(struct uproc *p, int fd) {
    struct vfs_file *f;

    if (fd < 0 || fd >= MAX_FDS) {
        return -1;
    }
    if (p->fds[fd]) {
        return 0;
    }
    if (fd > 2 || vfs_open("/dev/console", VFS_O_READ | VFS_O_WRITE, &f) < 0) {
        return -1;
    }
    p->fds[fd] = f;
    return 0;
}

static int sys_dup(struct regs *r) {
    struct uproc *p = cur_proc();
    int fd = (int)arg1(r);

    if (std_materialize(p, fd) < 0) {
        return -1;
    }
    int slot = -1;
    for (int i = 3; i < MAX_FDS; i++) {
        if (!p->fds[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }
    p->fds[fd]->refs++;
    p->fds[slot] = p->fds[fd];
    return slot;
}

static int sys_dup2(struct regs *r) {
    struct uproc *p = cur_proc();
    int oldfd = (int)arg1(r);
    int newfd = (int)arg2(r);

    if (oldfd < 0 || oldfd >= MAX_FDS || newfd < 0 || newfd >= MAX_FDS) {
        return -1;
    }
    if (oldfd == newfd) {
        return (p->fds[oldfd] || oldfd < 3) ? newfd : -1;
    }
    if (std_materialize(p, oldfd) < 0) {
        return -1;
    }
    struct vfs_file *old = p->fds[newfd];
    p->fds[oldfd]->refs++;
    p->fds[newfd] = p->fds[oldfd];
    if (old) {
        vfs_close(old);
    }
    return newfd;
}

static int sys_pipe(struct regs *r) {
    struct uproc *p = cur_proc();
    uint32_t uarr = arg1(r);

    if (!uptr_ok(uarr, 8)) {
        return -1;
    }
    int rf = -1, wf = -1;
    for (int i = 3; i < MAX_FDS; i++) {
        if (!p->fds[i]) {
            rf = i;
            break;
        }
    }
    for (int i = 3; i < MAX_FDS; i++) {
        if (i != rf && !p->fds[i]) {
            wf = i;
            break;
        }
    }
    if (rf < 0 || wf < 0) {
        return -1;
    }
    struct vfs_file *rfile, *wfile;

    if (kpipe_create(&rfile, &wfile) < 0) {
        return -1;
    }
    p->fds[rf] = rfile;
    p->fds[wf] = wfile;
    ((int32_t *)(uintptr_t)uarr)[0] = rf;
    ((int32_t *)(uintptr_t)uarr)[1] = wf;
    return 0;
}

static int sys_munmap(struct regs *r) {
    uint32_t addr = arg1(r);

    if (addr < USER_BASE || addr >= USER_STACK_TOP) {
        return -1;
    }
    return vmm_free((void *)(uintptr_t)addr);
}

static void fork_task_main(void *arg) {
    struct regs local = *(struct regs *)arg;

    kfree(arg);
    cli();
    fork_trampoline(&local);
}

static int sys_fork(struct regs *r) {
    struct uproc *parent = cur_proc();
    struct task *cur = sched_current();
    struct vm_space *space = vmm_space_clone(cur->name);

    if (!space) {
        return -1;
    }
    struct task *t = task_new(cur->name, fork_task_main, NULL, USER_KSTACK, space, 1);
    if (!t) {
        vmm_space_destroy(space);
        return -1;
    }
    struct uproc *cp = &uprocs[task_slot(t)];
    *cp = *parent;
    for (int i = 0; i < MAX_FDS; i++) {
        if (cp->fds[i]) {
            cp->fds[i]->refs++;
        }
    }
    t->priv = cp;
    t->sig_blocked = cur->sig_blocked;
    for (int i = 0; i < NSIG; i++) {
        t->sig_handler[i] = cur->sig_handler[i];
        t->sig_restorer[i] = cur->sig_restorer[i];
        t->sig_mask[i] = cur->sig_mask[i];
    }

    struct regs *saved = (struct regs *)kmalloc(sizeof(struct regs));
    if (!saved) {
        task_discard(t);
        return -1;
    }
    *saved = *r;
    saved->rax = 0;
    t->arg = saved;
    task_start(t);
    return t->pid;
}

static int sys_execve(struct regs *r) {
    struct uproc *p = cur_proc();
    char path[VFS_PATH_MAX];
    char image[VFS_PATH_MAX];
    char args[SPAWN_MAX_ARGS][SPAWN_ARG_LEN];
    char *argv[SPAWN_MAX_ARGS + 1];
    int argc;

    if (ustr_copy(path, arg1(r), sizeof(path)) < 0) {
        return -1;
    }
    if (copy_argv(arg2(r), p->is32, args, argv, &argc) < 0) {
        return -1;
    }
    if (argc == 0) {
        strlcpy(args[0], path, SPAWN_ARG_LEN);
        argv[0] = args[0];
        argv[1] = NULL;
        argc = 1;
    }
    if (!find_program(path, image, sizeof(image))) {
        return -1;
    }
    void *img;
    uint32_t imgsize;
    if (vfs_load(image, &img, &imgsize) < 0) {
        return -1;
    }

    struct task *t = sched_current();
    struct vm_space *old_space = t->space;
    struct vm_space *space = vmm_space_create(t->name);
    if (!space) {
        kfree(img);
        return -1;
    }
    vmm_space_switch(space);

    uint32_t entry = 0;
    int is32;
    int ok = load_elf((const uint8_t *)img, imgsize, &entry, &is32) == 0 &&
             vmm_alloc_at(USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE,
                          VM_READ | VM_WRITE | VM_USER | VM_DEMAND, "user-stack") &&
             vmm_alloc_at(USER_HEAP_BASE, USER_HEAP_MAX,
                          VM_READ | VM_WRITE | VM_USER | VM_DEMAND, "user-heap");
    uint32_t sp = 0;
    if (ok) {
        sp = build_stack(argc, argv, is32);
    }
    kfree(img);
    if (!ok) {
        vmm_space_switch(old_space);
        vmm_space_destroy(space);
        return -1;
    }
    vmm_space_destroy(old_space);
    t->space = space;
    p->is32 = is32;
    p->entry = entry;
    p->brk = USER_HEAP_BASE;
    t->sig_pending = 0;
    for (int i = 0; i < NSIG; i++) {
        t->sig_handler[i] = SIG_DFL_VAL;
        t->sig_restorer[i] = 0;
        t->sig_mask[i] = 0;
    }

    r->rip = entry;
    r->rsp = sp;
    r->rflags = 0x202;
    r->cs = is32 ? 0x1B : 0x2B;
    r->ss = 0x23;
    return 0;
}

static int sys_sigaction(struct regs *r) {
    int sig = (int)arg1(r);
    uint32_t actp = arg2(r);
    uint32_t oldp = arg3(r);
    struct task *t = sched_current();

    if (sig <= 0 || sig >= NSIG) {
        return -1;
    }
    if (oldp) {
        if (!uptr_ok(oldp, sizeof(struct k_sigaction))) {
            return -1;
        }
        struct k_sigaction *uold = (struct k_sigaction *)(uintptr_t)oldp;
        uold->handler = (uint32_t)t->sig_handler[sig - 1];
        uold->restorer = (uint32_t)t->sig_restorer[sig - 1];
        uold->mask = t->sig_mask[sig - 1];
        uold->flags = 0;
    }
    if (!actp) {
        return 0;
    }
    if (sig == SIGKILL || sig == SIGSTOP) {
        return -1;
    }
    if (!uptr_ok(actp, sizeof(struct k_sigaction))) {
        return -1;
    }
    struct k_sigaction *uact = (struct k_sigaction *)(uintptr_t)actp;
    uint32_t handler = uact->handler;

    if (handler != SIG_DFL_VAL && handler != SIG_IGN_VAL && !uptr_ok(handler, 1)) {
        return -1;
    }
    t->sig_handler[sig - 1] = handler;
    t->sig_restorer[sig - 1] = uact->restorer;
    t->sig_mask[sig - 1] = uact->mask & ~(1u << (SIGKILL - 1));
    if (handler == SIG_DFL_VAL || handler == SIG_IGN_VAL) {
        t->sig_pending &= ~(1u << (sig - 1));
    }
    return 0;
}

static int sys_sigreturn(struct regs *r) {
    struct sigframe {
        struct regs regs;
        uint64_t prev_blocked;
        uint64_t reserved;
    };
    struct task *t = sched_current();
    uint32_t sp = (uint32_t)r->rsp;

    if (!uptr_ok(sp, sizeof(struct sigframe))) {
        task_exit(128 + SIGSEGV);
    }
    struct sigframe *frame = (struct sigframe *)(uintptr_t)sp;
    struct regs saved = frame->regs;

    t->sig_blocked = (uint32_t)frame->prev_blocked;
    *r = saved;
    r->cs = 0x2B;
    r->ss = 0x23;
    r->rflags = (saved.rflags & 0xCD5ULL) | 0x202ULL;
    if (r->rip < USER_BASE || r->rip >= USER_STACK_TOP ||
        r->rsp < USER_BASE || r->rsp >= USER_STACK_TOP) {
        task_exit(128 + SIGSEGV);
    }
    return (int)r->rax;
}

static int sys_sigprocmask(struct regs *r) {
    int how = (int)arg1(r);
    uint32_t setp = arg2(r);
    uint32_t oldp = arg3(r);
    struct task *t = sched_current();

    if (oldp) {
        if (!uptr_ok(oldp, sizeof(uint32_t))) {
            return -1;
        }
        *(uint32_t *)(uintptr_t)oldp = t->sig_blocked;
    }
    if (!setp) {
        return 0;
    }
    if (!uptr_ok(setp, sizeof(uint32_t))) {
        return -1;
    }
    uint32_t set = *(uint32_t *)(uintptr_t)setp & ~(1u << (SIGKILL - 1));

    switch (how) {
    case SIG_BLOCK:   t->sig_blocked |= set; break;
    case SIG_UNBLOCK: t->sig_blocked &= ~set; break;
    case SIG_SETMASK: t->sig_blocked = set; break;
    default: return -1;
    }
    return 0;
}

static int alloc_fd_slot(struct uproc *p) {
    for (int i = 3; i < MAX_FDS; i++) {
        if (!p->fds[i]) {
            return i;
        }
    }
    return -1;
}

static int sys_socket(struct regs *r) {
    struct uproc *p = cur_proc();
    int slot = alloc_fd_slot(p);

    if (slot < 0) {
        return -1;
    }
    struct vfs_file *f = socket_create((int)arg1(r), (int)arg2(r), (int)arg3(r));
    if (!f) {
        return -1;
    }
    p->fds[slot] = f;
    return slot;
}

static int sys_bind(struct regs *r) {
    struct uproc *p = cur_proc();
    int fd = (int)arg1(r);

    if (fd < 0 || fd >= MAX_FDS || !p->fds[fd] || !socket_is_socket(p->fds[fd])) {
        return -1;
    }
    if (!uptr_ok(arg2(r), sizeof(struct sockaddr_in))) {
        return -1;
    }
    struct sockaddr_in addr;
    memcpy(&addr, (const void *)(uintptr_t)arg2(r), sizeof(addr));
    return socket_bind(p->fds[fd], &addr);
}

static int sys_connect(struct regs *r) {
    struct uproc *p = cur_proc();
    int fd = (int)arg1(r);

    if (fd < 0 || fd >= MAX_FDS || !p->fds[fd] || !socket_is_socket(p->fds[fd])) {
        return -1;
    }
    if (!uptr_ok(arg2(r), sizeof(struct sockaddr_in))) {
        return -1;
    }
    struct sockaddr_in addr;
    memcpy(&addr, (const void *)(uintptr_t)arg2(r), sizeof(addr));
    return socket_connect(p->fds[fd], &addr);
}

static int sys_listen(struct regs *r) {
    struct uproc *p = cur_proc();
    int fd = (int)arg1(r);

    if (fd < 0 || fd >= MAX_FDS || !p->fds[fd] || !socket_is_socket(p->fds[fd])) {
        return -1;
    }
    return socket_listen(p->fds[fd], (int)arg2(r));
}

static int sys_accept(struct regs *r) {
    struct uproc *p = cur_proc();
    int fd = (int)arg1(r);

    if (fd < 0 || fd >= MAX_FDS || !p->fds[fd] || !socket_is_socket(p->fds[fd])) {
        return -1;
    }
    int slot = alloc_fd_slot(p);
    if (slot < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    struct vfs_file *cf = socket_accept(p->fds[fd], &addr);
    if (!cf) {
        return -1;
    }
    if (arg2(r) && uptr_ok(arg2(r), sizeof(struct sockaddr_in))) {
        memcpy((void *)(uintptr_t)arg2(r), &addr, sizeof(addr));
    }
    p->fds[slot] = cf;
    return slot;
}

void syscall_dispatch(struct regs *r) {
    struct uproc *p = cur_proc();
    int ret = -1;

    uint32_t a1 = arg1(r);
    uint32_t call_no = (uint32_t)r->rax;

    sti();
    switch (call_no) {
    case SYS_EXIT:
        task_exit((int)a1);
    case SYS_WRITE:  ret = sys_write(r); break;
    case SYS_READ:   ret = sys_read(r); break;
    case SYS_OPEN:   ret = sys_open(r); break;
    case SYS_CLOSE:
        if (a1 < MAX_FDS && p->fds[a1]) {
            vfs_close(p->fds[a1]);
            p->fds[a1] = NULL;
            ret = 0;
        }
        break;
    case SYS_GETPID:  ret = sched_current()->pid; break;
    case SYS_GETPPID: ret = sched_current()->ppid; break;
    case SYS_UPTIME:  ret = (int)pit_uptime_ms(); break;
    case SYS_SLEEP:   sleep_ms(a1); ret = 0; break;
    case SYS_YIELD:   sched_yield(); ret = 0; break;
    case SYS_SPAWN:   ret = sys_spawn(r); break;
    case SYS_WAIT:    ret = sys_wait(r); break;
    case SYS_KILL:    ret = task_kill((int)a1, (int)arg2(r)); break;
    case SYS_LSEEK:   ret = sys_lseek(r); break;
    case SYS_STAT:    ret = sys_stat(r); break;
    case SYS_READDIR: ret = sys_readdir(r); break;
    case SYS_MKDIR:   ret = sys_mkdir(r); break;
    case SYS_UNLINK:  ret = sys_unlink(r); break;
    case SYS_RENAME:  ret = sys_rename(r); break;
    case SYS_CHDIR:   ret = sys_chdir(r); break;
    case SYS_GETCWD:  ret = sys_getcwd(r); break;
    case SYS_DUP:     ret = sys_dup(r); break;
    case SYS_DUP2:    ret = sys_dup2(r); break;
    case SYS_PIPE:    ret = sys_pipe(r); break;
    case SYS_TIME:    ret = (int)rtc_unix(); break;
    case SYS_MUNMAP:  ret = sys_munmap(r); break;
    case SYS_FORK:    ret = sys_fork(r); break;
    case SYS_EXECVE:  ret = sys_execve(r); break;
    case SYS_SIGACTION:   ret = sys_sigaction(r); break;
    case SYS_SIGRETURN:   ret = sys_sigreturn(r); break;
    case SYS_SIGPROCMASK: ret = sys_sigprocmask(r); break;
    case SYS_SOCKET:  ret = sys_socket(r); break;
    case SYS_BIND:    ret = sys_bind(r); break;
    case SYS_CONNECT: ret = sys_connect(r); break;
    case SYS_LISTEN:  ret = sys_listen(r); break;
    case SYS_ACCEPT:  ret = sys_accept(r); break;
    case SYS_GETUID:  ret = sched_current()->uid; break;
    case SYS_GETGID:  ret = sched_current()->gid; break;
    case SYS_GETEUID: ret = sched_current()->uid; break;
    case SYS_GETEGID: ret = sched_current()->gid; break;
    case SYS_SETUID:  sched_current()->uid = (uint16_t)a1; ret = 0; break;
    case SYS_SETGID:  sched_current()->gid = (uint16_t)a1; ret = 0; break;
    case SYS_GETTID:  ret = sched_current()->pid; break;
    case SYS_UNAME: {
        char *buf = (char *)(uintptr_t)a1;
        if (uptr_ok(a1, 64)) {
            strncpy(buf, "FelinOS\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 64);
            ret = 0;
        }
        break;
    }
    case SYS_GETTIMEOFDAY: {
        struct timeval *tv = (struct timeval *)(uintptr_t)a1;
        if (uptr_ok(a1, sizeof(struct timeval))) {
            tv->tv_sec = rtc_unix();
            tv->tv_usec = 0;
            ret = 0;
        }
        break;
    }
    case SYS_NANOSLEEP: {
        uint32_t req = a1, rem = arg2(r);
        if (uptr_ok(req, 8)) {
            uint32_t ms = *(uint32_t *)(uintptr_t)req;
            sleep_ms(ms);
            if (rem && uptr_ok(rem, 8)) {
                *(uint32_t *)(uintptr_t)rem = 0;
                *(uint32_t *)(uintptr_t)(rem + 4) = 0;
            }
            ret = 0;
        }
        break;
    }
    case SYS_GETDENTS: {
        int fd = (int)a1;
        char *buf = (char *)(uintptr_t)arg2(r);
        uint32_t count = arg3(r);
        if (fd >= 0 && fd < MAX_FDS && p->fds[fd] && uptr_ok(arg2(r), count)) {
            ret = vfs_readdir_fd(p->fds[fd], buf, count);
        }
        break;
    }
    case SYS_FCNTL: {
        int fd = (int)a1;
        int cmd = (int)arg2(r);
        if (fd >= 0 && fd < MAX_FDS && p->fds[fd]) {
            ret = vfs_fcntl(p->fds[fd], cmd, arg3(r));
        }
        break;
    }
    case SYS_IOCTL: {
        int fd = (int)a1;
        uint32_t request = arg2(r);
        if (fd >= 0 && fd < MAX_FDS && p->fds[fd]) {
            ret = vfs_ioctl(p->fds[fd], request, arg3(r));
        }
        break;
    }
    case SYS_FSYNC:
    case SYS_FDATASYNC: {
        int fd = (int)a1;
        if (fd >= 0 && fd < MAX_FDS && p->fds[fd]) {
            ret = vfs_sync_file(p->fds[fd]);
        }
        break;
    }
    case SYS_FCHDIR: {
        int fd = (int)a1;
        if (fd >= 0 && fd < MAX_FDS && p->fds[fd]) {
            ret = vfs_fchdir(p->fds[fd]);
        }
        break;
    }
    case SYS_CREAT: {
        char *path = (char *)(uintptr_t)a1;
        if (uptr_ok(a1, 256)) {
            struct vfs_file *f;
            ret = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC, &f);
            if (ret >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_FDS; i++) {
                    if (!p->fds[i]) {
                        slot = i;
                        break;
                    }
                }
                if (slot >= 0) {
                    p->fds[slot] = f;
                    ret = slot;
                } else {
                    vfs_close(f);
                    ret = -1;
                }
            }
        }
        break;
    }
    case SYS_LINK: {
        char *old = (char *)(uintptr_t)a1;
        char *new = (char *)(uintptr_t)arg2(r);
        if (uptr_ok(a1, 256) && uptr_ok(arg2(r), 256)) {
            ret = vfs_link(old, new);
        }
        break;
    }
    case SYS_SYMLINK: {
        char *target = (char *)(uintptr_t)a1;
        char *linkpath = (char *)(uintptr_t)arg2(r);
        if (uptr_ok(a1, 256) && uptr_ok(arg2(r), 256)) {
            ret = vfs_symlink(target, linkpath);
        }
        break;
    }
    case SYS_READLINK: {
        char *path = (char *)(uintptr_t)a1;
        char *buf = (char *)(uintptr_t)arg2(r);
        uint32_t bufsiz = arg3(r);
        if (uptr_ok(a1, 256) && uptr_ok(arg2(r), bufsiz)) {
            ret = vfs_readlink(path, buf, bufsiz);
        }
        break;
    }
    case SYS_CHMOD: {
        char *path = (char *)(uintptr_t)a1;
        uint16_t mode = (uint16_t)arg2(r);
        if (uptr_ok(a1, 256)) {
            ret = vfs_chmod(path, mode);
        }
        break;
    }
    case SYS_FCHMOD: {
        int fd = (int)a1;
        uint16_t mode = (uint16_t)arg2(r);
        if (fd >= 0 && fd < MAX_FDS && p->fds[fd]) {
            ret = vfs_fchmod(p->fds[fd], mode);
        }
        break;
    }
    case SYS_CHOWN: {
        char *path = (char *)(uintptr_t)a1;
        uint16_t uid = (uint16_t)arg2(r);
        uint16_t gid = (uint16_t)arg3(r);
        if (uptr_ok(a1, 256)) {
            ret = vfs_chown(path, uid, gid);
        }
        break;
    }
    case SYS_FCHOWN: {
        int fd = (int)a1;
        uint16_t uid = (uint16_t)arg2(r);
        uint16_t gid = (uint16_t)arg3(r);
        if (fd >= 0 && fd < MAX_FDS && p->fds[fd]) {
            ret = vfs_fchown(p->fds[fd], uid, gid);
        }
        break;
    }
    case SYS_LCHOWN: {
        char *path = (char *)(uintptr_t)a1;
        uint16_t uid = (uint16_t)arg2(r);
        uint16_t gid = (uint16_t)arg3(r);
        if (uptr_ok(a1, 256)) {
            ret = vfs_lchown(path, uid, gid);
        }
        break;
    }
    case SYS_UMASK: {
        ret = sched_current()->umask;
        sched_current()->umask = (uint16_t)a1;
        break;
    }
    case SYS_GETPGID: {
        int pid = (int)a1;
        struct task *t = task_find(pid);
        ret = t ? t->pgid : -1;
        break;
    }
    case SYS_SETPGID: {
        int pid = (int)a1;
        int pgid = (int)arg2(r);
        struct task *t = task_find(pid);
        if (t) {
            t->pgid = pgid;
            ret = 0;
        }
        break;
    }
    case SYS_GETSID: {
        int pid = (int)a1;
        struct task *t = task_find(pid);
        ret = t ? t->sid : -1;
        break;
    }
    case SYS_SETSID: {
        struct task *t = sched_current();
        t->sid = t->pid;
        t->pgid = t->pid;
        ret = t->sid;
        break;
    }
    case SYS_GETGROUPS: {
        int size = (int)a1;
        uint16_t *list = (uint16_t *)(uintptr_t)arg2(r);
        if (size <= 0 || !list || !uptr_ok(arg2(r), size * 2)) break;
        list[0] = sched_current()->gid;
        ret = 1;
        break;
    }
    case SYS_SETGROUPS: {
        int size = (int)a1;
        uint16_t *list = (uint16_t *)(uintptr_t)arg2(r);
        if (size > 0 && list && uptr_ok(arg2(r), size * 2)) {
            sched_current()->gid = list[0];
            ret = 0;
        }
        break;
    }
    case SYS_GETRESUID: {
        uint16_t *ruid = (uint16_t *)(uintptr_t)a1;
        uint16_t *euid = (uint16_t *)(uintptr_t)arg2(r);
        uint16_t *suid = (uint16_t *)(uintptr_t)arg3(r);
        if (uptr_ok(a1, 2) && uptr_ok(arg2(r), 2) && uptr_ok(arg3(r), 2)) {
            *ruid = sched_current()->uid;
            *euid = sched_current()->uid;
            *suid = sched_current()->uid;
            ret = 0;
        }
        break;
    }
    case SYS_GETRESGID: {
        uint16_t *rgid = (uint16_t *)(uintptr_t)a1;
        uint16_t *egid = (uint16_t *)(uintptr_t)arg2(r);
        uint16_t *sgid = (uint16_t *)(uintptr_t)arg3(r);
        if (uptr_ok(a1, 2) && uptr_ok(arg2(r), 2) && uptr_ok(arg3(r), 2)) {
            *rgid = sched_current()->gid;
            *egid = sched_current()->gid;
            *sgid = sched_current()->gid;
            ret = 0;
        }
        break;
    }
    case SYS_SETRESUID: {
        uint16_t ruid = (uint16_t)a1;
        uint16_t euid = (uint16_t)arg2(r);
        uint16_t suid = (uint16_t)arg3(r);
        sched_current()->uid = ruid;
        ret = 0;
        break;
    }
    case SYS_SETRESGID: {
        uint16_t rgid = (uint16_t)a1;
        uint16_t egid = (uint16_t)arg2(r);
        uint16_t sgid = (uint16_t)arg3(r);
        sched_current()->gid = rgid;
        ret = 0;
        break;
    }
    case SYS_SBRK: {
        int32_t inc = (int32_t)a1;
        uint32_t nb = p->brk + (uint32_t)inc;
        if (nb < USER_HEAP_BASE || nb > USER_HEAP_BASE + USER_HEAP_MAX) {
            break;
        }
        ret = (int)p->brk;
        p->brk = nb;
        break;
    }
    default: break;
    }
    cli();
    if (call_no != SYS_SIGRETURN) {
        r->rax = (uint64_t)(int64_t)ret;
    }
    sched_syscall_return(r);
}

/* ---------- shell glue ---------- */

int user_wait_foreground(int pid) {
    int code = 0;

    sched_set_foreground(pid);
    int reaped = task_wait(pid, &code);
    sched_set_foreground(sched_shell_pid());
    return reaped < 0 ? 1 : code;
}

void user_report_children(void) {
    char name[SCHED_NAME_LEN];
    int pid, code;
    int me = sched_current()->pid;

    while (task_collect(me, &pid, &code, name, sizeof(name))) {
        if (code == 0) {
            kprintf("[%d] done        %s\n", pid, name);
        } else if (code == 128 + SCHED_SIGINT) {
            kprintf("[%d] interrupted %s\n", pid, name);
        } else if (code == 128 + SCHED_SIGKILL || code == 128 + SCHED_SIGTERM) {
            kprintf("[%d] killed      %s\n", pid, name);
        } else {
            kprintf("[%d] exit %-6d %s\n", pid, code, name);
        }
    }
}

int user_program_exists(const char *name) {
    char path[VFS_PATH_MAX];

    return find_program(name, path, sizeof(path)) ? 1 : 0;
}

struct task *user_spawn_piped(int argc, char **argv, struct vfs_file *fin, struct vfs_file *fout) {
    char image[VFS_PATH_MAX];

    if (!find_program(argv[0], image, sizeof(image))) {
        if (fin) {
            vfs_close(fin);
        }
        if (fout) {
            vfs_close(fout);
        }
        return NULL;
    }
    return user_spawn_io(image, argc, argv, fin, fout);
}

int user_run_command(int argc, char **argv, int background) {
    char image[VFS_PATH_MAX];

    if (!find_program(argv[0], image, sizeof(image))) {
        return USER_NOTFOUND;
    }
    struct task *t = user_spawn(image, argc, argv);
    if (!t) {
        st_printf(stream_console(), "%s: cannot execute\n", argv[0]);
        return 126;
    }
    if (background) {
        st_printf(stream_console(), "[%d] %s\n", t->pid, t->name);
        return 0;
    }
    return user_wait_foreground(t->pid);
}

int cmd_exec(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        cmd_error(out, "exec", NULL, "usage: exec <program> [args...]");
        return 1;
    }
    int code = user_run_command(argc - 1, argv + 1, shell_take_background());
    if (code == USER_NOTFOUND) {
        cmd_error(out, "exec", argv[1], "not an executable");
        return 127;
    }
    return code;
}
