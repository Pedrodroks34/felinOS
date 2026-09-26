#ifndef FELINOS_BCACHE_H
#define FELINOS_BCACHE_H

#include <stdint.h>
#include "drivers/ata.h"

/*
 * Generic read-caching buffer cache sitting between the filesystem and the
 * ATA driver. One line = one 4 KB block (GatoFS's block size), so GatoFS's
 * dread()/dwrite() map onto exactly one cache line per call.
 *
 * Reads are cached: a hit skips the ATA round trip entirely. Writes are
 * write-through (issued to disk exactly as before, synchronously) and then
 * refresh the matching line, so a write never leaves stale data behind and
 * never changes GatoFS's existing on-disk durability guarantees -- only the
 * write-ahead journal's own explicit ata_flush() calls, unrelated to this
 * cache, decide when a write is durable. Purely a read accelerator.
 */

void bcache_init(void);
int bcache_read(struct ata_device *dev, uint32_t lba, uint8_t count, void *buf);
int bcache_write(struct ata_device *dev, uint32_t lba, uint8_t count, const void *buf);
void bcache_invalidate(struct ata_device *dev);   /* drop all lines for dev: format/mount/unmount */

struct bcache_stats {
    uint32_t lines;
    uint32_t valid;
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
    uint32_t bypassed;    /* requests that didn't align to a single 4 KB line */
};

void bcache_get_stats(struct bcache_stats *out);

#endif
