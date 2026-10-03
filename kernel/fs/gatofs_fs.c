#include "fs/vfs.h"
#include "fs/gatofs.h"
#include "drivers/ata.h"
#include "lib/string.h"
#include "sched.h"
#include "io.h"

static struct task *lock_owner;
static int lock_depth;
static int attached;

void gatofs_fs_lock(void) {
    struct task *me = sched_current();
    uint32_t flags = irq_save();

    if (lock_owner == me) {
        lock_depth++;
        irq_restore(flags);
        return;
    }
    while (lock_owner) {
        sched_wait_on((const void *)&lock_owner, "fslock");
    }
    lock_owner = me;
    lock_depth = 1;
    task_uninterruptible_enter();
    irq_restore(flags);
}

void gatofs_fs_unlock(void) {
    uint32_t flags = irq_save();

    if (lock_depth > 0 && --lock_depth == 0) {
        lock_owner = NULL;
        sched_wake((const void *)&lock_owner);
        task_uninterruptible_leave();
    }
    irq_restore(flags);
}

static uint8_t map_type(uint8_t type) {
    return type == GATOFS_DIR ? VFS_DIR : VFS_FILE;
}

static int be_mount(struct vfs_mount *m, const char *source) {
    struct ata_device *dev = NULL;
    struct gatofs_info info;
    int r = VFS_OK;

    if (attached) {
        return VFS_EBUSY;
    }
    if (source && *source && strcmp(source, "none") != 0) {
        dev = ata_find(source);
        if (!dev) {
            return VFS_ENODEV;
        }
    }

    gatofs_fs_lock();
    if (gatofs_mounted()) {
        gatofs_info(&info);
        if (dev && strcmp(info.dev, dev->name) != 0) {
            r = VFS_EBUSY;
        }
    } else if (dev) {
        r = gatofs_mount(dev) < 0 ? VFS_EINVAL : VFS_OK;
    } else {
        r = gatofs_probe() < 0 ? VFS_ENODEV : VFS_OK;
    }
    if (r == VFS_OK && gatofs_info(&info) == 0) {
        strlcpy(m->source, info.dev, sizeof(m->source));
    }
    gatofs_fs_unlock();

    if (r < 0) {
        return r;
    }
    attached = 1;
    return VFS_OK;
}

static void be_umount(struct vfs_mount *m) {
    gatofs_fs_lock();
    gatofs_unmount();
    gatofs_fs_unlock();
    attached = 0;
}

static int be_stat(struct vfs_mount *m, const char *path, struct vfs_stat *st) {
    struct gatofs_stat gs;

    gatofs_fs_lock();
    int r = gatofs_stat(path, &gs);
    gatofs_fs_unlock();
    if (r < 0) {
        return r;
    }
    st->type = map_type(gs.type);
    st->mode = gs.mode;
    st->size = gs.size;
    st->mtime = gs.mtime;
    st->ino = gs.ino;
    st->uid = gs.uid;
    st->gid = gs.gid;
    return VFS_OK;
}

static int be_open(struct vfs_mount *m, const char *path, int flags, struct vfs_file *f) {
    int vf = 0;

    if (flags & VFS_O_READ) {
        vf |= VF_READ;
    }
    if (flags & VFS_O_WRITE) {
        vf |= VF_WRITE;
    }
    if (flags & VFS_O_CREATE) {
        vf |= VF_CREATE;
    }
    if (flags & VFS_O_TRUNC) {
        vf |= VF_TRUNC;
    }
    if (flags & VFS_O_APPEND) {
        vf |= VF_APPEND;
    }

    /* Handle synthetic path for writeback: #ino:N */
    if (path[0] == '#' && path[1] == 'i' && path[2] == 'n' && path[3] == 'o' && path[4] == ':') {
        uint32_t ino = 0;
        for (int i = 5; path[i]; i++) {
            if (path[i] >= '0' && path[i] <= '9') {
                ino = ino * 10 + (path[i] - '0');
            }
        }
        if (ino != 0) {
            gatofs_fs_lock();
            int fd = gatofs_open_by_ino(ino, vf);
            gatofs_fs_unlock();
            if (fd >= 0) {
                f->priv = (void *)(uintptr_t)(fd + 1);
                f->pos = 0;
                return VFS_OK;
            }
            return fd;
        }
    }

    gatofs_fs_lock();
    int fd = gatofs_open(path, vf);
    gatofs_fs_unlock();
    if (fd < 0) {
        return fd;
    }
    f->priv = (void *)(uintptr_t)(fd + 1);
    f->pos = 0;
    return VFS_OK;
}

static void be_close(struct vfs_file *f) {
    gatofs_close((int)(uintptr_t)f->priv - 1);
}

static int be_read(struct vfs_file *f, void *buf, uint32_t len) {
    int fd = (int)(uintptr_t)f->priv - 1;

    if (f->pos > 0xFFFFFFFFull) {
        return 0;
    }
    gatofs_fs_lock();
    gatofs_seek(fd, (uint32_t)f->pos);
    int r = gatofs_read(fd, buf, len);
    if (r > 0) {
        f->pos = gatofs_tell(fd);
    }
    gatofs_fs_unlock();
    return r;
}

static int be_write(struct vfs_file *f, const void *buf, uint32_t len) {
    int fd = (int)(uintptr_t)f->priv - 1;

    if (f->pos > 0xFFFFFFFFull) {
        return VFS_EFBIG;
    }
    gatofs_fs_lock();
    gatofs_seek(fd, (uint32_t)f->pos);
    int r = gatofs_write(fd, buf, len);
    if (r > 0) {
        f->pos = gatofs_tell(fd);
    }
    gatofs_fs_unlock();
    return r;
}

static int be_size(struct vfs_file *f, uint64_t *out) {
    int fd = (int)(uintptr_t)f->priv - 1;

    gatofs_fs_lock();
    *out = gatofs_size(fd);
    gatofs_fs_unlock();
    return VFS_OK;
}

struct dir_ctx {
    vfs_dir_cb cb;
    void *ctx;
};

static int dir_adapter(const char *name, uint8_t type, uint32_t ino, void *c) {
    struct dir_ctx *d = (struct dir_ctx *)c;

    return d->cb(name, map_type(type), d->ctx);
}

static int be_readdir(struct vfs_mount *m, const char *path, vfs_dir_cb cb, void *ctx) {
    struct dir_ctx d = { cb, ctx };

    gatofs_fs_lock();
    int r = gatofs_readdir(path, dir_adapter, &d);
    gatofs_fs_unlock();
    return r;
}

static int be_mkdir(struct vfs_mount *m, const char *path) {
    gatofs_fs_lock();
    int r = gatofs_mkdir(path);
    gatofs_fs_unlock();
    return r;
}

static int be_remove(struct vfs_mount *m, const char *path) {
    gatofs_fs_lock();
    int r = gatofs_remove(path, 0);
    gatofs_fs_unlock();
    return r;
}

static int be_rename(struct vfs_mount *m, const char *from, const char *to) {
    gatofs_fs_lock();
    int r = gatofs_rename(from, to);
    gatofs_fs_unlock();
    return r;
}

static int be_chmod(struct vfs_mount *m, const char *path, uint16_t mode) {
    gatofs_fs_lock();
    int r = gatofs_chmod(path, mode);
    gatofs_fs_unlock();
    return r;
}

static int be_chown(struct vfs_mount *m, const char *path, uint16_t uid, uint16_t gid) {
    gatofs_fs_lock();
    int r = gatofs_chown(path, uid, gid);
    gatofs_fs_unlock();
    return r;
}

static int be_touch(struct vfs_mount *m, const char *path) {
    gatofs_fs_lock();
    int r = gatofs_touch(path);
    gatofs_fs_unlock();
    return r;
}

static int be_statfs(struct vfs_mount *m, struct vfs_statfs *sf) {
    struct gatofs_info gi;

    gatofs_fs_lock();
    int r = gatofs_info(&gi);
    gatofs_fs_unlock();
    if (r < 0) {
        return r;
    }
    sf->total = (uint64_t)gi.total_blocks * GATOFS_BLOCK;
    sf->avail = (uint64_t)gi.free_blocks * GATOFS_BLOCK;
    sf->used = sf->total - sf->avail;
    sf->files = gi.total_inodes - gi.free_inodes;
    return VFS_OK;
}

static int be_sync(struct vfs_mount *m) {
    gatofs_fs_lock();
    gatofs_sync();
    gatofs_fs_unlock();
    return VFS_OK;
}

static int be_truncate(struct vfs_file *f, uint64_t size) {
    int fd = (int)(uintptr_t)f->priv - 1;

    if (size > 0xFFFFFFFFull) {
        return VFS_EFBIG;
    }
    gatofs_fs_lock();
    int r = gatofs_truncate(fd, (uint32_t)size);
    if (r == 0) {
        f->pos = gatofs_tell(fd);
    }
    gatofs_fs_unlock();
    if (r >= 0) {
        return VFS_OK;
    }
    /* gatofs reports its own negative codes; map them onto the VFS set. */
    switch (r) {
    case GATOFS_EISDIR:   return VFS_EISDIR;
    case GATOFS_EINVAL:   return VFS_EINVAL;
    case GATOFS_ENOSPC:   return VFS_ENOSPC;
    case GATOFS_ENOMOUNT: return VFS_ENODEV;
    case GATOFS_EIO:      return VFS_EIO;
    default:              return VFS_EINVAL;
    }
}

const struct fs_ops gatofs_ops = {
    .name = "gatofs",
    .mount = be_mount,
    .umount = be_umount,
    .stat = be_stat,
    .open = be_open,
    .close = be_close,
    .read = be_read,
    .write = be_write,
    .readdir = be_readdir,
    .mkdir = be_mkdir,
    .remove = be_remove,
    .rename = be_rename,
    .chmod = be_chmod,
    .chown = be_chown,
    .touch = be_touch,
    .statfs = be_statfs,
    .sync = be_sync,
    .size = be_size,
    .truncate = be_truncate,
};
