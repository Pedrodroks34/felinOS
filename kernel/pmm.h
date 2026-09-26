#ifndef FELINOS_PMM_H
#define FELINOS_PMM_H

#include <stdint.h>

#define PMM_FRAME_SIZE 4096u

void pmm_init(uint32_t mem_top);
void pmm_reserve_region(uint32_t phys_start, uint32_t phys_end);
uint32_t pmm_alloc_frame(void);
uint32_t pmm_alloc_zeroed(void);
uint32_t pmm_alloc_contiguous(uint32_t frames);
void pmm_free_frame(uint32_t phys);
void pmm_ref_frame(uint32_t phys);
uint32_t pmm_unref_frame(uint32_t phys);
uint32_t pmm_frame_refs(uint32_t phys);
uint32_t pmm_total_frames(void);
uint32_t pmm_used_frames(void);
uint32_t pmm_free_frames(void);
uint32_t pmm_peak_frames(void);
uint32_t pmm_alloc_count(void);
uint32_t pmm_free_count(void);
uint32_t pmm_managed_top(void);

#endif
