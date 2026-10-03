#ifndef FELINOS_WRITEBACK_H
#define FELINOS_WRITEBACK_H

#include <stdint.h>
#include <stddef.h>
#include "mm/pagecache.h"
#include "fs/vfs.h"

#define WB_MAX_DIRTY_RATIO 20   /* percent of memory that can be dirty */
#define WB_DIRTY_BACKGROUND_RATIO 10  /* start background writeback at this % */

struct wb_writeback_work {
    struct vfs_mount *mnt;
    uint32_t ino;
    uint32_t nr_pages;
    int sync;
    int for_reclaim;
    int for_kupdate;
    int range_start;
    int range_end;
};

struct bdi_writeback {
    struct vfs_mount *mnt;
    struct page *dirty_pages;    /* list of dirty pages */
    uint32_t dirty_count;
    uint32_t writeback_count;
    struct task *flusher_thread;
    int flusher_active;
};

void writeback_init(void);
int wb_writeback(struct wb_writeback_work *work);
int wb_writeback_inode(struct vfs_mount *m, uint32_t ino, int sync);
int wb_sync(struct vfs_mount *m);
int wb_writeback_all(int sync);

/* Called when a page is marked dirty */
void pagecache_mark_dirty(struct page *page);

/* Called to write back dirty pages for a specific inode */
int pagecache_writeback_inode(struct vfs_mount *m, uint32_t ino, int sync);

/* Called to sync all dirty pages on a mount */
int pagecache_sync_mount(struct vfs_mount *m);

/* Called to write back all dirty pages */
int pagecache_writeback_all(int sync);

/* Balance dirty pages - throttle if too many dirty pages */
void balance_dirty_pages(void);

#endif