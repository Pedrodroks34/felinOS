#include "paging.h"
#include "pmm.h"
#include "system.h"
#include "lib/string.h"

/* x86-64 4-level paging. All virtual addresses handled here are < 4 GiB
 * (PML4 slot 0); physical memory below 4 GiB is identity-mapped for the kernel. */
typedef uint64_t pte_t;

#define PDE_INDEX(v)  ((v) >> 21)                /* 2 MiB region, 0..2047 */
#define PT_INDEX(v)   (((v) >> 12) & 0x1FFu)
#define PT_ENTRIES    512u
#define PAGE_BASE(v)  ((v) & PAGE_MASK)
#define RESERVED_PDE  2046u                      /* top 4 MiB stays unmapped */
#define PDE_FLAGS (PAGE_PRESENT | PAGE_RW | PAGE_USER)

#define TBL(frame) ((pte_t *)(uintptr_t)(frame))

extern uint32_t text_start;
extern uint32_t text_end;
extern uint32_t rodata_start;
extern uint32_t rodata_end;

static struct address_space kernel_space;
static struct address_space *current_space;
static uint32_t identity_top;
static uint32_t mapped_pages;
static uint32_t table_count;
static int paging_active;
static int global_pages;

static inline void invlpg(uint32_t addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"((uint64_t)addr) : "memory");
}

static inline void set_cr3(uint32_t phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64_t)phys) : "memory");
}

static inline uint32_t get_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return (uint32_t)v;
}

static int cpu_supports_pge(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (edx & (1u << 13)) != 0;
}

static void enable_global_pages(void) {
    uint64_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x00000080u;
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

static void enable_paging_bits(void) {
    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80010000u;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static pte_t *next_level(pte_t *slot, int create) {
    if (*slot & PAGE_PRESENT) {
        return TBL(PAGE_BASE(*slot));
    }
    if (!create) {
        return NULL;
    }
    uint32_t frame = pmm_alloc_frame();
    if (!frame) {
        return NULL;
    }
    memset((void *)(uintptr_t)frame, 0, PAGE_SIZE);
    *slot = frame | PDE_FLAGS;
    return TBL(frame);
}

/* Address of the page-directory entry that covers virt (its 2 MiB region). */
static pte_t *pde_slot(struct address_space *as, uint32_t virt, int create) {
    pte_t *pdpt = next_level(&as->pd[0], create);
    if (!pdpt) {
        return NULL;
    }
    pte_t *pd = next_level(&pdpt[virt >> 30], create);
    if (!pd) {
        return NULL;
    }
    return &pd[(virt >> 21) & 0x1FFu];
}

static pte_t *table_of(struct address_space *as, uint32_t virt, int create) {
    pte_t *slot = pde_slot(as, virt, create);
    if (!slot) {
        return NULL;
    }
    if (*slot & PAGE_PRESENT) {
        return TBL(PAGE_BASE(*slot));
    }
    if (!create) {
        return NULL;
    }
    uint32_t frame = pmm_alloc_frame();
    if (!frame) {
        return NULL;
    }
    memset((void *)(uintptr_t)frame, 0, PAGE_SIZE);
    *slot = frame | PDE_FLAGS;
    as->tables++;
    table_count++;
    return TBL(frame);
}

static void account_entry(struct address_space *as, pte_t old_entry, pte_t new_entry) {
    int was = (old_entry & PAGE_PRESENT) ? 1 : 0;
    int now = (new_entry & PAGE_PRESENT) ? 1 : 0;
    if (was == now) {
        return;
    }
    if (now) {
        as->mapped++;
        mapped_pages++;
    } else {
        if (as->mapped) {
            as->mapped--;
        }
        if (mapped_pages) {
            mapped_pages--;
        }
    }
}

int paging_map_in(struct address_space *as, uint32_t virt, uint32_t phys, uint32_t flags) {
    pte_t *table = table_of(as, virt, 1);
    if (!table) {
        return -1;
    }
    uint32_t idx = PT_INDEX(virt);
    pte_t entry = PAGE_BASE(phys) | (flags & 0xFFFu) | PAGE_PRESENT;
    account_entry(as, table[idx], entry);
    table[idx] = entry;
    if (paging_active && as == current_space) {
        invlpg(virt);
    }
    return 0;
}

int paging_map(uint32_t virt, uint32_t phys, uint32_t flags) {
    return paging_map_in(current_space, virt, phys, flags);
}

int paging_map_range(uint32_t virt, uint32_t phys, uint32_t size, uint32_t flags) {
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = 0; i < pages; i++) {
        if (paging_map(virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags) != 0) {
            for (uint32_t j = 0; j < i; j++) {
                paging_unmap(virt + j * PAGE_SIZE);
            }
            return -1;
        }
    }
    return 0;
}

void paging_unmap_in(struct address_space *as, uint32_t virt) {
    pte_t *table = table_of(as, virt, 0);
    if (!table) {
        return;
    }
    uint32_t idx = PT_INDEX(virt);
    account_entry(as, table[idx], 0);
    table[idx] = 0;
    if (paging_active && as == current_space) {
        invlpg(virt);
    }
}

void paging_unmap(uint32_t virt) {
    paging_unmap_in(current_space, virt);
}

int paging_protect(uint32_t virt, uint32_t flags) {
    pte_t *table = table_of(current_space, virt, 0);
    if (!table) {
        return -1;
    }
    uint32_t idx = PT_INDEX(virt);
    if (!(table[idx] & PAGE_PRESENT)) {
        return -1;
    }
    table[idx] = PAGE_BASE(table[idx]) | (flags & 0xFFFu) | PAGE_PRESENT;
    if (paging_active) {
        invlpg(virt);
    }
    return 0;
}

int paging_protect_range(uint32_t virt, uint32_t size, uint32_t flags) {
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    int done = 0;
    for (uint32_t i = 0; i < pages; i++) {
        if (paging_protect(virt + i * PAGE_SIZE, flags) == 0) {
            done++;
        }
    }
    return done;
}

uint32_t paging_get_entry_in(struct address_space *as, uint32_t virt) {
    pte_t *table = table_of(as, virt, 0);
    if (!table) {
        return 0;
    }
    return (uint32_t)table[PT_INDEX(virt)];
}

uint32_t paging_get_entry(uint32_t virt) {
    return paging_get_entry_in(current_space, virt);
}

int paging_set_entry_in(struct address_space *as, uint32_t virt, uint32_t entry) {
    pte_t *table = table_of(as, virt, 1);
    if (!table) {
        return -1;
    }
    uint32_t idx = PT_INDEX(virt);
    account_entry(as, table[idx], entry);
    table[idx] = entry;
    if (paging_active && as == current_space) {
        invlpg(virt);
    }
    return 0;
}

int paging_set_entry(uint32_t virt, uint32_t entry) {
    return paging_set_entry_in(current_space, virt, entry);
}

int paging_translate_in(struct address_space *as, uint32_t virt, uint32_t *phys_out) {
    uint32_t entry = paging_get_entry_in(as, virt);
    if (!(entry & PAGE_PRESENT)) {
        return -1;
    }
    if (phys_out) {
        *phys_out = PAGE_BASE(entry) | PAGE_OFFSET(virt);
    }
    return 0;
}

int paging_translate(uint32_t virt, uint32_t *phys_out) {
    return paging_translate_in(current_space, virt, phys_out);
}

int paging_is_mapped(uint32_t virt) {
    return (paging_get_entry(virt) & PAGE_PRESENT) ? 1 : 0;
}

void *paging_alloc_page(uint32_t virt, uint32_t flags) {
    uint32_t phys = pmm_alloc_zeroed();
    if (!phys) {
        return NULL;
    }
    if (paging_map(virt, phys, flags) != 0) {
        pmm_free_frame(phys);
        return NULL;
    }
    return (void *)virt;
}

void paging_free_page(uint32_t virt) {
    uint32_t phys;
    if (paging_translate(virt, &phys) == 0) {
        paging_unmap(virt);
        pmm_unref_frame(phys);
    }
}

void paging_invalidate(uint32_t virt) {
    if (paging_active) {
        invlpg(virt);
    }
}

void paging_flush_tlb(void) {
    if (paging_active) {
        set_cr3(get_cr3());
    }
}

int paging_next_mapping(struct address_space *as, uint32_t *virt, uint32_t *entry) {
    uint32_t addr = PAGE_BASE(*virt);

    while (addr < 0xFFFFF000u) {
        pte_t *slot = pde_slot(as, addr, 0);
        if (!slot || !(*slot & PAGE_PRESENT)) {
            uint32_t next = (addr & ~0x1FFFFFu) + 0x200000u;
            if (next <= addr) {
                break;
            }
            addr = next;
            continue;
        }
        pte_t *table = TBL(PAGE_BASE(*slot));
        pte_t value = table[PT_INDEX(addr)];
        if (value) {
            *virt = addr;
            *entry = (uint32_t)value;
            return 1;
        }
        addr += PAGE_SIZE;
    }
    return 0;
}

static pte_t *private_table(struct address_space *dst, struct address_space *src, uint32_t virt) {
    pte_t *dst_slot = pde_slot(dst, virt, 1);
    pte_t *src_slot = pde_slot(src, virt, 0);
    pte_t dst_pde, src_pde;

    if (!dst_slot) {
        return NULL;
    }
    dst_pde = *dst_slot;
    src_pde = src_slot ? *src_slot : 0;

    if ((dst_pde & PAGE_PRESENT) && PAGE_BASE(dst_pde) != PAGE_BASE(src_pde)) {
        return TBL(PAGE_BASE(dst_pde));
    }

    uint32_t frame = pmm_alloc_frame();
    if (!frame) {
        return NULL;
    }
    pte_t *table = TBL(frame);
    memset(table, 0, PAGE_SIZE);

    if (src_pde & PAGE_PRESENT) {
        pte_t *src_table = TBL(PAGE_BASE(src_pde));
        for (uint32_t i = 0; i < PT_ENTRIES; i++) {
            table[i] = src_table[i];
            if (src_table[i] & PAGE_PRESENT) {
                pmm_ref_frame(PAGE_BASE(src_table[i]));
                dst->mapped++;
            }
        }
    }
    *dst_slot = frame | PDE_FLAGS;
    dst->tables++;
    table_count++;
    return table;
}

int paging_clone_range(struct address_space *dst, struct address_space *src,
                       uint32_t base, uint32_t size, int cow) {
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t virt = PAGE_BASE(base) + i * PAGE_SIZE;
        pte_t *dst_table = private_table(dst, src, virt);
        if (!dst_table) {
            return -1;
        }
        pte_t *src_table = table_of(src, virt, 0);
        if (!src_table) {
            continue;
        }
        uint32_t idx = PT_INDEX(virt);
        pte_t entry = src_table[idx];
        if (!(entry & PAGE_PRESENT)) {
            dst_table[idx] = entry;
            continue;
        }
        if (cow && (entry & PAGE_RW)) {
            pte_t shared = (entry & ~(pte_t)PAGE_RW) | PAGE_COW;
            src_table[idx] = shared;
            dst_table[idx] = shared;
            if (src == current_space) {
                invlpg(virt);
            }
        } else {
            dst_table[idx] = entry;
        }
    }
    return 0;
}

void paging_release_range(struct address_space *as, uint32_t base, uint32_t size) {
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t virt = PAGE_BASE(base) + i * PAGE_SIZE;
        pte_t *table = table_of(as, virt, 0);
        if (!table) {
            continue;
        }
        uint32_t idx = PT_INDEX(virt);
        pte_t entry = table[idx];
        if (entry & PAGE_PRESENT) {
            pmm_unref_frame(PAGE_BASE(entry));
        }
        account_entry(as, entry, 0);
        table[idx] = 0;
        if (paging_active && as == current_space) {
            invlpg(virt);
        }
    }
}

int paging_reserve_tables(struct address_space *as, uint32_t base, uint32_t size) {
    uint32_t first = PDE_INDEX(PAGE_BASE(base));
    uint32_t last = PDE_INDEX(PAGE_BASE(base + size - 1));
    int created = 0;

    for (uint32_t pdi = first; pdi <= last && pdi < RESERVED_PDE; pdi++) {
        if (!table_of(as, pdi << 21, 1)) {
            return -1;
        }
        created++;
    }
    return created;
}

/* A new space gets its own PML4, PDPT and page directories, copied from the
 * kernel's so the kernel page tables underneath stay shared. */
int paging_space_create(struct address_space *as, int id) {
    uint32_t pml4 = pmm_alloc_frame();
    uint32_t pdpt = pmm_alloc_frame();
    if (!pml4 || !pdpt) {
        if (pml4) pmm_free_frame(pml4);
        if (pdpt) pmm_free_frame(pdpt);
        return -1;
    }
    pte_t *k_pdpt = TBL(PAGE_BASE(kernel_space.pd[0]));
    memset(TBL(pml4), 0, PAGE_SIZE);
    memcpy(TBL(pdpt), k_pdpt, PAGE_SIZE);
    TBL(pml4)[0] = pdpt | PDE_FLAGS;

    for (uint32_t i = 0; i < 4; i++) {
        if (!(k_pdpt[i] & PAGE_PRESENT)) {
            continue;
        }
        uint32_t pd = pmm_alloc_frame();
        if (!pd) {
            for (uint32_t j = 0; j < i; j++) {
                if (TBL(pdpt)[j] & PAGE_PRESENT) pmm_free_frame(PAGE_BASE(TBL(pdpt)[j]));
            }
            pmm_free_frame(pdpt);
            pmm_free_frame(pml4);
            return -1;
        }
        memcpy(TBL(pd), TBL(PAGE_BASE(k_pdpt[i])), PAGE_SIZE);
        TBL(pdpt)[i] = pd | PDE_FLAGS;
    }

    as->pd = TBL(pml4);
    as->pd_phys = pml4;
    as->tables = 0;
    as->mapped = 0;
    as->id = id;
    as->in_use = 1;
    return 0;
}

void paging_space_destroy(struct address_space *as) {
    if (!as || !as->in_use || as == &kernel_space) {
        return;
    }
    if (as == current_space) {
        paging_space_switch(&kernel_space);
    }
    pte_t *pdpt = TBL(PAGE_BASE(as->pd[0]));
    pte_t *k_pdpt = TBL(PAGE_BASE(kernel_space.pd[0]));

    for (uint32_t i = 0; i < 4; i++) {
        if (!(pdpt[i] & PAGE_PRESENT)) {
            continue;
        }
        pte_t *pd = TBL(PAGE_BASE(pdpt[i]));
        pte_t *k_pd = (k_pdpt[i] & PAGE_PRESENT) ? TBL(PAGE_BASE(k_pdpt[i])) : NULL;

        for (uint32_t j = 0; j < PT_ENTRIES; j++) {
            pte_t pde = pd[j];
            if (!(pde & PAGE_PRESENT)) {
                continue;
            }
            if (i * PT_ENTRIES + j >= RESERVED_PDE) {
                continue;
            }
            if (k_pd && PAGE_BASE(pde) == PAGE_BASE(k_pd[j])) {
                continue;
            }
            pte_t *table = TBL(PAGE_BASE(pde));
            for (uint32_t k = 0; k < PT_ENTRIES; k++) {
                if (table[k] & PAGE_PRESENT) {
                    pmm_unref_frame(PAGE_BASE(table[k]));
                }
            }
            pmm_free_frame(PAGE_BASE(pde));
            if (table_count) {
                table_count--;
            }
        }
        pmm_free_frame(PAGE_BASE(pdpt[i]));
    }
    pmm_free_frame(PAGE_BASE(as->pd[0]));
    pmm_free_frame(as->pd_phys);
    as->in_use = 0;
    as->pd = NULL;
    as->pd_phys = 0;
    as->tables = 0;
    as->mapped = 0;
}

void paging_space_switch(struct address_space *as) {
    if (!as || !as->pd_phys) {
        return;
    }
    current_space = as;
    set_cr3(as->pd_phys);
}

struct address_space *paging_kernel_space(void) {
    return &kernel_space;
}

struct address_space *paging_current_space(void) {
    return current_space;
}

void paging_init(uint32_t mem_top) {
    uint32_t pd_frame = pmm_alloc_frame();

    kernel_space.pd = TBL(pd_frame);
    kernel_space.pd_phys = pd_frame;
    kernel_space.tables = 0;
    kernel_space.mapped = 0;
    kernel_space.id = 0;
    kernel_space.in_use = 1;
    current_space = &kernel_space;
    mapped_pages = 0;
    table_count = 0;
    paging_active = 0;
    memset(kernel_space.pd, 0, PAGE_SIZE);

    global_pages = cpu_supports_pge();

    uint32_t base_flags = PAGE_PRESENT | PAGE_RW;
    if (global_pages) {
        base_flags |= PAGE_GLOBAL;
    }

    uint32_t ktext = (uint32_t)&text_start;
    uint32_t ktext_end = (uint32_t)&text_end;
    uint32_t krodata = (uint32_t)&rodata_start;
    uint32_t krodata_end = (uint32_t)&rodata_end;

    for (uint32_t addr = PAGE_SIZE; addr < mem_top; addr += PAGE_SIZE) {
        uint32_t flags = base_flags;
        if ((addr >= ktext && addr < ktext_end) || (addr >= krodata && addr < krodata_end)) {
            flags &= ~PAGE_RW;
        }
        if (paging_map_in(&kernel_space, addr, addr, flags) != 0) {
            break;
        }
        identity_top = addr + PAGE_SIZE;
    }

    set_cr3(pd_frame);
    if (global_pages) {
        enable_global_pages();
    }
    enable_paging_bits();
    paging_active = 1;
}

uint32_t paging_fault_address(void) {
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    return cr2 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)cr2;
}

uint32_t paging_directory_phys(void) {
    return current_space ? current_space->pd_phys : 0;
}

uint32_t paging_mapped_mb(void) {
    return identity_top / (1024u * 1024u);
}

uint32_t paging_mapped_pages(void) {
    return mapped_pages;
}

uint32_t paging_table_count(void) {
    return table_count;
}

uint32_t paging_identity_top(void) {
    return identity_top;
}

int paging_global_pages(void) {
    return global_pages;
}
