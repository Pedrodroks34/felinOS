#ifndef FELINOS_VFS_H
#define FELINOS_VFS_H

#include <stdint.h>
#include <stddef.h>

#define VFS_NAME_MAX 64
#define VFS_PATH_MAX 512
#define VFS_MOUNT_MAX 16
#define VFS_SOURCE_MAX 32
#define VFS_LOAD_MAX (16u * 1024u * 1024u)

#define VFS_FILE 1
#define VFS_DIR  2
#define VFS_CHR  3
#define VFS_BLK  4

#define VFS_O_READ   0x01
#define VFS_O_WRITE  0x02
#define VFS_O_CREATE 0x04
#define VFS_O_TRUNC  0x08
#define VFS_O_APPEND 0x10

#define VFS_CHOWN_KEEP 0xFFFFu

#define VFS_OK            0
#define VFS_ENOENT       (-1)
#define VFS_EEXIST       (-2)
#define VFS_ENOTDIR      (-3)
#define VFS_EISDIR       (-4)
#define VFS_ENOSPC       (-5)
#define VFS_EIO          (-6)
#define VFS_EINVAL       (-7)
#define VFS_ENOTEMPTY    (-8)
#define VFS_ENODEV       (-9)
#define VFS_ENAMETOOLONG (-10)
#define VFS_EBUSY        (-11)
#define VFS_EPERM        (-12)
#define VFS_EXDEV        (-13)
#define VFS_ENOSYS       (-14)
#define VFS_ENOMEM       (-15)
#define VFS_EFBIG        (-16)
#define VFS_EACCES       (-17)
#define VFS_EFAULT       (-18)

/* fcntl() commands Gato understands. */
#define F_GETFL   3
#define F_SETFL   4
#define F_SETSIZE 5

struct vfs_stat {
    uint8_t type;
    uint16_t mode;
    uint64_t size;
    uint32_t mtime;
    uint32_t ino;
    uint16_t uid;
    uint16_t gid;
};

struct vfs_statfs {
    uint64_t total;
    uint64_t used;
    uint64_t avail;
    uint32_t files;
};

struct vfs_dirent {
    char name[VFS_NAME_MAX];
    uint8_t type;
};

struct vfs_mount_info {
    char path[VFS_PATH_MAX];
    char source[VFS_SOURCE_MAX];
    const char *type;
    int open_files;
};

typedef int (*vfs_dir_cb)(const char *name, uint8_t type, void *ctx);

struct vfs_mount;
struct vfs_file;

struct fs_ops {
    const char *name;
    int (*mount)(struct vfs_mount *m, const char *source);
    void (*umount)(struct vfs_mount *m);
    int (*stat)(struct vfs_mount *m, const char *path, struct vfs_stat *st);
    int (*open)(struct vfs_mount *m, const char *path, int flags, struct vfs_file *f);
    void (*close)(struct vfs_file *f);
    int (*read)(struct vfs_file *f, void *buf, uint32_t len);
    int (*write)(struct vfs_file *f, const void *buf, uint32_t len);
    int (*readdir)(struct vfs_mount *m, const char *path, vfs_dir_cb cb, void *ctx);
    int (*mkdir)(struct vfs_mount *m, const char *path);
    int (*remove)(struct vfs_mount *m, const char *path);
    int (*rename)(struct vfs_mount *m, const char *from, const char *to);
    int (*chmod)(struct vfs_mount *m, const char *path, uint16_t mode);
    int (*chown)(struct vfs_mount *m, const char *path, uint16_t uid, uint16_t gid);
    int (*touch)(struct vfs_mount *m, const char *path);
    int (*statfs)(struct vfs_mount *m, struct vfs_statfs *sf);
    int (*sync)(struct vfs_mount *m);
    int (*size)(struct vfs_file *f, uint64_t *out);
    int (*fcntl)(struct vfs_file *f, int cmd, uint32_t arg);
    int (*ioctl)(struct vfs_file *f, uint32_t request, uint32_t arg);
    int (*fchdir)(struct vfs_file *f);
    int (*link)(struct vfs_mount *m, const char *oldpath, const char *newpath);
    int (*symlink)(struct vfs_mount *m, const char *target, const char *linkpath);
    int (*readlink)(struct vfs_mount *m, const char *path, char *buf, uint32_t bufsiz);
    int (*fchmod)(struct vfs_file *f, uint16_t mode);
    int (*fchown)(struct vfs_file *f, uint16_t uid, uint16_t gid);
    int (*lchown)(struct vfs_mount *m, const char *path, uint16_t uid, uint16_t gid);
    int (*readdir_fd)(struct vfs_file *f, char *buf, uint32_t count);
    /* Resizes an open file to `size`, zero-filling a gap and releasing
     * whatever the tail occupied. Optional: NULL means "not supported here",
     * which vfs_truncate() reports as an error. */
    int (*truncate)(struct vfs_file *f, uint64_t size);
};

struct vfs_mount {
    int used;
    char path[VFS_PATH_MAX];
    char source[VFS_SOURCE_MAX];
    const struct fs_ops *ops;
    void *priv;
    int open_files;
};

struct vfs_file {
    struct vfs_mount *mnt;
    void *priv;
    uint64_t pos;
    int flags;
    int refs;
    uint32_t ino;
};

extern const struct fs_ops ramfs_ops;
extern const struct fs_ops gatofs_ops;
extern const struct fs_ops devfs_ops;
extern const struct fs_ops procfs_ops;

void vfs_init(void);
int vfs_mount(const char *type, const char *source, const char *target);
int vfs_umount(const char *target);
int vfs_mount_count(void);
int vfs_mount_info(int index, struct vfs_mount_info *out);
int vfs_mount_of(const char *type, struct vfs_mount_info *out);
int vfs_fs_count(void);
const char *vfs_fs_name(int index);

int vfs_normalize(const char *path, char *out, size_t size);
const char *vfs_getcwd(void);
int vfs_chdir(const char *path);
const char *vfs_fs_of(const char *path);
int vfs_within(const char *dir, const char *path);
int vfs_join(char *out, size_t size, const char *dir, const char *name);
const char *vfs_basename(const char *path);

int vfs_stat(const char *path, struct vfs_stat *st);
int vfs_open(const char *path, int flags, struct vfs_file **out);
int vfs_read(struct vfs_file *f, void *buf, uint32_t len);
int vfs_write(struct vfs_file *f, const void *buf, uint32_t len);
void vfs_seek(struct vfs_file *f, uint64_t pos);
void vfs_file_ref(struct vfs_file *f);
int vfs_size(struct vfs_file *f, uint64_t *out);
void vfs_close(struct vfs_file *f);

int vfs_readdir(const char *path, vfs_dir_cb cb, void *ctx);
int vfs_list(const char *path, struct vfs_dirent **out, int *count);
int vfs_mkdir(const char *path);
int vfs_mkpath(const char *path);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_remove_tree(const char *path);
int vfs_rename(const char *from, const char *to);
int vfs_chmod(const char *path, uint16_t mode);
int vfs_chown(const char *path, uint16_t uid, uint16_t gid);
int vfs_touch(const char *path);
int vfs_statfs(const char *path, struct vfs_statfs *sf);
int vfs_sync(void);
int vfs_sync_file(struct vfs_file *f);
int vfs_ftruncate(struct vfs_file *f, uint64_t size);
int vfs_truncate(const char *path, uint64_t size);
int vfs_fcntl(struct vfs_file *f, int cmd, uint32_t arg);
int vfs_ioctl(struct vfs_file *f, uint32_t request, uint32_t arg);

/* Writeback support: open file by inode for internal writeback */
struct vfs_file *vfs_open_by_ino(struct vfs_mount *m, uint32_t ino, int flags);
int vfs_getcwd_buf(char *buf, uint32_t size);
int vfs_fchdir(struct vfs_file *f);
int vfs_link(const char *oldpath, const char *newpath);
int vfs_symlink(const char *target, const char *linkpath);
int vfs_readlink(const char *path, char *buf, uint32_t bufsiz);
int vfs_fchmod(struct vfs_file *f, uint16_t mode);
int vfs_fchown(struct vfs_file *f, uint16_t uid, uint16_t gid);
int vfs_lchown(const char *path, uint16_t uid, uint16_t gid);
int vfs_readdir_fd(struct vfs_file *f, char *buf, uint32_t count);

int vfs_load(const char *path, void **buf, uint32_t *size);
int vfs_save(const char *path, const void *buf, uint32_t size);
int vfs_append(const char *path, const void *buf, uint32_t size);
int vfs_copy_file(const char *src, const char *dst);
int vfs_copy_tree(const char *src, const char *dst);
uint64_t vfs_tree_size(const char *path);
uint32_t vfs_count_nodes(const char *path);

int vfs_root_is_empty(void);
void vfs_populate_defaults(void);
const char *vfs_strerror(int err);

#endif
