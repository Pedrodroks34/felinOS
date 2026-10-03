#include "mm/cgroup.h"
#include "mm/pagecache.h"
#include "pmm.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "lib/format.h"
#include "sync.h"
#include "sched.h"

struct cgroup_mem cgroup_root = {
    .max = 0,
    .high = 0,
    .low = 0,
    .min = 0,
    .current = 0,
    .peak = 0,
    .anon = 0,
    .file = 0,
    .kernel = 0,
    .slab = 0,
    .sock = 0,
    .shmem = 0,
    .max_fail = 0,
    .high_throttle = 0,
    .oom_kill = 0,
    .parent = NULL,
    .child_count = 0,
    .level = 0,
    .name = "root",
    .id = 0,
    .populated = 1,
    .frozen = 0
};

static uint32_t cgroup_next_id = 1;
static mutex_t cgroup_mtx = MUTEX_INIT("cgroup");

struct cgroup_mem *cgroup_current = &cgroup_root;

int cgroup_mem_init(void) {
    mutex_lock(&cgroup_mtx);
    cgroup_root.current = pmm_total_frames() * PAGE_SIZE - pmm_free_frames() * PAGE_SIZE;
    cgroup_root.peak = cgroup_root.current;
    cgroup_current = &cgroup_root;
    mutex_unlock(&cgroup_mtx);
    return 0;
}

struct cgroup_mem *cgroup_create(struct cgroup_mem *parent, const char *name) {
    if (!parent) parent = &cgroup_root;

    mutex_lock(&cgroup_mtx);

    if (parent->child_count >= CGROUP_MAX_CHILDREN) {
        mutex_unlock(&cgroup_mtx);
        return NULL;
    }

    struct cgroup_mem *cg = (struct cgroup_mem *)kzalloc(sizeof(struct cgroup_mem));
    if (!cg) {
        mutex_unlock(&cgroup_mtx);
        return NULL;
    }

    cg->parent = parent;
    cg->level = parent->level + 1;
    if (cg->level > CGROUP_MAX_DEPTH) {
        kfree(cg);
        mutex_unlock(&cgroup_mtx);
        return NULL;
    }

    cg->id = cgroup_next_id++;
    strlcpy(cg->name, name, CGROUP_NAME_MAX);
    cg->populated = 0;
    cg->frozen = 0;

    /* Inherit limits from parent if not set */
    cg->max = 0;  /* No limit by default */
    cg->high = 0;
    cg->low = 0;
    cg->min = 0;

    parent->children[parent->child_count++] = cg;

    mutex_unlock(&cgroup_mtx);
    return cg;
}

int cgroup_destroy(struct cgroup_mem *cg) {
    if (!cg || cg == &cgroup_root) return -1;

    mutex_lock(&cgroup_mtx);

    /* Cannot destroy if has tasks */
    if (cg->populated) {
        mutex_unlock(&cgroup_mtx);
        return -1;
    }

    /* Cannot destroy if has children */
    if (cg->child_count > 0) {
        mutex_unlock(&cgroup_mtx);
        return -1;
    }

    /* Remove from parent */
    if (cg->parent) {
        for (int i = 0; i < cg->parent->child_count; i++) {
            if (cg->parent->children[i] == cg) {
                cg->parent->children[i] = cg->parent->children[cg->parent->child_count - 1];
                cg->parent->child_count--;
                break;
            }
        }
    }

    kfree(cg);
    mutex_unlock(&cgroup_mtx);
    return 0;
}

int cgroup_attach_task(struct cgroup_mem *cg, struct task *task) {
    if (!cg || !task) return -1;

    mutex_lock(&cgroup_mtx);

    struct cgroup_mem *old = task_get_cgroup(task);
    if (old) {
        old->populated--;
    }

    task_set_cgroup(task, cg);
    cg->populated++;

    mutex_unlock(&cgroup_mtx);
    return 0;
}

int cgroup_detach_task(struct task *task) {
    if (!task) return -1;

    mutex_lock(&cgroup_mtx);

    struct cgroup_mem *old = task_get_cgroup(task);
    if (old) {
        old->populated--;
        task_set_cgroup(task, &cgroup_root);
        cgroup_root.populated++;
    }

    mutex_unlock(&cgroup_mtx);
    return 0;
}

/* Charge memory to cgroup and all ancestors */
int cgroup_charge(struct cgroup_mem *cg, uint64_t bytes, int type) {
    if (!cg || bytes == 0) return 0;

    mutex_lock(&cgroup_mtx);

    struct cgroup_mem *cur = cg;
    while (cur) {
        cur->current += bytes;
        if (cur->current > cur->peak) cur->peak = cur->current;

        switch (type) {
        case CGROUP_MEM_ANON:   cur->anon += bytes; break;
        case CGROUP_MEM_FILE:   cur->file += bytes; break;
        case CGROUP_MEM_KERNEL: cur->kernel += bytes; break;
        case CGROUP_MEM_SLAB:   cur->slab += bytes; break;
        case CGROUP_MEM_SOCK:   cur->sock += bytes; break;
        case CGROUP_MEM_SHMEM:  cur->shmem += bytes; break;
        }

        /* Check max limit */
        if (cur->max > 0 && cur->current > cur->max) {
            cur->max_fail++;
            mutex_unlock(&cgroup_mtx);
            return -1;
        }

        /* Check high limit - throttle */
        if (cur->high > 0 && cur->current > cur->high) {
            cur->high_throttle++;
            /* In real impl, would throttle the task here */
        }

        cur = cur->parent;
    }

    mutex_unlock(&cgroup_mtx);
    return 0;
}

void cgroup_uncharge(struct cgroup_mem *cg, uint64_t bytes, int type) {
    if (!cg || bytes == 0) return;

    mutex_lock(&cgroup_mtx);

    struct cgroup_mem *cur = cg;
    while (cur) {
        if (cur->current >= bytes) {
            cur->current -= bytes;
        } else {
            cur->current = 0;
        }

        switch (type) {
        case CGROUP_MEM_ANON:
            if (cur->anon >= bytes) cur->anon -= bytes; else cur->anon = 0;
            break;
        case CGROUP_MEM_FILE:
            if (cur->file >= bytes) cur->file -= bytes; else cur->file = 0;
            break;
        case CGROUP_MEM_KERNEL:
            if (cur->kernel >= bytes) cur->kernel -= bytes; else cur->kernel = 0;
            break;
        case CGROUP_MEM_SLAB:
            if (cur->slab >= bytes) cur->slab -= bytes; else cur->slab = 0;
            break;
        case CGROUP_MEM_SOCK:
            if (cur->sock >= bytes) cur->sock -= bytes; else cur->sock = 0;
            break;
        case CGROUP_MEM_SHMEM:
            if (cur->shmem >= bytes) cur->shmem -= bytes; else cur->shmem = 0;
            break;
        }

        cur = cur->parent;
    }

    mutex_unlock(&cgroup_mtx);
}

/* Try charge with reclaim attempt */
int cgroup_try_charge(struct cgroup_mem *cg, uint64_t bytes, int type) {
    int ret = cgroup_charge(cg, bytes, type);
    if (ret == 0) return 0;

    /* Try reclaim - in real impl, would call page reclaim */
    /* For now, just return failure */
    return -1;
}

/* OOM handling */
void cgroup_oom(struct cgroup_mem *cg) {
    if (!cg) return;

    mutex_lock(&cgroup_mtx);
    cg->oom_kill++;
    cg->oom_kill_time = 0;  /* Would use rtc_unix() */
    mutex_unlock(&cgroup_mtx);

    /* Select and kill victim */
    struct task *victim = cgroup_select_oom_victim(cg);
    if (victim) {
        task_kill(victim->pid, 9);  /* SIGKILL */
    }
}

struct task *cgroup_select_oom_victim(struct cgroup_mem *cg) {
    /* Simple heuristic: pick task with highest memory usage in this cgroup */
    /* In real impl, would iterate tasks in cgroup and score them */
    return NULL;
}

/* Setters */
int cgroup_set_max(struct cgroup_mem *cg, uint64_t bytes) {
    if (!cg) return -1;
    mutex_lock(&cgroup_mtx);
    cg->max = bytes;
    mutex_unlock(&cgroup_mtx);
    return 0;
}

int cgroup_set_high(struct cgroup_mem *cg, uint64_t bytes) {
    if (!cg) return -1;
    mutex_lock(&cgroup_mtx);
    cg->high = bytes;
    mutex_unlock(&cgroup_mtx);
    return 0;
}

int cgroup_set_low(struct cgroup_mem *cg, uint64_t bytes) {
    if (!cg) return -1;
    mutex_lock(&cgroup_mtx);
    cg->low = bytes;
    mutex_unlock(&cgroup_mtx);
    return 0;
}

int cgroup_set_min(struct cgroup_mem *cg, uint64_t bytes) {
    if (!cg) return -1;
    mutex_lock(&cgroup_mtx);
    cg->min = bytes;
    mutex_unlock(&cgroup_mtx);
    return 0;
}

/* Get stats */
void cgroup_get_stats(struct cgroup_mem *cg, struct cgroup_mem *out) {
    if (!cg || !out) return;
    mutex_lock(&cgroup_mtx);
    *out = *cg;
    mutex_unlock(&cgroup_mtx);
}

/* Task cgroup management */
struct cgroup_mem *task_get_cgroup(struct task *task) {
    return task ? task->cgroup : &cgroup_root;
}

void task_set_cgroup(struct task *task, struct cgroup_mem *cg) {
    if (task) task->cgroup = cg ? cg : &cgroup_root;
}

/* Filesystem interface stubs */
int cgroup_fs_read_max(struct cgroup_mem *cg, char *buf, size_t size) {
    if (!cg || !buf) return -1;
    return snprintf(buf, size, "%llu\n", cg->max);
}

int cgroup_fs_write_max(struct cgroup_mem *cg, const char *buf, size_t size) {
    if (!cg || !buf) return -1;
    uint64_t val = 0;
    for (size_t i = 0; i < size; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            val = val * 10 + (buf[i] - '0');
        }
    }
    return cgroup_set_max(cg, val);
}

int cgroup_fs_read_high(struct cgroup_mem *cg, char *buf, size_t size) {
    if (!cg || !buf) return -1;
    return snprintf(buf, size, "%llu\n", cg->high);
}

int cgroup_fs_write_high(struct cgroup_mem *cg, const char *buf, size_t size) {
    if (!cg || !buf) return -1;
    uint64_t val = 0;
    for (size_t i = 0; i < size; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            val = val * 10 + (buf[i] - '0');
        }
    }
    return cgroup_set_high(cg, val);
}

int cgroup_fs_read_current(struct cgroup_mem *cg, char *buf, size_t size) {
    if (!cg || !buf) return -1;
    return snprintf(buf, size, "%llu\n", cg->current);
}

int cgroup_fs_read_peak(struct cgroup_mem *cg, char *buf, size_t size) {
    if (!cg || !buf) return -1;
    return snprintf(buf, size, "%llu\n", cg->peak);
}

int cgroup_fs_read_events(struct cgroup_mem *cg, char *buf, size_t size) {
    if (!cg || !buf) return -1;
    return snprintf(buf, size,
        "max_fail %llu\nhigh_throttle %llu\noom_kill %llu\n",
        cg->max_fail, cg->high_throttle, cg->oom_kill);
}

int cgroup_fs_read_stat(struct cgroup_mem *cg, char *buf, size_t size) {
    if (!cg || !buf) return -1;
    return snprintf(buf, size,
        "anon %llu\nfile %llu\nkernel %llu\nslab %llu\nsock %llu\nshmem %llu\n",
        cg->anon, cg->file, cg->kernel, cg->slab, cg->sock, cg->shmem);
}