#pragma once
// Physical memory manager: hands out and takes back 4 KiB page frames.
//
// Addresses are physical. Reach the memory through phys_to_virt() (memory.h).
// Not safe to call from interrupt handlers or several CPUs yet.

#include <stddef.h>
#include <stdint.h>

struct limine_memmap_response;

// Take over every USABLE region of the bootloader's memory map.
void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm);

// One page. Returns 0 when memory is exhausted (page 0 is never handed out).
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t phys);

// `count` physically contiguous pages (for DMA buffers). Returns the first
// page's address, or 0 if no run that long is free.
uint64_t pmm_alloc_contiguous(size_t count);

// Like pmm_alloc_contiguous, but every page lies below physical address
// `limit` (for devices that can only reach the first 4 GiB, say).
uint64_t pmm_alloc_contiguous_below(size_t count, uint64_t limit);
void pmm_free_contiguous(uint64_t phys, size_t count);

// Hand BOOTLOADER_RECLAIMABLE memory to the allocator. Only call this once
// nothing uses Limine's memory any more: its responses (copy what you need
// first), the page tables it built and the boot stack.
void pmm_reclaim_bootloader_memory(void);

struct pmm_stats {
    uint64_t total_pages; // every page the allocator manages
    uint64_t free_pages;
    uint64_t reserved_pages; // kept by the allocator: its bitmaps, and page 0
};

struct pmm_stats pmm_get_stats(void);

// Boot self-test: allocate thousands of pages, fill them with patterns,
// check them, free them and check the free count comes back. Panics on failure.
void pmm_self_test(struct limine_memmap_response *memmap);
