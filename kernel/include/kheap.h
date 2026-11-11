#pragma once
// Kernel heap: kmalloc and friends, for kernel data structures of any size.
//
// The heap has its own range of virtual memory, [KHEAP_BASE, KHEAP_BASE +
// KHEAP_MAX_SIZE). It starts empty, maps fresh pages from the PMM as it
// grows, and gives whole free pages at its top back to the PMM.
//
// Debug aids, always on:
//   - new memory is not zeroed: it holds KHEAP_POISON (use kcalloc for zeroes)
//   - freed memory is filled with KHEAP_POISON; the allocator panics if a
//     write to freed memory has changed it by the time it is handed out again
//   - every allocation is followed by a redzone of KHEAP_REDZONE bytes that
//     kfree/krealloc check, so writing past the end panics
//   - kfree panics on a double free, a pointer the heap never handed out, or
//     a block header that has been overwritten
//
// Threads can share it: every call runs with preemption disabled. Not safe
// to call from interrupt handlers or several CPUs yet.

#include <stddef.h>
#include <stdint.h>

#define KHEAP_BASE     ((uint64_t)0xffffc00000000000)
#define KHEAP_MAX_SIZE ((uint64_t)64 << 30)
#define KHEAP_POISON   0x6b
#define KHEAP_REDZONE  0xbb

// Check that the heap's range is free in the kernel's page tables. Needs the
// VMM. Maps nothing yet.
void kheap_init(void);

// `size` bytes, 16-byte aligned. NULL if `size` is 0 or memory runs out.
void *kmalloc(size_t size);

// Like kmalloc, aligned to `align`: a power of two from 16 to 2 MiB.
void *kmalloc_aligned(size_t size, size_t align);

// `count * size` zeroed bytes. NULL on overflow, like kmalloc otherwise.
void *kcalloc(size_t count, size_t size);

// Resize `ptr` (from any of the above), keeping its contents up to the
// smaller size. NULL `ptr` allocates; a `size` of 0 frees and returns NULL.
// May move the block; the result is only 16-byte aligned. On failure,
// returns NULL and `ptr` is left alone.
void *krealloc(void *ptr, size_t size);

// Free a block from any of the above. NULL is ignored.
void kfree(void *ptr);

struct kheap_stats {
    uint64_t heap_bytes;    // mapped for the heap
    uint64_t used_bytes;    // asked for by live allocations
    uint64_t free_bytes;    // in free blocks (headers included)
    uint64_t allocations;   // live allocations
    uint64_t free_blocks;
    uint64_t largest_free;  // biggest free block (header included)
    uint64_t total_allocs;  // since boot
    uint64_t total_frees;
};

struct kheap_stats kheap_get_stats(void);

// Walk every block and check headers, redzones, poison and the free list.
// Panics on the first problem.
void kheap_check(void);

// Boot self-test: edge cases, alignment, large blocks, krealloc, then a long
// random sequence of mixed-size allocations with content checks, ending with
// no leaks (one free block, every page accounted for). Panics on failure.
void kheap_self_test(void);
