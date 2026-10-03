#ifndef FELINOS_HUGE_H
#define FELINOS_HUGE_H

#include <stdint.h>
#include <stddef.h>
#include "paging.h"

/* Huge Pages Support (2MB/1GB)
 *
 * Implements:
 * - 2MB huge pages (PMD level)
 * - 1GB huge pages (PUD level) - future
 * - hugetlbfs filesystem
 * - Transparent Huge Pages (THP) infrastructure
 */

#define HUGE_PAGE_2MB (2 * 1024 * 1024)
#define HUGE_PAGE_1GB (1024 * 1024 * 1024)

#define HUGE_FLAG_2MB  (1u << 0)
#define HUGE_FLAG_1GB  (1u << 1)

struct huge_region {
    uint32_t base;
    uint32_t size;
    uint32_t flags;
    int in_use;
};

#define MAX_HUGE_REGIONS 16

/* hugetlbfs inode flags */
#define HUGETLB_2MB  0x1
#define HUGETLB_1GB  0x2

void huge_init(void);

/* Reserve huge pages at boot */
int huge_reserve_2mb(uint32_t count);
int huge_reserve_1gb(uint32_t count);

/* Map/unmap huge pages */
int huge_map_2mb(uint32_t virt, uint32_t phys, uint32_t flags);
int huge_unmap_2mb(uint32_t virt);

/* Allocate huge page from pool */
uint32_t huge_alloc_page_2mb(void);
void huge_free_page_2mb(uint32_t phys);

/* Transparent Huge Pages (THP) */
int thp_handle_fault(uint32_t addr, uint32_t err_code);
int thp_collapse_pmd(uint32_t addr);
int thp_split_pmd(uint32_t addr);

/* Statistics */
void huge_get_stats(uint64_t *free_2mb, uint64_t *used_2mb, uint64_t *free_1gb, uint64_t *used_1gb);

/* hugetlbfs operations (called from fs) */
int hugetlbfs_fallocate(uint32_t size, uint32_t flags, uint32_t *phys_out);
int hugetlbfs_getattr(uint32_t phys, uint32_t *size_out, uint32_t *flags_out);

#endif