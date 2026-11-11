// malloc and friends: a first-fit free list (the allocator from K&R's "The
// C Programming Language", 8.7) over memory from mmap.
//
// Every block starts with a header: its size in header units (16 bytes,
// so blocks stay 16-byte aligned) and, while it is free, the next free
// block. The free list is circular and sorted by address, so free() can
// join a block with free neighbours. When nothing fits, more memory comes
// from mmap, at least ARENA bytes at a time; mmap hands out addresses one
// after another, so new memory usually joins the end of the old.

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA (64 * 1024)
#define PAGE  4096

typedef struct header {
    struct header *next; // the next free block (while free)
    size_t units;        // this block's size, header included
} __attribute__((aligned(16))) header;

_Static_assert(sizeof(header) == 16, "a header is one 16-byte unit");

static header base; // a zero-size block that keeps the list from being empty
static header *freep;

// Give a block back to the free list, joining it with its neighbours.
static void insert(header *b) {
    header *p = freep;
    // Find p with p < b < p->next, or the wrap-around point of the list.
    for (; !(b > p && b < p->next); p = p->next)
        if (p >= p->next && (b > p || b < p->next)) break;
    if (b + b->units == p->next) {
        b->units += p->next->units;
        b->next = p->next->next;
    } else {
        b->next = p->next;
    }
    if (p + p->units == b) {
        p->units += b->units;
        p->next = b->next;
    } else {
        p->next = b;
    }
    freep = p;
}

// More memory from the kernel, onto the free list. False if there is none.
static int grow(size_t units) {
    size_t bytes = units * sizeof(header);
    if (bytes < ARENA) bytes = ARENA;
    bytes = (bytes + PAGE - 1) & ~(size_t)(PAGE - 1);
    void *m = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return 0;
    header *b = m;
    b->units = bytes / sizeof(header);
    insert(b);
    return 1;
}

void *malloc(size_t size) {
    if (size == 0) size = 1;
    if (size > SIZE_MAX / 2) {
        errno = ENOMEM;
        return NULL;
    }
    size_t units = (size + sizeof(header) - 1) / sizeof(header) + 1;
    if (freep == NULL) {
        base.next = freep = &base;
        base.units = 0;
    }
    for (header *prev = freep, *p = prev->next;; prev = p, p = p->next) {
        if (p->units >= units) {
            if (p->units == units) {
                prev->next = p->next;
            } else { // hand out the tail end
                p->units -= units;
                p += p->units;
                p->units = units;
            }
            freep = prev;
            return p + 1;
        }
        if (p == freep) { // all the way round: nothing fits
            if (!grow(units)) {
                errno = ENOMEM;
                return NULL;
            }
            p = freep;
        }
    }
}

void free(void *ptr) {
    if (ptr) insert((header *)ptr - 1);
}

void *calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = malloc(count * size);
    return p ? memset(p, 0, count * size) : NULL;
}

void *realloc(void *ptr, size_t size) {
    if (ptr == NULL) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    size_t have = (((header *)ptr - 1)->units - 1) * sizeof(header);
    if (size <= have) return ptr;
    void *p = malloc(size);
    if (p == NULL) return NULL;
    memcpy(p, ptr, have);
    free(ptr);
    return p;
}
