#pragma once
// Basic memory definitions shared by the memory managers.

#include <stdint.h>

#define PAGE_SIZE  ((uint64_t)4096)
#define PAGE_SHIFT 12

#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))
#define ALIGN_UP(x, a)   ALIGN_DOWN((x) + (a) - 1, a)

// Limine maps all physical memory at this offset (the "higher-half direct
// map", HHDM). Set by pmm_init().
extern uint64_t hhdm_offset;

static inline void *phys_to_virt(uint64_t phys) { return (void *)(phys + hhdm_offset); }
static inline uint64_t virt_to_phys(const void *hhdm_ptr) { return (uint64_t)hhdm_ptr - hhdm_offset; }
