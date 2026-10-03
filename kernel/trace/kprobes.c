/* TODO: Implement kprobes/uprobes - Phase 9
 * Reference: linux/kernel/kprobes.c
 * 
 * Key features:
 * - kprobe (kernel probes)
 * - kretprobe (return probes)
 * - uprobe (user probes)
 * - Register/unregister
 * - Pre/post handlers
 * - Fault handlers
 * - Breakpoint handler
 * - Optimization (jump optimization)
 * - Blacklist (functions that cannot be probed)
 * - sysctl interface
 */

#include <kernel/kprobes.h>

// TODO: Implement kprobes/uprobes

/* kprobe */
struct kprobe {
    struct hlist_node hlist;
    struct list_head list;
    unsigned long nmissed;
    kprobe_opcode_t *addr;
    const char *symbol_name;
    unsigned long offset;
    kprobe_pre_handler_t pre_handler;
    kprobe_post_handler_t post_handler;
    kprobe_fault_handler_t fault_handler;
    kprobe_break_handler_t break_handler;
    struct kprobe_ctlblk *ctlblk;
    unsigned int flags;
    int state;
};

/* kretprobe */
struct kretprobe {
    struct kprobe kp;
    kretprobe_handler_t handler;
    kretprobe_handler_t entry_handler;
    int maxactive;
    int nmissed;
    size_t data_size;
    struct hlist_head free_rp_inst;
    struct hlist_head used_rp_inst;
    struct kretprobe_instance *instances;
};

/* kretprobe instance */
struct kretprobe_instance {
    struct hlist_node hlist;
    struct kretprobe *rp;
    struct pt_regs *regs;
    unsigned long ret_addr;
    unsigned long fp;
    char data[];
};

/* uprobe */
struct uprobe {
    struct rb_node rb_node;
    refcount_t ref;
    struct rw_semaphore consumer_rwsem;
    struct list_head consumers;
    struct inode *inode;
    loff_t offset;
    int flags;
};

/* uprobe consumer */
struct uprobe_consumer {
    struct list_head list;
    uprobe_handler_t handler;
    uprobe_handler_t ret_handler;
    struct uprobe *uprobe;
    struct file *file;
    void *data;
    int filter;
};

/* kprobe control block */
struct kprobe_ctlblk {
    unsigned long kprobe_status;
    unsigned long kprobe_old_ip;
    struct pt_regs *kprobe_saved_regs;
    struct pt_regs kprobe_regs;
    struct kretprobe_instance *kprobe_instance;
    struct uprobe *uprobe;
};

/* kprobe handlers */
typedef int (*kprobe_pre_handler_t)(struct kprobe *p, struct pt_regs *regs);
typedef void (*kprobe_post_handler_t)(struct kprobe *p, struct pt_regs *regs, unsigned long flags);
typedef int (*kprobe_fault_handler_t)(struct kprobe *p, struct pt_regs *regs, int trapnr);
typedef int (*kprobe_break_handler_t)(struct kprobe *p, struct pt_regs *regs);

/* kretprobe handler */
typedef int (*kretprobe_handler_t)(struct kretprobe_instance *ri, struct pt_regs *regs);

/* uprobe handler */
typedef int (*uprobe_handler_t)(struct uprobe_consumer *uc, struct pt_regs *regs);

/* TODO: Implement these functions */
int register_kprobe(struct kprobe *p) { return 0; }
void unregister_kprobe(struct kprobe *p) {}
int register_kretprobe(struct kretprobe *rp) { return 0; }
void unregister_kretprobe(struct kretprobe *rp) {}
int register_kprobes(struct kprobe **kps, int num) { return 0; }
void unregister_kprobes(struct kprobe **kps, int num) {}
int register_kretprobes(struct kretprobe **rps, int num) { return 0; }
void unregister_kretprobes(struct kretprobe **rps, int num) {}
int register_uprobe(struct uprobe *uprobe) { return 0; }
void unregister_uprobe(struct uprobe *uprobe) {}
int uprobe_register_consumer(struct uprobe *uprobe, struct uprobe_consumer *uc) { return 0; }
void uprobe_unregister_consumer(struct uprobe *uprobe, struct uprobe_consumer *uc) {}
void arch_prepare_kretprobe(struct kretprobe *rp, struct pt_regs *regs) {}
void arch_trampoline_kprobe(struct kprobe *p) {}