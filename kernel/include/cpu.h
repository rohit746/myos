#pragma once
// Small wrappers around x86-64 instructions that C can't express directly.

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %b0, %w1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %w1, %b0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %w0, %w1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %w1, %w0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %w1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

// A short delay for old ISA devices such as the 8259 PIC: port 0x80 is the
// POST code port, which nothing reads.
static inline void io_wait(void) { outb(0x80, 0); }

static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt" ::: "memory"); }
static inline void pause(void) { __asm__ volatile("pause" ::: "memory"); }

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

#define RFLAGS_IF (1ull << 9)

static inline uint64_t read_rflags(void) {
    uint64_t v;
    __asm__ volatile("pushfq; popq %0" : "=r"(v) : : "memory");
    return v;
}

static inline uint64_t read_cr0(void) { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr2(void) { uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr4(void) { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }

static inline void write_cr0(uint64_t v) { __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline void write_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline void write_cr4(uint64_t v) { __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory"); }

// Drop the TLB entry for one virtual address on this CPU.
static inline void invlpg(uint64_t virt) { __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory"); }

#define MSR_EFER           0xc0000080
#define MSR_STAR           0xc0000081
#define MSR_LSTAR          0xc0000082
#define MSR_FMASK          0xc0000084
#define MSR_GS_BASE        0xc0000101
#define MSR_KERNEL_GS_BASE 0xc0000102

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

// Stop this CPU for good: interrupts off, then sleep forever.
__attribute__((noreturn)) static inline void halt_forever(void) {
    for (;;) {
        cli();
        hlt();
    }
}

// What CPUID says this CPU is.
struct cpu_ident {
    char vendor[13]; // "GenuineIntel", "AuthenticAMD", ...
    char brand[49];  // "Intel(R) Core(TM) i5-8250U CPU @ 1.60GHz", or "unknown"
    unsigned family, model, stepping; // with the extended family and model added in
};

void cpu_identify(struct cpu_ident *id);
