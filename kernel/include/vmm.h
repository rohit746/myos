#pragma once
// Virtual memory manager: the kernel's own 4-level page tables.
//
// Addresses passed in are 4 KiB aligned. Safe to call from several CPUs at
// once (one lock covers every space), not from interrupt handlers.

#include <stdbool.h>
#include <stdint.h>

struct limine_memmap_response;

// One set of page tables (one address space).
struct vmm_space {
    uint64_t pml4; // physical address of the top-level table
};

// The kernel's page tables, live once vmm_init() returns.
extern struct vmm_space kernel_space;

// vmm_map() flags. With none of them a page is kernel-only, read-only, not
// executable and write-back cached.
#define VMM_WRITE (1u << 0)
#define VMM_EXEC  (1u << 1)
#define VMM_USER  (1u << 2)
#define VMM_WC    (1u << 3) // write-combining (framebuffers)
#define VMM_UC    (1u << 4) // uncached (device registers)

// Build the kernel's page tables and switch CR3 to them:
//   - the kernel image, each section with its own permissions
//     (.text r-x, .rodata r--, .data/.bss rw-, Limine requests rw-)
//   - the HHDM: every memory-map region Limine mapped there, at the same
//     offset, with 1 GiB / 2 MiB pages where they fit; framebuffers
//     write-combining
// Also turns on EFER.NXE, programs the PAT and keeps CR0.WP set. Needs the
// physical memory manager. Limine's page tables are unused afterwards.
void vmm_init(struct limine_memmap_response *memmap, uint64_t kernel_phys, uint64_t kernel_virt);

// The same CPU settings on an application processor, then load the
// kernel's page tables there.
void vmm_init_ap(void);

// Address spaces for user processes. Each has its own lower half (PML4
// entries 0-255) and shares the kernel half (256-511) with kernel_space:
// vmm_init() fills every kernel-half PML4 entry, so the tables below them
// are the same in every space, and kernel mappings made later show up in
// all of them.
//
// vmm_space_create: an empty lower half. False if there is no memory.
// vmm_space_destroy: free the lower half's tables and every page mapped
// there (the pages belong to the space). It must not be the active space.
bool vmm_space_create(struct vmm_space *space);
void vmm_space_destroy(struct vmm_space *space);

// Whether user mode may read (and with `write`, write) the page holding
// `virt` in `space`: every level present and user-accessible.
bool vmm_user_accessible(struct vmm_space *space, uint64_t virt, bool write);

// Map one 4 KiB page. Panics if `virt` is already mapped.
void vmm_map(struct vmm_space *space, uint64_t virt, uint64_t phys, uint32_t flags);

// Unmap one 4 KiB page and flush it from the TLB, this CPU's and every other
// one's that may hold it (a TLB shootdown, smp.h). Panics if it isn't
// mapped. Page tables left empty are not freed yet.
void vmm_unmap(struct vmm_space *space, uint64_t virt);

// The same for `pages` pages from `virt` on, with one shootdown for them
// all. Stores each page's physical address in phys[] (the caller frees
// them: only now is nothing using them).
void vmm_unmap_range(struct vmm_space *space, uint64_t virt, uint64_t pages, uint64_t *phys);

// Map `size` bytes of device registers at physical `phys` (any alignment),
// uncached and writable, and return their virtual address. The mapping lives
// in its own window, [VMM_MMIO_BASE, VMM_MMIO_BASE + VMM_MMIO_SIZE), and is
// never removed. Panics when the window is full.
#define VMM_MMIO_BASE ((uint64_t)0xffffd00000000000)
#define VMM_MMIO_SIZE ((uint64_t)1 << 30)
volatile void *vmm_map_mmio(uint64_t phys, uint64_t size);

// Translate `virt` (any byte, any page size). False if it isn't mapped.
bool vmm_virt_to_phys(struct vmm_space *space, uint64_t virt, uint64_t *phys);

struct vmm_stats {
    uint64_t table_pages;                    // pages used by the page tables
    uint64_t pages_1g, pages_2m, pages_4k;   // leaf mappings, by size
    bool nx;                                 // no-execute is supported and on
    uint64_t hhdm_end; // physical address just past the highest page in the HHDM
};

struct vmm_stats vmm_get_stats(void);

// Boot self-test: map/translate/unmap a page, then check that writing to
// .text and .rodata, executing from .data and touching an unmapped page all
// page-fault with the right error code. Panics on failure.
void vmm_self_test(void);
