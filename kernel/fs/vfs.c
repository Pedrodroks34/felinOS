#include "fs/vfs.h"
#include "sync.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "lib/format.h"
#include "sched.h"

static mutex_t vfs_mtx = MUTEX_INIT_RECURSIVE("vfs");
static int vfs_mount_l(const char *type, const char *source, const char *target);
static int vfs_umount_l(const char *target);
static int vfs_mount_count_l(void);
static int vfs_mount_info_l(int index, struct vfs_mount_info *out);
static int vfs_mount_of_l(const char *type, struct vfs_mount_info *out);
static const char *vfs_fs_of_l(const char *path);
static int vfs_chdir_l(const char *path);
static int vfs_stat_l(const char *path, struct vfs_stat *st);
static int vfs_open_l(const char *path, int flags, struct vfs_file **out);
static int vfs_list_l(const char *path, struct vfs_dirent **out, int *count);
static int vfs_readdir_l(const char *path, vfs_dir_cb cb, void *ctx);
static int vfs_mkdir_l(const char *path);
static int vfs_unlink_l(const char *path);
static int vfs_rmdir_l(const char *path);
static int vfs_rename_l(const char *from, const char *to);
static int vfs_chmod_l(const char *path, uint16_t mode);
static int vfs_chown_l(const char *path, uint16_t uid, uint16_t gid);
static int vfs_touch_l(const char *path);
static int vfs_statfs_l(const char *path, struct vfs_statfs *sf);
static int vfs_sync_l(void);

static struct vfs_mount mounts[VFS_MOUNT_MAX];
static char cwd[VFS_PATH_MAX] = "/";

static const struct fs_ops *const fs_types[] = {
    &ramfs_ops,
    &gatofs_ops,
    &devfs_ops,
    &procfs_ops,
};

#define FS_TYPE_COUNT ((int)(sizeof(fs_types) / sizeof(fs_types[0])))

const char *vfs_strerror(int err) {
    switch (err) {
    case VFS_OK:           return "success";
    case VFS_ENOENT:       return "no such file or directory";
    case VFS_EEXIST:       return "file exists";
    case VFS_ENOTDIR:      return "not a directory";
    case VFS_EISDIR:       return "is a directory";
    case VFS_ENOSPC:       return "no space left";
    case VFS_EIO:          return "input/output error";
    case VFS_EINVAL:       return "invalid argument";
    case VFS_ENOTEMPTY:    return "directory not empty";
    case VFS_ENODEV:       return "no such device";
    case VFS_ENAMETOOLONG: return "name too long";
    case VFS_EBUSY:        return "device or resource busy";
    case VFS_EPERM:        return "operation not permitted";
    case VFS_EXDEV:        return "cross-device link";
    case VFS_ENOSYS:       return "operation not supported";
    case VFS_ENOMEM:       return "out of memory";
    case VFS_EFBIG:        return "file too large";
    }
    return "unknown error";
}

void vfs_init(void) {
    memset(mounts, 0, sizeof(mounts));
    strcpy(cwd, "/");
}

int vfs_normalize(const char *path, char *out, size_t size) {
    if (!path || !*path || size < 2) {
        return VFS_EINVAL;
    }

    size_t len;
    if (path[0] == '/') {
        out[0] = '/';
        out[1] = '\0';
        len = 1;
    } else {
        if (strlcpy(out, cwd, size) >= size) {
            return VFS_ENAMETOOLONG;
        }
        len = strlen(out);
    }

    const char *p = path;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *start = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t clen = (size_t)(p - start);

        if (clen == 1 && start[0] == '.') {
            continue;
        }
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            while (len > 1 && out[len - 1] != '/') {
                len--;
            }
            if (len > 1) {
                len--;
            }
            out[len] = '\0';
            continue;
        }
        if (clen >= VFS_NAME_MAX || len + 1 + clen >= size) {
            return VFS_ENAMETOOLONG;
        }
        if (len > 1) {
            out[len++] = '/';
        }
        memcpy(out + len, start, clen);
        len += clen;
        out[len] = '\0';
    }
    return VFS_OK;
}

const char *vfs_getcwd(void) {
    return cwd;
}

int vfs_within(const char *dir, const char *path) {
    size_t dlen = strlen(dir);

    if (dlen == 1 && dir[0] == '/') {
        return path[0] == '/';
    }
    return strncmp(path, dir, dlen) == 0 && (path[dlen] == '\0' || path[dlen] == '/');
}

int vfs_join(char *out, size_t size, const char *dir, const char *name) {
    int root = dir[0] == '/' && dir[1] == '\0';
    int n = snprintf(out, size, "%s%s%s", dir, root ? "" : "/", name);

    return (n < 0 || (size_t)n >= size) ? VFS_ENAMETOOLONG : VFS_OK;
}

const char *vfs_basename(const char *path) {
    const char *slash = strrchr(path, '/');

    if (!slash || !slash[1]) {
        return path;
    }
    return slash + 1;
}

static size_t path_push(char *path, size_t len, const char *name) {
    size_t nl = strlen(name);
    size_t need = len + (len > 1 ? 1 : 0) + nl;

    if (need >= VFS_PATH_MAX) {
        return 0;
    }
    if (len > 1) {
        path[len++] = '/';
    }
    memcpy(path + len, name, nl + 1);
    return need;
}

static struct vfs_mount *find_mount(const char *abs, const char **rel) {
    struct vfs_mount *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        struct vfs_mount *m = &mounts[i];
        if (!m->used) {
            continue;
        }
        size_t len = strlen(m->path);
        if (!vfs_within(m->path, abs)) {
            continue;
        }
        if (!best || len >= best_len) {
            best = m;
            best_len = len;
        }
    }
    if (!best) {
        return NULL;
    }
    if (rel) {
        const char *r = abs + (best_len == 1 ? 0 : best_len);
        *rel = *r ? r : "/";
    }
    return best;
}

static int route(const char *path, char *abs, struct vfs_mount **m, const char **rel) {
    int r = vfs_normalize(path, abs, VFS_PATH_MAX);

    if (r < 0) {
        return r;
    }
    *m = find_mount(abs, rel);
    return *m ? VFS_OK : VFS_ENOENT;
}

static const struct fs_ops *find_type(const char *type) {
    for (int i = 0; i < FS_TYPE_COUNT; i++) {
        if (strcmp(fs_types[i]->name, type) == 0) {
            return fs_types[i];
        }
    }
    return NULL;
}

int vfs_fs_count(void) {
    return FS_TYPE_COUNT;
}

const char *vfs_fs_name(int index) {
    return (index >= 0 && index < FS_TYPE_COUNT) ? fs_types[index]->name : NULL;
}

static int vfs_mount_l(const char *type, const char *source, const char *target) {
    const struct fs_ops *ops = find_type(type);
    char abs[VFS_PATH_MAX];
    int r;

    if (!ops) {
        return VFS_ENODEV;
    }
    r = vfs_normalize(target, abs, sizeof(abs));
    if (r < 0) {
        return r;
    }

    struct vfs_mount *slot = NULL;
    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        if (mounts[i].used) {
            if (strcmp(mounts[i].path, abs) == 0) {
                return VFS_EBUSY;
            }
        } else if (!slot) {
            slot = &mounts[i];
        }
    }
    if (!slot) {
        return VFS_ENOSPC;
    }

    if (strcmp(abs, "/") != 0) {
        struct vfs_stat st;
        r = vfs_stat(abs, &st);
        if (r < 0) {
            return r;
        }
        if (st.type != VFS_DIR) {
            return VFS_ENOTDIR;
        }
    }

    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->path, abs, sizeof(slot->path));
    strlcpy(slot->source, (source && *source) ? source : "none", sizeof(slot->source));
    slot->ops = ops;

    if (ops->mount) {
        r = ops->mount(slot, source);
        if (r < 0) {
            memset(slot, 0, sizeof(*slot));
            return r;
        }
    }
    slot->used = 1;
    return VFS_OK;
}

static int vfs_umount_l(const char *target) {
    char abs[VFS_PATH_MAX];
    int r = vfs_normalize(target, abs, sizeof(abs));

    if (r < 0) {
        return r;
    }

    struct vfs_mount *m = NULL;
    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        if (mounts[i].used && strcmp(mounts[i].path, abs) == 0) {
            m = &mounts[i];
        }
    }
    if (!m) {
        return VFS_EINVAL;
    }
    if (strcmp(abs, "/") == 0 || m->open_files > 0 || vfs_within(abs, cwd)) {
        return VFS_EBUSY;
    }
    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        if (&mounts[i] != m && mounts[i].used && vfs_within(abs, mounts[i].path)) {
            return VFS_EBUSY;
        }
    }
    if (m->ops->sync) {
        m->ops->sync(m);
    }
    if (m->ops->umount) {
        m->ops->umount(m);
    }
    memset(m, 0, sizeof(*m));
    return VFS_OK;
}

static void fill_info(const struct vfs_mount *m, struct vfs_mount_info *out) {
    strlcpy(out->path, m->path, sizeof(out->path));
    strlcpy(out->source, m->source, sizeof(out->source));
    out->type = m->ops->name;
    out->open_files = m->open_files;
}

static int vfs_mount_count_l(void) {
    int n = 0;

    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        if (mounts[i].used) {
            n++;
        }
    }
    return n;
}

static int vfs_mount_info_l(int index, struct vfs_mount_info *out) {
    int n = 0;

    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        if (!mounts[i].used) {
            continue;
        }
        if (n++ == index) {
            fill_info(&mounts[i], out);
            return VFS_OK;
        }
    }
    return VFS_ENOENT;
}

static int vfs_mount_of_l(const char *type, struct vfs_mount_info *out) {
    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        if (mounts[i].used && strcmp(mounts[i].ops->name, type) == 0) {
            if (out) {
                fill_info(&mounts[i], out);
            }
            return VFS_OK;
        }
    }
    return VFS_ENOENT;
}

static const char *vfs_fs_of_l(const char *path) {
    char abs[VFS_PATH_MAX];
    struct vfs_mount *m;

    if (vfs_normalize(path, abs, sizeof(abs)) < 0) {
        return NULL;
    }
    m = find_mount(abs, NULL);
    return m ? m->ops->name : NULL;
}

static int vfs_chdir_l(const char *path) {
    char abs[VFS_PATH_MAX];
    struct vfs_stat st;
    int r = vfs_normalize(path, abs, sizeof(abs));

    if (r < 0) {
        return r;
    }
    r = vfs_stat(abs, &st);
    if (r < 0) {
        return r;
    }
    if (st.type != VFS_DIR) {
        return VFS_ENOTDIR;
    }
    strlcpy(cwd, abs, sizeof(cwd));
    return VFS_OK;
}

static void path_dirname(const char *abs, char *out, size_t size) {
    strlcpy(out, abs, size);
    char *slash = strrchr(out, '/');

    if (!slash) {
        strlcpy(out, "/", size);
        return;
    }
    if (slash == out) {
        out[1] = 0;
    } else {
        *slash = 0;
    }
}

static int perm_check(const struct vfs_stat *st, int want) {
    struct task *ct = sched_current();
    uint16_t uid = ct ? ct->uid : 0;
    uint16_t gid = ct ? ct->gid : 0;
    int bits;

    if (uid == 0) {
        return 1;
    }
    if (st->uid == uid) {
        bits = (st->mode >> 6) & 7;
    } else if (st->gid == gid) {
        bits = (st->mode >> 3) & 7;
    } else {
        bits = st->mode & 7;
    }
    return (bits & want) == want;
}

static int perm_is_owner(const struct vfs_stat *st) {
    struct task *ct = sched_current();
    uint16_t uid = ct ? ct->uid : 0;

    return uid == 0 || st->uid == uid;
}

static int vfs_stat_l(const char *path, struct vfs_stat *st) {
    char abs[VFS_PATH_MAX];
    struct vfs_mount *m;
    const char *rel;
    int r = route(path, abs, &m, &rel);

    if (r < 0) {
        return r;
    }
    if (!m->ops->stat) {
        return VFS_ENOSYS;
    }
    memset(st, 0, sizeof(*st));
    return m->ops->stat(m, rel, st);
}

static int vfs_open_l(const char *path, int flags, struct vfs_file **out) {
    char abs[VFS_PATH_MAX];
    struct vfs_mount *m;
    const char *rel;
    int r;

    if (!(flags & (VFS_O_READ | VFS_O_WRITE))) {
        return VFS_EINVAL;
    }
    r = route(path, abs, &m, &rel);
    if (r < 0) {
        return r;
    }
    if (!m->ops->open) {
        return VFS_ENOSYS;
    }

    int want = 0;
    if (flags & VFS_O_READ) {
        want |= 4;
    }
    if (flags & VFS_O_WRITE) {
        want |= 2;
    }

    struct vfs_stat st;
    int has_stat = m->ops->stat && m->ops->stat(m, rel, &st) == 0;

    if (has_stat) {
        if (!perm_check(&st, want)) {
            return VFS_EPERM;
        }
    } else if (flags & VFS_O_CREATE) {
        char parent[VFS_PATH_MAX];
        char pabs[VFS_PATH_MAX];
        struct vfs_mount *pm;
        const char *prel;

        path_dirname(abs, parent, sizeof(parent));
        if (route(parent, pabs, &pm, &prel) == 0 && pm->ops->stat) {
            struct vfs_stat pst;
            if (pm->ops->stat(pm, prel, &pst) == 0 && !perm_check(&pst, 2)) {
                return VFS_EPERM;
            }
        }
    }

    struct vfs_file *f = (struct vfs_file *)kzalloc(sizeof(struct vfs_file));
    if (!f) {
        return VFS_ENOMEM;
    }
    f->mnt = m;
    f->flags = flags;
    f->refs = 1;
    r = m->ops->open(m, rel, flags, f);
    if (r < 0) {
        kfree(f);
        return r;
    }
    __atomic_add_fetch(&m->open_files, 1, __ATOMIC_SEQ_CST);
    *out = f;
    return VFS_OK;
}

int vfs_read(struct vfs_file *f, void *buf, uint32_t len) {
    if (!f || !(f->flags & VFS_O_READ)) {
        return VFS_EINVAL;
    }
    if (!f->mnt->ops->read) {
        return VFS_ENOSYS;
    }
    if (len == 0) {
        return 0;
    }
    return f->mnt->ops->read(f, buf, len);
}

int vfs_write(struct vfs_file *f, const void *buf, uint32_t len) {
    if (!f || !(f->flags & VFS_O_WRITE)) {
        return VFS_EINVAL;
    }
    if (!f->mnt->ops->write) {
        return VFS_EPERM;
    }
    if (len == 0) {
        return 0;
    }
    return f->mnt->ops->write(f, buf, len);
}

void vfs_seek(struct vfs_file *f, uint64_t pos) {
    f->pos = pos;
}

int vfs_size(struct vfs_file *f, uint64_t *out) {
    if (!f || !out) {
        return VFS_EINVAL;
    }
    if (!f->mnt->ops->size) {
        return VFS_ENOSYS;
    }
    return f->mnt->ops->size(f, out);
}

void vfs_close(struct vfs_file *f) {
    if (!f) {
        return;
    }
    if (__atomic_sub_fetch(&f->refs, 1, __ATOMIC_SEQ_CST) > 0) {
        return;
    }
    if (f->mnt->ops->close) {
        f->mnt->ops->close(f);
    }
    if (f->mnt->open_files > 0) {
        __atomic_sub_fetch(&f->mnt->open_files, 1, __ATOMIC_SEQ_CST);
    }
    kfree(f);
}

struct gather {
    struct vfs_dirent *list;
    int count;
    int cap;
    int err;
};

static int gather_add(struct gather *g, const char *name, uint8_t type) {
    if (g->count == g->cap) {
        int cap = g->cap ? g->cap * 2 : 32;
        struct vfs_dirent *grown = (struct vfs_dirent *)krealloc(g->list, sizeof(struct vfs_dirent) * (size_t)cap);
        if (!grown) {
            g->err = VFS_ENOMEM;
            return -1;
        }
        g->list = grown;
        g->cap = cap;
    }
    strlcpy(g->list[g->count].name, name, VFS_NAME_MAX);
    g->list[g->count].type = type;
    g->count++;
    return 0;
}

static int gather_cb(const char *name, uint8_t type, void *ctx) {
    return gather_add((struct gather *)ctx, name, type);
}

static void merge_mounts(const char *dir, struct gather *g) {
    size_t dlen = strlen(dir);

    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        struct vfs_mount *m = &mounts[i];
        if (!m->used || m->path[1] == '\0') {
            continue;
        }
        const char *slash = strrchr(m->path, '/');
        size_t plen = (size_t)(slash - m->path);
        int match = (plen == 0) ? (dlen == 1) : (plen == dlen && strncmp(m->path, dir, plen) == 0);
        if (!match) {
            continue;
        }
        int found = 0;
        for (int j = 0; j < g->count; j++) {
            if (strcmp(g->list[j].name, slash + 1) == 0) {
                found = 1;
                break;
            }
        }
        if (!found && gather_add(g, slash + 1, VFS_DIR) < 0) {
            return;
        }
    }
}

static int vfs_list_l(const char *path, struct vfs_dirent **out, int *count) {
    char abs[VFS_PATH_MAX];
    struct vfs_mount *m;
    const char *rel;
    struct gather g = { NULL, 0, 0, 0 };
    int r = route(path, abs, &m, &rel);

    if (r < 0) {
        return r;
    }
    if (!m->ops->readdir) {
        return VFS_ENOSYS;
    }
    r = m->ops->readdir(m, rel, gather_cb, &g);
    if (r < 0 || g.err) {
        kfree(g.list);
        return r < 0 ? r : g.err;
    }
    merge_mounts(abs, &g);
    if (g.err) {
        kfree(g.list);
        return g.err;
    }

    for (int i = 1; i < g.count; i++) {
        struct vfs_dirent key = g.list[i];
        int j = i - 1;
        while (j >= 0 && strcmp(g.list[j].name, key.name) > 0) {
            g.list[j + 1] = g.list[j];
            j--;
        }
        g.list[j + 1] = key;
    }
    *out = g.list;
    *count = g.count;
    return VFS_OK;
}

static int vfs_readdir_l(const char *path, vfs_dir_cb cb, void *ctx) {
    struct vfs_dirent *list = NULL;
    int count = 0;
    int r = vfs_list(path, &list, &count);

    if (r < 0) {
        return r;
    }
    for (int i = 0; i < count; i++) {
        if (cb(list[i].name, list[i].type, ctx)) {
            break;
        }
    }
    kfree(list);
    return VFS_OK;
}

static int vfs_mkdir_l(const char *path) {
    char abs[VFS_PATH_MAX];
    struct vfs_mount *m;
    const char *rel;
    int r = route(path, abs, &m, &rel);

    if (r < 0) {
        return r;
    }
    if (strcmp(rel, "/") == 0) {
        return VFS_EEXIST;
    }
    if (!m->ops->mkdir) {
        return VFS_EPERM;
    }
    return m->ops->mkdir(m, rel);
}

int vfs_mkpath(const char *path) {
    char abs[VFS_PATH_MAX];
    char cur[VFS_PATH_MAX];
    int r = vfs_normalize(path, abs, sizeof(abs));

    if (r < 0) {
        return r;
    }
    const char *p = abs + 1;
    while (*p) {
        const char *end = p;
        while (*end && *end != '/') {
            end++;
        }
        size_t n = (size_t)(end - abs);
        memcpy(cur, abs, n);
        cur[n] = '\0';

        struct vfs_stat st;
        r = vfs_stat(cur, &st);
        if (r == VFS_ENOENT) {
            r = vfs_mkdir(cur);
            if (r < 0 && r != VFS_EEXIST) {
                return r;
            }
        } else if (r < 0) {
            return r;
        } else if (st.type != VFS_DIR) {
            return VFS_ENOTDIR;
        }
        p = *end ? end + 1 : end;
    }
    return VFS_OK;
}

static int remove_checked(const char *path, int want_dir) {
    char abs[VFS_PATH_MAX];
    struct vfs_mount *m;
    const char *rel;
    struct vfs_stat st;
    int r = route(path, abs, &m, &rel);

    if (r < 0) {
        return r;
    }
    if (strcmp(rel, "/") == 0) {
        return VFS_EBUSY;
    }
    if (!m->ops->stat) {
        return VFS_ENOSYS;
    }
    r = m->ops->stat(m, rel, &st);
    if (r < 0) {
        return r;
    }
    if (want_dir && st.type != VFS_DIR) {
        return VFS_ENOTDIR;
    }
    if (!want_dir && st.type == VFS_DIR) {
        return VFS_EISDIR;
    }
    if (!m->ops->remove) {
        return VFS_EPERM;
    }
    return m->ops->remove(m, rel);
}

static int vfs_unlink_l(const char *path) {
    return remove_checked(path, 0);
}

static int vfs_rmdir_l(const char *path) {
    return remove_checked(path, 1);
}

static int remove_walk(char *path, size_t len) {
    struct vfs_stat st;
    int r = vfs_stat(path, &st);

    if (r < 0) {
        return r;
    }
    if (st.type != VFS_DIR) {
        return vfs_unlink(path);
    }

    struct vfs_dirent *list = NULL;
    int count = 0;
    r = vfs_list(path, &list, &count);
    if (r < 0) {
        return r;
    }
    int first = 0;
    for (int i = 0; i < count; i++) {
        size_t nl = path_push(path, len, list[i].name);
        if (!nl) {
            if (!first) {
                first = VFS_ENAMETOOLONG;
            }
            continue;
        }
        r = remove_walk(path, nl);
        path[len] = '\0';
        if (r < 0 && !first) {
            first = r;
        }
    }
    kfree(list);
    if (first) {
        return first;
    }
    return vfs_rmdir(path);
}

int vfs_remove_tree(const char *path) {
    char abs[VFS_PATH_MAX];
    int r = vfs_normalize(path, abs, sizeof(abs));

    if (r < 0) {
        return r;
    }
    if (strcmp(abs, "/") == 0) {
        return VFS_EBUSY;
    }
    return remove_walk(abs, strlen(abs));
}

static int join_rest(char *out, size_t size, const char *base, const char *rest) {
    int n = snprintf(out, size, "%s%s", strcmp(base, "/") == 0 ? "" : base, rest);

    if (n < 0 || (size_t)n >= size) {
        return VFS_ENAMETOOLONG;
    }
    if (out[0] == '\0') {
        strcpy(out, "/");
    }
    return VFS_OK;
}

static int vfs_rename_l(const char *from, const char *to) {
    char a[VFS_PATH_MAX];
    char b[VFS_PATH_MAX];
    struct vfs_mount *ma;
    struct vfs_mount *mb;
    const char *ra;
    const char *rb;
    int r = route(from, a, &ma, &ra);

    if (r < 0) {
        return r;
    }
    r = route(to, b, &mb, &rb);
    if (r < 0) {
        return r;
    }
    if (strcmp(a, b) == 0) {
        return VFS_OK;
    }
    if (ma != mb) {
        return VFS_EXDEV;
    }
    if (strcmp(ra, "/") == 0 || strcmp(rb, "/") == 0) {
        return VFS_EBUSY;
    }
    if (vfs_within(a, b)) {
        return VFS_EINVAL;
    }
    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        if (mounts[i].used && &mounts[i] != ma && vfs_within(a, mounts[i].path)) {
            return VFS_EBUSY;
        }
    }
    if (!ma->ops->rename) {
        return VFS_EPERM;
    }
    r = ma->ops->rename(ma, ra, rb);
    if (r == 0 && vfs_within(a, cwd)) {
        char moved[VFS_PATH_MAX];
        if (join_rest(moved, sizeof(moved), b, cwd + strlen(a)) == 0) {
            strlcpy(cwd, moved, sizeof(cwd));
        }
    }
    return r;
}

static int vfs_chmod_l(const char *path, uint16_t mode) {
    char abs[VFS_PATH_MAX];
    struct vfs_mount *m;
    const char *rel;
    int r = route(path, abs, &m, &rel);

    if (r < 0) {
        return r;
    }
    if (!m->ops->chmod) {
        return VFS_EPERM;
    }
    struct vfs_stat st;
    if (m->ops->stat && m->ops->stat(m, rel, &st) == 0 && !perm_is_owner(&st)) {
        return VFS_EPERM;
    }
    return m->ops->chmod(m, rel, mode);
}

static int vfs_chown_l(const char *path, uint16_t uid, uint16_t gid) {
    char abs[VFS_PATH_MAX];
    struct vfs_mount *m;
    const char *rel;
    int r = route(path, abs, &m, &rel);

    if (r < 0) {
        return r;
    }
    if (!m->ops->chown) {
        return VFS_EPERM;
    }
    struct vfs_stat st;
    if (m->ops->stat && m->ops->stat(m, rel, &st) == 0 && !perm_is_owner(&st)) {
        return VFS_EPERM;
    }
    return m->ops->chown(m, rel, uid, gid);
}

static int vfs_touch_l(const char *path) {
    char abs[VFS_PATH_MAX];
    struct vfs_mount *m;
    const char *rel;
    int r = route(path, abs, &m, &rel);

    if (r < 0) {
        return r;
    }
    if (!m->ops->touch) {
        return VFS_EPERM;
    }
    return m->ops->touch(m, rel);
}

static int vfs_statfs_l(const char *path, struct vfs_statfs *sf) {
    char abs[VFS_PATH_MAX];
    struct vfs_mount *m;
    const char *rel;
    int r = route(path, abs, &m, &rel);

    memset(sf, 0, sizeof(*sf));
    if (r < 0) {
        return r;
    }
    if (!m->ops->statfs) {
        return VFS_OK;
    }
    return m->ops->statfs(m, sf);
}

static int vfs_sync_l(void) {
    int status = VFS_OK;

    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        if (mounts[i].used && mounts[i].ops->sync) {
            int r = mounts[i].ops->sync(&mounts[i]);
            if (r < 0) {
                status = r;
            }
        }
    }
    return status;
}

int vfs_load(const char *path, void **buf, uint32_t *size) {
    struct vfs_stat st;
    struct vfs_file *f;
    int r = vfs_open(path, VFS_O_READ, &f);

    if (r < 0) {
        return r;
    }
    uint32_t cap = 4096;
    if (vfs_stat(path, &st) == 0 && st.type == VFS_FILE && st.size < VFS_LOAD_MAX) {
        cap = (uint32_t)st.size + 2;
    }
    uint8_t *data = (uint8_t *)kmalloc(cap);
    if (!data) {
        vfs_close(f);
        return VFS_ENOMEM;
    }
    uint32_t used = 0;
    for (;;) {
        uint32_t room = cap - 1 - used;
        if (room == 0) {
            if (cap - 1 >= VFS_LOAD_MAX) {
                kfree(data);
                vfs_close(f);
                return VFS_EFBIG;
            }
            uint32_t ncap = cap * 2;
            uint8_t *grown = (uint8_t *)krealloc(data, ncap);
            if (!grown) {
                kfree(data);
                vfs_close(f);
                return VFS_ENOMEM;
            }
            data = grown;
            cap = ncap;
            continue;
        }
        int n = vfs_read(f, data + used, room);
        if (n < 0) {
            kfree(data);
            vfs_close(f);
            return n;
        }
        if (n == 0) {
            break;
        }
        used += (uint32_t)n;
    }
    vfs_close(f);
    data[used] = '\0';
    *buf = data;
    *size = used;
    return VFS_OK;
}

static int write_all(struct vfs_file *f, const void *buf, uint32_t size) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t done = 0;

    while (done < size) {
        uint32_t chunk = size - done > 65536u ? 65536u : size - done;
        int n = vfs_write(f, p + done, chunk);
        if (n < 0) {
            return n;
        }
        if (n == 0) {
            return VFS_ENOSPC;
        }
        done += (uint32_t)n;
    }
    return VFS_OK;
}

int vfs_save(const char *path, const void *buf, uint32_t size) {
    struct vfs_file *f;
    int r = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC, &f);

    if (r < 0) {
        return r;
    }
    r = write_all(f, buf, size);
    vfs_close(f);
    return r;
}

int vfs_append(const char *path, const void *buf, uint32_t size) {
    struct vfs_file *f;
    int r = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_APPEND, &f);

    if (r < 0) {
        return r;
    }
    r = write_all(f, buf, size);
    vfs_close(f);
    return r;
}

int vfs_copy_file(const char *src, const char *dst) {
    char a[VFS_PATH_MAX];
    char b[VFS_PATH_MAX];
    struct vfs_stat st;
    struct vfs_file *in;
    struct vfs_file *out;
    int r = vfs_normalize(src, a, sizeof(a));

    if (r < 0) {
        return r;
    }
    r = vfs_normalize(dst, b, sizeof(b));
    if (r < 0) {
        return r;
    }
    if (strcmp(a, b) == 0) {
        return VFS_EINVAL;
    }
    r = vfs_stat(a, &st);
    if (r < 0) {
        return r;
    }
    r = vfs_open(a, VFS_O_READ, &in);
    if (r < 0) {
        return r;
    }
    r = vfs_open(b, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC, &out);
    if (r < 0) {
        vfs_close(in);
        return r;
    }

    const uint32_t chunk = 32768u;
    uint8_t *buf = (uint8_t *)kmalloc(chunk);
    if (!buf) {
        vfs_close(in);
        vfs_close(out);
        return VFS_ENOMEM;
    }
    for (;;) {
        int n = vfs_read(in, buf, chunk);
        if (n < 0) {
            r = n;
            break;
        }
        if (n == 0) {
            break;
        }
        r = write_all(out, buf, (uint32_t)n);
        if (r < 0) {
            break;
        }
    }
    kfree(buf);
    vfs_close(in);
    vfs_close(out);
    if (r == 0) {
        vfs_chmod(b, st.mode);
    }
    return r;
}

static int copy_walk(char *src, size_t slen, char *dst, size_t dlen) {
    struct vfs_stat st;
    int r = vfs_stat(src, &st);

    if (r < 0) {
        return r;
    }
    if (st.type != VFS_DIR) {
        return vfs_copy_file(src, dst);
    }

    struct vfs_stat dst_st;
    r = vfs_stat(dst, &dst_st);
    if (r == VFS_ENOENT) {
        r = vfs_mkdir(dst);
        if (r < 0) {
            return r;
        }
        vfs_chmod(dst, st.mode);
    } else if (r < 0) {
        return r;
    } else if (dst_st.type != VFS_DIR) {
        return VFS_ENOTDIR;
    }

    struct vfs_dirent *list = NULL;
    int count = 0;
    r = vfs_list(src, &list, &count);
    if (r < 0) {
        return r;
    }
    int first = 0;
    for (int i = 0; i < count; i++) {
        size_t sl = path_push(src, slen, list[i].name);
        size_t dl = path_push(dst, dlen, list[i].name);
        if (!sl || !dl) {
            src[slen] = '\0';
            dst[dlen] = '\0';
            if (!first) {
                first = VFS_ENAMETOOLONG;
            }
            continue;
        }
        r = copy_walk(src, sl, dst, dl);
        src[slen] = '\0';
        dst[dlen] = '\0';
        if (r < 0 && !first) {
            first = r;
        }
    }
    kfree(list);
    return first;
}

int vfs_copy_tree(const char *src, const char *dst) {
    char a[VFS_PATH_MAX];
    char b[VFS_PATH_MAX];
    int r = vfs_normalize(src, a, sizeof(a));

    if (r < 0) {
        return r;
    }
    r = vfs_normalize(dst, b, sizeof(b));
    if (r < 0) {
        return r;
    }
    if (vfs_within(a, b)) {
        return VFS_EINVAL;
    }
    return copy_walk(a, strlen(a), b, strlen(b));
}

static uint64_t size_walk(char *path, size_t len) {
    struct vfs_stat st;

    if (vfs_stat(path, &st) < 0) {
        return 0;
    }
    if (st.type == VFS_FILE) {
        return st.size;
    }
    if (st.type != VFS_DIR) {
        return 0;
    }

    struct vfs_dirent *list = NULL;
    int count = 0;
    uint64_t total = 0;
    if (vfs_list(path, &list, &count) < 0) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        size_t nl = path_push(path, len, list[i].name);
        if (!nl) {
            continue;
        }
        total += size_walk(path, nl);
        path[len] = '\0';
    }
    kfree(list);
    return total;
}

uint64_t vfs_tree_size(const char *path) {
    char abs[VFS_PATH_MAX];

    if (vfs_normalize(path, abs, sizeof(abs)) < 0) {
        return 0;
    }
    return size_walk(abs, strlen(abs));
}

static uint32_t count_walk(char *path, size_t len) {
    struct vfs_stat st;

    if (vfs_stat(path, &st) < 0) {
        return 0;
    }
    uint32_t total = 1;
    if (st.type != VFS_DIR) {
        return total;
    }

    struct vfs_dirent *list = NULL;
    int count = 0;
    if (vfs_list(path, &list, &count) < 0) {
        return total;
    }
    for (int i = 0; i < count; i++) {
        size_t nl = path_push(path, len, list[i].name);
        if (!nl) {
            continue;
        }
        total += count_walk(path, nl);
        path[len] = '\0';
    }
    kfree(list);
    return total;
}

uint32_t vfs_count_nodes(const char *path) {
    char abs[VFS_PATH_MAX];

    if (vfs_normalize(path, abs, sizeof(abs)) < 0) {
        return 0;
    }
    return count_walk(abs, strlen(abs));
}

static int count_entries_cb(const char *name, uint8_t type, void *ctx) {
    (*(int *)ctx)++;
    return 0;
}

int vfs_root_is_empty(void) {
    struct vfs_mount *root = find_mount("/", NULL);
    int entries = 0;

    if (!root || !root->ops->readdir) {
        return 1;
    }
    if (root->ops->readdir(root, "/", count_entries_cb, &entries) < 0) {
        return 0;
    }
    return entries == 0;
}

void vfs_populate_defaults(void) {
    static const char *const dirs[] = {
        "/bin", "/dev", "/etc", "/home", "/mnt", "/proc", "/root", "/tmp", "/var/log"
    };

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        vfs_mkpath(dirs[i]);
    }

    const char *motd =
        "Welcome to FelinOS running the Gato kernel.\n"
        "Type 'help' for the command list, 'man <command>' for details.\n";
    vfs_save("/etc/motd", motd, (uint32_t)strlen(motd));
    vfs_save("/etc/hostname", "felinos\n", 8);

    const char *release =
        "NAME=FelinOS\n"
        "KERNEL=Gato\n"
        "VERSION=0.2\n"
        "ARCH=x86_64\n";
    vfs_save("/etc/os-release", release, (uint32_t)strlen(release));

    const char *readme =
        "FelinOS builds its file tree from a mount table. / lives on the\n"
        "GatoFS disk when one is found (in RAM when there is none), /dev and\n"
        "/proc are generated by the kernel, and /tmp is a RAM filesystem.\n"
        "\n"
        "First boot: FelinOS formats the largest blank disk by itself.\n"
        "For any other disk: gatofs format <disk> [label]   (see lsblk)\n"
        "\n"
        "Everything written outside /tmp, /dev and /proc is saved to the\n"
        "disk as it happens. Use mount to see the mount table, sync to\n"
        "flush, and treat /dev/null, /dev/zero, /dev/console and /dev/hdX\n"
        "as ordinary files. System information is in /proc:\n"
        "meminfo, uptime, version, cpuinfo, mounts and one directory per\n"
        "process (/proc/<pid>/status).\n";
    vfs_save("/root/README", readme, (uint32_t)strlen(readme));
}

/* ---- locking ----
 * One recursive mutex guards the mount table, the working directory and every
 * path-based operation (open, stat, mkdir, ...). Reads and writes on an open
 * file deliberately do not take it: they may block for as long as a device
 * likes (the console waits for a key) and the backends lock themselves. */
int vfs_mount(const char *type, const char *source, const char *target) {
    mutex_lock(&vfs_mtx);
    int r = vfs_mount_l(type, source, target);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_umount(const char *target) {
    mutex_lock(&vfs_mtx);
    int r = vfs_umount_l(target);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_mount_count(void) {
    mutex_lock(&vfs_mtx);
    int r = vfs_mount_count_l();
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_mount_info(int index, struct vfs_mount_info *out) {
    mutex_lock(&vfs_mtx);
    int r = vfs_mount_info_l(index, out);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_mount_of(const char *type, struct vfs_mount_info *out) {
    mutex_lock(&vfs_mtx);
    int r = vfs_mount_of_l(type, out);
    mutex_unlock(&vfs_mtx);
    return r;
}

const char *vfs_fs_of(const char *path) {
    mutex_lock(&vfs_mtx);
    const char *r = vfs_fs_of_l(path);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_chdir(const char *path) {
    mutex_lock(&vfs_mtx);
    int r = vfs_chdir_l(path);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_stat(const char *path, struct vfs_stat *st) {
    mutex_lock(&vfs_mtx);
    int r = vfs_stat_l(path, st);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_open(const char *path, int flags, struct vfs_file **out) {
    mutex_lock(&vfs_mtx);
    int r = vfs_open_l(path, flags, out);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_list(const char *path, struct vfs_dirent **out, int *count) {
    mutex_lock(&vfs_mtx);
    int r = vfs_list_l(path, out, count);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_readdir(const char *path, vfs_dir_cb cb, void *ctx) {
    mutex_lock(&vfs_mtx);
    int r = vfs_readdir_l(path, cb, ctx);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_mkdir(const char *path) {
    mutex_lock(&vfs_mtx);
    int r = vfs_mkdir_l(path);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_unlink(const char *path) {
    mutex_lock(&vfs_mtx);
    int r = vfs_unlink_l(path);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_rmdir(const char *path) {
    mutex_lock(&vfs_mtx);
    int r = vfs_rmdir_l(path);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_rename(const char *from, const char *to) {
    mutex_lock(&vfs_mtx);
    int r = vfs_rename_l(from, to);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_chmod(const char *path, uint16_t mode) {
    mutex_lock(&vfs_mtx);
    int r = vfs_chmod_l(path, mode);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_chown(const char *path, uint16_t uid, uint16_t gid) {
    mutex_lock(&vfs_mtx);
    int r = vfs_chown_l(path, uid, gid);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_touch(const char *path) {
    mutex_lock(&vfs_mtx);
    int r = vfs_touch_l(path);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_statfs(const char *path, struct vfs_statfs *sf) {
    mutex_lock(&vfs_mtx);
    int r = vfs_statfs_l(path, sf);
    mutex_unlock(&vfs_mtx);
    return r;
}

int vfs_sync(void) {
    mutex_lock(&vfs_mtx);
    int r = vfs_sync_l();
    mutex_unlock(&vfs_mtx);
    return r;
}


void vfs_file_ref(struct vfs_file *f) {
    __atomic_add_fetch(&f->refs, 1, __ATOMIC_SEQ_CST);
}
