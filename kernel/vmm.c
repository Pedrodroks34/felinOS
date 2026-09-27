#include "vmm.h"
#include "pmm.h"
#include "swap.h"
#include "system.h"
#include "lib/heap.h"
#include "lib/format.h"
#include "lib/string.h"
#include "sync.h"

#define VMM_RECLAIM_BATCH 64u
#define VMM_HEAP_RESERVE  (8u * 1024u * 1024u)
#define VMM_HEAP_MIN      (4u * 1024u * 1024u)
#define VMM_HEAP_MAX      (VMM_HEAP_LIMIT - VMM_HEAP_BASE)

static struct vm_region *vmm_find_region_l(struct vm_space *space, uint32_t addr);
static int vmm_region_count_l(struct vm_space *space);
static uint32_t vmm_reclaim_l(uint32_t pages);
static uint32_t vmm_evict_range_l(uint32_t base, uint32_t size, uint32_t pages);
static int vmm_handle_fault_l(uint32_t addr, uint32_t err_code);
static int vmm_commit_l(uint32_t base, uint32_t size);
static int vmm_release_l(uint32_t base, uint32_t size);
static void *vmm_alloc_at_l(uint32_t base, uint32_t size, uint32_t flags, const char *name);
static void *vmm_alloc_l(uint32_t size, uint32_t flags, const char *name);
static void *vmm_map_physical_l(uint32_t phys, uint32_t size, uint32_t flags, const char *name);
static int vmm_free_l(void *ptr);
static struct vm_space *vmm_space_create_l(const char *name);
static struct vm_space *vmm_space_clone_l(const char *name);
static void vmm_space_destroy_l(struct vm_space *space);
static int vmm_swap_in_all_l(void);
static void vmm_get_stats_l(struct vmm_stats *out);
static void vmm_describe_l(uint32_t addr, char *buf, uint32_t size);

static mutex_t vmm_mtx = MUTEX_INIT_RECURSIVE("vmm");
static struct vm_region region_pool[VMM_MAX_REGIONS];
static struct vm_space space_pool[VMM_MAX_SPACES];
static struct vm_space *current_space;
static struct vmm_stats stats;
static int in_reclaim;
static uint32_t heap_base;
static uint32_t heap_bytes;

static uint32_t page_flags(const struct vm_region *region) {
    uint32_t flags = PAGE_PRESENT;

    if (region->flags & VM_WRITE) {
        flags |= PAGE_RW;
    }
    if (region->flags & VM_USER) {
        flags |= PAGE_USER;
    }
    if (region->flags & VM_UNCACHED) {
        flags |= PAGE_PCD | PAGE_PWT;
    }
    if (region->flags & VM_PINNED) {
        flags |= PAGE_PINNED;
    }
    return flags;
}

static struct vm_region *region_alloc(void) {
    for (int i = 0; i < VMM_MAX_REGIONS; i++) {
        if (!region_pool[i].in_use) {
            memset(&region_pool[i], 0, sizeof(struct vm_region));
            region_pool[i].in_use = 1;
            return &region_pool[i];
        }
    }
    return NULL;
}

static void region_release(struct vm_region *region) {
    region->in_use = 0;
    region->next = NULL;
    region->space = NULL;
}

static void region_insert(struct vm_space *space, struct vm_region *region) {
    struct vm_region **link = &space->regions;

    while (*link && (*link)->base < region->base) {
        link = &(*link)->next;
    }
    region->next = *link;
    region->space = space;
    *link = region;
}

static void region_unlink(struct vm_space *space, struct vm_region *region) {
    struct vm_region **link = &space->regions;

    while (*link) {
        if (*link == region) {
            *link = region->next;
            return;
        }
        link = &(*link)->next;
    }
}

static int range_is_free(struct vm_space *space, uint32_t base, uint32_t size) {
    for (struct vm_region *r = space->regions; r; r = r->next) {
        if (base < r->base + r->size && r->base < base + size) {
            return 0;
        }
    }
    return 1;
}

static uint32_t find_hole(struct vm_space *space, uint32_t size, uint32_t low, uint32_t high) {
    uint32_t addr = low;

    for (struct vm_region *r = space->regions; r; r = r->next) {
        if (r->base + r->size <= addr) {
            continue;
        }
        if (r->base >= high) {
            break;
        }
        if (addr + size + PAGE_SIZE <= r->base) {
            return addr;
        }
        addr = r->base + r->size + PAGE_SIZE;
    }
    if (addr + size <= high) {
        return addr;
    }
    return 0;
}

static struct vm_region *region_create(struct vm_space *space, uint32_t base, uint32_t size,
                                       uint32_t flags, uint32_t phys, const char *name) {
    struct vm_region *region = region_alloc();

    if (!region) {
        return NULL;
    }
    region->base = base;
    region->size = size;
    region->flags = flags;
    region->phys = phys;
    region->resident = 0;
    region->swapped = 0;
    strlcpy(region->name, name ? name : "anon", VM_NAME_LEN);
    region_insert(space, region);
    return region;
}

static struct vm_region *vmm_find_region_l(struct vm_space *space, uint32_t addr) {
    if (!space) {
        return NULL;
    }
    for (struct vm_region *r = space->regions; r; r = r->next) {
        if (addr >= r->base && addr < r->base + r->size) {
            return r;
        }
        if (r->base > addr) {
            break;
        }
    }
    return NULL;
}

struct vm_region *vmm_region_list(struct vm_space *space) {
    return space ? space->regions : NULL;
}

static int vmm_region_count_l(struct vm_space *space) {
    int count = 0;

    if (!space) {
        return 0;
    }
    for (struct vm_region *r = space->regions; r; r = r->next) {
        count++;
    }
    return count;
}

static uint32_t acquire_frame(void) {
    uint32_t frame = pmm_alloc_frame();

    if (frame) {
        return frame;
    }
    if (!in_reclaim && swap_active()) {
        vmm_reclaim(VMM_RECLAIM_BATCH);
        frame = pmm_alloc_frame();
    }
    return frame;
}

static int evict_page(struct vm_region *region, uint32_t addr, uint32_t entry) {
    uint32_t slot;

    if (swap_alloc_slot(&slot) != SWAP_OK) {
        return -1;
    }
    uint32_t phys = entry & PAGE_MASK;
    if (swap_write_page(slot, (const void *)phys) != SWAP_OK) {
        swap_free_slot(slot);
        return -1;
    }
    paging_set_entry(addr, PAGE_SWAP_ENTRY(slot));
    pmm_free_frame(phys);
    if (region->resident) {
        region->resident--;
    }
    region->swapped++;
    stats.evictions++;
    stats.swap_outs++;
    return 0;
}

static uint32_t vmm_reclaim_l(uint32_t pages) {
    if (!swap_active() || in_reclaim || pages == 0) {
        return 0;
    }
    in_reclaim = 1;
    stats.reclaims++;

    uint32_t done = 0;
    for (int pass = 0; pass < 2 && done < pages; pass++) {
        for (struct vm_region *r = current_space->regions; r && done < pages; r = r->next) {
            if (!(r->flags & VM_SWAPPABLE) || (r->flags & (VM_PINNED | VM_FIXED | VM_GUARD))) {
                continue;
            }
            for (uint32_t off = 0; off < r->size && done < pages; off += PAGE_SIZE) {
                uint32_t addr = r->base + off;
                uint32_t entry = paging_get_entry(addr);
                if (!(entry & PAGE_PRESENT) || (entry & (PAGE_COW | PAGE_PINNED))) {
                    continue;
                }
                if (pmm_frame_refs(entry & PAGE_MASK) > 1) {
                    continue;
                }
                if (pass == 0 && (entry & PAGE_ACCESSED)) {
                    paging_set_entry(addr, entry & ~PAGE_ACCESSED);
                    continue;
                }
                if (evict_page(r, addr, entry) == 0) {
                    done++;
                }
            }
        }
    }
    in_reclaim = 0;
    return done;
}

static uint32_t vmm_evict_range_l(uint32_t base, uint32_t size, uint32_t pages) {
    if (!swap_active() || in_reclaim || !pages) {
        return 0;
    }
    in_reclaim = 1;

    uint32_t done = 0;
    for (uint32_t off = 0; off < size && done < pages; off += PAGE_SIZE) {
        uint32_t addr = (base & PAGE_MASK) + off;
        struct vm_region *region = vmm_find_region(current_space, addr);
        if (!region || !(region->flags & VM_SWAPPABLE) ||
            (region->flags & (VM_PINNED | VM_FIXED | VM_GUARD))) {
            continue;
        }
        uint32_t entry = paging_get_entry(addr);
        if (!(entry & PAGE_PRESENT) || (entry & (PAGE_COW | PAGE_PINNED))) {
            continue;
        }
        if (pmm_frame_refs(entry & PAGE_MASK) > 1) {
            continue;
        }
        if (evict_page(region, addr, entry) == 0) {
            done++;
        }
    }
    in_reclaim = 0;
    return done;
}

static int map_demand_page(struct vm_region *region, uint32_t addr) {
    uint32_t frame = acquire_frame();

    if (!frame) {
        return -1;
    }
    memset((void *)frame, 0, PAGE_SIZE);
    if (paging_map(addr, frame, page_flags(region)) != 0) {
        pmm_free_frame(frame);
        return -1;
    }
    region->resident++;
    return 0;
}

static int map_swapped_page(struct vm_region *region, uint32_t addr, uint32_t entry) {
    uint32_t slot = PAGE_SWAP_SLOT(entry);
    uint32_t frame = acquire_frame();

    if (!frame) {
        return -1;
    }
    if (swap_read_page(slot, (void *)frame) != SWAP_OK) {
        pmm_free_frame(frame);
        return -1;
    }
    if (paging_map(addr, frame, page_flags(region)) != 0) {
        pmm_free_frame(frame);
        return -1;
    }
    swap_free_slot(slot);
    if (region->swapped) {
        region->swapped--;
    }
    region->resident++;
    stats.swap_ins++;
    return 0;
}

static int copy_on_write(struct vm_region *region, uint32_t addr, uint32_t entry) {
    uint32_t phys = entry & PAGE_MASK;
    uint32_t flags = page_flags(region);

    /* If the page is already COW (shared but copy-on-write), just make it writable.
     * This handles the case where a second write fault occurs on an already-COW page. */
    if (entry & PAGE_COW) {
        if (pmm_frame_refs(phys) <= 1) {
            paging_set_entry(addr, phys | flags);
            return 0;
        }
        /* Still shared but already COW - need to actually copy */
    }

    if (pmm_frame_refs(phys) <= 1) {
        paging_set_entry(addr, phys | flags);
        return 0;
    }
    uint32_t fresh = acquire_frame();
    if (!fresh) {
        return -1;
    }
    memcpy((void *)fresh, (const void *)phys, PAGE_SIZE);
    paging_set_entry(addr, fresh | flags);
    pmm_unref_frame(phys);
    return 0;
}

static int vmm_handle_fault_l(uint32_t addr, uint32_t err_code) {
    stats.page_faults++;

    struct vm_region *region = vmm_find_region(current_space, addr);
    if (!region || (region->flags & VM_GUARD)) {
        stats.invalid_faults++;
        return -1;
    }

    uint32_t page = addr & PAGE_MASK;
    uint32_t entry = paging_get_entry(page);
    int write = (err_code & 0x2u) != 0;
    int user = (err_code & 0x4u) != 0;

    if (user && !(region->flags & VM_USER)) {
        stats.protection_faults++;
        return -1;
    }
    if (write && !(region->flags & VM_WRITE)) {
        stats.protection_faults++;
        return -1;
    }

    if (entry & PAGE_PRESENT) {
        if (write && (entry & PAGE_COW)) {
            if (copy_on_write(region, page, entry) != 0) {
                stats.oom_events++;
                return -1;
            }
            stats.cow_faults++;
            return 0;
        }
        stats.protection_faults++;
        return -1;
    }

    if (PAGE_IS_SWAPPED(entry)) {
        if (map_swapped_page(region, page, entry) != 0) {
            stats.oom_events++;
            return -1;
        }
        stats.swap_faults++;
        return 0;
    }

    if (!(region->flags & VM_DEMAND)) {
        stats.invalid_faults++;
        return -1;
    }

    if (map_demand_page(region, page) != 0) {
        stats.oom_events++;
        return -1;
    }
    stats.demand_faults++;
    return 0;
}

static int vmm_commit_l(uint32_t base, uint32_t size) {
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t done = 0;

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t addr = (base & PAGE_MASK) + i * PAGE_SIZE;
        struct vm_region *region = vmm_find_region(current_space, addr);
        if (!region) {
            return -1;
        }
        uint32_t entry = paging_get_entry(addr);
        if (entry & PAGE_PRESENT) {
            continue;
        }
        if (PAGE_IS_SWAPPED(entry)) {
            if (map_swapped_page(region, addr, entry) != 0) {
                return -1;
            }
        } else if (map_demand_page(region, addr) != 0) {
            return -1;
        }
        done++;
    }
    return (int)done;
}

static int vmm_release_l(uint32_t base, uint32_t size) {
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t done = 0;

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t addr = (base & PAGE_MASK) + i * PAGE_SIZE;
        struct vm_region *region = vmm_find_region(current_space, addr);
        if (!region) {
            continue;
        }
        uint32_t entry = paging_get_entry(addr);
        if (entry & PAGE_PRESENT) {
            paging_unmap(addr);
            pmm_unref_frame(entry & PAGE_MASK);
            if (region->resident) {
                region->resident--;
            }
            done++;
        } else if (PAGE_IS_SWAPPED(entry)) {
            swap_free_slot(PAGE_SWAP_SLOT(entry));
            paging_set_entry(addr, 0);
            if (region->swapped) {
                region->swapped--;
            }
            done++;
        }
    }
    return (int)done;
}

static void *vmm_alloc_at_l(uint32_t base, uint32_t size, uint32_t flags, const char *name) {
    if (!size) {
        return NULL;
    }
    base &= PAGE_MASK;
    size = (size + PAGE_SIZE - 1) & PAGE_MASK;

    if (!range_is_free(current_space, base, size)) {
        return NULL;
    }
    struct vm_region *region = region_create(current_space, base, size, flags, 0, name);
    if (!region) {
        return NULL;
    }
    if (flags & VM_SHARED) {
        paging_reserve_tables(current_space->as, base, size);
    }
    if (!(flags & VM_DEMAND)) {
        if (vmm_commit(base, size) < 0) {
            vmm_release(base, size);
            region_unlink(current_space, region);
            region_release(region);
            return NULL;
        }
    }
    return (void *)base;
}

static void *vmm_alloc_l(uint32_t size, uint32_t flags, const char *name) {
    if (!size) {
        return NULL;
    }
    size = (size + PAGE_SIZE - 1) & PAGE_MASK;
    uint32_t base = find_hole(current_space, size, VMM_VMALLOC_BASE, VMM_VMALLOC_END);
    if (!base) {
        return NULL;
    }
    return vmm_alloc_at(base, size, flags, name);
}

static void *vmm_map_physical_l(uint32_t phys, uint32_t size, uint32_t flags, const char *name) {
    if (!size) {
        return NULL;
    }
    uint32_t offset = phys & 0xFFFu;
    uint32_t aligned = phys & PAGE_MASK;
    uint32_t bytes = (size + offset + PAGE_SIZE - 1) & PAGE_MASK;
    uint32_t base = find_hole(current_space, bytes, VMM_MMIO_BASE, VMM_MMIO_END);

    if (!base) {
        return NULL;
    }
    flags |= VM_FIXED;
    flags &= ~VM_DEMAND;
    struct vm_region *region = region_create(current_space, base, bytes, flags, aligned, name);
    if (!region) {
        return NULL;
    }
    if (paging_map_range(base, aligned, bytes, page_flags(region)) != 0) {
        region_unlink(current_space, region);
        region_release(region);
        return NULL;
    }
    region->resident = bytes / PAGE_SIZE;
    return (void *)(base + offset);
}

static int vmm_free_l(void *ptr) {
    uint32_t addr = (uint32_t)ptr;
    struct vm_region *region = vmm_find_region(current_space, addr);

    if (!region || (region->flags & VM_PINNED)) {
        return -1;
    }
    if (region->flags & VM_FIXED) {
        for (uint32_t off = 0; off < region->size; off += PAGE_SIZE) {
            paging_unmap(region->base + off);
        }
    } else {
        vmm_release(region->base, region->size);
    }
    region_unlink(current_space, region);
    region_release(region);
    return 0;
}

static void region_drop_swap(struct vm_space *space, struct vm_region *region) {
    for (uint32_t off = 0; off < region->size; off += PAGE_SIZE) {
        uint32_t addr = region->base + off;
        uint32_t entry = paging_get_entry_in(space->as, addr);
        if (PAGE_IS_SWAPPED(entry)) {
            swap_free_slot(PAGE_SWAP_SLOT(entry));
            paging_set_entry_in(space->as, addr, 0);
        }
    }
}

static struct vm_space *space_alloc(const char *name) {
    for (int i = 1; i < VMM_MAX_SPACES; i++) {
        if (!space_pool[i].in_use) {
            memset(&space_pool[i], 0, sizeof(struct vm_space));
            space_pool[i].in_use = 1;
            space_pool[i].id = i;
            space_pool[i].as = &space_pool[i].as_storage;
            strlcpy(space_pool[i].name, name ? name : "space", VM_NAME_LEN);
            return &space_pool[i];
        }
    }
    return NULL;
}

static struct vm_space *vmm_space_create_l(const char *name) {
    struct vm_space *space = space_alloc(name);

    if (!space) {
        return NULL;
    }
    if (paging_space_create(space->as, space->id) != 0) {
        space->in_use = 0;
        return NULL;
    }
    for (struct vm_region *r = space_pool[0].regions; r; r = r->next) {
        if (r->flags & (VM_FIXED | VM_SHARED | VM_GUARD)) {
            region_create(space, r->base, r->size, r->flags, r->phys, r->name);
        }
    }
    return space;
}

static struct vm_space *vmm_space_clone_l(const char *name) {
    struct vm_space *space = space_alloc(name);

    if (!space) {
        return NULL;
    }
    if (paging_space_create(space->as, space->id) != 0) {
        space->in_use = 0;
        return NULL;
    }

    for (struct vm_region *r = current_space->regions; r; r = r->next) {
        struct vm_region *copy = region_create(space, r->base, r->size, r->flags, r->phys, r->name);
        if (!copy) {
            continue;
        }
        if (r->flags & (VM_FIXED | VM_SHARED | VM_GUARD)) {
            copy->resident = r->resident;
            copy->swapped = r->swapped;
            continue;
        }
        if (r->swapped) {
            for (uint32_t off = 0; off < r->size; off += PAGE_SIZE) {
                uint32_t entry = paging_get_entry(r->base + off);
                if (PAGE_IS_SWAPPED(entry)) {
                    map_swapped_page(r, r->base + off, entry);
                }
            }
        }
        if (paging_clone_range(space->as, current_space->as, r->base, r->size, 1) != 0) {
            vmm_space_destroy(space);
            return NULL;
        }
        copy->resident = r->resident;
    }
    return space;
}

static void vmm_space_destroy_l(struct vm_space *space) {
    if (!space || !space->in_use || space == &space_pool[0]) {
        return;
    }
    if (space == current_space) {
        vmm_space_switch(&space_pool[0]);
    }
    struct vm_region *r = space->regions;
    while (r) {
        struct vm_region *next = r->next;
        if (!(r->flags & (VM_FIXED | VM_SHARED | VM_GUARD))) {
            region_drop_swap(space, r);
        }
        region_release(r);
        r = next;
    }
    space->regions = NULL;
    paging_space_destroy(space->as);
    space->in_use = 0;
}

void vmm_space_switch(struct vm_space *space) {
    if (!space || !space->in_use) {
        return;
    }
    current_space = space;
    paging_space_switch(space->as);
}

struct vm_space *vmm_kernel_space(void) {
    return &space_pool[0];
}

struct vm_space *vmm_current_space(void) {
    return current_space;
}

int vmm_space_count(void) {
    int count = 0;

    for (int i = 0; i < VMM_MAX_SPACES; i++) {
        if (space_pool[i].in_use) {
            count++;
        }
    }
    return count;
}

struct vm_space *vmm_space_get(int index) {
    if (index < 0 || index >= VMM_MAX_SPACES || !space_pool[index].in_use) {
        return NULL;
    }
    return &space_pool[index];
}

static int vmm_swap_in_all_l(void) {
    int restored = 0;

    for (int i = 0; i < VMM_MAX_SPACES; i++) {
        struct vm_space *space = &space_pool[i];
        if (!space->in_use || space != current_space) {
            continue;
        }
        for (struct vm_region *r = space->regions; r; r = r->next) {
            if (!r->swapped) {
                continue;
            }
            for (uint32_t off = 0; off < r->size; off += PAGE_SIZE) {
                uint32_t addr = r->base + off;
                uint32_t entry = paging_get_entry(addr);
                if (!PAGE_IS_SWAPPED(entry)) {
                    continue;
                }
                if (map_swapped_page(r, addr, entry) != 0) {
                    return -1;
                }
                restored++;
            }
        }
    }
    return restored;
}

uint32_t vmm_available_pages(void) {
    return pmm_free_frames() + swap_free_slots();
}

uint32_t vmm_heap_base(void) {
    return heap_base;
}

uint32_t vmm_heap_size(void) {
    return heap_bytes;
}

static int heap_extend_l(uint32_t bytes, uint32_t *grown) {
    struct vm_region *region = vmm_find_region_l(&space_pool[0], heap_base);

    if (!region || !bytes) {
        return -1;
    }
    bytes &= PAGE_MASK;
    if (heap_base + heap_bytes + bytes > VMM_HEAP_LIMIT) {
        bytes = VMM_HEAP_LIMIT - heap_base - heap_bytes;
    }
    if (!bytes) {
        return -1;
    }
    region->size += bytes;
    heap_bytes += bytes;
    paging_reserve_tables(space_pool[0].as, heap_base, heap_bytes);
    *grown = bytes;
    return 0;
}

static void vmm_get_stats_l(struct vmm_stats *out) {
    if (!out) {
        return;
    }
    *out = stats;
    out->resident_pages = 0;
    out->swapped_pages = 0;
    out->virtual_pages = 0;
    out->regions = 0;
    out->spaces = 0;

    for (int i = 0; i < VMM_MAX_SPACES; i++) {
        if (!space_pool[i].in_use) {
            continue;
        }
        out->spaces++;
        for (struct vm_region *r = space_pool[i].regions; r; r = r->next) {
            out->regions++;
            out->resident_pages += r->resident;
            out->swapped_pages += r->swapped;
            out->virtual_pages += r->size / PAGE_SIZE;
        }
    }
}

const char *vmm_flags_string(uint32_t flags, char *buf, uint32_t size) {
    char tmp[12];
    uint32_t i = 0;

    tmp[i++] = (flags & VM_READ) ? 'r' : '-';
    tmp[i++] = (flags & VM_WRITE) ? 'w' : '-';
    tmp[i++] = (flags & VM_EXEC) ? 'x' : '-';
    tmp[i++] = (flags & VM_USER) ? 'u' : 'k';
    tmp[i++] = (flags & VM_DEMAND) ? 'd' : '-';
    tmp[i++] = (flags & VM_SWAPPABLE) ? 's' : '-';
    tmp[i++] = (flags & VM_SHARED) ? 'S' : '-';
    tmp[i++] = (flags & VM_PINNED) ? 'p' : '-';
    tmp[i++] = (flags & VM_GUARD) ? 'g' : '-';
    tmp[i] = '\0';
    strlcpy(buf, tmp, size);
    return buf;
}

static void vmm_describe_l(uint32_t addr, char *buf, uint32_t size) {
    struct vm_region *region = vmm_find_region(current_space, addr);
    char flags[12];

    if (!region) {
        strlcpy(buf, "no region mapped at this address", size);
        return;
    }
    vmm_flags_string(region->flags, flags, sizeof(flags));
    snprintf(buf, size, "region %s [%08x-%08x] %s", region->name, region->base,
             region->base + region->size, flags);
}

void vmm_init(void) {
    memset(region_pool, 0, sizeof(region_pool));
    memset(space_pool, 0, sizeof(space_pool));
    memset(&stats, 0, sizeof(stats));
    in_reclaim = 0;

    space_pool[0].in_use = 1;
    space_pool[0].id = 0;
    space_pool[0].regions = NULL;
    space_pool[0].as = paging_kernel_space();
    strlcpy(space_pool[0].name, "kernel", VM_NAME_LEN);
    current_space = &space_pool[0];

    struct vm_region *region;

    region_create(current_space, 0, PAGE_SIZE, VM_GUARD, 0, "null");

    region = region_create(current_space, PAGE_SIZE, 0x100000u - PAGE_SIZE,
                           VM_READ | VM_WRITE | VM_FIXED | VM_PINNED, PAGE_SIZE, "lowmem");
    if (region) {
        region->resident = region->size / PAGE_SIZE;
    }

    uint32_t kstart = system_kernel_start();
    uint32_t kend = system_kernel_end();
    region = region_create(current_space, kstart, kend - kstart,
                           VM_READ | VM_WRITE | VM_EXEC | VM_FIXED | VM_PINNED, kstart, "kernel");
    if (region) {
        region->resident = region->size / PAGE_SIZE;
    }

    uint32_t phys_top = paging_identity_top();
    if (phys_top > kend) {
        region = region_create(current_space, kend, phys_top - kend,
                               VM_READ | VM_WRITE | VM_FIXED | VM_PINNED, kend, "physmap");
        if (region) {
            region->resident = region->size / PAGE_SIZE;
        }
    }

    uint32_t available = pmm_free_frames() * PAGE_SIZE;
    uint32_t size = (available > VMM_HEAP_RESERVE) ? available - VMM_HEAP_RESERVE : VMM_HEAP_MIN;
    if (size > VMM_HEAP_MAX) {
        size = VMM_HEAP_MAX;
    }
    if (size < VMM_HEAP_MIN) {
        size = VMM_HEAP_MIN;
    }
    size &= PAGE_MASK;

    heap_base = VMM_HEAP_BASE;
    heap_bytes = size;
    region_create(current_space, heap_base, heap_bytes,
                  VM_READ | VM_WRITE | VM_DEMAND | VM_SWAPPABLE | VM_SHARED, 0, "kheap");
    paging_reserve_tables(space_pool[0].as, heap_base, heap_bytes);
}

/* ---- locking: every entry point takes the (recursive) VMM mutex ---- */
struct vm_region *vmm_find_region(struct vm_space *space, uint32_t addr) {
    mutex_lock(&vmm_mtx);
    struct vm_region *r = vmm_find_region_l(space, addr);
    mutex_unlock(&vmm_mtx);
    return r;
}

int vmm_region_count(struct vm_space *space) {
    mutex_lock(&vmm_mtx);
    int r = vmm_region_count_l(space);
    mutex_unlock(&vmm_mtx);
    return r;
}

uint32_t vmm_reclaim(uint32_t pages) {
    mutex_lock(&vmm_mtx);
    uint32_t r = vmm_reclaim_l(pages);
    mutex_unlock(&vmm_mtx);
    return r;
}

uint32_t vmm_evict_range(uint32_t base, uint32_t size, uint32_t pages) {
    mutex_lock(&vmm_mtx);
    uint32_t r = vmm_evict_range_l(base, size, pages);
    mutex_unlock(&vmm_mtx);
    return r;
}

int vmm_handle_fault(uint32_t addr, uint32_t err_code) {
    mutex_lock(&vmm_mtx);
    int r = vmm_handle_fault_l(addr, err_code);
    mutex_unlock(&vmm_mtx);
    return r;
}

int vmm_commit(uint32_t base, uint32_t size) {
    mutex_lock(&vmm_mtx);
    int r = vmm_commit_l(base, size);
    mutex_unlock(&vmm_mtx);
    return r;
}

int vmm_release(uint32_t base, uint32_t size) {
    mutex_lock(&vmm_mtx);
    int r = vmm_release_l(base, size);
    mutex_unlock(&vmm_mtx);
    return r;
}

void *vmm_alloc_at(uint32_t base, uint32_t size, uint32_t flags, const char *name) {
    mutex_lock(&vmm_mtx);
    void *r = vmm_alloc_at_l(base, size, flags, name);
    mutex_unlock(&vmm_mtx);
    return r;
}

/* Allocates from the first hole that fits in [lo, hi). mmap() uses this to
 * place a mapping inside the user window instead of the kernel's vmalloc
 * range. */
void *vmm_alloc_range(uint32_t lo, uint32_t hi, uint32_t size, uint32_t flags, const char *name) {
    if (!size || lo >= hi) {
        return NULL;
    }
    size = (size + PAGE_SIZE - 1) & PAGE_MASK;
    mutex_lock(&vmm_mtx);
    uint32_t base = find_hole(current_space, size, lo & PAGE_MASK, hi & PAGE_MASK);
    void *r = base ? vmm_alloc_at_l(base, size, flags, name) : NULL;
    mutex_unlock(&vmm_mtx);
    return r;
}

/* Adjusts the permissions of an existing mapping, for mprotect(). The region's
 * behaviour flags and the already-present page table entries are both updated,
 * so pages that were never touched pick the new permissions up when they fault
 * in later. */
int vmm_protect_region(uint32_t base, uint32_t size, uint32_t add_flags, uint32_t clear_flags) {
    if (!size) {
        return 0;
    }
    uint32_t pages = ((size + PAGE_SIZE - 1) & PAGE_MASK) / PAGE_SIZE;
    int changed = 0;

    mutex_lock(&vmm_mtx);
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t addr = (base & PAGE_MASK) + i * PAGE_SIZE;
        struct vm_region *region = vmm_find_region_l(current_space, addr);
        if (!region) {
            continue;
        }
        region->flags = (region->flags | add_flags) & ~clear_flags;

        uint32_t entry = paging_get_entry(addr);
        if (entry & PAGE_PRESENT) {
            entry &= ~(PAGE_RW | PAGE_USER);
            if (region->flags & VM_WRITE) {
                entry |= PAGE_RW;
            }
            if (region->flags & VM_USER) {
                entry |= PAGE_USER;
            }
            paging_set_entry(addr, entry);
            paging_invalidate(addr);
        }
        changed++;
    }
    mutex_unlock(&vmm_mtx);
    return changed;
}

void *vmm_alloc(uint32_t size, uint32_t flags, const char *name) {
    mutex_lock(&vmm_mtx);
    void *r = vmm_alloc_l(size, flags, name);
    mutex_unlock(&vmm_mtx);
    return r;
}

void *vmm_map_physical(uint32_t phys, uint32_t size, uint32_t flags, const char *name) {
    mutex_lock(&vmm_mtx);
    void *r = vmm_map_physical_l(phys, size, flags, name);
    mutex_unlock(&vmm_mtx);
    return r;
}

int vmm_free(void *ptr) {
    mutex_lock(&vmm_mtx);
    int r = vmm_free_l(ptr);
    mutex_unlock(&vmm_mtx);
    return r;
}

struct vm_space *vmm_space_create(const char *name) {
    mutex_lock(&vmm_mtx);
    struct vm_space *r = vmm_space_create_l(name);
    mutex_unlock(&vmm_mtx);
    return r;
}

struct vm_space *vmm_space_clone(const char *name) {
    mutex_lock(&vmm_mtx);
    struct vm_space *r = vmm_space_clone_l(name);
    mutex_unlock(&vmm_mtx);
    return r;
}

void vmm_space_destroy(struct vm_space *space) {
    mutex_lock(&vmm_mtx);
    vmm_space_destroy_l(space);
    mutex_unlock(&vmm_mtx);
}

int vmm_swap_in_all(void) {
    mutex_lock(&vmm_mtx);
    int r = vmm_swap_in_all_l();
    mutex_unlock(&vmm_mtx);
    return r;
}

void vmm_get_stats(struct vmm_stats *out) {
    mutex_lock(&vmm_mtx);
    vmm_get_stats_l(out);
    mutex_unlock(&vmm_mtx);
}

void vmm_describe(uint32_t addr, char *buf, uint32_t size) {
    /* also used by the panic path: never wait for the lock there */
    if (!mutex_trylock(&vmm_mtx)) {
        strlcpy(buf, "(virtual memory manager busy)", size);
        return;
    }
    vmm_describe_l(addr, buf, size);
    mutex_unlock(&vmm_mtx);
}

int vmm_heap_extend(uint32_t bytes) {
    uint32_t grown = 0;

    mutex_lock(&vmm_mtx);
    int r = heap_extend_l(bytes, &grown);
    mutex_unlock(&vmm_mtx);
    if (r == 0) {
        heap_grow(grown);     /* heap mutex is taken after, never inside, the VMM one */
    }
    return r;
}

void vmm_lock(void) {
    mutex_lock(&vmm_mtx);
}

void vmm_unlock(void) {
    mutex_unlock(&vmm_mtx);
}
