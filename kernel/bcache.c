#include "bcache.h"
#include "sync.h"
#include "lib/string.h"
#include "lib/heap.h"

#define LINE_SECTORS 8u                            /* 4 KB / 512 */
#define LINE_BYTES   (LINE_SECTORS * ATA_SECTOR_SIZE)
#define BCACHE_LINES 1024u                          /* 4 MB of cached blocks */

struct line {
    struct ata_device *dev;
    uint32_t lba;
    uint32_t last_use;
    int valid;
    uint8_t buf[LINE_BYTES] __attribute__((aligned(16)));
};

static struct line *lines;
static mutex_t cache_mtx = MUTEX_INIT("bcache");
static uint32_t clock_counter;
static struct bcache_stats stats;

void bcache_init(void) {
    lines = (struct line *)kzalloc(sizeof(struct line) * BCACHE_LINES);
    memset(&stats, 0, sizeof(stats));
    stats.lines = lines ? BCACHE_LINES : 0;
}

static uint32_t hash(struct ata_device *dev, uint32_t lba) {
    uint32_t h = (uint32_t)(uintptr_t)dev * 2654435761u;
    h ^= lba * 40503u;
    return (h ^ (h >> 15)) % BCACHE_LINES;
}

/* Fully-associative would be nicer, but a small linear probe past the hashed
   slot gives most of the hit rate at O(1)-ish cost, without a separate index
   structure. */
#define PROBE_DEPTH 8u

static struct line *find(struct ata_device *dev, uint32_t lba) {
    uint32_t start = hash(dev, lba);

    for (uint32_t i = 0; i < PROBE_DEPTH; i++) {
        struct line *l = &lines[(start + i) % BCACHE_LINES];
        if (l->valid && l->dev == dev && l->lba == lba) {
            return l;
        }
    }
    return NULL;
}

static struct line *victim(struct ata_device *dev, uint32_t lba) {
    uint32_t start = hash(dev, lba);
    struct line *best = &lines[start];

    for (uint32_t i = 0; i < PROBE_DEPTH; i++) {
        struct line *l = &lines[(start + i) % BCACHE_LINES];
        if (!l->valid) {
            return l;
        }
        if (l->last_use < best->last_use) {
            best = l;
        }
    }
    if (best->valid) {
        stats.evictions++;
    }
    return best;
}

int bcache_read(struct ata_device *dev, uint32_t lba, uint8_t count, void *buf) {
    if (!lines || count != LINE_SECTORS) {
        stats.bypassed++;
        return ata_read_sectors(dev, lba, count, buf);
    }

    mutex_lock(&cache_mtx);
    struct line *l = find(dev, lba);
    if (l) {
        memcpy(buf, l->buf, LINE_BYTES);
        l->last_use = ++clock_counter;
        stats.hits++;
        mutex_unlock(&cache_mtx);
        return 0;
    }
    mutex_unlock(&cache_mtx);

    /* Miss: read from disk without holding the lock, so one slow transfer
       does not stall every other task's cache hits. */
    int r = ata_read_sectors(dev, lba, count, buf);
    stats.misses++;
    if (r == 0) {
        mutex_lock(&cache_mtx);
        struct line *v = find(dev, lba);      /* someone else may have raced us in */
        if (!v) {
            v = victim(dev, lba);
        }
        v->dev = dev;
        v->lba = lba;
        v->valid = 1;
        v->last_use = ++clock_counter;
        memcpy(v->buf, buf, LINE_BYTES);
        mutex_unlock(&cache_mtx);
    }
    return r;
}

int bcache_write(struct ata_device *dev, uint32_t lba, uint8_t count, const void *buf) {
    int r = ata_write_nf(dev, lba, count, buf);

    if (r != 0 || !lines || count != LINE_SECTORS) {
        if (count != LINE_SECTORS) {
            stats.bypassed++;
        }
        return r;
    }
    mutex_lock(&cache_mtx);
    struct line *l = find(dev, lba);
    if (!l) {
        l = victim(dev, lba);
    }
    l->dev = dev;
    l->lba = lba;
    l->valid = 1;
    l->last_use = ++clock_counter;
    memcpy(l->buf, buf, LINE_BYTES);
    mutex_unlock(&cache_mtx);
    return 0;
}

void bcache_invalidate(struct ata_device *dev) {
    if (!lines) {
        return;
    }
    mutex_lock(&cache_mtx);
    for (uint32_t i = 0; i < BCACHE_LINES; i++) {
        if (lines[i].valid && lines[i].dev == dev) {
            lines[i].valid = 0;
        }
    }
    mutex_unlock(&cache_mtx);
}

void bcache_get_stats(struct bcache_stats *out) {
    mutex_lock(&cache_mtx);
    *out = stats;
    out->valid = 0;
    if (lines) {
        for (uint32_t i = 0; i < BCACHE_LINES; i++) {
            if (lines[i].valid) {
                out->valid++;
            }
        }
    }
    mutex_unlock(&cache_mtx);
}
