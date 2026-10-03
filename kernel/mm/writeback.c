#include "mm/writeback.h"
#include "mm/pagecache.h"
#include "pmm.h"
#include "sched.h"
#include "drivers/pit.h"
#include "lib/string.h"

static struct bdi_writeback global_wb;
static int wb_inited;

static void wb_init(void) {
    if (!wb_inited) {
        memset(&global_wb, 0, sizeof(global_wb));
        wb_inited = 1;
    }
}

void writeback_init(void) {
    wb_init();
    /* Flusher thread will be started on demand */
}

/* Check if we have too many dirty pages and should throttle */
void balance_dirty_pages(void) {
    int dirty = pagecache_count_dirty();
    uint32_t total = pmm_total_frames();
    uint32_t max_dirty = (total * WB_MAX_DIRTY_RATIO) / 100;
    uint32_t bg_dirty = (total * WB_DIRTY_BACKGROUND_RATIO) / 100;

    if ((uint32_t)dirty >= max_dirty) {
        /* Too many dirty pages - force synchronous writeback */
        wb_writeback_all(1);
    } else if ((uint32_t)dirty >= bg_dirty) {
        /* Background writeback threshold reached - wake flusher */
        if (global_wb.flusher_thread && !global_wb.flusher_active) {
            global_wb.flusher_active = 1;
            sched_wake(global_wb.flusher_thread);
        }
    }
}

/* Flusher thread main function */
static void flusher_thread(void *arg) {
    (void)arg;
    while (1) {
        sched_sleep_ticks(500);  /* Wake up every ~5 seconds at 100Hz */

        if (!global_wb.flusher_active) {
            continue;
        }

        int dirty = pagecache_count_dirty();
        if (dirty == 0) {
            global_wb.flusher_active = 0;
            continue;
        }

        uint32_t total = pmm_total_frames();
        uint32_t bg_dirty = (total * WB_DIRTY_BACKGROUND_RATIO) / 100;

        if ((uint32_t)dirty > bg_dirty) {
            pagecache_writeback_all(0);
        }

        dirty = pagecache_count_dirty();
        if (dirty == 0) {
            global_wb.flusher_active = 0;
        }
    }
}

/* Start the flusher thread if not already running */
static void wb_start_flusher(void) {
    if (!global_wb.flusher_thread) {
        global_wb.flusher_thread = kthread_create("flusher", flusher_thread, NULL);
        if (global_wb.flusher_thread) {
            task_start(global_wb.flusher_thread);
        }
    }
}

/* Public API */
int wb_writeback(struct wb_writeback_work *work) {
    if (!work) {
        return pagecache_writeback_all(0);
    }

    if (work->mnt && work->ino) {
        return pagecache_writeback_inode(work->mnt, work->ino, work->sync);
    } else if (work->mnt) {
        return pagecache_sync_mount(work->mnt);
    }
    return pagecache_writeback_all(work->sync);
}

int wb_writeback_inode(struct vfs_mount *m, uint32_t ino, int sync) {
    return pagecache_writeback_inode(m, ino, sync);
}

int wb_sync(struct vfs_mount *m) {
    if (m) {
        return pagecache_sync_mount(m);
    }
    return pagecache_writeback_all(1);
}

int wb_writeback_all(int sync) {
    return pagecache_writeback_all(sync);
}