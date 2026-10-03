#include "mm/pagecache.h"
#include "mm/writeback.h"
#include "paging.h"
#include "pmm.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "sync.h"
#include "fs/vfs.h"

#define PC_HASH_SIZE 64

static struct page *pc_hash[PC_HASH_SIZE];
static mutex_t pc_mtx = MUTEX_INIT("pagecache");
static int pc_inited;

static void pc_init(void) {
    if (!pc_inited) {
        memset(pc_hash, 0, sizeof(pc_hash));
        pc_inited = 1;
    }
}

static uint32_t pc_hash_val(struct vfs_mount *m, uint32_t ino, uint32_t index) {
    return (m == NULL ? 0u : (uint32_t)(uintptr_t)m) + ino * 131u + index * 17u;
}

static struct page *pc_find(struct vfs_file *f, uint32_t index) {
    uint32_t h = pc_hash_val(f->mnt, f->ino, index) % PC_HASH_SIZE;

    for (struct page *p = pc_hash[h]; p; p = p->next) {
        if (p->mnt == f->mnt && p->ino == f->ino && p->index == index) {
            return p;
        }
    }
    return NULL;
}

static struct page *pc_get(struct vfs_file *f, uint32_t index, int create) {
    pc_init();
    mutex_lock(&pc_mtx);
    struct page *p = pc_find(f, index);

    if (p) {
        p->refs++;
        mutex_unlock(&pc_mtx);
        return p;
    }
    if (!create) {
        mutex_unlock(&pc_mtx);
        return NULL;
    }
    p = (struct page *)kzalloc(sizeof(*p));
    if (!p) {
        mutex_unlock(&pc_mtx);
        return NULL;
    }
    p->mnt = f->mnt;
    p->ino = f->ino;
    p->index = index;
    p->refs = 1;
    p->phys = pmm_alloc_frame();
    if (!p->phys) {
        kfree(p);
        mutex_unlock(&pc_mtx);
        return NULL;
    }
    memset((void *)p->phys, 0, PAGECACHE_PAGE_SIZE);
    uint32_t h = pc_hash_val(f->mnt, f->ino, index) % PC_HASH_SIZE;

    p->next = pc_hash[h];
    pc_hash[h] = p;
    mutex_unlock(&pc_mtx);
    return p;
}

static void pc_put(struct page *p) {
    if (!p) {
        return;
    }
    mutex_lock(&pc_mtx);
    if (p->refs > 0) {
        p->refs--;
    }
    mutex_unlock(&pc_mtx);
}

static int pc_read_page(struct vfs_file *f, struct page *p) {
    struct vfs_mount *m = f->mnt;
    uint64_t old_pos = f->pos;
    uint32_t off = p->index * PAGECACHE_PAGE_SIZE;

    f->pos = off;
    int n = m->ops->read(f, (void *)p->phys, PAGECACHE_PAGE_SIZE);

    f->pos = old_pos;
    if (n < 0) {
        return n;
    }
    if ((uint32_t)n < PAGECACHE_PAGE_SIZE) {
        memset((void *)p->phys + n, 0, PAGECACHE_PAGE_SIZE - (uint32_t)n);
    }
    p->present = 1;
    return n;
}

static int pc_write_page(struct vfs_file *f, struct page *p, uint32_t off, uint32_t len) {
    struct vfs_mount *m = f->mnt;
    uint64_t old_pos = f->pos;
    uint32_t file_off = p->index * PAGECACHE_PAGE_SIZE + off;

    f->pos = file_off;
    int n = m->ops->write(f, (const void *)p->phys + off, len);

    f->pos = old_pos;
    if (n < 0) {
        return n;
    }
    p->present = 1;
    p->dirty = 1;
    pagecache_mark_dirty(p);
    return n;
}

int pagecache_read(struct vfs_file *f, void *buf, uint32_t len, uint64_t pos) {
    if (!f || !(f->flags & VFS_O_READ) || !f->mnt->ops->read) {
        return VFS_EINVAL;
    }
    uint32_t done = 0;
    uint32_t start = (uint32_t)pos;

    while (len > 0) {
        uint32_t index = start / PAGECACHE_PAGE_SIZE;
        uint32_t off = start % PAGECACHE_PAGE_SIZE;
        struct page *p = pc_get(f, index, 1);

        if (!p) {
            return VFS_ENOMEM;
        }
        if (!p->present) {
            int n = pc_read_page(f, p);

            if (n < 0) {
                pc_put(p);
                return n;
            }
        }
        uint32_t to_copy = PAGECACHE_PAGE_SIZE - off;

        if (to_copy > len) {
            to_copy = len;
        }
        uint64_t size;

        if (f->mnt->ops->size && f->mnt->ops->size(f, &size) == 0) {
            if (pos >= size) {
                pc_put(p);
                break;
            }
            uint32_t avail = (uint32_t)(size - pos);

            if (to_copy > avail) {
                to_copy = avail;
            }
        }
        memcpy((char *)buf + done, (const void *)p->phys + off, to_copy);
        start += to_copy;
        pos += to_copy;
        done += to_copy;
        len -= to_copy;
        pc_put(p);
        if (to_copy == 0) {
            break;
        }
    }
    return (int)done;
}

int pagecache_write(struct vfs_file *f, const void *buf, uint32_t len, uint64_t pos) {
    if (!f || !(f->flags & VFS_O_WRITE) || !f->mnt->ops->write) {
        return VFS_EINVAL;
    }
    uint32_t done = 0;
    uint32_t start = (uint32_t)pos;

    while (len > 0) {
        uint32_t index = start / PAGECACHE_PAGE_SIZE;
        uint32_t off = start % PAGECACHE_PAGE_SIZE;
        struct page *p = pc_get(f, index, 1);

        if (!p) {
            return VFS_ENOMEM;
        }
        if (!p->present) {
            (void)pc_read_page(f, p);
        }
        uint32_t to_copy = PAGECACHE_PAGE_SIZE - off;

        if (to_copy > len) {
            to_copy = len;
        }
        memcpy((void *)p->phys + off, (const char *)buf + done, to_copy);
        int n = pc_write_page(f, p, off, to_copy);

        if (n < 0) {
            pc_put(p);
            return n;
        }
        if ((uint32_t)n != to_copy) {
            pc_put(p);
            return VFS_EIO;
        }
        start += to_copy;
        pos += to_copy;
        done += to_copy;
        len -= to_copy;
        pc_put(p);
    }
    return (int)done;
}

void pagecache_init(void) {
    pc_init();
}

void pagecache_invalidate_mount(struct vfs_mount *m) {
    pc_init();
    mutex_lock(&pc_mtx);
    for (int h = 0; h < PC_HASH_SIZE; h++) {
        struct page *prev = NULL;
        struct page *p = pc_hash[h];

        while (p) {
            if (p->mnt == m) {
                struct page *dead = p;

                p = p->next;
                if (prev) {
                    prev->next = p;
                } else {
                    pc_hash[h] = p;
                }
                pmm_free_frame(dead->phys);
                kfree(dead);
                continue;
            }
            prev = p;
            p = p->next;
        }
    }
    mutex_unlock(&pc_mtx);
}

void pagecache_invalidate(struct vfs_mount *m, uint32_t ino) {
    pc_init();
    mutex_lock(&pc_mtx);
    for (int h = 0; h < PC_HASH_SIZE; h++) {
        struct page *prev = NULL;
        struct page *p = pc_hash[h];

        while (p) {
            if (p->mnt == m && p->ino == ino) {
                struct page *dead = p;

                p = p->next;
                if (prev) {
                    prev->next = p;
                } else {
                    pc_hash[h] = p;
                }
                pmm_free_frame(dead->phys);
                kfree(dead);
                continue;
            }
            prev = p;
            p = p->next;
        }
    }
    mutex_unlock(&pc_mtx);
}

/* ---- Writeback support ---- */

static int writeback_one_page(struct page *p) {
    if (!p->dirty || !p->present) {
        return 0;
    }

    struct vfs_file *f = vfs_open_by_ino(p->mnt, p->ino, VFS_O_WRITE);
    if (!f) {
        return -1;
    }

    f->pos = p->index * PAGECACHE_PAGE_SIZE;
    int n = vfs_write(f, (const void *)p->phys, PAGECACHE_PAGE_SIZE);
    vfs_close(f);

    if (n == PAGECACHE_PAGE_SIZE) {
        p->dirty = 0;
        return 0;
    }
    return -1;
}

int pagecache_count_dirty(void) {
    int count = 0;
    mutex_lock(&pc_mtx);
    for (int h = 0; h < PC_HASH_SIZE; h++) {
        for (struct page *p = pc_hash[h]; p; p = p->next) {
            if (p->dirty) {
                count++;
            }
        }
    }
    mutex_unlock(&pc_mtx);
    return count;
}

int pagecache_writeback_inode(struct vfs_mount *m, uint32_t ino, int sync) {
    int written = 0;
    mutex_lock(&pc_mtx);

    for (int h = 0; h < PC_HASH_SIZE; h++) {
        for (struct page *p = pc_hash[h]; p; p = p->next) {
            if (p->mnt == m && p->ino == ino && p->dirty && p->present) {
                mutex_unlock(&pc_mtx);
                if (writeback_one_page(p) == 0) {
                    written++;
                }
                mutex_lock(&pc_mtx);
                if (!sync && written >= 32) {
                    break;
                }
            }
        }
    }
    mutex_unlock(&pc_mtx);
    return written;
}

int pagecache_sync_mount(struct vfs_mount *m) {
    int written = 0;
    mutex_lock(&pc_mtx);

    for (int h = 0; h < PC_HASH_SIZE; h++) {
        for (struct page *p = pc_hash[h]; p; p = p->next) {
            if (p->mnt == m && p->dirty && p->present) {
                mutex_unlock(&pc_mtx);
                if (writeback_one_page(p) == 0) {
                    written++;
                }
                mutex_lock(&pc_mtx);
            }
        }
    }
    mutex_unlock(&pc_mtx);
    return written;
}

int pagecache_writeback_all(int sync) {
    int written = 0;
    mutex_lock(&pc_mtx);

    for (int h = 0; h < PC_HASH_SIZE; h++) {
        for (struct page *p = pc_hash[h]; p; p = p->next) {
            if (p->dirty && p->present) {
                mutex_unlock(&pc_mtx);
                if (writeback_one_page(p) == 0) {
                    written++;
                }
                mutex_lock(&pc_mtx);
                if (!sync && written >= 64) {
                    break;
                }
            }
        }
    }
    mutex_unlock(&pc_mtx);
    return written;
}

void pagecache_mark_dirty(struct page *page) {
    if (!page || page->dirty) {
        return;
    }
    page->dirty = 1;
    balance_dirty_pages();
}
