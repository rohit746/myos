#pragma once
// System calls: user programs run `syscall` with the number in rax and up to
// six arguments in rdi, rsi, rdx, r10, r8 and r9; the result comes back in
// rax, a negative errno value on failure. rcx and r11 are clobbered (the CPU
// keeps the return address and RFLAGS there); every other register is kept.
//
// The numbers, flags and errors are Linux's, so the user programs and libc
// to come can use what they already know. The #defines are also for
// assembly (user_programs.S).

#define SYS_write         1  // write(fd, buf, len): fd 1 and 2 are the console
#define SYS_mmap          9  // mmap(addr, len, prot, flags, fd, offset): anonymous memory only
#define SYS_nanosleep     35 // nanosleep(const struct timespec *req, struct timespec *rem)
#define SYS_getpid        39 // getpid()
#define SYS_exit          60 // exit(status): ends the process
#define SYS_clock_gettime 228 // clock_gettime(clock, struct timespec *ts)

// mmap
#define PROT_NONE     0
#define PROT_READ     1
#define PROT_WRITE    2
#define PROT_EXEC     4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

// clock_gettime
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct interrupt_frame;

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

// Whether SMAP is on: the kernel then needs stac/clac around every touch of
// user memory (copy_from_user, copy_to_user). Read by isr.S.
extern bool smap_enabled;

// Turn on `syscall`/`sysret` (EFER.SCE, STAR, LSTAR, FMASK), and SMEP and
// SMAP when the CPU has them. Writes a line for the boot log.
void syscall_init(char *line, size_t size);

// Called from syscall_entry (syscall.S) with interrupts on. The frame holds
// the user's registers, laid out like an interrupt's; the result goes in
// its rax.
void syscall_dispatch(struct interrupt_frame *f);

#endif
