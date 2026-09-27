#include "bcache.h"
#include "sync.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "drivers/ata.h"

#define LINE_SECTORS 8u                            /* 4 KB / 512 */
#define LINE_BYTES   (LINE_SECTORS * ATA_SECTOR_SIZE)
#define BCACHE_LINES 1024u                          /* 4 MB of cached blocks */

/* Larger probe depth for better hit rate with 1024 lines */
#define PROBE_DEPTH 32u

/* Read-ahead: number of extra lines to prefetch on sequential access */
#define READAHEAD_LINES 4u

struct line {
    struct ata_device *dev;
    uint32_t lba;                 /* Starting LBA of this line (8-sector aligned) */
    uint32_t last_use;
    int valid;
    uint8_t buf[LINE_BYTES] __attribute__((aligned(16)));
};

static struct line *lines;
static mutex_t cache_mtx = MUTEX_INIT("bcache");
static uint32_t clock_counter;
static struct bcache_stats stats;

/* Per-device sequential detection state */
struct readahead_state {
    struct ata_device *dev;
    uint32_t last_lba;
    uint32_t streak;
    int active;
};

static struct readahead_state ra_state[ATA_MAX_DEVICES];

static uint32_t hash(struct ata_device *dev, uint32_t lba) {
    uint32_t h = (uint32_t)(uintptr_t)dev * 2654435761u;
    h ^= lba * 40503u;
    return (h ^ (h >> 15)) % BCACHE_LINES;
}

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

/* Try to read a single 8-sector line from cache. Returns 1 if served from cache. */
static int try_cache_line(struct ata_device *dev, uint32_t lba, void *dst) {
    if (!lines) return 0;

    uint32_t aligned_lba = lba & ~(LINE_SECTORS - 1);
    mutex_lock(&cache_mtx);
    struct line *l = find(dev, aligned_lba);
    if (l) {
        uint32_t offset = (lba - aligned_lba) * ATA_SECTOR_SIZE;
        uint32_t bytes = LINE_BYTES - offset;
        memcpy(dst, l->buf + offset, bytes);
        l->last_use = ++clock_counter;
        stats.hits++;
        mutex_unlock(&cache_mtx);
        return 1;
    }
    mutex_unlock(&cache_mtx);
    return 0;
}

/* Fetch a full line into cache (caller holds no lock) */
static int fetch_line(struct ata_device *dev, uint32_t lba) {
    uint32_t aligned_lba = lba & ~(LINE_SECTORS - 1);
    uint8_t tmp[LINE_BYTES];
    int r = ata_read_sectors(dev, aligned_lba, LINE_SECTORS, tmp);
    if (r != 0) return r;

    mutex_lock(&cache_mtx);
    struct line *v = find(dev, aligned_lba);
    if (!v) {
        v = victim(dev, aligned_lba);
    }
    v->dev = dev;
    v->lba = aligned_lba;
    v->valid = 1;
    v->last_use = ++clock_counter;
    memcpy(v->buf, tmp, LINE_BYTES);
    mutex_unlock(&cache_mtx);
    return 0;
}

/* Update sequential read-ahead state and trigger prefetch if warranted */
static void update_readahead(struct ata_device *dev, uint32_t lba) {
    int idx = -1;
    for (int i = 0; i < ATA_MAX_DEVICES; i++) {
        if (ra_state[i].dev == dev) { idx = i; break; }
        if (!ra_state[i].dev && idx < 0) idx = i;
    }
    if (idx < 0) return;

    struct readahead_state *s = &ra_state[idx];
    if (!s->dev) {
        s->dev = dev;
        s->last_lba = lba + LINE_SECTORS;
        s->streak = 1;
        s->active = 0;
        return;
    }

    if (s->dev != dev) {
        s->dev = dev;
        s->last_lba = lba + LINE_SECTORS;
        s->streak = 1;
        s->active = 0;
        return;
    }

    if (lba == s->last_lba) {
        s->streak++;
        s->last_lba += LINE_SECTORS;
        if (s->streak >= 2 && !s->active) {
            s->active = 1;
        }
    } else {
        s->streak = 1;
        s->last_lba = lba + LINE_SECTORS;
        s->active = 0;
    }

    if (s->active) {
        for (uint32_t i = 0; i < READAHEAD_LINES; i++) {
            uint32_t prefetch_lba = s->last_lba + i * LINE_SECTORS;
            if (find(dev, prefetch_lba)) continue;
            fetch_line(dev, prefetch_lba);
        }
        s->last_lba += READAHEAD_LINES * LINE_SECTORS;
    }
}

int bcache_read(struct ata_device *dev, uint32_t lba, uint8_t count, void *buf) {
    if (!lines) {
        stats.bypassed++;
        return ata_read_sectors(dev, lba, count, buf);
    }

    uint8_t *dst = (uint8_t *)buf;
    uint32_t remaining = count;
    uint32_t cur_lba = lba;

    while (remaining > 0) {
        if (try_cache_line(dev, cur_lba, dst)) {
            dst += ATA_SECTOR_SIZE;
            cur_lba++;
            remaining--;
            continue;
        }

        /* Miss: fetch the aligned line containing this sector */
        int r = fetch_line(dev, cur_lba);
        if (r != 0) {
            stats.bypassed++;
            return ata_read_sectors(dev, lba, count, buf);
        }
        stats.misses++;
        /* Loop will retry and hit the newly cached line */
    }

    update_readahead(dev, lba);
    return 0;
}

int bcache_write(struct ata_device *dev, uint32_t lba, uint8_t count, const void *buf) {
    int r = ata_write_nf(dev, lba, count, buf);

    if (r != 0 || !lines) {
        if (count != LINE_SECTORS) stats.bypassed++;
        return r;
    }

    /* Write-through: update cache if we have the line */
    uint32_t aligned_lba = lba & ~(LINE_SECTORS - 1);
    uint32_t end_lba = (lba + count + LINE_SECTORS - 1) & ~(LINE_SECTORS - 1);

    for (uint32_t blk = aligned_lba; blk < end_lba; blk += LINE_SECTORS) {
        mutex_lock(&cache_mtx);
        struct line *l = find(dev, blk);
        if (l) {
            uint32_t offset = (lba > blk) ? (lba - blk) * ATA_SECTOR_SIZE : 0;
            uint32_t bytes = LINE_BYTES - offset;
            uint32_t to_write = count * ATA_SECTOR_SIZE;
            if (bytes > to_write) bytes = to_write;
            memcpy(l->buf + offset, buf, bytes);
            l->last_use = ++clock_counter;
        }
        mutex_unlock(&cache_mtx);
    }
    return 0;
}

void bcache_invalidate(struct ata_device *dev) {
    if (!lines) return;
    mutex_lock(&cache_mtx);
    for (uint32_t i = 0; i < BCACHE_LINES; i++) {
        if (lines[i].valid && lines[i].dev == dev) {
            lines[i].valid = 0;
        }
    }
    /* Reset readahead state */
    for (int j = 0; j < ATA_MAX_DEVICES; j++) {
        if (ra_state[j].dev == dev) {
            ra_state[j].dev = NULL;
            ra_state[j].streak = 0;
            ra_state[j].active = 0;
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
            if (lines[i].valid) out->valid++;
        }
    }
    mutex_unlock(&cache_mtx);
}

void bcache_init(void) {
    lines = (struct line *)kzalloc(sizeof(struct line) * BCACHE_LINES);
    memset(&stats, 0, sizeof(stats));
    stats.lines = lines ? BCACHE_LINES : 0;
    memset(ra_state, 0, sizeof(ra_state));
}