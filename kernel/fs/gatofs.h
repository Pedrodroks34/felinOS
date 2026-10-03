#ifndef FELINOS_GATOFS_H
#define FELINOS_GATOFS_H

#include <stdint.h>
#include "drivers/ata.h"

/* GatoFS: disk filesystem. 4 KB blocks, block bitmap, inode table,
 * 12 direct + 1 indirect + 1 double-indirect pointers (files up to 4 GB-1),
 * volumes up to 128 GB (LBA28). Paths are absolute inside the volume. */

#define GATOFS_NAME_MAX 57
#define GATOFS_BLOCK 4096

#define VF_READ   1
#define VF_WRITE  2
#define VF_CREATE 4
#define VF_TRUNC  8
#define VF_APPEND 16

#define GATOFS_FILE 1
#define GATOFS_DIR  2

#define GATOFS_OK        0
#define GATOFS_ENOENT   -1
#define GATOFS_EEXIST   -2
#define GATOFS_ENOTDIR  -3
#define GATOFS_EISDIR   -4
#define GATOFS_ENOSPC   -5
#define GATOFS_EIO      -6
#define GATOFS_EINVAL   -7
#define GATOFS_ENOTEMPTY -8
#define GATOFS_ENOMOUNT -9
#define GATOFS_ENAMETOOLONG -10

struct gatofs_stat {
    uint8_t type;
    uint16_t mode;
    uint32_t size;
    uint32_t mtime;
    uint32_t blocks;   /* 4 KB blocks used, metadata included */
    uint32_t ino;
    uint16_t uid, gid;
};

struct gatofs_info {
    char label[32];
    uint32_t total_blocks, free_blocks, total_inodes, free_inodes;
    char dev[8];
};

typedef int (*gatofs_dir_cb)(const char *name, uint8_t type, uint32_t ino, void *ctx);
typedef void (*gatofs_fsck_cb)(const char *msg, void *ctx);

struct gatofs_fsck_result {
    uint32_t inodes_checked, blocks_checked, errors, fixed;
};

const char *gatofs_strerror(int err);
int gatofs_format(struct ata_device *dev, const char *label);
int gatofs_probe(void);                 /* mounts first GatoFS disk found */
int gatofs_autoformat(void);
int gatofs_mount(struct ata_device *dev);
void gatofs_unmount(void);
int gatofs_mounted(void);
int gatofs_info(struct gatofs_info *out);
void gatofs_sync(void);
int gatofs_fsck(int repair, gatofs_fsck_cb cb, void *ctx, struct gatofs_fsck_result *res);

int gatofs_stat(const char *path, struct gatofs_stat *st);
int gatofs_readdir(const char *path, gatofs_dir_cb cb, void *ctx);
int gatofs_mkdir(const char *path);
int gatofs_mkdirs(const char *path);    /* like mkdir -p */
int gatofs_remove(const char *path, int recursive);
int gatofs_rename(const char *from, const char *to);
int gatofs_chmod(const char *path, uint16_t mode);
int gatofs_chown(const char *path, uint16_t uid, uint16_t gid);
int gatofs_touch(const char *path);

/* file handles for apps */
int gatofs_open(const char *path, int flags);   /* fd >= 0 or error */
int gatofs_open_by_ino(uint32_t ino, int flags);  /* for writeback */
int gatofs_read(int fd, void *buf, uint32_t len);
int gatofs_write(int fd, const void *buf, uint32_t len);
int gatofs_truncate(int fd, uint32_t size);
int gatofs_seek(int fd, uint32_t pos);
uint32_t gatofs_tell(int fd);
uint32_t gatofs_size(int fd);
int gatofs_close(int fd);

/* whole-file helpers: gatofs_load returns a kmalloc buffer (caller kfree) */
int gatofs_load(const char *path, void **buf, uint32_t *size);
int gatofs_save(const char *path, const void *buf, uint32_t size);

void gatofs_fs_lock(void);
void gatofs_fs_unlock(void);

#endif
