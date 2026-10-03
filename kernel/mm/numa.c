#include "mm/numa.h"
#include "pmm.h"
#include "vmm.h"
#include "lib/format.h"
#include "console.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "sync.h"
#include "sched.h"

static struct numa_node nodes[MAX_NUMNODES];
static int num_nodes = 0;
static int numa_inited = 0;
static struct numa_policy_state default_policy = {
    .mode = MPOL_DEFAULT,
    .preferred_node = 0,
    .node_mask = 0,
    .il_next = 0
};
static mutex_t numa_mtx = MUTEX_INIT("numa");
static struct numa_stats numa_stats = {0};

void numa_init(void) {
    if (numa_inited) return;

    mutex_lock(&numa_mtx);

    /* Default: single node (node 0) covering all memory */
    uint32_t total_frames = pmm_total_frames();
    nodes[0].node_id = 0;
    nodes[0].start_pfn = 0;
    nodes[0].end_pfn = total_frames;
    nodes[0].total_pages = total_frames;
    nodes[0].free_pages = pmm_free_frames();
    nodes[0].online = 1;

    for (int i = 0; i < MAX_NUMNODES; i++) {
        nodes[0].distance[i] = (i == 0) ? NUMA_DISTANCE_LOCAL : NUMA_DISTANCE_REMOTE;
    }

    num_nodes = 1;
    numa_inited = 1;

    mutex_unlock(&numa_mtx);
}

int numa_node_online(int node) {
    if (node < 0 || node >= MAX_NUMNODES) return -1;
    return nodes[node].online ? 1 : 0;
}

int numa_add_node(int node, uint32_t start_pfn, uint32_t end_pfn) {
    if (node < 0 || node >= MAX_NUMNODES) return -1;
    if (nodes[node].online) return -1;  /* Already exists */

    mutex_lock(&numa_mtx);

    nodes[node].node_id = node;
    nodes[node].start_pfn = start_pfn;
    nodes[node].end_pfn = end_pfn;
    nodes[node].total_pages = end_pfn - start_pfn;
    nodes[node].free_pages = nodes[node].total_pages;  /* Approximate */
    nodes[node].online = 1;

    for (int i = 0; i < MAX_NUMNODES; i++) {
        nodes[node].distance[i] = (i == node) ? NUMA_DISTANCE_LOCAL : NUMA_DISTANCE_REMOTE;
        if (nodes[i].online) {
            nodes[i].distance[node] = NUMA_DISTANCE_REMOTE;
        }
    }

    if (node >= num_nodes) num_nodes = node + 1;

    mutex_unlock(&numa_mtx);
    return 0;
}

int numa_set_distance(int from, int to, uint32_t distance) {
    if (from < 0 || from >= MAX_NUMNODES || to < 0 || to >= MAX_NUMNODES) return -1;
    if (!nodes[from].online || !nodes[to].online) return -1;

    nodes[from].distance[to] = distance;
    nodes[to].distance[from] = distance;
    return 0;
}

int numa_get_distance(int from, int to) {
    if (from < 0 || from >= MAX_NUMNODES || to < 0 || to >= MAX_NUMNODES) return NUMA_DISTANCE_REMOTE;
    if (!nodes[from].online || !nodes[to].online) return NUMA_DISTANCE_REMOTE;
    return nodes[from].distance[to];
}

/* Find best node for allocation based on policy and current node */
static int numa_find_best_node(struct numa_policy_state *pol, int current_node) {
    int best = -1;
    uint32_t best_dist = 0xFFFFFFFF;

    switch (pol->mode) {
    case MPOL_DEFAULT:
    case MPOL_LOCAL:
        if (current_node >= 0 && nodes[current_node].online) return current_node;
        return 0;  /* Fallback to node 0 */

    case MPOL_PREFERRED:
        if (nodes[pol->preferred_node].online) return pol->preferred_node;
        /* Fallback: find closest online node */
        for (int i = 0; i < MAX_NUMNODES; i++) {
            if (nodes[i].online) {
                uint32_t d = numa_get_distance(pol->preferred_node, i);
                if (d < best_dist) {
                    best_dist = d;
                    best = i;
                }
            }
        }
        return best >= 0 ? best : 0;

    case MPOL_BIND:
        /* Find closest allowed node */
        for (int i = 0; i < MAX_NUMNODES; i++) {
            if ((pol->node_mask & (1u << i)) && nodes[i].online) {
                uint32_t d = (current_node >= 0) ? numa_get_distance(current_node, i) : 0;
                if (d < best_dist) {
                    best_dist = d;
                    best = i;
                }
            }
        }
        return best >= 0 ? best : 0;

    case MPOL_INTERLEAVE:
        /* Round-robin through allowed nodes */
        for (int i = 0; i < MAX_NUMNODES; i++) {
            int idx = (pol->il_next + i) % MAX_NUMNODES;
            if ((pol->node_mask & (1u << idx)) && nodes[idx].online) {
                pol->il_next = (idx + 1) % MAX_NUMNODES;
                return idx;
            }
        }
        return 0;
    default:
        return 0;
    }
    return 0;
}

int numa_get_policy(void) {
    return default_policy.mode;
}

int numa_set_policy(enum numa_policy mode, uint32_t node_mask) {
    if (mode > MPOL_MAX) return -1;
    mutex_lock(&numa_mtx);
    default_policy.mode = mode;
    default_policy.node_mask = node_mask;
    default_policy.preferred_node = 0;
    default_policy.il_next = 0;
    mutex_unlock(&numa_mtx);
    return 0;
}

int numa_get_mempolicy(int *policy, uint32_t *node_mask, uint32_t maxnode) {
    if (!policy) return -1;
    mutex_lock(&numa_mtx);
    *policy = default_policy.mode;
    if (node_mask && maxnode > 0) {
        *node_mask = default_policy.node_mask & ((1u << maxnode) - 1);
    }
    mutex_unlock(&numa_mtx);
    return 0;
}

int numa_set_mempolicy(int mode, const uint32_t *node_mask, uint32_t maxnode) {
    if (mode > MPOL_MAX) return -1;
    mutex_lock(&numa_mtx);
    default_policy.mode = mode;
    if (node_mask && maxnode > 0) {
        default_policy.node_mask = *node_mask & ((1u << maxnode) - 1);
    } else {
        default_policy.node_mask = 0;
    }
    default_policy.il_next = 0;
    mutex_unlock(&numa_mtx);
    return 0;
}

/* Node-aware page allocation */
uint32_t numa_alloc_onnode(int node, uint32_t pages, uint32_t flags) {
    if (node < 0 || node >= MAX_NUMNODES || !nodes[node].online) return 0;

    mutex_lock(&numa_mtx);
    if (nodes[node].free_pages < pages) {
        mutex_unlock(&numa_mtx);
        return 0;
    }
    /* In real implementation, would allocate from node's page pool */
    uint32_t frame = pmm_alloc_frame();
    if (frame) {
        nodes[node].free_pages -= pages;
        numa_stats.alloc_local++;
    } else {
        numa_stats.alloc_failed++;
    }
    mutex_unlock(&numa_mtx);
    return frame;
}

uint32_t numa_alloc_interleave(uint32_t pages, uint32_t flags) {
    mutex_lock(&numa_mtx);
    int node = numa_find_best_node(&default_policy, -1);
    uint32_t frame = pmm_alloc_frame();
    if (frame) {
        nodes[node].free_pages -= pages;
        numa_stats.alloc_interleave++;
    } else {
        numa_stats.alloc_failed++;
    }
    mutex_unlock(&numa_mtx);
    return frame;
}

uint32_t numa_alloc_preferred(int preferred, uint32_t pages, uint32_t flags) {
    mutex_lock(&numa_mtx);
    int node = (preferred >= 0 && nodes[preferred].online) ? preferred : 0;
    uint32_t frame = pmm_alloc_frame();
    if (frame) {
        nodes[node].free_pages -= pages;
        numa_stats.alloc_local++;
    } else {
        numa_stats.alloc_failed++;
    }
    mutex_unlock(&numa_mtx);
    return frame;
}

/* mbind - set memory policy for a virtual address range */
int numa_mbind(uint32_t addr, uint32_t len, int mode, const uint32_t *node_mask, uint32_t maxnode, int flags) {
    (void)addr; (void)len; (void)mode; (void)node_mask; (void)maxnode; (void)flags;
    /* In real implementation, would attach policy to vmm regions */
    return 0;  /* Not fully implemented yet */
}

void numa_get_stats(int node, struct numa_stats *out) {
    if (node < 0 || node >= MAX_NUMNODES) return;
    mutex_lock(&numa_mtx);
    *out = numa_stats;
    mutex_unlock(&numa_mtx);
}

void numa_dump_info(void) {
    mutex_lock(&numa_mtx);
    for (int i = 0; i < num_nodes; i++) {
        if (nodes[i].online) {
            kprintf("NUMA node %d: %u pages total, %u free\n",
                 i, nodes[i].total_pages, nodes[i].free_pages);
        }
    }
    mutex_unlock(&numa_mtx);
}

struct numa_policy_state *numa_get_task_policy(void) {
    struct task *t = sched_current();
    if (t && t->numa_policy) return t->numa_policy;
    return &default_policy;
}

void numa_set_task_policy(struct numa_policy_state *pol) {
    struct task *t = sched_current();
    if (t) t->numa_policy = pol;
}

uint32_t numa_get_default_node_mask(void) {
    mutex_lock(&numa_mtx);
    uint32_t mask = default_policy.node_mask;
    mutex_unlock(&numa_mtx);
    return mask;
}