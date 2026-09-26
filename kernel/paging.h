#ifndef FELINOS_PAGING_H
#define FELINOS_PAGING_H

#include <stdint.h>

#define PAGE_SIZE       4096u
#define PAGE_MASK       0xFFFFF000u
#define PAGE_OFFSET(v)  ((v) & 0xFFFu)

#define PAGE_PRESENT    0x001u
#define PAGE_RW         0x002u
#define PAGE_USER       0x004u
#define PAGE_PWT        0x008u
#define PAGE_PCD        0x010u
#define PAGE_ACCESSED   0x020u
#define PAGE_DIRTY      0x040u
#define PAGE_GLOBAL     0x100u
#define PAGE_COW        0x200u
#define PAGE_RESERVED   0x400u
#define PAGE_PINNED     0x800u

#define PAGE_SWAP_MARK  0x002u
#define PAGE_SWAP_SLOT(e) ((e) >> 12)
#define PAGE_SWAP_ENTRY(slot) (((slot) << 12) | PAGE_SWAP_MARK)
#define PAGE_IS_SWAPPED(e) ((((e) & PAGE_PRESENT) == 0) && ((e) & PAGE_SWAP_MARK))

struct address_space {
    uint64_t *pd;   /* PML4 */
    uint32_t pd_phys; /* PML4 physical address */
    uint32_t tables;
    uint32_t mapped;
    int id;
    int in_use;
};

void paging_init(uint32_t mem_top);

struct address_space *paging_kernel_space(void);
struct address_space *paging_current_space(void);
int paging_space_create(struct address_space *as, int id);
void paging_space_destroy(struct address_space *as);
void paging_space_switch(struct address_space *as);

int paging_map_in(struct address_space *as, uint32_t virt, uint32_t phys, uint32_t flags);
int paging_map(uint32_t virt, uint32_t phys, uint32_t flags);
int paging_map_range(uint32_t virt, uint32_t phys, uint32_t size, uint32_t flags);
void paging_unmap_in(struct address_space *as, uint32_t virt);
void paging_unmap(uint32_t virt);
int paging_protect(uint32_t virt, uint32_t flags);
int paging_protect_range(uint32_t virt, uint32_t size, uint32_t flags);

uint32_t paging_get_entry_in(struct address_space *as, uint32_t virt);
uint32_t paging_get_entry(uint32_t virt);
int paging_set_entry_in(struct address_space *as, uint32_t virt, uint32_t entry);
int paging_set_entry(uint32_t virt, uint32_t entry);

int paging_translate_in(struct address_space *as, uint32_t virt, uint32_t *phys_out);
int paging_translate(uint32_t virt, uint32_t *phys_out);
int paging_is_mapped(uint32_t virt);

void *paging_alloc_page(uint32_t virt, uint32_t flags);
void paging_free_page(uint32_t virt);

void paging_invalidate(uint32_t virt);
void paging_flush_tlb(void);

int paging_reserve_tables(struct address_space *as, uint32_t base, uint32_t size);
int paging_next_mapping(struct address_space *as, uint32_t *virt, uint32_t *entry);
int paging_clone_range(struct address_space *dst, struct address_space *src,
                       uint32_t base, uint32_t size, int cow);
void paging_release_range(struct address_space *as, uint32_t base, uint32_t size);

uint32_t paging_fault_address(void);
uint32_t paging_directory_phys(void);
uint32_t paging_mapped_mb(void);
uint32_t paging_mapped_pages(void);
uint32_t paging_table_count(void);
uint32_t paging_identity_top(void);
int paging_global_pages(void);

#endif
