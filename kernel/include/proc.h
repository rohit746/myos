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
// check) the kernel kills the process, says why on its stderr, and carries
// on. The kernel touches
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
//
// Parents and children: a process the kernel starts (process_start) has no
// parent; the kernel waits for it with process_wait and frees it. A process
// that a process starts (process_spawn, the spawn system call) is its
// child: when it ends it stays a zombie, holding only its exit status,
// until its parent reaps it with process_waitpid. If the parent ends first
// the child is an orphan, and nobody waits for it: it frees itself when it
// ends (and an orphan that is already a zombie is freed then and there).
//
// File descriptors: each process has a table of PROC_MAX_FDS descriptors,
// each an open file of the VFS (vfs.h) or free. A process the kernel starts
// gets /dev/console as 0, 1 and 2 (stdin, stdout, stderr); a child gets a
// copy of its parent's table, sharing the open files and their offsets.

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
#define PROC_MAX_FDS     16

// waitpid options (Linux's value).
#define WNOHANG 1

struct interrupt_frame;

struct process {
    uint64_t pid;
    char name[24];
    struct vmm_space space;
    uint64_t entry;     // where it starts running
    uint64_t user_rsp;  // its stack pointer at the start
    uint64_t arg;       // rdi at the entry point
    uint64_t mmap_next; // where the next mmap goes
    struct semaphore done; // up once a process without a parent has ended and its memory is freed
    int fds[PROC_MAX_FDS]; // open files of the VFS, -1 for a free descriptor

    // The process tree, under a lock of its own (proc.c).
    struct process *parent;   // NULL if the kernel started it, or it is an orphan
    struct process *children; // its children that haven't been reaped, newest first
    struct process *sibling;  // the next child of the same parent
    struct condvar child_ended; // broadcast when one of its children becomes a zombie
    bool zombie;              // ended, and waiting for its parent to reap it
    bool orphan;              // its parent ended first: it frees itself

    // For the self-test: with `out` set, writes to fd 1 and 2 go into this
    // buffer instead (kept NUL-terminated; what doesn't fit is dropped).
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
// with process_free). One without a parent first gets /dev/console as fds
// 0, 1 and 2.
bool process_start(struct process *p);

// Start the executable at `path` as a child of `parent`, which is the
// running process, with a copy of its file descriptors. Returns the
// child's pid, or process_exec's errors.
int64_t process_spawn(struct process *parent, const char *path, char *const argv[], char *const envp[]);

// Wait for a child of the running process `p` to end (any child, with pid
// -1), reap it and return its pid, with its wait status in *status the way
// Linux encodes it: (exit status << 8), or the signal that killed it (see
// process_signal). With WNOHANG, returns 0 if no such child has ended yet.
// -ECHILD if there is no such child, -EINVAL for other pids or options.
int64_t process_waitpid(struct process *p, int64_t pid, int *status, int options);

// The pid of the running process `p`'s parent, or 0 if it has none.
uint64_t process_parent_pid(struct process *p);

// The signal a Unix would have sent for exception `vector` (SIGSEGV for a
// page fault, SIGFPE for a divide error, ...).
int process_signal(int vector);

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

// Write to the process's `out` buffer; without one, to its stderr (fd 2),
// or the console if it has none. Interrupts on.
void process_output(struct process *p, const char *s, size_t n);

// Copy between the kernel and the running process's memory. 0, or -EFAULT
// if any byte of the user range is not mapped for the user (or, for
// copy_to_user, not writable); nothing is copied then.
int copy_from_user(void *dst, uint64_t user_src, size_t n);
int copy_to_user(uint64_t user_dst, const void *src, size_t n);

// Copy the NUL-terminated string at `user_src` into `dst` (`size` bytes, NUL
// included). Its length, or -EFAULT if it runs into memory the user can't
// read, or -ENAMETOOLONG if it doesn't fit.
int64_t copy_string_from_user(char *dst, uint64_t user_src, size_t size);

// Whether [addr, addr + n) is all mapped for the running process's user
// mode (and writable, with `write`).
bool user_range_ok(uint64_t addr, size_t n, bool write);

// Boot self-test: load a test program from an ELF image built in memory
// and run it, and check that broken images are refused with -ENOEXEC. Run
// the built-in programs in ring 3. One makes every
// system call and checks the results, including -EFAULT for kernel and
// unmapped pointers; one reads a kernel address and one runs `cli`, and both
// get killed; two spin at once, at the same addresses in their own address
// spaces, and get preempted. No memory leaks. (The process tree and the
// file descriptors are checked from user space, by /bin/init.) Writes a
// line for the boot log; panics on failure.
void process_self_test(char *line, size_t size);
