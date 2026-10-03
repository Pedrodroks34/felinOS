#ifndef FELINOS_KSM_H
#define FELINOS_KSM_H

#include <stdint.h>
#include <stddef.h>
#include "mm/pagecache.h"

/* KSM - Kernel Samepage Merging
 *
 * Deduplicates identical memory pages across processes.
 * Uses a stable tree (RB-tree) for known identical pages,
 * and a scan to find new candidates.
 */

#define KSM_MAX_PAGES_PER_SCAN 100
#define KSM_SLEEP_MS 20

/* Stable tree node - represents a page known to be identical */
struct ksm_stable_node {
    struct page *page;              /* Sample page from this cluster */
    uint64_t checksum;              /* Page checksum for quick comparison */
    struct ksm_stable_node *left;
    struct ksm_stable_node *right;
    struct ksm_stable_node *parent;
    uint8_t color;                  /* RB-tree color: 0=red, 1=black */
    uint32_t rmap_count;            /* Number of pages pointing to this */
};

/* Unstable tree node - candidate page being scanned */
struct ksm_unstable_node {
    struct page *page;
    uint64_t checksum;
    struct ksm_unstable_node *left;
    struct ksm_unstable_node *right;
    struct ksm_unstable_node *parent;
    uint8_t color;
};

struct ksm_stats {
    uint64_t pages_scanned;
    uint64_t pages_merged;
    uint64_t pages_volatile;
    uint64_t full_scans;
    uint64_t stable_tree_size;
    uint64_t unstable_tree_size;
};

void ksm_init(void);
void ksm_scan(void);
void ksm_add_page(struct page *page);
void ksm_remove_page(struct page *page);
void ksm_get_stats(struct ksm_stats *out);

/* Run one scan cycle (for testing) */
int ksm_do_scan(void);

/* Background thread control */
void ksm_start_thread(void);
void ksm_stop_thread(void);

#endif