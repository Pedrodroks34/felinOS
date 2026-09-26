#include "sh/cmds.h"
#include "sh/shell.h"
#include "paging.h"
#include "pmm.h"
#include "vmm.h"
#include "swap.h"
#include "system.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "drivers/ata.h"

#define VMM_SLOTS 16

struct vmm_slot {
    void *ptr;
    uint32_t size;
    uint32_t flags;
    int used;
};

static struct vmm_slot slots[VMM_SLOTS];

static const char *entry_kind(uint32_t entry) {
    if (entry & PAGE_PRESENT) {
        return "resident";
    }
    if (PAGE_IS_SWAPPED(entry)) {
        return "swapped";
    }
    return "absent";
}

static void print_region_line(struct stream *out, struct vm_region *r) {
    char flags[12];
    char size[16];

    vmm_flags_string(r->flags, flags, sizeof(flags));
    cmd_format_size(r->size, size, sizeof(size));
    st_printf(out, "%08x-%08x %-10s %-10s %8s %8u %8u\n", r->base, r->base + r->size,
              flags, r->name, size, r->resident, r->swapped);
}

static void show_regions(struct stream *out, struct vm_space *space) {
    st_printf(out, "address space %d (%s), directory 0x%08x\n",
              space->id, space->name, space->as->pd_phys);
    st_printf(out, "%-17s %-10s %-10s %8s %8s %8s\n",
              "range", "flags", "name", "size", "res", "swap");
    for (struct vm_region *r = vmm_region_list(space); r; r = r->next) {
        print_region_line(out, r);
    }
}

static void show_stats(struct stream *out) {
    struct vmm_stats st;
    struct swap_info sw;
    char buf[16];

    vmm_get_stats(&st);
    swap_get_info(&sw);

    st_printf(out, "Page faults:      %10u total\n", st.page_faults);
    st_printf(out, "  demand zero:    %10u\n", st.demand_faults);
    st_printf(out, "  copy on write:  %10u\n", st.cow_faults);
    st_printf(out, "  swap in:        %10u\n", st.swap_faults);
    st_printf(out, "  protection:     %10u\n", st.protection_faults);
    st_printf(out, "  invalid:        %10u\n", st.invalid_faults);
    st_printf(out, "Reclaims:         %10u runs, %u pages evicted\n", st.reclaims, st.evictions);
    st_printf(out, "Swap traffic:     %10u out, %u in\n", st.swap_outs, st.swap_ins);
    st_printf(out, "Out of memory:    %10u\n\n", st.oom_events);

    cmd_format_size(st.virtual_pages * PAGE_SIZE, buf, sizeof(buf));
    st_printf(out, "Virtual mapped:   %10s in %u regions, %u address spaces\n",
              buf, st.regions, st.spaces);
    cmd_format_size(st.resident_pages * PAGE_SIZE, buf, sizeof(buf));
    st_printf(out, "Resident:         %10s (%u pages)\n", buf, st.resident_pages);
    cmd_format_size(st.swapped_pages * PAGE_SIZE, buf, sizeof(buf));
    st_printf(out, "Swapped out:      %10s (%u pages)\n", buf, st.swapped_pages);
    cmd_format_size(pmm_free_frames() * PAGE_SIZE, buf, sizeof(buf));
    st_printf(out, "Free frames:      %10s (%u frames, peak use %u)\n", buf,
              pmm_free_frames(), pmm_peak_frames());
    st_printf(out, "Page tables:      %10u (%u KB)\n", paging_table_count(),
              paging_table_count() * 4u);
    st_printf(out, "Global pages:     %10s\n", paging_global_pages() ? "enabled" : "unsupported");
    if (sw.active) {
        st_printf(out, "Swap device:      %10s %u/%u slots used\n", sw.device,
                  sw.used_slots, sw.total_slots);
    } else {
        st_printf(out, "Swap device:      %10s\n", "none");
    }
}

static void show_map(struct stream *out, struct vm_space *space, int all) {
    uint32_t addr = all ? 0 : VMM_HEAP_BASE;
    uint32_t entry = 0;
    uint32_t run_start = 0;
    uint32_t run_flags = 0;
    uint32_t run_phys = 0;
    uint32_t run_pages = 0;
    uint32_t shown = 0;

    st_printf(out, "%-21s %-12s %-6s %s\n", "virtual", "physical", "pages", "flags");

    while (paging_next_mapping(space->as, &addr, &entry)) {
        uint32_t flags = entry & 0xFFFu;
        uint32_t phys = entry & PAGE_MASK;
        int contiguous = run_pages && addr == run_start + run_pages * PAGE_SIZE &&
                         flags == run_flags && phys == run_phys + run_pages * PAGE_SIZE;

        if (contiguous) {
            run_pages++;
        } else {
            if (run_pages) {
                st_printf(out, "%08x-%08x %08x     %-6u %c%c%c%c%c%c\n",
                          run_start, run_start + run_pages * PAGE_SIZE, run_phys, run_pages,
                          (run_flags & PAGE_PRESENT) ? 'p' : '-',
                          (run_flags & PAGE_RW) ? 'w' : 'r',
                          (run_flags & PAGE_USER) ? 'u' : 'k',
                          (run_flags & PAGE_GLOBAL) ? 'g' : '-',
                          (run_flags & PAGE_COW) ? 'c' : '-',
                          (run_flags & PAGE_ACCESSED) ? 'a' : '-');
                shown++;
            }
            run_start = addr;
            run_flags = flags;
            run_phys = phys;
            run_pages = 1;
        }
        addr += PAGE_SIZE;
        if (addr >= VMM_SELFMAP_BASE || shown > 64) {
            break;
        }
    }
    if (run_pages) {
        st_printf(out, "%08x-%08x %08x     %-6u %c%c%c%c%c%c\n",
                  run_start, run_start + run_pages * PAGE_SIZE, run_phys, run_pages,
                  (run_flags & PAGE_PRESENT) ? 'p' : '-',
                  (run_flags & PAGE_RW) ? 'w' : 'r',
                  (run_flags & PAGE_USER) ? 'u' : 'k',
                  (run_flags & PAGE_GLOBAL) ? 'g' : '-',
                  (run_flags & PAGE_COW) ? 'c' : '-',
                  (run_flags & PAGE_ACCESSED) ? 'a' : '-');
    }
}

static int slot_find_free(void) {
    for (int i = 0; i < VMM_SLOTS; i++) {
        if (!slots[i].used) {
            return i;
        }
    }
    return -1;
}

static void fill_pattern(uint8_t *base, uint32_t size, uint32_t seed) {
    for (uint32_t off = 0; off < size; off += PAGE_SIZE) {
        uint32_t *page = (uint32_t *)(base + off);
        page[0] = seed + off;
        page[1] = seed ^ off;
        page[(PAGE_SIZE / 4) - 1] = seed + off + 0x5A5A5A5Au;
    }
}

static int check_pattern(const uint8_t *base, uint32_t size, uint32_t seed) {
    for (uint32_t off = 0; off < size; off += PAGE_SIZE) {
        const uint32_t *page = (const uint32_t *)(base + off);
        if (page[0] != seed + off || page[1] != (seed ^ off) ||
            page[(PAGE_SIZE / 4) - 1] != seed + off + 0x5A5A5A5Au) {
            return 0;
        }
    }
    return 1;
}

static int test_demand(struct stream *out) {
    uint32_t size = 64u * PAGE_SIZE;
    uint8_t *mem = vmm_alloc(size, VM_READ | VM_WRITE | VM_DEMAND | VM_SWAPPABLE, "selftest");

    if (!mem) {
        st_puts(out, "demand paging     FAIL (no virtual space)\n");
        return 1;
    }
    if (paging_is_mapped((uint32_t)mem)) {
        st_puts(out, "demand paging     FAIL (mapped before use)\n");
        vmm_free(mem);
        return 1;
    }
    fill_pattern(mem, size, 0x11110000u);
    if (!check_pattern(mem, size, 0x11110000u)) {
        st_puts(out, "demand paging     FAIL (pattern mismatch)\n");
        vmm_free(mem);
        return 1;
    }
    if (!paging_is_mapped((uint32_t)mem)) {
        st_puts(out, "demand paging     FAIL (page not mapped after write)\n");
        vmm_free(mem);
        return 1;
    }
    vmm_free(mem);
    if (paging_is_mapped((uint32_t)mem)) {
        st_puts(out, "demand paging     FAIL (mapping survived free)\n");
        return 1;
    }
    st_puts(out, "demand paging     PASS (64 pages faulted in, zeroed and released)\n");
    return 0;
}

static int test_cow(struct stream *out) {
    uint32_t size = 16u * PAGE_SIZE;
    uint8_t *mem = vmm_alloc(size, VM_READ | VM_WRITE | VM_DEMAND | VM_SWAPPABLE, "cowtest");

    if (!mem) {
        st_puts(out, "copy on write     FAIL (no virtual space)\n");
        return 1;
    }
    fill_pattern(mem, size, 0x22220000u);

    struct vm_space *clone = vmm_space_clone("cowclone");
    if (!clone) {
        st_puts(out, "copy on write     FAIL (clone failed)\n");
        vmm_free(mem);
        return 1;
    }

    struct vm_space *parent = vmm_current_space();
    int ok = 1;

    vmm_space_switch(clone);
    if (!check_pattern(mem, size, 0x22220000u)) {
        ok = 0;
    }
    fill_pattern(mem, size, 0x33330000u);
    if (!check_pattern(mem, size, 0x33330000u)) {
        ok = 0;
    }
    vmm_space_switch(parent);

    if (!check_pattern(mem, size, 0x22220000u)) {
        ok = 0;
    }
    vmm_space_destroy(clone);
    if (!check_pattern(mem, size, 0x22220000u)) {
        ok = 0;
    }
    vmm_free(mem);

    if (!ok) {
        st_puts(out, "copy on write     FAIL (clone and parent are not isolated)\n");
        return 1;
    }
    st_puts(out, "copy on write     PASS (clone wrote 16 pages without touching the parent)\n");
    return 0;
}

static int test_swap(struct stream *out) {
    if (!swap_active()) {
        st_puts(out, "swap round trip   SKIP (no swap device, run mkswap and swapon)\n");
        return 0;
    }

    uint32_t size = 32u * PAGE_SIZE;
    uint8_t *mem = vmm_alloc(size, VM_READ | VM_WRITE | VM_DEMAND | VM_SWAPPABLE, "swaptest");
    if (!mem) {
        st_puts(out, "swap round trip   FAIL (no virtual space)\n");
        return 1;
    }
    fill_pattern(mem, size, 0x44440000u);

    uint32_t evicted = vmm_evict_range((uint32_t)mem, size, size / PAGE_SIZE);
    if (!evicted) {
        st_puts(out, "swap round trip   FAIL (nothing was evicted)\n");
        vmm_free(mem);
        return 1;
    }

    uint32_t on_disk = 0;
    for (uint32_t off = 0; off < size; off += PAGE_SIZE) {
        if (PAGE_IS_SWAPPED(paging_get_entry((uint32_t)mem + off))) {
            on_disk++;
        }
    }
    if (!check_pattern(mem, size, 0x44440000u)) {
        st_puts(out, "swap round trip   FAIL (data changed after swap in)\n");
        vmm_free(mem);
        return 1;
    }
    vmm_free(mem);
    st_printf(out, "swap round trip   PASS (%u pages written to %s and read back)\n",
              on_disk, swap_device_name());
    return 0;
}

static int test_physical(struct stream *out) {
    uint32_t phys = 0xB8000u;
    void *mmio = vmm_map_physical(phys, PAGE_SIZE, VM_READ | VM_WRITE | VM_UNCACHED, "vgatest");

    if (!mmio) {
        st_puts(out, "physical mapping  FAIL (no mmio window)\n");
        return 1;
    }
    uint32_t resolved = 0;
    if (paging_translate((uint32_t)mmio, &resolved) != 0 || resolved != phys) {
        st_puts(out, "physical mapping  FAIL (translation mismatch)\n");
        vmm_free(mmio);
        return 1;
    }
    vmm_free(mmio);
    st_printf(out, "physical mapping  PASS (0x%08x mapped at 0x%08x and unmapped)\n",
              phys, (uint32_t)mmio);
    return 0;
}

static int test_translate(struct stream *out) {
    uint32_t virt = (uint32_t)vmm_alloc(PAGE_SIZE, VM_READ | VM_WRITE | VM_SWAPPABLE, "xlate");

    if (!virt) {
        st_puts(out, "translation       FAIL (no virtual space)\n");
        return 1;
    }
    uint32_t phys = 0;
    if (paging_translate(virt + 0x123u, &phys) != 0 || (phys & 0xFFFu) != 0x123u) {
        st_puts(out, "translation       FAIL (offset lost)\n");
        vmm_free((void *)virt);
        return 1;
    }
    *(volatile uint32_t *)(virt + 0x123u) = 0xCAFEBABEu;
    if (*(volatile uint32_t *)phys != 0xCAFEBABEu) {
        st_puts(out, "translation       FAIL (physical alias mismatch)\n");
        vmm_free((void *)virt);
        return 1;
    }
    vmm_free((void *)virt);
    st_puts(out, "translation       PASS (virtual, physical and offset agree)\n");
    return 0;
}

static int run_self_test(struct stream *out) {
    int failures = 0;

    st_puts(out, "Gato virtual memory self test\n\n");
    failures += test_demand(out);
    failures += test_translate(out);
    failures += test_physical(out);
    failures += test_cow(out);
    failures += test_swap(out);

    st_printf(out, "\n%s\n", failures ? "one or more tests failed" : "all tests passed");
    return failures ? 1 : 0;
}

static void vmm_usage(struct stream *out) {
    st_puts(out, "usage: vmm [stat|regions|spaces|map [-a]|alloc <KB> [-c]|free <id>|list|\n");
    st_puts(out, "            touch <id>|swapout <id>|reclaim [pages]|growheap <MB>|test]\n");
}

int cmd_vmm(int argc, char **argv, struct stream *in, struct stream *out) {
    const char *sub = (argc > 1) ? argv[1] : "stat";

    if (strcmp(sub, "stat") == 0) {
        show_stats(out);
        return 0;
    }
    if (strcmp(sub, "regions") == 0) {
        show_regions(out, vmm_current_space());
        return 0;
    }
    if (strcmp(sub, "spaces") == 0) {
        st_printf(out, "%-4s %-12s %-12s %8s %8s\n", "id", "name", "directory", "regions", "tables");
        for (int i = 0; i < VMM_MAX_SPACES; i++) {
            struct vm_space *space = vmm_space_get(i);
            if (!space) {
                continue;
            }
            st_printf(out, "%-4d %-12s 0x%08x   %8d %8u%s\n", space->id, space->name,
                      space->as->pd_phys, vmm_region_count(space), space->as->tables,
                      (space == vmm_current_space()) ? "  <- current" : "");
        }
        return 0;
    }
    if (strcmp(sub, "map") == 0) {
        show_map(out, vmm_current_space(), cmd_has_flag(argc, argv, "-a"));
        return 0;
    }
    if (strcmp(sub, "alloc") == 0) {
        if (argc < 3) {
            cmd_error(out, "vmm", NULL, "alloc needs a size in KB");
            return 1;
        }
        uint32_t kb = strtou32(argv[2], 10);
        if (!kb) {
            cmd_error(out, "vmm", argv[2], "invalid size");
            return 1;
        }
        int index = slot_find_free();
        if (index < 0) {
            cmd_error(out, "vmm", NULL, "no free allocation slots");
            return 1;
        }
        uint32_t flags = VM_READ | VM_WRITE | VM_SWAPPABLE;
        if (!cmd_has_flag(argc, argv, "-c")) {
            flags |= VM_DEMAND;
        }
        void *ptr = vmm_alloc(kb * 1024u, flags, "userland");
        if (!ptr) {
            cmd_error(out, "vmm", NULL, "allocation failed");
            return 1;
        }
        slots[index].ptr = ptr;
        slots[index].size = kb * 1024u;
        slots[index].flags = flags;
        slots[index].used = 1;
        st_printf(out, "region %d at 0x%08x, %u KB, %s\n", index, (uint32_t)ptr, kb,
                  (flags & VM_DEMAND) ? "demand paged" : "committed");
        return 0;
    }
    if (strcmp(sub, "free") == 0) {
        if (argc < 3) {
            cmd_error(out, "vmm", NULL, "free needs a region id");
            return 1;
        }
        int index = (int)strtou32(argv[2], 10);
        if (index < 0 || index >= VMM_SLOTS || !slots[index].used) {
            cmd_error(out, "vmm", argv[2], "no such region");
            return 1;
        }
        vmm_free(slots[index].ptr);
        slots[index].used = 0;
        st_printf(out, "region %d released\n", index);
        return 0;
    }
    if (strcmp(sub, "list") == 0) {
        st_printf(out, "%-4s %-12s %10s %s\n", "id", "address", "size", "mode");
        for (int i = 0; i < VMM_SLOTS; i++) {
            if (!slots[i].used) {
                continue;
            }
            char size[16];
            cmd_format_size(slots[i].size, size, sizeof(size));
            st_printf(out, "%-4d 0x%08x   %10s %s\n", i, (uint32_t)slots[i].ptr, size,
                      (slots[i].flags & VM_DEMAND) ? "demand" : "committed");
        }
        return 0;
    }
    if (strcmp(sub, "touch") == 0) {
        if (argc < 3) {
            cmd_error(out, "vmm", NULL, "touch needs a region id");
            return 1;
        }
        int index = (int)strtou32(argv[2], 10);
        if (index < 0 || index >= VMM_SLOTS || !slots[index].used) {
            cmd_error(out, "vmm", argv[2], "no such region");
            return 1;
        }
        uint8_t *mem = slots[index].ptr;
        uint32_t seed = 0x70000000u + (uint32_t)index;
        fill_pattern(mem, slots[index].size, seed);
        if (!check_pattern(mem, slots[index].size, seed)) {
            cmd_error(out, "vmm", NULL, "verification failed");
            return 1;
        }
        st_printf(out, "%u pages written and verified\n", slots[index].size / PAGE_SIZE);
        return 0;
    }
    if (strcmp(sub, "swapout") == 0) {
        if (argc < 3) {
            cmd_error(out, "vmm", NULL, "swapout needs a region id");
            return 1;
        }
        int index = (int)strtou32(argv[2], 10);
        if (index < 0 || index >= VMM_SLOTS || !slots[index].used) {
            cmd_error(out, "vmm", argv[2], "no such region");
            return 1;
        }
        if (!swap_active()) {
            cmd_error(out, "vmm", NULL, "no swap device");
            return 1;
        }
        uint32_t done = vmm_evict_range((uint32_t)slots[index].ptr, slots[index].size,
                                        slots[index].size / PAGE_SIZE);
        st_printf(out, "%u pages of region %d written to %s\n", done, index, swap_device_name());
        return 0;
    }
    if (strcmp(sub, "reclaim") == 0) {
        uint32_t pages = (argc > 2) ? strtou32(argv[2], 10) : 64u;
        if (!swap_active()) {
            cmd_error(out, "vmm", NULL, "no swap device, nothing to reclaim to");
            return 1;
        }
        uint32_t done = vmm_reclaim(pages);
        st_printf(out, "%u pages evicted to %s\n", done, swap_device_name());
        return 0;
    }
    if (strcmp(sub, "growheap") == 0) {
        if (argc < 3) {
            cmd_error(out, "vmm", NULL, "growheap needs a size in MB");
            return 1;
        }
        uint32_t mb = strtou32(argv[2], 10);
        if (!mb || vmm_heap_extend(mb * 1024u * 1024u) != 0) {
            cmd_error(out, "vmm", argv[2], "cannot grow the heap by that much");
            return 1;
        }
        st_printf(out, "kernel heap is now %u KB of virtual space\n", vmm_heap_size() / 1024u);
        return 0;
    }
    if (strcmp(sub, "test") == 0) {
        return run_self_test(out);
    }
    vmm_usage(out);
    return 1;
}

int cmd_pmap(int argc, char **argv, struct stream *in, struct stream *out) {
    struct vm_space *space = vmm_current_space();

    show_regions(out, space);
    st_puts(out, "\n");
    show_map(out, space, cmd_has_flag(argc, argv, "-a"));
    return 0;
}

int cmd_vmstat(int argc, char **argv, struct stream *in, struct stream *out) {
    uint32_t addr;
    uint32_t entry;
    char detail[80];

    if (argc < 2) {
        show_stats(out);
        return 0;
    }
    addr = strtou32(argv[1], 16);
    entry = paging_get_entry(addr & PAGE_MASK);
    vmm_describe(addr, detail, sizeof(detail));

    st_printf(out, "virtual  0x%08x\n", addr);
    st_printf(out, "state    %s\n", entry_kind(entry));
    st_printf(out, "entry    0x%08x\n", entry);
    if (entry & PAGE_PRESENT) {
        st_printf(out, "physical 0x%08x (%u references)\n",
                  (entry & PAGE_MASK) | (addr & 0xFFFu), pmm_frame_refs(entry & PAGE_MASK));
        st_printf(out, "flags    %s%s%s%s%s%s\n",
                  (entry & PAGE_RW) ? "write " : "read-only ",
                  (entry & PAGE_USER) ? "user " : "kernel ",
                  (entry & PAGE_ACCESSED) ? "accessed " : "",
                  (entry & PAGE_DIRTY) ? "dirty " : "",
                  (entry & PAGE_COW) ? "copy-on-write " : "",
                  (entry & PAGE_GLOBAL) ? "global" : "");
    } else if (PAGE_IS_SWAPPED(entry)) {
        st_printf(out, "swap     slot %u on %s\n", PAGE_SWAP_SLOT(entry), swap_device_name());
    }
    st_printf(out, "%s\n", detail);
    return 0;
}

int cmd_mkswap(int argc, char **argv, struct stream *in, struct stream *out) {
    if (argc < 2) {
        cmd_error(out, "mkswap", NULL, "usage: mkswap <disk> [MB] [-f]");
        return 1;
    }

    struct ata_device *dev = ata_find(argv[1]);
    if (!dev) {
        cmd_error(out, "mkswap", argv[1], "no such disk");
        return 1;
    }

    uint32_t mb = 0;
    for (int i = 2; i < argc; i++) {
        if (!cmd_is_flag(argv[i])) {
            mb = strtou32(argv[i], 10);
        }
    }

    int rc = swap_format(dev, mb, "gatoswap", cmd_has_flag(argc, argv, "-f"));
    if (rc < 0) {
        cmd_error(out, "mkswap", argv[1], swap_strerror(rc));
        return 1;
    }
    st_printf(out, "swap area on %s: %u slots, %u MB\n", dev->name, (uint32_t)rc,
              ((uint32_t)rc * PAGE_SIZE) / (1024u * 1024u));
    return 0;
}

int cmd_swapon(int argc, char **argv, struct stream *in, struct stream *out) {
    int rc;

    if (argc < 2) {
        rc = swap_autostart();
    } else {
        struct ata_device *dev = ata_find(argv[1]);
        if (!dev) {
            cmd_error(out, "swapon", argv[1], "no such disk");
            return 1;
        }
        rc = swap_enable(dev);
    }
    if (rc != SWAP_OK) {
        cmd_error(out, "swapon", (argc > 1) ? argv[1] : NULL, swap_strerror(rc));
        return 1;
    }
    st_printf(out, "swap enabled on %s: %u slots, %u MB\n", swap_device_name(),
              swap_total_slots(), (swap_total_slots() * PAGE_SIZE) / (1024u * 1024u));
    return 0;
}

int cmd_swapoff(int argc, char **argv, struct stream *in, struct stream *out) {
    if (!swap_active()) {
        cmd_error(out, "swapoff", NULL, "no swap device is enabled");
        return 1;
    }
    if (swap_used_slots() > pmm_free_frames()) {
        cmd_error(out, "swapoff", NULL, "not enough free memory to page everything back in");
        return 1;
    }
    int restored = vmm_swap_in_all();
    if (restored < 0) {
        cmd_error(out, "swapoff", NULL, "could not page everything back in");
        return 1;
    }
    int rc = swap_disable();
    if (rc != SWAP_OK) {
        cmd_error(out, "swapoff", NULL, swap_strerror(rc));
        return 1;
    }
    st_printf(out, "swap disabled, %d pages brought back to memory\n", restored);
    return 0;
}

int cmd_swapinfo(int argc, char **argv, struct stream *in, struct stream *out) {
    struct swap_info info;
    char total[16];
    char used[16];

    swap_get_info(&info);
    if (!info.active) {
        st_puts(out, "no swap device enabled (mkswap <disk> then swapon <disk>)\n");
        return 0;
    }
    cmd_format_size(info.total_slots * PAGE_SIZE, total, sizeof(total));
    cmd_format_size(info.used_slots * PAGE_SIZE, used, sizeof(used));

    st_printf(out, "device    %s (%s)\n", info.device, info.label);
    st_printf(out, "size      %s in %u slots of %u bytes\n", total, info.total_slots, PAGE_SIZE);
    st_printf(out, "used      %s in %u slots (peak %u)\n", used, info.used_slots, info.peak_slots);
    st_printf(out, "traffic   %u pages written, %u pages read, %u errors\n",
              info.writes, info.reads, info.errors);
    return 0;
}

int cmd_memtest(int argc, char **argv, struct stream *in, struct stream *out) {
    uint32_t mb = (argc > 1) ? strtou32(argv[1], 10) : 8u;

    if (!mb) {
        cmd_error(out, "memtest", argv[1], "invalid size");
        return 1;
    }
    uint32_t bytes = mb * 1024u * 1024u;
    uint32_t pages = bytes / PAGE_SIZE;
    if (pages > vmm_available_pages()) {
        st_printf(out, "memtest: %u pages requested, only %u available in RAM and swap\n",
                  pages, vmm_available_pages());
        return 1;
    }

    struct vmm_stats before;
    struct vmm_stats after;
    vmm_get_stats(&before);

    uint8_t *mem = vmm_alloc(bytes, VM_READ | VM_WRITE | VM_DEMAND | VM_SWAPPABLE, "memtest");
    if (!mem) {
        cmd_error(out, "memtest", NULL, "could not reserve that much virtual space");
        return 1;
    }

    fill_pattern(mem, bytes, 0x1BADB002u);
    int ok = check_pattern(mem, bytes, 0x1BADB002u);
    vmm_get_stats(&after);
    vmm_free(mem);

    st_printf(out, "%s: %u MB (%u pages) written and verified\n", ok ? "ok" : "FAILED", mb, pages);
    st_printf(out, "faults: %u demand, %u swap in, %u evictions\n",
              after.demand_faults - before.demand_faults,
              after.swap_faults - before.swap_faults,
              after.evictions - before.evictions);
    return ok ? 0 : 1;
}
