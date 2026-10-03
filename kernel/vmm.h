#ifndef FELINOS_VMM_H
#define FELINOS_VMM_H

#include <stdint.h>
#include "paging.h"

#define VM_READ       0x0001u
#define VM_WRITE      0x0002u
#define VM_USER       0x0004u
#define VM_EXEC       0x0008u
#define VM_DEMAND     0x0010u
#define VM_SWAPPABLE  0x0020u
#define VM_FIXED      0x0040u
#define VM_GUARD      0x0080u
#define VM_SHARED     0x0100u
#define VM_PINNED     0x0200u
#define VM_UNCACHED   0x0400u
#define VM_GROWSDOWN  0x0800u
#define VM_FILE       0x1000u

#define VMM_HEAP_BASE     0xC0000000u
#define VMM_HEAP_LIMIT    0xDFC00000u
#define VMM_VMALLOC_BASE  0xE0000000u
#define VMM_VMALLOC_END   0xF0000000u
#define VMM_MMIO_BASE     0xF0000000u
#define VMM_MMIO_END      0xFF800000u
#define VMM_SELFMAP_BASE  0xFFC00000u

#define VM_NAME_LEN 16
#define VMM_MAX_REGIONS 512
#define VMM_MAX_SPACES 24

struct vm_space;

struct vm_region {
    uint32_t base;
    uint32_t size;
    uint32_t flags;
    uint32_t phys;
    uint32_t resident;
    uint32_t swapped;
    char name[VM_NAME_LEN];
    struct vm_space *space;
    struct vm_region *next;
    int in_use;
    void *file;
    uint64_t file_off;
};

struct vm_space {
    struct address_space *as;
    struct address_space as_storage;
    struct vm_region *regions;
    char name[VM_NAME_LEN];
    int in_use;
    int id;
};

struct vmm_stats {
    uint32_t page_faults;
    uint32_t demand_faults;
    uint32_t cow_faults;
    uint32_t swap_faults;
    uint32_t protection_faults;
    uint32_t invalid_faults;
    uint32_t reclaims;
    uint32_t evictions;
    uint32_t swap_ins;
    uint32_t swap_outs;
    uint32_t oom_events;
    uint32_t resident_pages;
    uint32_t swapped_pages;
    uint32_t virtual_pages;
    uint32_t regions;
    uint32_t spaces;
};

void vmm_init(void);
void vmm_lock(void);
void vmm_unlock(void);

struct vm_space *vmm_kernel_space(void);
struct vm_space *vmm_current_space(void);
struct vm_space *vmm_space_create(const char *name);
struct vm_space *vmm_space_clone(const char *name);
void vmm_space_destroy(struct vm_space *space);
void vmm_space_switch(struct vm_space *space);
int vmm_space_count(void);
struct vm_space *vmm_space_get(int index);

void *vmm_alloc(uint32_t size, uint32_t flags, const char *name);
void *vmm_alloc_at(uint32_t base, uint32_t size, uint32_t flags, const char *name);
void *vmm_alloc_range(uint32_t lo, uint32_t hi, uint32_t size, uint32_t flags, const char *name);
int vmm_protect_region(uint32_t base, uint32_t size, uint32_t add_flags, uint32_t clear_flags);
void *vmm_map_physical(uint32_t phys, uint32_t size, uint32_t flags, const char *name);
int vmm_free(void *ptr);
int vmm_commit(uint32_t base, uint32_t size);
int vmm_release(uint32_t base, uint32_t size);

struct vm_region *vmm_find_region(struct vm_space *space, uint32_t addr);
struct vm_region *vmm_region_list(struct vm_space *space);
int vmm_region_count(struct vm_space *space);

int vmm_handle_fault(uint32_t addr, uint32_t err_code);
void vmm_describe(uint32_t addr, char *buf, uint32_t size);

uint32_t vmm_reclaim(uint32_t pages);
uint32_t vmm_evict_range(uint32_t base, uint32_t size, uint32_t pages);
int vmm_swap_in_all(void);
uint32_t vmm_available_pages(void);

uint32_t vmm_heap_base(void);
uint32_t vmm_heap_size(void);
int vmm_heap_extend(uint32_t bytes);

void vmm_get_stats(struct vmm_stats *out);
const char *vmm_flags_string(uint32_t flags, char *buf, uint32_t size);

#endif
