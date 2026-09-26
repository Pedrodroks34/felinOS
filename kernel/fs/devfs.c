#include "fs/vfs.h"
#include "sync.h"
#include "fs/gatofs.h"
#include "drivers/ata.h"
#include "drivers/input.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "console.h"
#include "sched.h"
#include "swap.h"
#include "system.h"

#define DISK_CHUNK_SECTORS 64
#define LINE_MAX_BYTES 256

enum {
    DEV_NULL,
    DEV_ZERO,
    DEV_FULL,
    DEV_RANDOM,
    DEV_CONSOLE,
    DEV_KMSG,
    DEV_DISK
};

struct dev_entry {
    char name[8];
    uint8_t type;
    uint8_t kind;
    uint16_t mode;
    struct ata_device *ata;
};

struct dev_handle {
    struct dev_entry *entry;
    uint32_t line_len;
    uint32_t line_pos;
    char line[LINE_MAX_BYTES];
};

#define DEV_MAX (8 + ATA_MAX_DEVICES)

static struct dev_entry table[DEV_MAX];
static int table_count;
static uint64_t rng_state;
static spinlock_t rng_lock = SPINLOCK_INIT("devfs-rng");

static void add_char(const char *name, uint8_t kind) {
    struct dev_entry *e = &table[table_count++];

    strlcpy(e->name, name, sizeof(e->name));
    e->type = VFS_CHR;
    e->kind = kind;
    e->mode = 0666;
    e->ata = NULL;
}

static void build_table(void) {
    table_count = 0;
    add_char("null", DEV_NULL);
    add_char("zero", DEV_ZERO);
    add_char("full", DEV_FULL);
    add_char("random", DEV_RANDOM);
    add_char("urandom", DEV_RANDOM);
    add_char("console", DEV_CONSOLE);
    add_char("tty", DEV_CONSOLE);
    add_char("kmsg", DEV_KMSG);

    for (int i = 0; i < ata_device_count() && table_count < DEV_MAX; i++) {
        struct ata_device *d = ata_get_device(i);
        struct dev_entry *e = &table[table_count++];
        strlcpy(e->name, d->name, sizeof(e->name));
        e->type = VFS_BLK;
        e->kind = DEV_DISK;
        e->mode = 0660;
        e->ata = d;
    }
}

static struct dev_entry *find_entry(const char *path, int *index) {
    if (path[0] != '/' || path[1] == '\0' || strchr(path + 1, '/')) {
        return NULL;
    }
    for (int i = 0; i < table_count; i++) {
        if (strcmp(table[i].name, path + 1) == 0) {
            if (index) {
                *index = i;
            }
            return &table[i];
        }
    }
    return NULL;
}

static uint64_t disk_bytes(const struct dev_entry *e) {
    if (e->ata->type != ATA_TYPE_ATA) {
        return 0;
    }
    return (uint64_t)e->ata->sectors * ATA_SECTOR_SIZE;
}

static int dev_mount(struct vfs_mount *m, const char *source) {
    build_table();
    return VFS_OK;
}

static int dev_stat(struct vfs_mount *m, const char *path, struct vfs_stat *st) {
    st->mtime = system_boot_unix();
    if (strcmp(path, "/") == 0) {
        st->type = VFS_DIR;
        st->mode = 0755;
        st->ino = 1;
        return VFS_OK;
    }
    int index = 0;
    struct dev_entry *e = find_entry(path, &index);
    if (!e) {
        return strchr(path + 1, '/') ? VFS_ENOTDIR : VFS_ENOENT;
    }
    st->type = e->type;
    st->mode = e->mode;
    st->size = (e->kind == DEV_DISK) ? disk_bytes(e) : 0;
    st->ino = (uint32_t)index + 2;
    return VFS_OK;
}

static int disk_busy(const struct ata_device *d) {
    struct gatofs_info gi;

    if (gatofs_mounted() && gatofs_info(&gi) == 0 && strcmp(gi.dev, d->name) == 0) {
        return 1;
    }
    return swap_active() && strcmp(swap_device_name(), d->name) == 0;
}

static int dev_open(struct vfs_mount *m, const char *path, int flags, struct vfs_file *f) {
    if (strcmp(path, "/") == 0) {
        return VFS_EISDIR;
    }
    struct dev_entry *e = find_entry(path, NULL);
    if (!e) {
        return (flags & VFS_O_CREATE) ? VFS_EPERM : VFS_ENOENT;
    }
    if (e->kind == DEV_DISK && (flags & VFS_O_WRITE) && disk_busy(e->ata)) {
        return VFS_EBUSY;
    }
    struct dev_handle *h = (struct dev_handle *)kzalloc(sizeof(struct dev_handle));
    if (!h) {
        return VFS_ENOMEM;
    }
    h->entry = e;
    f->priv = h;
    f->pos = 0;
    return VFS_OK;
}

static void dev_close(struct vfs_file *f) {
    kfree(f->priv);
}

static uint64_t rng_next(void) {
    uint32_t lf = spin_lock_irqsave(&rng_lock);
    if (!rng_state) {
        rng_state = ((uint64_t)pit_ticks() << 32) ^ rtc_unix() ^ 0x9E3779B97F4A7C15ull;
    }
    rng_state ^= (uint64_t)pit_ticks();
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    uint64_t rv = rng_state * 0x2545F4914F6CDD1Dull;
    spin_unlock_irqrestore(&rng_lock, lf);
    return rv;
}

static int console_read(struct dev_handle *h, uint8_t *buf, uint32_t len) {
    if (!sched_is_foreground(sched_current())) {
        return 0;
    }
    if (h->line_pos >= h->line_len) {
        h->line_len = 0;
        h->line_pos = 0;
        for (;;) {
            int key = input_getkey();
            if (key == 4) {
                if (h->line_len == 0) {
                    return 0;
                }
                break;
            }
            if (key == 3) {
                console_write("^C\n");
                h->line_len = 0;
                return 0;
            }
            if (key == '\n') {
                console_putchar('\n');
                h->line[h->line_len++] = '\n';
                break;
            }
            if (key == '\b') {
                if (h->line_len) {
                    h->line_len--;
                    console_write("\b \b");
                }
                continue;
            }
            if (key >= 32 && key < 127 && h->line_len < LINE_MAX_BYTES - 1) {
                h->line[h->line_len++] = (char)key;
                console_putchar((char)key);
            }
        }
    }
    uint32_t n = 0;
    while (n < len && h->line_pos < h->line_len) {
        buf[n++] = (uint8_t)h->line[h->line_pos++];
    }
    return (int)n;
}

static int disk_io(struct vfs_file *f, struct dev_entry *e, uint8_t *buf, uint32_t len, int write) {
    if (e->ata->type != ATA_TYPE_ATA) {
        return VFS_EIO;
    }
    uint64_t size = disk_bytes(e);
    if (f->pos >= size) {
        return write ? VFS_ENOSPC : 0;
    }
    if (len > size - f->pos) {
        len = (uint32_t)(size - f->pos);
    }
    uint8_t *bounce = (uint8_t *)kmalloc(DISK_CHUNK_SECTORS * ATA_SECTOR_SIZE);
    if (!bounce) {
        return VFS_ENOMEM;
    }

    uint32_t done = 0;
    int error = 0;
    while (done < len) {
        uint64_t p = f->pos + done;
        uint32_t lba = (uint32_t)(p / ATA_SECTOR_SIZE);
        uint32_t off = (uint32_t)(p % ATA_SECTOR_SIZE);
        uint32_t remaining = len - done;
        uint32_t sectors = (off + remaining + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
        if (sectors > DISK_CHUNK_SECTORS) {
            sectors = DISK_CHUNK_SECTORS;
        }
        uint32_t span = sectors * ATA_SECTOR_SIZE - off;
        if (span > remaining) {
            span = remaining;
        }

        if (!write) {
            if (ata_read_sectors(e->ata, lba, (uint8_t)sectors, bounce) < 0) {
                error = VFS_EIO;
                break;
            }
            memcpy(buf + done, bounce + off, span);
        } else {
            if (off != 0 || (off + span) % ATA_SECTOR_SIZE != 0) {
                if (ata_read_sectors(e->ata, lba, (uint8_t)sectors, bounce) < 0) {
                    error = VFS_EIO;
                    break;
                }
            }
            memcpy(bounce + off, buf + done, span);
            if (ata_write_sectors(e->ata, lba, (uint8_t)sectors, bounce) < 0) {
                error = VFS_EIO;
                break;
            }
        }
        done += span;
    }
    kfree(bounce);
    f->pos += done;
    if (done == 0 && error) {
        return error;
    }
    return (int)done;
}

static int dev_read(struct vfs_file *f, void *buf, uint32_t len) {
    struct dev_handle *h = (struct dev_handle *)f->priv;
    uint8_t *out = (uint8_t *)buf;

    switch (h->entry->kind) {
    case DEV_NULL:
        return 0;
    case DEV_ZERO:
    case DEV_FULL:
        memset(out, 0, len);
        return (int)len;
    case DEV_RANDOM:
        for (uint32_t i = 0; i < len; i += 8) {
            uint64_t v = rng_next();
            uint32_t n = len - i < 8 ? len - i : 8;
            memcpy(out + i, &v, n);
        }
        return (int)len;
    case DEV_CONSOLE:
        return console_read(h, out, len);
    case DEV_KMSG: {
        const char *log = klog_buffer();
        uint64_t total = strlen(log);
        if (f->pos >= total) {
            return 0;
        }
        if (len > total - f->pos) {
            len = (uint32_t)(total - f->pos);
        }
        memcpy(out, log + f->pos, len);
        f->pos += len;
        return (int)len;
    }
    case DEV_DISK:
        return disk_io(f, h->entry, out, len, 0);
    }
    return VFS_EIO;
}

static int dev_write(struct vfs_file *f, const void *buf, uint32_t len) {
    struct dev_handle *h = (struct dev_handle *)f->priv;
    const uint8_t *in = (const uint8_t *)buf;

    switch (h->entry->kind) {
    case DEV_NULL:
    case DEV_ZERO:
    case DEV_RANDOM:
        return (int)len;
    case DEV_FULL:
        return VFS_ENOSPC;
    case DEV_CONSOLE:
        for (uint32_t i = 0; i < len; i++) {
            console_putchar((char)in[i]);
        }
        return (int)len;
    case DEV_KMSG: {
        char line[LINE_MAX_BYTES];
        uint32_t n = len < sizeof(line) - 1 ? len : (uint32_t)sizeof(line) - 1;
        memcpy(line, in, n);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            n--;
        }
        line[n] = '\0';
        klog("%s", line);
        return (int)len;
    }
    case DEV_DISK:
        return disk_io(f, h->entry, (uint8_t *)(uintptr_t)in, len, 1);
    }
    return VFS_EIO;
}

static int dev_size(struct vfs_file *f, uint64_t *out) {
    struct dev_handle *h = (struct dev_handle *)f->priv;

    switch (h->entry->kind) {
    case DEV_KMSG:
        *out = strlen(klog_buffer());
        return VFS_OK;
    case DEV_DISK:
        *out = disk_bytes(h->entry);
        return VFS_OK;
    default:
        return VFS_EINVAL;
    }
}

static int dev_readdir(struct vfs_mount *m, const char *path, vfs_dir_cb cb, void *ctx) {
    if (strcmp(path, "/") != 0) {
        return find_entry(path, NULL) ? VFS_ENOTDIR : VFS_ENOENT;
    }
    for (int i = 0; i < table_count; i++) {
        if (cb(table[i].name, table[i].type, ctx)) {
            break;
        }
    }
    return VFS_OK;
}

const struct fs_ops devfs_ops = {
    .name = "devfs",
    .mount = dev_mount,
    .stat = dev_stat,
    .open = dev_open,
    .close = dev_close,
    .read = dev_read,
    .write = dev_write,
    .readdir = dev_readdir,
    .size = dev_size,
};
