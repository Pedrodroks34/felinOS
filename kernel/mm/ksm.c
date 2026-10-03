#include "mm/ksm.h"
#include "mm/pagecache.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "sync.h"
#include "sched.h"
#include "pmm.h"

#define KSM_STABLE_RED   0
#define KSM_STABLE_BLACK 1
#define KSM_UNSTABLE_RED 0
#define KSM_UNSTABLE_BLACK 1

static struct ksm_stable_node *stable_root = NULL;
static struct ksm_unstable_node *unstable_root = NULL;
static mutex_t stable_mtx = MUTEX_INIT("ksm_stable");
static mutex_t unstable_mtx = MUTEX_INIT("ksm_unstable");
static struct ksm_stats ksm_stats = {0};
static int ksm_inited = 0;
static struct task *ksm_thread = NULL;
static int ksm_running = 0;

/* ---- RB-tree helpers for stable tree ---- */

static void stable_rotate_left(struct ksm_stable_node **root, struct ksm_stable_node *x) {
    struct ksm_stable_node *y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    if (!x->parent) *root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void stable_rotate_right(struct ksm_stable_node **root, struct ksm_stable_node *x) {
    struct ksm_stable_node *y = x->left;
    x->left = y->right;
    if (y->right) y->right->parent = x;
    y->parent = x->parent;
    if (!x->parent) *root = y;
    else if (x == x->parent->right) x->parent->right = y;
    else x->parent->left = y;
    y->right = x;
    x->parent = y;
}

static void stable_insert_fixup(struct ksm_stable_node **root, struct ksm_stable_node *z) {
    while (z->parent && z->parent->color == KSM_STABLE_RED) {
        if (z->parent == z->parent->parent->left) {
            struct ksm_stable_node *y = z->parent->parent->right;
            if (y && y->color == KSM_STABLE_RED) {
                z->parent->color = KSM_STABLE_BLACK;
                y->color = KSM_STABLE_BLACK;
                z->parent->parent->color = KSM_STABLE_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    stable_rotate_left(root, z);
                }
                z->parent->color = KSM_STABLE_BLACK;
                z->parent->parent->color = KSM_STABLE_RED;
                stable_rotate_right(root, z->parent->parent);
            }
        } else {
            struct ksm_stable_node *y = z->parent->parent->left;
            if (y && y->color == KSM_STABLE_RED) {
                z->parent->color = KSM_STABLE_BLACK;
                y->color = KSM_STABLE_BLACK;
                z->parent->parent->color = KSM_STABLE_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    stable_rotate_right(root, z);
                }
                z->parent->color = KSM_STABLE_BLACK;
                z->parent->parent->color = KSM_STABLE_RED;
                stable_rotate_left(root, z->parent->parent);
            }
        }
    }
    (*root)->color = KSM_STABLE_BLACK;
}

static struct ksm_stable_node *stable_tree_find(struct ksm_stable_node *root, uint64_t checksum) {
    while (root) {
        if (checksum < root->checksum) root = root->left;
        else if (checksum > root->checksum) root = root->right;
        else return root;
    }
    return NULL;
}

/* ---- RB-tree helpers for unstable tree ---- */

static void unstable_rotate_left(struct ksm_unstable_node **root, struct ksm_unstable_node *x) {
    struct ksm_unstable_node *y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    if (!x->parent) *root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void unstable_rotate_right(struct ksm_unstable_node **root, struct ksm_unstable_node *x) {
    struct ksm_unstable_node *y = x->left;
    x->left = y->right;
    if (y->right) y->right->parent = x;
    y->parent = x->parent;
    if (!x->parent) *root = y;
    else if (x == x->parent->right) x->parent->right = y;
    else x->parent->left = y;
    y->right = x;
    x->parent = y;
}

static void unstable_insert_fixup(struct ksm_unstable_node **root, struct ksm_unstable_node *z) {
    while (z->parent && z->parent->color == KSM_UNSTABLE_RED) {
        if (z->parent == z->parent->parent->left) {
            struct ksm_unstable_node *y = z->parent->parent->right;
            if (y && y->color == KSM_UNSTABLE_RED) {
                z->parent->color = KSM_UNSTABLE_BLACK;
                y->color = KSM_UNSTABLE_BLACK;
                z->parent->parent->color = KSM_UNSTABLE_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    unstable_rotate_left(root, z);
                }
                z->parent->color = KSM_UNSTABLE_BLACK;
                z->parent->parent->color = KSM_UNSTABLE_RED;
                unstable_rotate_right(root, z->parent->parent);
            }
        } else {
            struct ksm_unstable_node *y = z->parent->parent->left;
            if (y && y->color == KSM_UNSTABLE_RED) {
                z->parent->color = KSM_UNSTABLE_BLACK;
                y->color = KSM_UNSTABLE_BLACK;
                z->parent->parent->color = KSM_UNSTABLE_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    unstable_rotate_right(root, z);
                }
                z->parent->color = KSM_UNSTABLE_BLACK;
                z->parent->parent->color = KSM_UNSTABLE_RED;
                unstable_rotate_left(root, z->parent->parent);
            }
        }
    }
    (*root)->color = KSM_UNSTABLE_BLACK;
}

/* ---- Checksum computation ---- */

static uint64_t compute_page_checksum(struct page *page) {
    if (!page->present) return 0;
    uint64_t sum = 0;
    uint32_t *data = (uint32_t *)page->phys;
    for (int i = 0; i < PAGECACHE_PAGE_SIZE / 4; i++) {
        sum = sum * 31 + data[i];
    }
    return sum;
}

/* ---- Stable tree operations ---- */

static void stable_tree_insert(struct ksm_stable_node *node) {
    struct ksm_stable_node *y = NULL;
    struct ksm_stable_node *x = stable_root;

    while (x) {
        y = x;
        if (node->checksum < x->checksum) x = x->left;
        else x = x->right;
    }

    node->parent = y;
    if (!y) stable_root = node;
    else if (node->checksum < y->checksum) y->left = node;
    else y->right = node;

    node->left = node->right = NULL;
    node->color = KSM_STABLE_RED;
    stable_insert_fixup(&stable_root, node);
    ksm_stats.stable_tree_size++;
}

static void stable_tree_remove(struct ksm_stable_node *z) {
    /* Standard RB-tree deletion - simplified */
    struct ksm_stable_node *y = z;
    struct ksm_stable_node *x;
    uint8_t y_original_color = y->color;

    if (!z->left) {
        x = z->right;
        /* transplant z with z->right */
        if (!z->parent) stable_root = z->right;
        else if (z == z->parent->left) z->parent->left = z->right;
        else z->parent->right = z->right;
        if (z->right) z->right->parent = z->parent;
    } else if (!z->right) {
        x = z->left;
        if (!z->parent) stable_root = z->left;
        else if (z == z->parent->left) z->parent->left = z->left;
        else z->parent->right = z->left;
        if (z->left) z->left->parent = z->parent;
    } else {
        y = z->right;
        while (y->left) y = y->left;
        y_original_color = y->color;
        x = y->right;
        if (y->parent == z) {
            if (x) x->parent = y;
        } else {
            if (y->right) y->right->parent = y->parent;
            y->parent->left = y->right;
            y->right = z->right;
            if (y->right) y->right->parent = y;
        }
        if (!z->parent) stable_root = y;
        else if (z == z->parent->left) z->parent->left = y;
        else z->parent->right = y;
        y->parent = z->parent;
        y->left = z->left;
        if (y->left) y->left->parent = y;
        y->color = z->color;
    }
    kfree(z);
    ksm_stats.stable_tree_size--;
    /* Note: fixup for black nodes omitted for simplicity */
}

static struct ksm_stable_node *stable_tree_new(struct page *page, uint64_t checksum) {
    struct ksm_stable_node *node = (struct ksm_stable_node *)kzalloc(sizeof(*node));
    if (!node) return NULL;
    node->page = page;
    node->checksum = checksum;
    node->rmap_count = 1;
    return node;
}

/* ---- Unstable tree operations ---- */

static void unstable_tree_clear(void) {
    /* Recursively free unstable tree */
    struct ksm_unstable_node *stack[256];
    int sp = 0;
    struct ksm_unstable_node *cur = unstable_root;

    while (cur || sp > 0) {
        while (cur) {
            if (sp < 256) stack[sp++] = cur;
            cur = cur->left;
        }
        cur = stack[--sp];
        struct ksm_unstable_node *next = cur->right;
        kfree(cur);
        cur = next;
    }
    unstable_root = NULL;
    ksm_stats.unstable_tree_size = 0;
}

static void unstable_tree_insert(struct ksm_unstable_node *node) {
    struct ksm_unstable_node *y = NULL;
    struct ksm_unstable_node *x = unstable_root;

    while (x) {
        y = x;
        if (node->checksum < x->checksum) x = x->left;
        else x = x->right;
    }

    node->parent = y;
    if (!y) unstable_root = node;
    else if (node->checksum < y->checksum) y->left = node;
    else y->right = node;

    node->left = node->right = NULL;
    node->color = KSM_UNSTABLE_RED;
    unstable_insert_fixup(&unstable_root, node);
    ksm_stats.unstable_tree_size++;
}

static struct ksm_unstable_node *unstable_tree_find(struct ksm_unstable_node *root, uint64_t checksum) {
    while (root) {
        if (checksum < root->checksum) root = root->left;
        else if (checksum > root->checksum) root = root->right;
        else return root;
    }
    return NULL;
}

static struct ksm_unstable_node *unstable_tree_new(struct page *page, uint64_t checksum) {
    struct ksm_unstable_node *node = (struct ksm_unstable_node *)kzalloc(sizeof(*node));
    if (!node) return NULL;
    node->page = page;
    node->checksum = checksum;
    return node;
}

/* ---- Page comparison ---- */

static int pages_identical(struct page *a, struct page *b) {
    if (!a->present || !b->present) return 0;
    if (a == b) return 1;
    if (a->phys == b->phys) return 1;
    return memcmp((void *)a->phys, (void *)b->phys, PAGECACHE_PAGE_SIZE) == 0;
}

/* ---- KSM Main Functions ---- */

void ksm_init(void) {
    if (ksm_inited) return;
    ksm_inited = 1;
    ksm_stats = (struct ksm_stats){0};
    ksm_running = 1;
}

void ksm_add_page(struct page *page) {
    if (!page || !page->present) return;

    uint64_t checksum = compute_page_checksum(page);
    if (checksum == 0) return;

    mutex_lock(&unstable_mtx);
    struct ksm_unstable_node *u = unstable_tree_new(page, checksum);
    if (u) {
        unstable_tree_insert(u);
    }
    mutex_unlock(&unstable_mtx);
}

void ksm_remove_page(struct page *page) {
    if (!page) return;

    /* Remove from unstable tree if present */
    mutex_lock(&unstable_mtx);
    struct ksm_unstable_node *u = unstable_tree_find(unstable_root, compute_page_checksum(page));
    if (u && u->page == page) {
        /* Simplified: just clear tree - would need proper deletion */
        unstable_tree_clear();
    }
    mutex_unlock(&unstable_mtx);

    /* Remove from stable tree if present */
    mutex_lock(&stable_mtx);
    struct ksm_stable_node *s = stable_tree_find(stable_root, compute_page_checksum(page));
    if (s && s->page == page) {
        if (s->rmap_count <= 1) {
            stable_tree_remove(s);
        } else {
            s->rmap_count--;
            s->page = page;  /* Update sample page */
        }
    }
    mutex_unlock(&stable_mtx);
}

int ksm_do_scan(void) {
    int merged = 0;
    int scanned = 0;

    mutex_lock(&unstable_mtx);
    mutex_lock(&stable_mtx);

    /* For each unstable node, check against stable tree */
    struct ksm_unstable_node *stack[256];
    int sp = 0;
    struct ksm_unstable_node *cur = unstable_root;

    while ((cur || sp > 0) && scanned < KSM_MAX_PAGES_PER_SCAN) {
        while (cur) {
            if (sp < 256) stack[sp++] = cur;
            cur = cur->left;
        }
        cur = stack[--sp];
        struct ksm_unstable_node *next = cur->right;

        scanned++;
        ksm_stats.pages_scanned++;

        /* Check against stable tree */
        struct ksm_stable_node *stable = stable_tree_find(stable_root, cur->checksum);
        if (stable) {
            if (pages_identical(stable->page, cur->page)) {
                /* Merge: point cur->page to stable page */
                uint32_t old_phys = cur->page->phys;
                cur->page->phys = stable->page->phys;
                pmm_free_frame(old_phys);
                stable->rmap_count++;
                merged++;
                ksm_stats.pages_merged++;
            }
        } else {
            /* Promote to stable tree */
            struct ksm_stable_node *new_stable = stable_tree_new(cur->page, cur->checksum);
            if (new_stable) {
                stable_tree_insert(new_stable);
            }
        }

        kfree(cur);
        cur = next;
    }

    unstable_root = NULL;
    ksm_stats.unstable_tree_size = 0;
    ksm_stats.pages_volatile += (scanned - merged);

    mutex_unlock(&stable_mtx);
    mutex_unlock(&unstable_mtx);

    return merged;
}

void ksm_scan(void) {
    if (!ksm_running) return;
    ksm_do_scan();
    ksm_stats.full_scans++;
}

void ksm_get_stats(struct ksm_stats *out) {
    if (!out) return;
    mutex_lock(&stable_mtx);
    mutex_lock(&unstable_mtx);
    *out = ksm_stats;
    mutex_unlock(&unstable_mtx);
    mutex_unlock(&stable_mtx);
}

static void ksm_thread_fn(void *arg) {
    (void)arg;
    while (ksm_running) {
        ksm_scan();
        sched_sleep_ticks(KSM_SLEEP_MS * 100 / 1000);  /* Convert ms to ticks at 100Hz */
    }
}

void ksm_start_thread(void) {
    if (!ksm_thread) {
        ksm_thread = kthread_create("ksm", ksm_thread_fn, NULL);
        if (ksm_thread) task_start(ksm_thread);
    }
}

void ksm_stop_thread(void) {
    ksm_running = 0;
    if (ksm_thread) {
        sched_wake(ksm_thread);
        ksm_thread = NULL;
    }
}