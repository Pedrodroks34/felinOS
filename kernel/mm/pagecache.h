#ifndef FELINOS_PAGECACHE_H
#define FELINOS_PAGECACHE_H

#include <stdint.h>
#include <stddef.h>
#include "fs/vfs.h"

#define PAGECACHE_PAGE_SIZE 4096u

/* Minimal unified page cache: one entry per (mount, inode, page index). */
struct page {
    struct vfs_mount *mnt;
    uint32_t ino;
    uint32_t index;
    uint32_t phys;
    int refs;
    int dirty;
    uint8_t present;
    struct page *next;
};

void pagecache_init(void);
void pagecache_invalidate(struct vfs_mount *m, uint32_t ino);
void pagecache_invalidate_mount(struct vfs_mount *m);
int pagecache_read(struct vfs_file *f, void *buf, uint32_t len, uint64_t pos);
int pagecache_write(struct vfs_file *f, const void *buf, uint32_t len, uint64_t pos);
void pagecache_mark_dirty(struct page *page);

/* Writeback support */
int pagecache_count_dirty(void);
int pagecache_writeback_inode(struct vfs_mount *m, uint32_t ino, int sync);
int pagecache_sync_mount(struct vfs_mount *m);
int pagecache_writeback_all(int sync);

#endif
