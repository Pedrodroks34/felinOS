#ifndef FELINOS_CGROUP_H
#define FELINOS_CGROUP_H

#include <stdint.h>
#include <stddef.h>
#include "mm/pagecache.h"
struct task;  /* Forward declaration */

/* Cgroups v2 Memory Controller
 *
 * Implements memory cgroup hierarchy with:
 * - memory.max (hard limit)
 * - memory.high (throttle limit)
 * - memory.low (protection)
 * - memory.current (usage)
 * - OOM killer per cgroup
 */

#define CGROUP_MAX_DEPTH 16
#define CGROUP_NAME_MAX 64
#define CGROUP_MAX_CHILDREN 32

struct cgroup_mem {
    /* Memory limits */
    uint64_t max;           /* memory.max - hard limit (0 = no limit) */
    uint64_t high;          /* memory.high - throttle limit (0 = no limit) */
    uint64_t low;           /* memory.low - protection (0 = no protection) */
    uint64_t min;           /* memory.min - hard protection (0 = no protection) */

    /* Current usage */
    uint64_t current;       /* memory.current - current usage */
    uint64_t peak;          /* memory.peak - peak usage */

    /* Page accounting */
    uint64_t anon;          /* Anonymous memory */
    uint64_t file;          /* Page cache (file-backed) */
    uint64_t kernel;        /* Kernel memory */
    uint64_t slab;          /* Slab memory */
    uint64_t sock;          /* Socket memory */
    uint64_t shmem;         /* Shared memory */

    /* Events */
    uint64_t max_fail;      /* Number of times max was hit */
    uint64_t high_throttle; /* Times throttled at high */
    uint64_t oom_kill;      /* OOM kills */
    uint64_t oom_kill_time; /* Last OOM kill timestamp */

    /* Hierarchy */
    struct cgroup_mem *parent;
    struct cgroup_mem *children[CGROUP_MAX_CHILDREN];
    int child_count;
    int level;

    /* Identification */
    char name[CGROUP_NAME_MAX];
    uint32_t id;
    int populated;          /* Has tasks */
    int frozen;             /* Frozen (not schedulable) */
};

/* Root cgroup */
extern struct cgroup_mem cgroup_root;

/* Cgroup operations */
int cgroup_mem_init(void);
struct cgroup_mem *cgroup_create(struct cgroup_mem *parent, const char *name);
int cgroup_destroy(struct cgroup_mem *cg);
int cgroup_attach_task(struct cgroup_mem *cg, struct task *task);
int cgroup_detach_task(struct task *task);

/* Memory limit setters */
int cgroup_set_max(struct cgroup_mem *cg, uint64_t bytes);
int cgroup_set_high(struct cgroup_mem *cg, uint64_t bytes);
int cgroup_set_low(struct cgroup_mem *cg, uint64_t bytes);
int cgroup_set_min(struct cgroup_mem *cg, uint64_t bytes);

/* Charge/uncharge memory */
int cgroup_charge(struct cgroup_mem *cg, uint64_t bytes, int type);
void cgroup_uncharge(struct cgroup_mem *cg, uint64_t bytes, int type);

/* Memory types */
#define CGROUP_MEM_ANON   0
#define CGROUP_MEM_FILE   1
#define CGROUP_MEM_KERNEL 2
#define CGROUP_MEM_SLAB   3
#define CGROUP_MEM_SOCK   4
#define CGROUP_MEM_SHMEM  5

/* OOM handling */
int cgroup_try_charge(struct cgroup_mem *cg, uint64_t bytes, int type);
void cgroup_oom(struct cgroup_mem *cg);
struct task *cgroup_select_oom_victim(struct cgroup_mem *cg);

/* Statistics */
void cgroup_get_stats(struct cgroup_mem *cg, struct cgroup_mem *out);

/* Task's current cgroup */
struct cgroup_mem *task_get_cgroup(struct task *task);
void task_set_cgroup(struct task *task, struct cgroup_mem *cg);

/* Filesystem interface (for cgroupfs) */
int cgroup_fs_read_max(struct cgroup_mem *cg, char *buf, size_t size);
int cgroup_fs_write_max(struct cgroup_mem *cg, const char *buf, size_t size);
int cgroup_fs_read_high(struct cgroup_mem *cg, char *buf, size_t size);
int cgroup_fs_write_high(struct cgroup_mem *cg, const char *buf, size_t size);
int cgroup_fs_read_current(struct cgroup_mem *cg, char *buf, size_t size);
int cgroup_fs_read_peak(struct cgroup_mem *cg, char *buf, size_t size);
int cgroup_fs_read_events(struct cgroup_mem *cg, char *buf, size_t size);
int cgroup_fs_read_stat(struct cgroup_mem *cg, char *buf, size_t size);

#endif