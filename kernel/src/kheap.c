// Kernel heap: a first-fit free list with block splitting and coalescing.
//
// The heap is one run of blocks from KHEAP_BASE up to `heap_end`, each
// starting with a 32-byte header. A block knows its own size and the size of
// the block below it, so kfree can merge a block with both neighbours.
// Free blocks are also on a doubly linked list, threaded through the first 16
// bytes of their payload. No two free blocks are ever next to each other.
//
//   used block:  [header][payload: `requested` bytes][redzone][...]
//   free block:  [header][next, prev][poison .......................]
//
// Every byte of a used block past its payload is redzone, and every byte of a
// free block past its list links is poison; kheap_check() verifies both.
// When no free block fits, the heap grows by mapping fresh PMM pages at
// `heap_end`. When the top block is free and large, its pages are unmapped
// and returned.

#include <kheap.h>
#include <kprintf.h>
#include <memory.h>
#include <pmm.h>
#include <sched.h>
#include <stdbool.h>
#include <string.h>
#include <vmm.h>

#define MAGIC_USED 0x55534544u // "USED"
#define MAGIC_FREE 0x46524545u // "FREE"

#define ALIGN       16
#define MAX_ALIGN   (2ull << 20)
#define REDZONE_MIN 16                       // bytes of redzone at least
#define GROW_MIN    (64 * 1024)              // grow by at least this much
#define TRIM_MIN    (256 * 1024)             // give back the top once this much is free

struct block {
    uint64_t size;      // the whole block, header included; a multiple of 16
    uint64_t prev_size; // size of the block just below; 0 for the first block
    uint64_t requested; // bytes the caller asked for; 0 in a free block
    uint32_t unused;
    uint32_t magic;     // last, right below the payload, so small underflows hit it
};

struct links {
    struct block *next, *prev;
};

#define HDR       sizeof(struct block)
#define MIN_BLOCK (HDR + sizeof(struct links)) // smallest free block
_Static_assert(HDR % ALIGN == 0, "payloads must stay 16-byte aligned");

static uintptr_t heap_end = KHEAP_BASE;
static struct block *top; // the block that ends at heap_end, or NULL if the heap is empty
static struct block *free_head;
static struct kheap_stats stats;
static bool ready;

static inline uint8_t *payload(struct block *b) { return (uint8_t *)b + HDR; }
static inline struct links *links(struct block *b) { return (struct links *)payload(b); }
static inline uintptr_t block_end(struct block *b) { return (uintptr_t)b + b->size; }

static struct block *next_block(struct block *b) {
    return b == top ? NULL : (struct block *)block_end(b);
}
static struct block *prev_block(struct block *b) {
    return (uintptr_t)b == KHEAP_BASE ? NULL : (struct block *)((uintptr_t)b - b->prev_size);
}
static void fix_next_prev_size(struct block *b) {
    struct block *n = next_block(b);
    if (n) n->prev_size = b->size;
}

static void list_insert(struct block *b) {
    links(b)->prev = NULL;
    links(b)->next = free_head;
    if (free_head) links(free_head)->prev = b;
    free_head = b;
    stats.free_blocks++;
    stats.free_bytes += b->size;
}

static void list_remove(struct block *b) {
    struct links *l = links(b);
    if (l->prev) links(l->prev)->next = l->next;
    else free_head = l->next;
    if (l->next) links(l->next)->prev = l->prev;
    stats.free_blocks--;
    stats.free_bytes -= b->size;
}

// The first byte in [p, p + n) that isn't `value`, or NULL.
static const uint8_t *find_not(const uint8_t *p, size_t n, uint8_t value) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != value) return p + i;
    return NULL;
}

// ---------------------------------------------------------------------------
// Growing and shrinking the heap.
// ---------------------------------------------------------------------------

static void unmap_pages(uintptr_t start, uintptr_t end) {
    for (uintptr_t v = start; v < end; v += PAGE_SIZE) {
        uint64_t phys;
        if (!vmm_virt_to_phys(&kernel_space, v, &phys)) panic("kheap: heap page %p is not mapped", (void *)v);
        vmm_unmap(&kernel_space, v);
        pmm_free_page(phys);
    }
}

// Add at least `want` bytes of free space at the top of the heap.
static bool grow(uint64_t want) {
    uint64_t bytes = ALIGN_UP(want < GROW_MIN ? GROW_MIN : want, PAGE_SIZE);
    if (want > KHEAP_MAX_SIZE || bytes > KHEAP_BASE + KHEAP_MAX_SIZE - heap_end) return false;
    // vmm_map() panics if it can't get a page table, so make sure the PMM has
    // the pages plus the most tables they could need (PTs, PDs, a PDPT).
    uint64_t pages = bytes / PAGE_SIZE;
    if (pmm_get_stats().free_pages < pages + pages / 512 + pages / (512 * 512) + 5) return false;
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            unmap_pages(heap_end, heap_end + off);
            return false;
        }
        vmm_map(&kernel_space, heap_end + off, phys, VMM_WRITE);
    }
    uintptr_t start = heap_end;
    memset((void *)start, KHEAP_POISON, bytes);
    heap_end += bytes;
    stats.heap_bytes += bytes;

    if (top && top->magic == MAGIC_FREE) {
        top->size += bytes;
        stats.free_bytes += bytes;
        return true;
    }
    struct block *b = (struct block *)start;
    *b = (struct block){.size = bytes, .prev_size = top ? top->size : 0, .magic = MAGIC_FREE};
    top = b;
    list_insert(b);
    return true;
}

// Give the free pages at the top of the heap back, if there are enough.
static void trim(void) {
    if (top == NULL || top->magic != MAGIC_FREE) return;
    uintptr_t keep = ALIGN_UP((uintptr_t)top + MIN_BLOCK, PAGE_SIZE);
    if (heap_end - keep < TRIM_MIN) return;
    unmap_pages(keep, heap_end);
    stats.heap_bytes -= heap_end - keep;
    stats.free_bytes -= heap_end - keep;
    heap_end = keep;
    top->size = keep - (uintptr_t)top;
}

// ---------------------------------------------------------------------------
// Allocating and freeing blocks.
// ---------------------------------------------------------------------------

// Bytes a block needs to hold `size` bytes plus at least REDZONE_MIN of redzone.
static uint64_t block_size_for(size_t size) { return HDR + ALIGN_UP(size, ALIGN) + REDZONE_MIN; }

// Where a block for `need` bytes with its payload aligned to `align` would
// start inside free block `b`, or 0 if it doesn't fit. Any gap left below it
// must be big enough to stay a free block of its own.
static uintptr_t fit(struct block *b, uint64_t need, uint64_t align) {
    uintptr_t start = (uintptr_t)b;
    uintptr_t u = ALIGN_UP(start + HDR, align) - HDR;
    while (u != start && u - start < MIN_BLOCK) u += align;
    return u + need <= block_end(b) ? u : 0;
}

// Turn the part of free block `b` from `u` onwards into a used block of
// `need` bytes, holding `size` bytes for the caller.
static void *carve(struct block *b, uintptr_t u_addr, uint64_t need, size_t size) {
    struct block *u = (struct block *)u_addr;
    uint64_t u_size = block_end(b) - u_addr;
    bool tail = u_size - need >= MIN_BLOCK;
    if (tail) u_size = need;

    // Nothing may have written to the free block since it was freed. Its
    // header and links are the only bytes that aren't poison.
    uintptr_t from = (uintptr_t)payload(b) + sizeof(struct links);
    if (from < u_addr + HDR) from = u_addr + HDR;
    const uint8_t *bad = find_not((const uint8_t *)from, u_addr + u_size - from, KHEAP_POISON);
    if (bad)
        panic("kheap: freed memory at %p was written to after kfree (byte is 0x%02x, not 0x%02x)",
              bad, *bad, KHEAP_POISON);

    if (u != b) {
        // Split off the gap below: `b` stays on the free list, smaller.
        uint64_t gap = u_addr - (uintptr_t)b;
        u->size = b->size - gap;
        u->prev_size = gap;
        stats.free_bytes -= u->size;
        b->size = gap;
        if (top == b) top = u;
    } else {
        list_remove(b);
        memset(links(b), KHEAP_POISON, sizeof(struct links)); // hand out poison, not old pointers
    }
    if (tail) {
        // Split off the space above as a new free block. Its neighbour above
        // is used, since the old free block had no free neighbours.
        struct block *t = (struct block *)(u_addr + need);
        *t = (struct block){.size = u->size - need, .prev_size = need, .magic = MAGIC_FREE};
        u->size = need;
        if (top == u) top = t;
        fix_next_prev_size(t);
        list_insert(t);
    }
    fix_next_prev_size(u);

    u->magic = MAGIC_USED;
    u->requested = size;
    memset(payload(u) + size, KHEAP_REDZONE, u->size - HDR - size);
    stats.used_bytes += size;
    stats.allocations++;
    stats.total_allocs++;
    return payload(u);
}

// Free block `b` (not on the free list): poison it and merge it with its
// free neighbours.
static void release(struct block *b) {
    memset(payload(b), KHEAP_POISON, b->size - HDR);
    b->magic = MAGIC_FREE;
    b->requested = 0;

    struct block *n = next_block(b);
    if (n && n->magic == MAGIC_FREE) {
        list_remove(n);
        b->size += n->size;
        if (top == n) top = b;
        memset(n, KHEAP_POISON, MIN_BLOCK); // its header and links are now inside b
        fix_next_prev_size(b);
    }
    struct block *p = prev_block(b);
    if (p && p->magic == MAGIC_FREE) {
        p->size += b->size;
        stats.free_bytes += b->size;
        if (top == b) top = p;
        memset(b, KHEAP_POISON, HDR); // b's links were never written
        fix_next_prev_size(p);
    } else {
        list_insert(b);
    }
    trim();
}

static void *alloc_locked(size_t size, uint64_t align) {
    if (!ready) panic("kmalloc before kheap_init");
    if (size == 0 || size > KHEAP_MAX_SIZE) return NULL;
    uint64_t need = block_size_for(size);
    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block *b = free_head; b; b = links(b)->next) {
            uintptr_t u = fit(b, need, align);
            if (u) return carve(b, u, need, size);
        }
        if (attempt > 0) break;
        // Enough for any alignment gap. If the top block is free it grows
        // too, so only the difference is needed (it was too small to fit).
        uint64_t want = need + (align > ALIGN ? align + MIN_BLOCK : 0);
        if (top && top->magic == MAGIC_FREE) want = want > top->size ? want - top->size : PAGE_SIZE;
        if (!grow(want)) return NULL;
    }
    panic("kheap: grew the heap for %lu bytes (align %lu) but nothing fits", (uint64_t)size, align);
}

// Why `ptr`, inside the heap, has no valid header in front of it. Walks the
// heap to find the block it is in; only runs on the way to a panic.
__attribute__((noreturn)) static void bad_pointer(const char *who, void *ptr, uint32_t magic) {
    uintptr_t p = (uintptr_t)ptr;
    for (uintptr_t a = KHEAP_BASE; a < heap_end;) {
        struct block *b = (struct block *)a;
        if ((b->magic != MAGIC_USED && b->magic != MAGIC_FREE) || b->size < MIN_BLOCK || b->size % ALIGN ||
            b->size > heap_end - a)
            break; // a damaged header: the walk can't go further
        if (p < block_end(b)) {
            if (b->magic == MAGIC_FREE) panic("%s: %p is in free memory (double free?)", who, ptr);
            if (p > (uintptr_t)payload(b))
                panic("%s: %p points %lu bytes into the block at %p", who, ptr, p - (uintptr_t)payload(b),
                      payload(b));
            break;
        }
        a += b->size;
    }
    panic("%s: %p has a bad block header (magic 0x%08x): not from kmalloc, or overwritten", who, ptr, magic);
}

// The block behind `ptr`, after checking it is a live allocation whose
// header and redzone are intact.
static struct block *checked_block(const char *who, void *ptr) {
    uintptr_t p = (uintptr_t)ptr;
    if (p < KHEAP_BASE + HDR || p >= heap_end || p % ALIGN)
        panic("%s: %p is not a heap pointer", who, ptr);
    struct block *b = (struct block *)(p - HDR);
    if (b->magic != MAGIC_USED) bad_pointer(who, ptr, b->magic);
    if (b->size % ALIGN || b->size < HDR + b->requested + REDZONE_MIN || block_end(b) > heap_end)
        panic("%s: %p has a corrupted block header (size %lu, requested %lu)", who, ptr, b->size, b->requested);
    uint8_t *rz = payload(b) + b->requested;
    const uint8_t *bad = find_not(rz, block_end(b) - (uintptr_t)rz, KHEAP_REDZONE);
    if (bad)
        panic("%s: heap overflow: %p (%lu bytes) was written %lu bytes past its end", who, ptr, b->requested,
              (uint64_t)(bad - rz));
    return b;
}

// ---------------------------------------------------------------------------
// Public interface.
// ---------------------------------------------------------------------------

void kheap_init(void) {
    // The heap's range must not overlap the HHDM, the kernel image, or
    // anything else already mapped.
    uint64_t hhdm_top = hhdm_offset + vmm_get_stats().hhdm_end;
    if (hhdm_offset < KHEAP_BASE + KHEAP_MAX_SIZE && hhdm_top > KHEAP_BASE)
        panic("kheap: heap range %p overlaps the HHDM (%p - %p)", (void *)KHEAP_BASE, (void *)hhdm_offset,
              (void *)hhdm_top);
    uint64_t phys;
    if (vmm_virt_to_phys(&kernel_space, KHEAP_BASE, &phys) ||
        vmm_virt_to_phys(&kernel_space, KHEAP_BASE + KHEAP_MAX_SIZE - PAGE_SIZE, &phys))
        panic("kheap: heap range %p is already mapped", (void *)KHEAP_BASE);
    ready = true;
}

// Every entry point runs with preemption off, so threads can share the heap.
static void *alloc(size_t size, uint64_t align) {
    preempt_disable();
    void *p = alloc_locked(size, align);
    preempt_enable();
    return p;
}

void *kmalloc(size_t size) { return alloc(size, ALIGN); }

void *kmalloc_aligned(size_t size, size_t align) {
    if (align < ALIGN || align > MAX_ALIGN || (align & (align - 1)))
        panic("kmalloc_aligned: alignment %lu is not a power of two from 16 to 2 MiB", (uint64_t)align);
    return alloc(size, align);
}

void *kcalloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    void *p = kmalloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}

void kfree(void *ptr) {
    if (ptr == NULL) return;
    preempt_disable();
    struct block *b = checked_block("kfree", ptr);
    stats.used_bytes -= b->requested;
    stats.allocations--;
    stats.total_frees++;
    release(b);
    preempt_enable();
}

static void *realloc_locked(void *ptr, size_t size);

void *krealloc(void *ptr, size_t size) {
    preempt_disable();
    void *p = realloc_locked(ptr, size);
    preempt_enable();
    return p;
}

static void *realloc_locked(void *ptr, size_t size) {
    if (ptr == NULL) return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    struct block *b = checked_block("krealloc", ptr);
    if (size > KHEAP_MAX_SIZE) return NULL;
    uint64_t need = block_size_for(size);
    if (need > b->size) {
        void *q = kmalloc(size);
        if (q == NULL) return NULL;
        memcpy(q, ptr, b->requested);
        kfree(ptr);
        return q;
    }
    // Fits in place. Give back the end of the block if it is big enough.
    stats.used_bytes += size - b->requested;
    b->requested = size;
    if (b->size - need >= MIN_BLOCK) {
        struct block *t = (struct block *)((uintptr_t)b + need);
        *t = (struct block){.size = b->size - need, .prev_size = need, .magic = MAGIC_USED};
        b->size = need;
        if (top == b) top = t;
        fix_next_prev_size(t);
        release(t);
    }
    memset(payload(b) + size, KHEAP_REDZONE, b->size - HDR - size);
    return ptr;
}

struct kheap_stats kheap_get_stats(void) {
    preempt_disable();
    struct kheap_stats s = stats;
    s.largest_free = 0;
    for (struct block *b = free_head; b; b = links(b)->next)
        if (b->size > s.largest_free) s.largest_free = b->size;
    preempt_enable();
    return s;
}

static void check_locked(void);

void kheap_check(void) {
    preempt_disable();
    check_locked();
    preempt_enable();
}

static void check_locked(void) {
    uint64_t used = 0, live = 0, free_bytes = 0, free_count = 0, prev_size = 0;
    bool prev_free = false;
    struct block *last = NULL;
    for (uintptr_t a = KHEAP_BASE; a < heap_end;) {
        struct block *b = (struct block *)a;
        if (b->magic != MAGIC_USED && b->magic != MAGIC_FREE)
            panic("kheap_check: block %p has bad magic 0x%08x", b, b->magic);
        if (b->size < MIN_BLOCK || b->size % ALIGN || b->size > heap_end - a)
            panic("kheap_check: block %p has bad size %lu", b, b->size);
        if (b->prev_size != prev_size)
            panic("kheap_check: block %p says the block below is %lu bytes, it is %lu", b, b->prev_size, prev_size);
        if (b->magic == MAGIC_USED) {
            checked_block("kheap_check", payload(b));
            used += b->requested;
            live++;
            prev_free = false;
        } else {
            if (prev_free) panic("kheap_check: free blocks at %p and below were not merged", b);
            uint8_t *from = payload(b) + sizeof(struct links);
            const uint8_t *bad = find_not(from, block_end(b) - (uintptr_t)from, KHEAP_POISON);
            if (bad) panic("kheap_check: freed memory at %p was written to after kfree (0x%02x)", bad, *bad);
            free_bytes += b->size;
            free_count++;
            prev_free = true;
        }
        prev_size = b->size;
        last = b;
        a += b->size;
    }
    if (last != top) panic("kheap_check: top block is %p, the walk ended at %p", top, last);

    uint64_t listed = 0;
    struct block *prev = NULL;
    for (struct block *b = free_head; b; prev = b, b = links(b)->next) {
        if (b->magic != MAGIC_FREE) panic("kheap_check: %p is on the free list but not free", b);
        if (links(b)->prev != prev) panic("kheap_check: free list back link at %p is broken", b);
        if (++listed > free_count) panic("kheap_check: free list has more entries than free blocks (loop?)");
    }
    if (listed != free_count) panic("kheap_check: %lu free blocks, %lu on the free list", free_count, listed);
    if (used != stats.used_bytes || live != stats.allocations || free_bytes != stats.free_bytes ||
        free_count != stats.free_blocks || heap_end - KHEAP_BASE != stats.heap_bytes)
        panic("kheap_check: stats disagree with the heap (used %lu/%lu, live %lu/%lu, free %lu/%lu)", used,
              stats.used_bytes, live, stats.allocations, free_bytes, stats.free_bytes);
}

// ---------------------------------------------------------------------------
// Boot self-test (moves into a KTEST once the test framework exists).
// ---------------------------------------------------------------------------

#define STRESS_SLOTS 512
#define STRESS_OPS   30000

static uint64_t rng_state = 0x243f6a8885a308d3ull;
static uint64_t rng(void) { // xorshift64*
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545f4914f6cdd1dull;
}

static inline uint8_t fill_byte(uint64_t seed, size_t i) { return (uint8_t)(seed + i * 131 + (i >> 9)); }

static void fill(void *p, size_t n, uint64_t seed) {
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++) b[i] = fill_byte(seed, i);
}

static void expect_fill(const char *what, const void *p, size_t n, uint64_t seed) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        if (b[i] != fill_byte(seed, i))
            panic("heap self-test: %s %p byte %lu is 0x%02x, expected 0x%02x", what, p, (uint64_t)i, b[i],
                  fill_byte(seed, i));
}

// A size: mostly small, sometimes a few pages, now and then hundreds of KiB.
static size_t random_size(void) {
    uint64_t r = rng() % 100;
    if (r < 70) return 1 + rng() % 128;
    if (r < 90) return 129 + rng() % 1920;
    if (r < 98) return 2048 + rng() % (30 * 1024);
    return 32 * 1024 + rng() % (480 * 1024);
}

static struct { void *p; size_t size; uint64_t seed; } slots[STRESS_SLOTS];

void kheap_self_test(void) {
    const struct pmm_stats pmm0 = pmm_get_stats();
    const struct vmm_stats vmm0 = vmm_get_stats();
    const struct kheap_stats h0 = kheap_get_stats();

    // Edge cases.
    if (kmalloc(0) != NULL) panic("heap self-test: kmalloc(0) did not return NULL");
    kfree(NULL);
    if (kmalloc(KHEAP_MAX_SIZE + 1) != NULL) panic("heap self-test: kmalloc(64 GiB + 1) succeeded");
    if (kcalloc(SIZE_MAX / 2, 4) != NULL) panic("heap self-test: kcalloc overflow not caught");
    uint8_t *z = kcalloc(100, 7);
    if (z == NULL || (uintptr_t)z % 16) panic("heap self-test: kcalloc returned %p", z);
    if (find_not(z, 700, 0)) panic("heap self-test: kcalloc memory is not zeroed");
    uint8_t *one = kmalloc(1);
    if (one == NULL || (uintptr_t)one % 16) panic("heap self-test: kmalloc(1) returned %p", one);
    if (one[0] != KHEAP_POISON) panic("heap self-test: new memory is 0x%02x, not poison", one[0]);
    kfree(one);
    kfree(z);

    // Alignment.
    static const size_t aligns[] = {16, 64, 256, 4096, 65536, 2 << 20};
    void *al[sizeof aligns / sizeof aligns[0]];
    for (size_t i = 0; i < sizeof aligns / sizeof aligns[0]; i++) {
        size_t n = 4096 + i * 100;
        al[i] = kmalloc_aligned(n, aligns[i]);
        if (al[i] == NULL || (uintptr_t)al[i] % aligns[i])
            panic("heap self-test: kmalloc_aligned(%lu, %lu) returned %p", (uint64_t)n, (uint64_t)aligns[i], al[i]);
        fill(al[i], n, i);
    }
    for (size_t i = 0; i < sizeof aligns / sizeof aligns[0]; i++) expect_fill("aligned block", al[i], 4096 + i * 100, i);
    for (size_t i = 0; i < sizeof aligns / sizeof aligns[0]; i++) kfree(al[i]);
    kheap_check();

    // A large block, many pages, every byte distinct per offset: two pages
    // mapped to the same frame would show up as a mismatch.
    const size_t big = (4 << 20) + 123;
    uint8_t *b = kmalloc(big);
    if (b == NULL) panic("heap self-test: kmalloc(4 MiB) failed");
    fill(b, big, 99);
    expect_fill("4 MiB block", b, big, 99);
    for (size_t off = 0; off < big; off += PAGE_SIZE) {
        uint64_t phys;
        if (!vmm_virt_to_phys(&kernel_space, (uint64_t)(b + off), &phys))
            panic("heap self-test: 4 MiB block page %p is not mapped", b + off);
    }

    // krealloc: grow (moves), shrink (in place), and the NULL / 0 cases.
    uint8_t *r = krealloc(NULL, 100);
    fill(r, 100, 7);
    r = krealloc(r, 10000);
    if (r == NULL) panic("heap self-test: krealloc grow failed");
    expect_fill("krealloc'd block", r, 100, 7);
    fill(r, 10000, 8);
    uint8_t *s = krealloc(r, 50);
    if (s != r) panic("heap self-test: krealloc shrink moved the block");
    expect_fill("shrunk block", s, 50, 8);
    if (krealloc(s, 0) != NULL) panic("heap self-test: krealloc(p, 0) did not return NULL");
    kfree(b);
    kheap_check();

    // Random sequence: allocate, check, resize and free blocks of mixed sizes.
    uint64_t live = 0, live_bytes = 0;
    for (uint64_t op = 0; op < STRESS_OPS; op++) {
        size_t i = rng() % STRESS_SLOTS;
        if (slots[i].p == NULL) {
            size_t n = random_size();
            uint64_t kind = rng() % 16;
            void *p = kind == 0 ? kmalloc_aligned(n, 1ull << (4 + rng() % 9)) : kind == 1 ? kcalloc(1, n) : kmalloc(n);
            if (p == NULL) panic("heap self-test: out of memory allocating %lu bytes at op %lu", (uint64_t)n, op);
            if (kind == 1 && find_not(p, n, 0)) panic("heap self-test: kcalloc memory is not zeroed");
            slots[i].p = p;
            slots[i].size = n;
            slots[i].seed = rng();
            fill(p, n, slots[i].seed);
            live++;
            live_bytes += n;
        } else if (rng() % 4 == 0) {
            size_t n = random_size();
            void *p = krealloc(slots[i].p, n);
            if (p == NULL) panic("heap self-test: krealloc to %lu bytes failed at op %lu", (uint64_t)n, op);
            expect_fill("krealloc'd block", p, n < slots[i].size ? n : slots[i].size, slots[i].seed);
            live_bytes += n - slots[i].size;
            slots[i].p = p;
            slots[i].size = n;
            slots[i].seed = rng();
            fill(p, n, slots[i].seed);
        } else {
            expect_fill("block", slots[i].p, slots[i].size, slots[i].seed);
            kfree(slots[i].p);
            slots[i].p = NULL;
            live--;
            live_bytes -= slots[i].size;
        }
        if (op % 1024 == 0) {
            kheap_check();
            if (stats.allocations != h0.allocations + live || stats.used_bytes != h0.used_bytes + live_bytes)
                panic("heap self-test: heap counts %lu blocks / %lu bytes, test has %lu / %lu", stats.allocations,
                      stats.used_bytes, live, live_bytes);
        }
    }
    for (size_t i = 0; i < STRESS_SLOTS; i++) {
        if (slots[i].p == NULL) continue;
        expect_fill("block", slots[i].p, slots[i].size, slots[i].seed);
        kfree(slots[i].p);
        slots[i].p = NULL;
    }
    kheap_check();

    // No leaks: the heap is back to where it was, and is a single free block.
    struct kheap_stats h = kheap_get_stats();
    if (h.allocations != h0.allocations || h.used_bytes != h0.used_bytes)
        panic("heap self-test: %lu allocations (%lu bytes) left over", h.allocations - h0.allocations,
              h.used_bytes - h0.used_bytes);
    if (h0.allocations == 0 && (h.free_blocks != 1 || h.free_bytes != h.heap_bytes))
        panic("heap self-test: empty heap is %lu free blocks, %lu of %lu bytes free", h.free_blocks, h.free_bytes,
              h.heap_bytes);
    if (h.heap_bytes >= TRIM_MIN + PAGE_SIZE)
        panic("heap self-test: %lu KiB still mapped after freeing everything", h.heap_bytes / 1024);
    // Every page the PMM lost is mapped in the heap or holds a new page table.
    uint64_t pages_gone = pmm0.free_pages - pmm_get_stats().free_pages;
    uint64_t pages_kept = (h.heap_bytes - h0.heap_bytes) / PAGE_SIZE + (vmm_get_stats().table_pages - vmm0.table_pages);
    if (pages_gone != pages_kept)
        panic("heap self-test: PMM is down %lu pages, heap and page tables account for %lu", pages_gone, pages_kept);
    if (h.total_allocs - h0.total_allocs != h.total_frees - h0.total_frees)
        panic("heap self-test: %lu allocations but %lu frees", h.total_allocs - h0.total_allocs,
              h.total_frees - h0.total_frees);
}
