// Physical memory manager: a bitmap page-frame allocator.
//
// Two bitmaps hold one bit per 4 KiB page, from address 0 up to the highest
// page the allocator could ever manage:
//
//   used     1 = allocated, or not ours to hand out;  0 = free
//   managed  1 = the page came from a USABLE (or reclaimed) region
//
// Pages outside USABLE regions (the kernel image, framebuffer, ACPI tables,
// reserved ranges, holes) are never marked free, so they can't be handed out.
// `managed` lets pmm_free_page() tell a double free from a free of memory the
// allocator never owned. A bitmap (rather than a free-page stack) makes
// contiguous runs for DMA buffers easy to find.
//
// The bitmaps live at the start of the first USABLE region big enough to hold
// them, reached through the HHDM. For 512 MiB of RAM they take 32 KiB.
//
// One spinlock covers the bitmaps and counters, so any CPU or thread may
// call in. It is the innermost lock: nothing here takes another.

#include <kprintf.h>
#include <limine.h>
#include <memory.h>
#include <pmm.h>
#include <stdbool.h>
#include <string.h>
#include <sync.h>

uint64_t hhdm_offset;

static uint64_t *used_map, *managed_map;
static uint64_t map_words;  // length of each bitmap in 64-bit words
static uint64_t page_count; // pages the bitmaps describe
static uint64_t meta_first, meta_end; // pages holding the bitmaps
static uint64_t total_pages, free_pages, reserved_pages;
static uint64_t next_word; // where the next single-page search starts

// BOOTLOADER_RECLAIMABLE ranges, copied because the memory map itself lives
// in bootloader-reclaimable memory.
#define MAX_RECLAIMABLE 64
static struct { uint64_t base, length; } reclaimable[MAX_RECLAIMABLE];
static size_t reclaimable_count;
static bool reclaimed;
static struct spinlock lock = SPINLOCK_INIT("pmm");

static inline bool bit_test(const uint64_t *map, uint64_t page) {
    return (map[page / 64] >> (page % 64)) & 1;
}
static inline void bit_set(uint64_t *map, uint64_t page) { map[page / 64] |= 1ull << (page % 64); }
static inline void bit_clear(uint64_t *map, uint64_t page) { map[page / 64] &= ~(1ull << (page % 64)); }

// The whole pages inside [base, base + length), as page numbers [*first, *end).
static bool whole_pages(uint64_t base, uint64_t length, uint64_t *first, uint64_t *end) {
    *first = ALIGN_UP(base, PAGE_SIZE) / PAGE_SIZE;
    *end = ALIGN_DOWN(base + length, PAGE_SIZE) / PAGE_SIZE;
    return *end > *first;
}

static void add_free_pages(uint64_t first, uint64_t end) {
    if (end > page_count) end = page_count;
    for (uint64_t p = first; p < end; p++) {
        if (bit_test(managed_map, p)) continue; // overlapping entries: count once
        bit_set(managed_map, p);
        bit_clear(used_map, p);
        total_pages++;
        free_pages++;
    }
}

// Take a free page out of circulation for good (it stays counted in the total).
static void reserve_page(uint64_t p) {
    if (p >= page_count || !bit_test(managed_map, p) || bit_test(used_map, p)) return;
    bit_set(used_map, p);
    free_pages--;
    reserved_pages++;
}

void pmm_init(struct limine_memmap_response *mm, uint64_t hhdm) {
    hhdm_offset = hhdm;

    // How far up the bitmaps must reach.
    uint64_t highest = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE && e->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
            continue;
        if (e->base + e->length > highest) highest = e->base + e->length;
    }
    page_count = ALIGN_DOWN(highest, PAGE_SIZE) / PAGE_SIZE;
    map_words = (page_count + 63) / 64;
    uint64_t meta_pages = ALIGN_UP(2 * map_words * sizeof(uint64_t), PAGE_SIZE) / PAGE_SIZE;

    // Somewhere to put them: the first usable region with room (never page 0).
    for (uint64_t i = 0; i < mm->entry_count && meta_end == 0; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        uint64_t first, end;
        if (e->type != LIMINE_MEMMAP_USABLE || !whole_pages(e->base, e->length, &first, &end))
            continue;
        if (first == 0) first = 1;
        if (end - first >= meta_pages) {
            meta_first = first;
            meta_end = first + meta_pages;
        }
    }
    if (meta_end == 0)
        panic("pmm: no usable region can hold the %lu KiB page bitmap", meta_pages * PAGE_SIZE / 1024);

    used_map = phys_to_virt(meta_first * PAGE_SIZE);
    managed_map = used_map + map_words;
    memset(used_map, 0xff, map_words * sizeof(uint64_t)); // everything used until proven free
    memset(managed_map, 0, map_words * sizeof(uint64_t));

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        uint64_t first, end;
        if (e->type == LIMINE_MEMMAP_USABLE && whole_pages(e->base, e->length, &first, &end))
            add_free_pages(first, end);
        if (e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            if (reclaimable_count < MAX_RECLAIMABLE)
                reclaimable[reclaimable_count++] = (typeof(reclaimable[0])){e->base, e->length};
            else
                kprintf("  pmm: too many bootloader-reclaimable regions, ignoring %016lx\n", e->base);
        }
    }

    for (uint64_t p = meta_first; p < meta_end; p++) reserve_page(p);
    reserve_page(0); // so that 0 can mean "no page"
}

// Panic unless `phys` is a page this allocator handed out and hasn't taken back.
static void check_allocated(const char *who, uint64_t phys) {
    uint64_t p = phys / PAGE_SIZE;
    if (phys % PAGE_SIZE) panic("%s: %p is not page-aligned", who, (void *)phys);
    if (p >= page_count || !bit_test(managed_map, p))
        panic("%s: %p was never allocated (not memory the allocator manages)", who, (void *)phys);
    if (p == 0 || (p >= meta_first && p < meta_end))
        panic("%s: %p is reserved by the allocator", who, (void *)phys);
    if (!bit_test(used_map, p)) panic("%s: %p is already free (double free?)", who, (void *)phys);
}

static uint64_t alloc_page_locked(void) {
    if (free_pages == 0) return 0;
    for (uint64_t i = 0; i < map_words; i++) {
        uint64_t w = (next_word + i) % map_words;
        if (used_map[w] == ~0ull) continue;
        // Bits past page_count were set by the initial memset and never cleared.
        uint64_t p = w * 64 + (uint64_t)__builtin_ctzll(~used_map[w]);
        bit_set(used_map, p);
        free_pages--;
        next_word = w;
        return p * PAGE_SIZE;
    }
    panic("pmm: %lu pages counted free but none found in the bitmap", free_pages);
}

uint64_t pmm_alloc_page(void) {
    spin_lock(&lock);
    uint64_t phys = alloc_page_locked();
    spin_unlock(&lock);
    return phys;
}

void pmm_free_page(uint64_t phys) {
    spin_lock(&lock);
    check_allocated("pmm_free_page", phys);
    bit_clear(used_map, phys / PAGE_SIZE);
    free_pages++;
    spin_unlock(&lock);
}

uint64_t pmm_alloc_contiguous(size_t count) { return pmm_alloc_contiguous_below(count, UINT64_MAX); }

static uint64_t alloc_contiguous_locked(size_t count, uint64_t limit);

uint64_t pmm_alloc_contiguous_below(size_t count, uint64_t limit) {
    spin_lock(&lock);
    uint64_t phys = alloc_contiguous_locked(count, limit);
    spin_unlock(&lock);
    return phys;
}

static uint64_t alloc_contiguous_locked(size_t count, uint64_t limit) {
    if (count == 0 || count > free_pages) return 0;
    if (count == 1 && limit == UINT64_MAX) return alloc_page_locked();

    // First fit, skipping fully used words quickly.
    uint64_t run = 0;
    uint64_t end = limit / PAGE_SIZE < page_count ? limit / PAGE_SIZE : page_count;
    for (uint64_t p = 0; p < end; p++) {
        if (p % 64 == 0 && used_map[p / 64] == ~0ull) {
            run = 0;
            p += 63;
            continue;
        }
        if (bit_test(used_map, p)) {
            run = 0;
            continue;
        }
        if (++run == count) {
            uint64_t first = p + 1 - count;
            for (uint64_t q = first; q <= p; q++) bit_set(used_map, q);
            free_pages -= count;
            return first * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free_contiguous(uint64_t phys, size_t count) {
    spin_lock(&lock);
    for (size_t i = 0; i < count; i++) check_allocated("pmm_free_contiguous", phys + i * PAGE_SIZE);
    for (size_t i = 0; i < count; i++) bit_clear(used_map, phys / PAGE_SIZE + i);
    free_pages += count;
    spin_unlock(&lock);
}

void pmm_reclaim_bootloader_memory(void) {
    spin_lock(&lock);
    if (!reclaimed) {
        reclaimed = true;
        for (size_t i = 0; i < reclaimable_count; i++) {
            uint64_t first, end;
            if (whole_pages(reclaimable[i].base, reclaimable[i].length, &first, &end))
                add_free_pages(first, end);
        }
    }
    spin_unlock(&lock);
}

struct pmm_stats pmm_get_stats(void) {
    spin_lock(&lock);
    struct pmm_stats st = {.total_pages = total_pages, .free_pages = free_pages, .reserved_pages = reserved_pages};
    spin_unlock(&lock);
    return st;
}

// ---------------------------------------------------------------------------
// Boot self-test (moves into a KTEST once the test framework exists).
// ---------------------------------------------------------------------------

#define SELF_TEST_PAGES 4096
#define SELF_TEST_RUN   64

static uint64_t pattern(uint64_t phys, uint64_t word) {
    return (phys * 0x9e3779b97f4a7c15ull) ^ (word * 0xbf58476d1ce4e5b9ull) ^ 0x5a5a5a5a5a5a5a5aull;
}

static bool in_usable_region(struct limine_memmap_response *mm, uint64_t phys, uint64_t len) {
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && phys >= e->base && phys + len <= e->base + e->length)
            return true;
    }
    return false;
}

void pmm_self_test(struct limine_memmap_response *mm) {
    // The allocator manages exactly the usable memory in the map, and nothing
    // has been allocated yet beyond its own reservations.
    uint64_t usable_pages = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        uint64_t first, end;
        if (e->type == LIMINE_MEMMAP_USABLE && whole_pages(e->base, e->length, &first, &end))
            usable_pages += end - first;
    }
    if (total_pages != usable_pages)
        panic("pmm self-test: manages %lu pages, memory map has %lu usable", total_pages, usable_pages);
    if (free_pages + reserved_pages != total_pages)
        panic("pmm self-test: %lu free + %lu reserved != %lu total", free_pages, reserved_pages, total_pages);

    const uint64_t start_free = free_pages;
    uint64_t want = SELF_TEST_PAGES;
    if (want > start_free / 2) want = start_free / 2;

    // Allocate pages one by one and chain them into a list through their first
    // word; fill the rest with a pattern unique to each page. If two
    // allocations ever overlapped, the chain or a pattern would break.
    const uint64_t words = PAGE_SIZE / sizeof(uint64_t);
    uint64_t head = 0;
    for (uint64_t n = 0; n < want; n++) {
        uint64_t p = pmm_alloc_page();
        if (p == 0) panic("pmm self-test: out of memory after %lu pages", n);
        if (!in_usable_region(mm, p, PAGE_SIZE))
            panic("pmm self-test: handed out %p, outside every usable region", (void *)p);
        uint64_t *v = phys_to_virt(p);
        v[0] = head;
        for (uint64_t k = 1; k < words; k++) v[k] = pattern(p, k);
        head = p;
    }
    if (free_pages != start_free - want)
        panic("pmm self-test: free count %lu after %lu allocations from %lu", free_pages, want, start_free);

    uint64_t seen = 0;
    for (uint64_t p = head; p != 0 && seen <= want; seen++) {
        uint64_t *v = phys_to_virt(p);
        for (uint64_t k = 1; k < words; k++)
            if (v[k] != pattern(p, k)) panic("pmm self-test: page %p word %lu corrupted", (void *)p, k);
        p = v[0];
    }
    if (seen != want) panic("pmm self-test: page chain has %lu pages, expected %lu", seen, want);

    for (uint64_t p = head; p != 0;) {
        uint64_t next = ((uint64_t *)phys_to_virt(p))[0];
        pmm_free_page(p);
        p = next;
    }
    if (free_pages != start_free)
        panic("pmm self-test: free count %lu after freeing everything, expected %lu", free_pages, start_free);

    // A contiguous run: one HHDM range, every page inside usable memory.
    uint64_t run = pmm_alloc_contiguous(SELF_TEST_RUN);
    if (run == 0) panic("pmm self-test: no %d-page contiguous run", SELF_TEST_RUN);
    if (!in_usable_region(mm, run, SELF_TEST_RUN * PAGE_SIZE))
        panic("pmm self-test: contiguous run at %p leaves usable memory", (void *)run);
    uint64_t *v = phys_to_virt(run);
    for (uint64_t k = 0; k < SELF_TEST_RUN * words; k++) v[k] = pattern(run, k);
    for (uint64_t k = 0; k < SELF_TEST_RUN * words; k++)
        if (v[k] != pattern(run, k)) panic("pmm self-test: contiguous run word %lu corrupted", k);
    pmm_free_contiguous(run, SELF_TEST_RUN);
    if (free_pages != start_free)
        panic("pmm self-test: free count %lu after contiguous free, expected %lu", free_pages, start_free);
}
