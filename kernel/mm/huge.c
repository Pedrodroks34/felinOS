#include "mm/huge.h"
#include "pmm.h"
#include "vmm.h"
#include "paging.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "sync.h"

static struct huge_region huge_regions[MAX_HUGE_REGIONS];
static int huge_inited = 0;
static mutex_t huge_mtx = MUTEX_INIT("huge");
static uint32_t huge_pool_2mb = 0;
static uint32_t huge_pool_1gb = 0;
static uint32_t huge_free_2mb = 0;
static uint32_t huge_free_1gb = 0;

/* 2MB huge page pool - linked list of free pages */
static uint32_t *huge_free_list_2mb = NULL;

void huge_init(void) {
    if (huge_inited) return;

    mutex_lock(&huge_mtx);
    memset(huge_regions, 0, sizeof(huge_regions));
    huge_inited = 1;
    mutex_unlock(&huge_mtx);
}

/* Reserve 2MB huge pages from physical memory */
int huge_reserve_2mb(uint32_t count) {
    mutex_lock(&huge_mtx);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t frame = pmm_alloc_frame();
        if (!frame) {
            mutex_unlock(&huge_mtx);
            return -1;  /* Out of memory */
        }

        /* Add to free list */
        uint32_t *entry = (uint32_t *)frame;
        *entry = (uint32_t)huge_free_list_2mb;
        huge_free_list_2mb = entry;
        huge_pool_2mb++;
        huge_free_2mb++;
    }

    mutex_unlock(&huge_mtx);
    return 0;
}

int huge_reserve_1gb(uint32_t count) {
    /* 1GB pages need 1GB contiguous physical memory - very unlikely on 32MB system */
    (void)count;
    return -1;  /* Not supported yet */
}

/* Allocate a 2MB huge page from pool */
uint32_t huge_alloc_page_2mb(void) {
    mutex_lock(&huge_mtx);

    if (!huge_free_list_2mb) {
        mutex_unlock(&huge_mtx);
        return 0;
    }

    uint32_t frame = (uint32_t)huge_free_list_2mb;
    huge_free_list_2mb = (uint32_t *)*huge_free_list_2mb;
    huge_free_2mb--;

    mutex_unlock(&huge_mtx);
    return frame;
}

void huge_free_page_2mb(uint32_t phys) {
    if (!phys) return;

    mutex_lock(&huge_mtx);

    uint32_t *entry = (uint32_t *)phys;
    *entry = (uint32_t)huge_free_list_2mb;
    huge_free_list_2mb = entry;
    huge_free_2mb++;

    mutex_unlock(&huge_mtx);
}

/* Map a 2MB huge page at PMD level */
int huge_map_2mb(uint32_t virt, uint32_t phys, uint32_t flags) {
    /* Use paging to map 2MB page at PMD level */
    /* Flags should include PAGE_PSE (Page Size Extension) for 2MB pages */
    uint32_t pg_flags = flags | 0x80;  /* PAGE_PSE bit */
    return paging_map_range(virt, phys, HUGE_PAGE_2MB, pg_flags);
}

/* Unmap a 2MB huge page */
int huge_unmap_2mb(uint32_t virt) {
    /* Unmap 2MB range */
    struct address_space *as = paging_current_space();
    paging_release_range(as, virt, HUGE_PAGE_2MB);
    return 0;
}

/* THP: Handle page fault by attempting to map 2MB page */
int thp_handle_fault(uint32_t addr, uint32_t err_code) {
    (void)err_code;

    /* Check if address is aligned to 2MB */
    if (addr & (HUGE_PAGE_2MB - 1)) {
        return -1;  /* Not aligned */
    }

    /* Check if we have a free 2MB page */
    uint32_t frame = huge_alloc_page_2mb();
    if (!frame) {
        return -1;  /* No huge pages available */
    }

    /* Map it */
    uint32_t aligned_addr = addr & ~(HUGE_PAGE_2MB - 1);
    int ret = huge_map_2mb(aligned_addr, frame, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    if (ret != 0) {
        huge_free_page_2mb(frame);
        return -1;
    }

    return 0;
}

/* THP: Attempt to collapse adjacent 4KB pages into 2MB page */
int thp_collapse_pmd(uint32_t addr) {
    uint32_t aligned = addr & ~(HUGE_PAGE_2MB - 1);

    /* Check if all 512 PTEs in this PMD are present and contiguous */
    for (uint32_t i = 0; i < 512; i++) {
        uint32_t pte_addr = aligned + i * PAGE_SIZE;
        uint32_t entry = paging_get_entry(pte_addr);
        if (!(entry & PAGE_PRESENT)) {
            return -1;  /* Not all pages present */
        }
        /* Check if physically contiguous */
        if (i == 0) {
            /* First page */
        } else {
            uint32_t prev_entry = paging_get_entry(pte_addr - PAGE_SIZE);
            uint32_t prev_phys = prev_entry & PAGE_MASK;
            uint32_t phys = entry & PAGE_MASK;
            if (phys != prev_phys + PAGE_SIZE) {
                return -1;  /* Not contiguous */
            }
        }
    }

    /* All pages present and contiguous - can collapse */
    /* Unmap all 4KB pages */
    for (uint32_t i = 0; i < 512; i++) {
        uint32_t pte_addr = aligned + i * PAGE_SIZE;
        uint32_t entry = paging_get_entry(pte_addr);
        (void)entry;
        paging_unmap(pte_addr);
        /* Don't free frames - we'll reuse them */
    }

    /* Allocate a 2MB huge page and map it */
    uint32_t huge_frame = huge_alloc_page_2mb();
    if (!huge_frame) {
        /* Fallback: remap 4KB pages */
        return -1;
    }

    /* Copy data from first page to huge page (already in place physically) */
    int ret = huge_map_2mb(aligned, huge_frame, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    if (ret != 0) {
        huge_free_page_2mb(huge_frame);
        return -1;
    }

    return 0;
}

/* THP: Split a 2MB page back into 4KB pages */
int thp_split_pmd(uint32_t addr) {
    uint32_t aligned = addr & ~(HUGE_PAGE_2MB - 1);
    uint32_t entry = paging_get_entry(aligned);

    if (!(entry & PAGE_PRESENT)) return -1;
    if (!(entry & 0x80)) return -1;  /* Not a huge page (no PSE) */

    uint32_t phys = entry & PAGE_MASK;

    /* Unmap huge page */
    huge_unmap_2mb(aligned);

    /* Return huge page to pool */
    huge_free_page_2mb(phys);

    /* Map 512 4KB pages */
    for (uint32_t i = 0; i < 512; i++) {
        uint32_t pte_addr = aligned + i * PAGE_SIZE;
        uint32_t page_phys = phys + i * PAGE_SIZE;
        if (paging_map(pte_addr, page_phys, PAGE_PRESENT | PAGE_RW | PAGE_USER) != 0) {
            return -1;
        }
    }

    return 0;
}

void huge_get_stats(uint64_t *free_2mb, uint64_t *used_2mb, uint64_t *free_1gb, uint64_t *used_1gb) {
    mutex_lock(&huge_mtx);
    if (free_2mb) *free_2mb = huge_free_2mb * HUGE_PAGE_2MB;
    if (used_2mb) *used_2mb = (huge_pool_2mb - huge_free_2mb) * HUGE_PAGE_2MB;
    if (free_1gb) *free_1gb = huge_free_1gb * HUGE_PAGE_1GB;
    if (used_1gb) *used_1gb = (huge_pool_1gb - huge_free_1gb) * HUGE_PAGE_1GB;
    mutex_unlock(&huge_mtx);
}

/* hugetlbfs interface */
int hugetlbfs_fallocate(uint32_t size, uint32_t flags, uint32_t *phys_out) {
    if (!phys_out) return -1;

    if (flags & HUGETLB_2MB) {
        if (size > HUGE_PAGE_2MB) size = HUGE_PAGE_2MB;
        uint32_t frame = huge_alloc_page_2mb();
        if (!frame) return -1;
        *phys_out = frame;
        return 0;
    }
    return -1;
}

int hugetlbfs_getattr(uint32_t phys, uint32_t *size_out, uint32_t *flags_out) {
    (void)phys;
    if (size_out) *size_out = HUGE_PAGE_2MB;
    if (flags_out) *flags_out = HUGETLB_2MB;
    return 0;
}