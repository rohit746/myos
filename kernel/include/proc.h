#pragma once
// User processes: programs that run in ring 3, each in its own address
// space, and ask the kernel for things through system calls (syscall.h).
//
// A process has one thread. Its address space has the user's mappings in
// the lower half and shares the kernel half (PML4 entries 256-511) with
// every other space, so the kernel is mapped the same way whichever process
// is running; user pages have the U bit, kernel pages don't, so user code
// can't touch the kernel.
//
// User memory layout:
//   ELF_LOAD_MIN..   a program's segments (elf.h); a built-in program goes
//                    at USER_CODE_BASE (read-only, executable) and is
//                    entered at its first byte with the process's `arg` in
//                    rdi
//   USER_MMAP_BASE   up: what mmap hands out
//   USER_STACK_TOP   down: the stack, USER_STACK_PAGES pages; an ELF
//                    program finds its arguments on it (process_create_elf)
//   USER_END         the last page of the lower half is never mapped: a
//                    `syscall` at its very end would make sysret return to
//                    a non-canonical address, which faults in ring 0
//
// When user code faults (any exception but NMI, double fault and machine
// check) the kernel kills the process and carries on. The kernel touches
// user memory only through copy_from_user/copy_to_user, which check that
// every byte is mapped for the user (and writable, for copy_to_user), so a
// bad pointer in a system call fails with -EFAULT instead of faulting in the
// kernel. With SMAP, any other kernel access to user memory page-faults;
// with SMEP, so does running user code in ring 0.
//
// Programs: static ELF executables, loaded from files (process_exec; the
// ones in the initramfs are built from user/), and a few flat test programs
// built into the kernel (user_programs.S) that the self-test and the shell's
// `user` command run.

#include <stdbool.h>
#include <stddef.h>
#include <memory.h>
#include <stdint.h>
#include <sync.h>
#include <vmm.h>

#define USER_CODE_BASE   ((uint64_t)0x400000)
#define USER_MMAP_BASE   ((uint64_t)0x100000000000)
#define USER_END         ((uint64_t)0x00007ffffffff000)
#define USER_STACK_TOP   USER_END
#define USER_STACK_PAGES 4
#define USER_MMAP_END    (USER_STACK_TOP - ((uint64_t)1 << 32)) // leaves room for the stack to grow later

struct interrupt_frame;

struct process {
    uint64_t pid;
    char name[24];
    struct vmm_space space;
    uint64_t entry;     // where it starts running
    uint64_t user_rsp;  // its stack pointer at the start
    uint64_t arg;       // rdi at the entry point
    uint64_t mmap_next; // where the next mmap goes
    struct semaphore done; // up once the process has ended and its memory is freed

    // Where writes to fd 1 and 2 go: the console, or with `out` set, this
    // buffer (kept NUL-terminated; what doesn't fit is dropped).
    char *out;
    size_t out_size, out_len;

    // How it ended. `fault` is the exception that killed it, or -1 if it
    // called exit(status).
    int status;
    int fault;
    uint64_t fault_addr, fault_error, fault_rip; // cr2 for a page fault, the error code, where
    uint64_t ticks, switches; // its thread's, at the end (see struct thread)
};

// A new process running `size` bytes of code (copied) with `arg` in rdi.
// It doesn't start until process_start(). NULL if there is no memory.
struct process *process_create(const char *name, const void *code, size_t size, uint64_t arg);

// A new process running the static ELF executable `image` (`size` bytes,
// copied; see elf.h), with the NULL-terminated lists `argv` and `envp`
// copied to the top of its stack the way the System V ABI lays them out:
//
//   rsp ->  argc
//           argv[0] ... argv[argc - 1], NULL
//           envp[0] ... NULL
//           auxiliary vector: (type, value) pairs, ending with AT_NULL
//           ...
//           the strings, up to USER_STACK_TOP
//
// rsp is 16-byte aligned, and every other register is zero. Sets *out and
// returns 0, or returns -ENOEXEC (elf_load), -E2BIG if the arguments take
// more than USER_ARGS_MAX bytes, or -ENOMEM. Not started yet, as for
// process_create.
#define USER_ARGS_MAX (USER_STACK_PAGES * PAGE_SIZE / 4)
int process_create_elf(const char *name, const void *image, size_t size, char *const argv[], char *const envp[],
                       struct process **out);

// The same for the executable file at `path`, named after the file. Also
// -ENOENT and the other errors of opening and reading it, and -EACCES if
// it isn't a regular file.
int process_exec(const char *path, char *const argv[], char *const envp[], struct process **out);

// The environment programs the kernel starts get: PATH=/bin and HOME=/.
extern char *const process_default_env[];

// Run the executable at `path` with `argv` and the default environment, and
// wait for it to end. Returns 0 and sets *status to its exit status, or -1
// if it was killed (the kill was reported on the console); or returns
// process_exec's error.
int process_run(const char *path, char *const argv[], int *status);

// Start it in a thread of its own. False if there is no memory (then free it
// with process_free).
bool process_start(struct process *p);

// Wait for a started process to end, then free it with process_free().
void process_wait(struct process *p);
void process_free(struct process *p);

// The running thread's process, or NULL in a kernel thread.
struct process *process_current(void);

// End the running process: free its memory, wake process_wait() and exit
// its thread. Interrupts on. Set p->status / p->fault first.
__attribute__((noreturn)) void process_exit(void);

// An exception in user mode (isr_dispatch): report it and kill the process.
__attribute__((noreturn)) void process_fault(struct interrupt_frame *f);

// Write to the process's fd 1/2 sink: its `out` buffer, or the console.
void process_output(struct process *p, const char *s, size_t n);

// Copy between the kernel and the running process's memory. 0, or -EFAULT
// if any byte of the user range is not mapped for the user (or, for
// copy_to_user, not writable); nothing is copied then.
int copy_from_user(void *dst, uint64_t user_src, size_t n);
int copy_to_user(uint64_t user_dst, const void *src, size_t n);

// Whether [addr, addr + n) is all mapped for the running process's user
// mode (and writable, with `write`).
bool user_range_ok(uint64_t addr, size_t n, bool write);

// Boot self-test: load a test program from an ELF image built in memory
// and run it, and check that broken images are refused with -ENOEXEC. Run
// the built-in programs in ring 3. One makes every
// system call and checks the results, including -EFAULT for kernel and
// unmapped pointers; one reads a kernel address and one runs `cli`, and both
// get killed; two spin at once, at the same addresses in their own address
// spaces, and get preempted. No memory leaks. Writes a line for the boot
// log; panics on failure.
void process_self_test(char *line, size_t size);
