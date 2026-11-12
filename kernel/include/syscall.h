#pragma once
// System calls: user programs run `syscall` with the number in rax and up to
// six arguments in rdi, rsi, rdx, r10, r8 and r9; the result comes back in
// rax, a negative errno value on failure. rcx and r11 are clobbered (the CPU
// keeps the return address and RFLAGS there); every other register is kept.
//
// The numbers, flags and errors are Linux's, so the user programs and libc
// can use what they already know; the calls Linux doesn't have are numbered
// from 500. The #defines are also for assembly (user_programs.S).

#define SYS_read          0  // read(fd, buf, len): the console waits for a key, then gives what was typed
#define SYS_write         1  // write(fd, buf, len)
#define SYS_open          2  // open(path, flags, mode): O_RDONLY/O_WRONLY/O_RDWR, O_CREAT, O_TRUNC, O_APPEND (vfs.h); mode is ignored
#define SYS_close         3  // close(fd)
#define SYS_mmap          9  // mmap(addr, len, prot, flags, fd, offset): anonymous memory only
#define SYS_dup           32 // dup(fd): the lowest free descriptor, on the same open file
#define SYS_dup2          33 // dup2(fd, newfd): newfd (closed first if open) on the same open file
#define SYS_nanosleep     35 // nanosleep(const struct timespec *req, struct timespec *rem)
#define SYS_getpid        39 // getpid()
#define SYS_socket        41 // socket(domain, type, protocol): AF_INET; TCP, UDP or ping sockets (socket.h)
#define SYS_connect       42 // connect(fd, addr, len)
#define SYS_sendto        44 // sendto(fd, buf, len, flags, addr, len)
#define SYS_recvfrom      45 // recvfrom(fd, buf, len, flags, addr, int *len)
#define SYS_bind          49 // bind(fd, addr, len)
#define SYS_setsockopt    54 // setsockopt(fd, level, name, value, len): SO_RCVTIMEO, SO_SNDTIMEO
#define SYS_exit          60 // exit(status): ends the process
#define SYS_wait4         61 // wait4(pid, int *status, options, rusage): waitpid (proc.h); rusage must be NULL
#define SYS_getppid       110 // getppid(): 0 for a process the kernel started
#define SYS_clock_gettime 228 // clock_gettime(clock, struct timespec *ts)

// myos's own.
#define SYS_spawn          500 // spawn(path, argv, envp): run a program as a child (process_spawn); its pid
#define SYS_kernel_command 501 // kernel_command(argv): run a command of the kernel shell (shell.h); -ENOENT if none has that name

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

// The same on an application processor, once syscall_init() has run.
void syscall_init_ap(void);

// Called from syscall_entry (syscall.S) with interrupts on. The frame holds
// the user's registers, laid out like an interrupt's; the result goes in
// its rax.
void syscall_dispatch(struct interrupt_frame *f);

#endif
