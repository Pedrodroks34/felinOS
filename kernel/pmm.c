#include "pmm.h"
#include "system.h"
#include "multiboot.h"
#include "lib/string.h"
#include "sync.h"

#define PMM_MANAGED_BYTES 0x40000000u
#define PMM_MAX_FRAMES    (PMM_MANAGED_BYTES / PMM_FRAME_SIZE)

static spinlock_t pmm_lock = SPINLOCK_INIT("pmm");
static uint32_t frame_bitmap[PMM_MAX_FRAMES / 32];
static uint8_t *frame_refs;
static uint32_t total_frames;
static uint32_t used_frames;
static uint32_t peak_frames;
static uint32_t alloc_count;
static uint32_t free_count;
static uint32_t search_from;
static uint32_t managed_top;

static inline void bitmap_set(uint32_t frame) {
    frame_bitmap[frame / 32] |= (1u << (frame % 32));
}

static inline void bitmap_clear(uint32_t frame) {
    frame_bitmap[frame / 32] &= ~(1u << (frame % 32));
}

static inline int bitmap_test(uint32_t frame) {
    return (frame_bitmap[frame / 32] >> (frame % 32)) & 1u;
}

static void mark_used(uint32_t frame) {
    if (frame >= total_frames || bitmap_test(frame)) {
        return;
    }
    bitmap_set(frame);
    used_frames++;
    if (used_frames > peak_frames) {
        peak_frames = used_frames;
    }
}

static void mark_free(uint32_t frame) {
    if (frame >= total_frames || !bitmap_test(frame)) {
        return;
    }
    bitmap_clear(frame);
    used_frames--;
}

void pmm_reserve_region(uint32_t phys_start, uint32_t phys_end) {
    if (phys_end <= phys_start) {
        return;
    }
    uint32_t first = phys_start / PMM_FRAME_SIZE;
    uint32_t last = (phys_end - 1) / PMM_FRAME_SIZE;
    for (uint32_t f = first; f <= last && f < total_frames; f++) {
        mark_used(f);
        if (frame_refs) {
            frame_refs[f] = 1;
        }
    }
}

static uint32_t find_run(uint32_t frames) {
    uint32_t run = 0;
    uint32_t start = 0;

    for (uint32_t i = 0; i < total_frames; i++) {
        uint32_t f = search_from + i;
        if (f >= total_frames) {
            f -= total_frames;
        }
        if (f == 0) {
            /* a run must be physically contiguous: it cannot continue across
             * the end of memory back to frame 0 */
            run = 0;
        }
        if (bitmap_test(f)) {
            run = 0;
            continue;
        }
        if (run == 0) {
            start = f;
        }
        if (++run == frames) {
            return start;
        }
    }
    return 0;
}

void pmm_init(uint32_t mem_top) {
    if (mem_top > PMM_MANAGED_BYTES) {
        mem_top = PMM_MANAGED_BYTES;
    }
    managed_top = mem_top;
    total_frames = mem_top / PMM_FRAME_SIZE;
    used_frames = 0;
    peak_frames = 0;
    alloc_count = 0;
    free_count = 0;
    search_from = 0;
    frame_refs = NULL;

    memset(frame_bitmap, 0xFF, sizeof(frame_bitmap));
    for (uint32_t f = 0; f < total_frames; f++) {
        bitmap_clear(f);
    }

    pmm_reserve_region(0, 0x100000u);
    pmm_reserve_region(system_kernel_start(), system_kernel_end());

    struct multiboot_info *mbi = system_multiboot();
    if (mbi) {
        pmm_reserve_region((uint32_t)mbi, (uint32_t)mbi + sizeof(struct multiboot_info));
        if ((mbi->flags & (1 << 6)) && mbi->mmap_length) {
            pmm_reserve_region(mbi->mmap_addr, mbi->mmap_addr + mbi->mmap_length);
            uint32_t addr = mbi->mmap_addr;
            uint32_t end = addr + mbi->mmap_length;
            while (addr < end) {
                struct multiboot_mmap_entry *entry = (struct multiboot_mmap_entry *)addr;
                if (entry->type != 1) {
                    uint32_t base = (uint32_t)entry->addr;
                    uint32_t len = (uint32_t)entry->len;
                    pmm_reserve_region(base, base + len);
                }
                addr += entry->size + 4;
            }
        }
    }

    uint32_t refs_frames = (total_frames + 255) / 256;
    uint32_t refs_base = find_run(refs_frames);
    if (refs_base) {
        for (uint32_t i = 0; i < refs_frames; i++) {
            mark_used(refs_base + i);
        }
        frame_refs = (uint8_t *)(refs_base * PMM_FRAME_SIZE);
        memset(frame_refs, 0, total_frames);
        for (uint32_t f = 0; f < total_frames; f++) {
            if (bitmap_test(f)) {
                frame_refs[f] = 1;
            }
        }
    }
}

static uint32_t alloc_frame_l(void) {
    for (uint32_t i = 0; i < total_frames; i++) {
        uint32_t f = search_from + i;
        if (f >= total_frames) {
            f -= total_frames;
        }
        if (!bitmap_test(f)) {
            mark_used(f);
            if (frame_refs) {
                frame_refs[f] = 1;
            }
            search_from = f + 1;
            if (search_from >= total_frames) {
                search_from = 0;
            }
            alloc_count++;
            return f * PMM_FRAME_SIZE;
        }
    }
    return 0;
}

uint32_t pmm_alloc_zeroed(void) {
    uint32_t phys = pmm_alloc_frame();
    if (phys) {
        memset((void *)phys, 0, PMM_FRAME_SIZE);
    }
    return phys;
}

static uint32_t alloc_contiguous_l(uint32_t frames) {
    if (frames == 0) {
        return 0;
    }
    uint32_t base = find_run(frames);
    if (!base) {
        return 0;
    }
    for (uint32_t i = 0; i < frames; i++) {
        mark_used(base + i);
        if (frame_refs) {
            frame_refs[base + i] = 1;
        }
    }
    alloc_count += frames;
    return base * PMM_FRAME_SIZE;
}

static void free_frame_l(uint32_t phys) {
    uint32_t frame = phys / PMM_FRAME_SIZE;
    if (frame >= total_frames || !bitmap_test(frame)) {
        return;
    }
    if (frame_refs) {
        frame_refs[frame] = 0;
    }
    mark_free(frame);
    free_count++;
}

static void ref_frame_l(uint32_t phys) {
    uint32_t frame = phys / PMM_FRAME_SIZE;
    if (frame >= total_frames || !frame_refs) {
        return;
    }
    if (frame_refs[frame] < 255) {
        frame_refs[frame]++;
    }
}

static uint32_t unref_frame_l(uint32_t phys) {
    uint32_t frame = phys / PMM_FRAME_SIZE;
    if (frame >= total_frames) {
        return 0;
    }
    if (!frame_refs) {
        free_frame_l(phys);
        return 0;
    }
    if (frame_refs[frame] > 1) {
        frame_refs[frame]--;
        return frame_refs[frame];
    }
    free_frame_l(phys);
    return 0;
}

uint32_t pmm_frame_refs(uint32_t phys) {
    uint32_t frame = phys / PMM_FRAME_SIZE;
    if (frame >= total_frames || !frame_refs) {
        return 0;
    }
    return frame_refs[frame];
}

uint32_t pmm_total_frames(void) {
    return total_frames;
}

uint32_t pmm_used_frames(void) {
    return used_frames;
}

uint32_t pmm_free_frames(void) {
    return total_frames - used_frames;
}

uint32_t pmm_peak_frames(void) {
    return peak_frames;
}

uint32_t pmm_alloc_count(void) {
    return alloc_count;
}

uint32_t pmm_free_count(void) {
    return free_count;
}

uint32_t pmm_managed_top(void) {
    return managed_top;
}

uint32_t pmm_alloc_frame(void) {
    spin_lock(&pmm_lock);
    uint32_t r = alloc_frame_l();
    spin_unlock(&pmm_lock);
    return r;
}

uint32_t pmm_alloc_contiguous(uint32_t frames) {
    spin_lock(&pmm_lock);
    uint32_t r = alloc_contiguous_l(frames);
    spin_unlock(&pmm_lock);
    return r;
}

void pmm_free_frame(uint32_t phys) {
    spin_lock(&pmm_lock);
    free_frame_l(phys);
    spin_unlock(&pmm_lock);
}

void pmm_ref_frame(uint32_t phys) {
    spin_lock(&pmm_lock);
    ref_frame_l(phys);
    spin_unlock(&pmm_lock);
}

uint32_t pmm_unref_frame(uint32_t phys) {
    spin_lock(&pmm_lock);
    uint32_t r = unref_frame_l(phys);
    spin_unlock(&pmm_lock);
    return r;
}
