#include "fs/vfs.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "drivers/rtc.h"

#define RAMFS_MAX_FILE 0x40000000u

struct rnode {
    char name[VFS_NAME_MAX];
    uint8_t type;
    uint16_t mode;
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
    uint32_t mtime;
    uint32_t ino;
    int opens;
    int dead;
    struct rnode *parent;
    struct rnode *children;
    struct rnode *next;
};

struct ramfs {
    struct rnode *root;
    uint32_t nodes;
    uint32_t next_ino;
};

static struct rnode *node_new(struct ramfs *fs, const char *name, uint8_t type, struct rnode *parent) {
    struct rnode *n = (struct rnode *)kzalloc(sizeof(struct rnode));

    if (!n) {
        return NULL;
    }
    strlcpy(n->name, name, VFS_NAME_MAX);
    n->type = type;
    n->mode = (type == VFS_DIR) ? 0755 : 0644;
    n->parent = parent;
    n->mtime = rtc_unix();
    n->ino = fs->next_ino++;
    fs->nodes++;
    return n;
}

static void node_free(struct ramfs *fs, struct rnode *n) {
    if (n->data) {
        kfree(n->data);
    }
    kfree(n);
    fs->nodes--;
}

static void node_attach(struct rnode *parent, struct rnode *n) {
    n->parent = parent;
    n->next = NULL;
    if (!parent->children) {
        parent->children = n;
        return;
    }
    struct rnode *cur = parent->children;
    while (cur->next) {
        cur = cur->next;
    }
    cur->next = n;
}

static void node_detach(struct rnode *n) {
    struct rnode *parent = n->parent;

    if (!parent) {
        return;
    }
    if (parent->children == n) {
        parent->children = n->next;
    } else {
        struct rnode *cur = parent->children;
        while (cur && cur->next != n) {
            cur = cur->next;
        }
        if (cur) {
            cur->next = n->next;
        }
    }
    n->next = NULL;
}

static struct rnode *lookup(struct rnode *dir, const char *name) {
    for (struct rnode *c = dir->children; c; c = c->next) {
        if (strcmp(c->name, name) == 0) {
            return c;
        }
    }
    return NULL;
}

static int resolve(struct ramfs *fs, const char *path, struct rnode **out) {
    struct rnode *cur = fs->root;
    const char *p = path;

    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        char part[VFS_NAME_MAX];
        size_t i = 0;
        while (*p && *p != '/') {
            if (i < VFS_NAME_MAX - 1) {
                part[i++] = *p;
            }
            p++;
        }
        part[i] = '\0';
        if (cur->type != VFS_DIR) {
            return VFS_ENOTDIR;
        }
        cur = lookup(cur, part);
        if (!cur) {
            return VFS_ENOENT;
        }
    }
    *out = cur;
    return VFS_OK;
}

static int resolve_parent(struct ramfs *fs, const char *path, struct rnode **parent, char *leaf) {
    const char *slash = strrchr(path, '/');
    char dir[VFS_PATH_MAX];

    if (!slash || !slash[1]) {
        return VFS_EINVAL;
    }
    strlcpy(leaf, slash + 1, VFS_NAME_MAX);
    size_t n = (size_t)(slash - path);
    if (n == 0) {
        strcpy(dir, "/");
    } else {
        memcpy(dir, path, n);
        dir[n] = '\0';
    }
    int r = resolve(fs, dir, parent);
    if (r < 0) {
        return r;
    }
    return ((*parent)->type == VFS_DIR) ? VFS_OK : VFS_ENOTDIR;
}

static void free_tree(struct ramfs *fs, struct rnode *n) {
    while (n->children) {
        struct rnode *c = n->children;
        n->children = c->next;
        free_tree(fs, c);
    }
    node_free(fs, n);
}

static uint64_t tree_bytes(const struct rnode *n) {
    if (n->type == VFS_FILE) {
        return n->size;
    }
    uint64_t total = 0;
    for (const struct rnode *c = n->children; c; c = c->next) {
        total += tree_bytes(c);
    }
    return total;
}

static int ensure_capacity(struct rnode *n, uint32_t needed) {
    if (n->capacity >= needed) {
        return 0;
    }
    uint32_t cap = n->capacity ? n->capacity : 64;
    while (cap < needed) {
        if (cap > RAMFS_MAX_FILE) {
            return -1;
        }
        cap *= 2;
    }
    uint8_t *buf = (uint8_t *)kmalloc(cap);
    if (!buf) {
        return -1;
    }
    memset(buf, 0, cap);
    if (n->data) {
        memcpy(buf, n->data, n->size);
        kfree(n->data);
    }
    n->data = buf;
    n->capacity = cap;
    return 0;
}

static int ramfs_mount(struct vfs_mount *m, const char *source) {
    struct ramfs *fs = (struct ramfs *)kzalloc(sizeof(struct ramfs));

    if (!fs) {
        return VFS_ENOMEM;
    }
    fs->next_ino = 1;
    fs->root = node_new(fs, "/", VFS_DIR, NULL);
    if (!fs->root) {
        kfree(fs);
        return VFS_ENOMEM;
    }
    fs->root->parent = fs->root;
    m->priv = fs;
    return VFS_OK;
}

static void ramfs_umount(struct vfs_mount *m) {
    struct ramfs *fs = (struct ramfs *)m->priv;

    free_tree(fs, fs->root);
    kfree(fs);
    m->priv = NULL;
}

static int ramfs_stat(struct vfs_mount *m, const char *path, struct vfs_stat *st) {
    struct rnode *n;
    int r = resolve((struct ramfs *)m->priv, path, &n);

    if (r < 0) {
        return r;
    }
    st->type = n->type;
    st->mode = n->mode;
    st->size = (n->type == VFS_DIR) ? 0 : n->size;
    st->mtime = n->mtime;
    st->ino = n->ino;
    return VFS_OK;
}

static struct rnode *find_by_ino(struct rnode *root, uint32_t ino) {
    if (root->ino == ino) {
        return root;
    }
    for (struct rnode *c = root->children; c; c = c->next) {
        struct rnode *found = find_by_ino(c, ino);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static int ramfs_open(struct vfs_mount *m, const char *path, int flags, struct vfs_file *f) {
    struct ramfs *fs = (struct ramfs *)m->priv;
    struct rnode *n;

    /* Handle synthetic path for writeback: #ino:N */
    if (path[0] == '#' && path[1] == 'i' && path[2] == 'n' && path[3] == 'o' && path[4] == ':') {
        uint32_t ino = 0;
        for (int i = 5; path[i]; i++) {
            if (path[i] >= '0' && path[i] <= '9') {
                ino = ino * 10 + (path[i] - '0');
            }
        }
        if (ino != 0) {
            n = find_by_ino(fs->root, ino);
            if (n && n->type == VFS_FILE) {
                n->opens++;
                f->priv = n;
                f->pos = 0;
                return VFS_OK;
            }
        }
        return VFS_ENOENT;
    }

    int r = resolve(fs, path, &n);

    if (r == VFS_ENOENT && (flags & VFS_O_CREATE)) {
        struct rnode *parent;
        char leaf[VFS_NAME_MAX];
        r = resolve_parent(fs, path, &parent, leaf);
        if (r < 0) {
            return r;
        }
        n = node_new(fs, leaf, VFS_FILE, parent);
        if (!n) {
            return VFS_ENOMEM;
        }
        node_attach(parent, n);
        parent->mtime = rtc_unix();
    } else if (r < 0) {
        return r;
    }
    if (n->type == VFS_DIR) {
        return VFS_EISDIR;
    }
    if ((flags & VFS_O_TRUNC) && (flags & VFS_O_WRITE)) {
        if (n->data) {
            memset(n->data, 0, n->size);
        }
        n->size = 0;
        n->mtime = rtc_unix();
    }
    n->opens++;
    f->priv = n;
    f->pos = 0;
    return VFS_OK;
}

static void ramfs_close(struct vfs_file *f) {
    struct rnode *n = (struct rnode *)f->priv;

    if (n->opens > 0) {
        n->opens--;
    }
    if (n->dead && n->opens == 0) {
        node_free((struct ramfs *)f->mnt->priv, n);
    }
}

static int ramfs_read(struct vfs_file *f, void *buf, uint32_t len) {
    struct rnode *n = (struct rnode *)f->priv;

    if (f->pos >= n->size) {
        return 0;
    }
    uint32_t off = (uint32_t)f->pos;
    uint32_t avail = n->size - off;
    if (len > avail) {
        len = avail;
    }
    memcpy(buf, n->data + off, len);
    f->pos += len;
    return (int)len;
}

static int ramfs_write(struct vfs_file *f, const void *buf, uint32_t len) {
    struct rnode *n = (struct rnode *)f->priv;

    if (f->flags & VFS_O_APPEND) {
        f->pos = n->size;
    }
    if (f->pos >= RAMFS_MAX_FILE || len > RAMFS_MAX_FILE - (uint32_t)f->pos) {
        return VFS_ENOSPC;
    }
    uint32_t off = (uint32_t)f->pos;
    if (ensure_capacity(n, off + len + 1) < 0) {
        return VFS_ENOSPC;
    }
    memcpy(n->data + off, buf, len);
    if (off + len > n->size) {
        n->size = off + len;
    }
    n->data[n->size] = '\0';
    n->mtime = rtc_unix();
    f->pos = off + len;
    return (int)len;
}

static int ramfs_size(struct vfs_file *f, uint64_t *out) {
    *out = ((struct rnode *)f->priv)->size;
    return VFS_OK;
}

static int ramfs_readdir(struct vfs_mount *m, const char *path, vfs_dir_cb cb, void *ctx) {
    struct rnode *dir;
    int r = resolve((struct ramfs *)m->priv, path, &dir);

    if (r < 0) {
        return r;
    }
    if (dir->type != VFS_DIR) {
        return VFS_ENOTDIR;
    }
    for (struct rnode *c = dir->children; c; c = c->next) {
        if (cb(c->name, c->type, ctx)) {
            break;
        }
    }
    return VFS_OK;
}

static int ramfs_mkdir(struct vfs_mount *m, const char *path) {
    struct ramfs *fs = (struct ramfs *)m->priv;
    struct rnode *parent;
    char leaf[VFS_NAME_MAX];
    int r = resolve_parent(fs, path, &parent, leaf);

    if (r < 0) {
        return r;
    }
    if (lookup(parent, leaf)) {
        return VFS_EEXIST;
    }
    struct rnode *n = node_new(fs, leaf, VFS_DIR, parent);
    if (!n) {
        return VFS_ENOMEM;
    }
    node_attach(parent, n);
    parent->mtime = rtc_unix();
    return VFS_OK;
}

static int ramfs_remove(struct vfs_mount *m, const char *path) {
    struct ramfs *fs = (struct ramfs *)m->priv;
    struct rnode *n;
    int r = resolve(fs, path, &n);

    if (r < 0) {
        return r;
    }
    if (n == fs->root) {
        return VFS_EBUSY;
    }
    if (n->type == VFS_DIR && n->children) {
        return VFS_ENOTEMPTY;
    }
    struct rnode *parent = n->parent;
    node_detach(n);
    if (parent) {
        parent->mtime = rtc_unix();
    }
    if (n->opens > 0) {
        n->dead = 1;
        n->parent = NULL;
    } else {
        node_free(fs, n);
    }
    return VFS_OK;
}

static int ramfs_rename(struct vfs_mount *m, const char *from, const char *to) {
    struct ramfs *fs = (struct ramfs *)m->priv;
    struct rnode *n;
    struct rnode *np;
    char leaf[VFS_NAME_MAX];
    int r = resolve(fs, from, &n);

    if (r < 0) {
        return r;
    }
    if (n == fs->root) {
        return VFS_EBUSY;
    }
    r = resolve_parent(fs, to, &np, leaf);
    if (r < 0) {
        return r;
    }
    if (lookup(np, leaf)) {
        return VFS_EEXIST;
    }
    for (struct rnode *c = np;; c = c->parent) {
        if (c == n) {
            return VFS_EINVAL;
        }
        if (c == fs->root) {
            break;
        }
    }
    struct rnode *old_parent = n->parent;
    node_detach(n);
    strlcpy(n->name, leaf, VFS_NAME_MAX);
    node_attach(np, n);
    n->mtime = rtc_unix();
    np->mtime = n->mtime;
    if (old_parent) {
        old_parent->mtime = n->mtime;
    }
    return VFS_OK;
}

static int ramfs_chmod(struct vfs_mount *m, const char *path, uint16_t mode) {
    struct rnode *n;
    int r = resolve((struct ramfs *)m->priv, path, &n);

    if (r < 0) {
        return r;
    }
    n->mode = mode & 0777;
    return VFS_OK;
}

static int ramfs_touch(struct vfs_mount *m, const char *path) {
    struct rnode *n;
    int r = resolve((struct ramfs *)m->priv, path, &n);

    if (r < 0) {
        return r;
    }
    n->mtime = rtc_unix();
    return VFS_OK;
}

static int ramfs_statfs(struct vfs_mount *m, struct vfs_statfs *sf) {
    struct ramfs *fs = (struct ramfs *)m->priv;
    uint32_t total, used, largest, blocks;

    heap_stats(&total, &used, &largest, &blocks);
    sf->used = tree_bytes(fs->root);
    sf->avail = total - used;
    sf->total = sf->used + sf->avail;
    sf->files = fs->nodes;
    return VFS_OK;
}

static int ramfs_truncate(struct vfs_file *f, uint64_t size) {
    struct rnode *n = (struct rnode *)f->priv;

    if (!n || n->type == VFS_DIR) {
        return VFS_EISDIR;
    }
    if (size > RAMFS_MAX_FILE) {
        return VFS_EFBIG;
    }
    if (size > n->size) {
        /* Growing zero-fills the gap, as on Linux. */
        if (ensure_capacity(n, (uint32_t)size + 1) < 0) {
            return VFS_ENOSPC;
        }
        memset(n->data + n->size, 0, (size_t)(size - n->size));
    }
    n->size = (uint32_t)size;
    if (n->data) {
        n->data[n->size] = '\0';
    }
    if (f->pos > n->size) {
        f->pos = n->size;
    }
    n->mtime = rtc_unix();
    return 0;
}

const struct fs_ops ramfs_ops = {
    .name = "ramfs",
    .mount = ramfs_mount,
    .umount = ramfs_umount,
    .stat = ramfs_stat,
    .open = ramfs_open,
    .close = ramfs_close,
    .read = ramfs_read,
    .write = ramfs_write,
    .readdir = ramfs_readdir,
    .mkdir = ramfs_mkdir,
    .remove = ramfs_remove,
    .rename = ramfs_rename,
    .chmod = ramfs_chmod,
    .touch = ramfs_touch,
    .statfs = ramfs_statfs,
    .size = ramfs_size,
    .truncate = ramfs_truncate,
};
