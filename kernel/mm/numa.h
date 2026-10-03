#ifndef FELINOS_NUMA_H
#define FELINOS_NUMA_H

#include <stdint.h>
#include <stddef.h>

/* NUMA (Non-Uniform Memory Access) support
 *
 * FelinOS NUMA implementation:
 * - Node distance table
 * - Per-node memory stats
 * - Node-aware allocation policies
 * - mbind() / set_mempolicy() syscalls
 */

#define MAX_NUMNODES 8
#define NUMA_NO_NODE (-1)
#define NUMA_DISTANCE_LOCAL 10
#define NUMA_DISTANCE_REMOTE 20

enum numa_policy {
    MPOL_DEFAULT = 0,    /* Use system default */
    MPOL_PREFERRED,      /* Prefer specific node */
    MPOL_BIND,           /* Allocate only from node mask */
    MPOL_INTERLEAVE,     /* Interleave across nodes */
    MPOL_LOCAL,          /* Prefer local node */
    MPOL_MAX
};

struct numa_node {
    uint32_t node_id;
    uint32_t start_pfn;
    uint32_t end_pfn;
    uint32_t total_pages;
    uint32_t free_pages;
    uint32_t distance[MAX_NUMNODES];
    uint8_t online;
};

struct numa_policy_state {
    enum numa_policy mode;
    uint32_t preferred_node;
    uint32_t node_mask;  /* Bitmask for MPOL_BIND/INTERLEAVE */
    uint32_t il_next;    /* Next node for interleave */
};

struct numa_stats {
    uint64_t alloc_local;
    uint64_t alloc_remote;
    uint64_t alloc_interleave;
    uint64_t alloc_failed;
};

/* Node management */
void numa_init(void);
int numa_node_online(int node);
int numa_add_node(int node, uint32_t start_pfn, uint32_t end_pfn);
int numa_set_distance(int from, int to, uint32_t distance);
int numa_get_distance(int from, int to);

/* Memory policy */
int numa_get_policy(void);
int numa_set_policy(enum numa_policy mode, uint32_t node_mask);
int numa_get_mempolicy(int *policy, uint32_t *node_mask, uint32_t maxnode);
int numa_set_mempolicy(int mode, const uint32_t *node_mask, uint32_t maxnode);

/* Node-aware allocation */
uint32_t numa_alloc_onnode(int node, uint32_t pages, uint32_t flags);
uint32_t numa_alloc_interleave(uint32_t pages, uint32_t flags);
uint32_t numa_alloc_preferred(int preferred, uint32_t pages, uint32_t flags);

/* mbind - set memory policy for a virtual address range */
int numa_mbind(uint32_t addr, uint32_t len, int mode, const uint32_t *node_mask, uint32_t maxnode, int flags);

/* Statistics */
void numa_get_stats(int node, struct numa_stats *out);
void numa_dump_info(void);

/* Current task's memory policy */
struct numa_policy_state *numa_get_task_policy(void);
void numa_set_task_policy(struct numa_policy_state *pol);

/* Get default policy node mask (for syscalls) */
uint32_t numa_get_default_node_mask(void);

#endif