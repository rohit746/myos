// Built-in user-mode test programs (until there is an ELF loader, #19).
//
// Each is flat, position-independent code between `user_<name>` and
// `user_<name>_end`. The kernel copies it to USER_CODE_BASE in a new
// process and runs it in ring 3 from its first byte, with the process's
// argument in rdi. The bytes sit in .rodata: the kernel never runs them.
// Each program ends with exit(); a `ud2` after it would get the process
// killed if exit ever returned.

#include <errno.h>
#include <syscall.h>

#define SYSCALL(n) movl $n, %eax; syscall
#define KERNEL_ADDR 0xffffffff80000000 // the start of the kernel image

.section .rodata.user_programs, "a"

// hello: print a greeting, then make every system call and check what it
// returns. exit(0) if all is well, else exit(n) for the first check n that
// failed.
.global user_hello, user_hello_end
user_hello:
    // 1: write(1, greeting) writes all of it.
    movl $1, %ebx
    movl $1, %edi
    leaq greeting(%rip), %rsi
    movl $(greeting_end - greeting), %edx
    SYSCALL(SYS_write)
    cmpq $(greeting_end - greeting), %rax
    jne hello_fail

    // 2: getpid() is positive. Keep it in r12.
    movl $2, %ebx
    SYSCALL(SYS_getpid)
    testq %rax, %rax
    jle hello_fail
    movq %rax, %r12

    // 3: nanosleep(20 ms) returns 0 and takes at least 20 ms by
    // clock_gettime(CLOCK_MONOTONIC). Two timespecs on the stack: the
    // start time at 0(%rsp), the end time at 16(%rsp).
    movl $3, %ebx
    subq $32, %rsp
    movl $CLOCK_MONOTONIC, %edi
    movq %rsp, %rsi
    SYSCALL(SYS_clock_gettime)
    testq %rax, %rax
    jnz hello_fail
    movq $0, 16(%rsp)
    movq $20000000, 24(%rsp)
    leaq 16(%rsp), %rdi
    xorl %esi, %esi
    SYSCALL(SYS_nanosleep)
    testq %rax, %rax
    jnz hello_fail
    movl $CLOCK_MONOTONIC, %edi
    leaq 16(%rsp), %rsi
    SYSCALL(SYS_clock_gettime)
    testq %rax, %rax
    jnz hello_fail
    movq 16(%rsp), %rax                      // (end - start) in ns
    subq 0(%rsp), %rax
    imulq $1000000000, %rax
    addq 24(%rsp), %rax
    subq 8(%rsp), %rax
    addq $32, %rsp
    cmpq $20000000, %rax
    jl hello_fail

    // 4: mmap two pages, read/write: they are zero, and hold what is
    // written to them. Keep the address in r13.
    movl $4, %ebx
    xorl %edi, %edi
    movl $8192, %esi
    movl $(PROT_READ | PROT_WRITE), %edx
    movl $(MAP_PRIVATE | MAP_ANONYMOUS), %r10d
    movq $-1, %r8
    xorl %r9d, %r9d
    SYSCALL(SYS_mmap)
    cmpq $-4095, %rax                        // -4095..-1 are errors
    jae hello_fail
    movq %rax, %r13
    cmpq $0, (%r13)
    jne hello_fail
    cmpq $0, 8184(%r13)
    jne hello_fail
    movq %r12, (%r13)
    movq %r12, 8184(%r13)
    cmpq %r12, (%r13)
    jne hello_fail
    cmpq %r12, 8184(%r13)
    jne hello_fail

    // 5: bad pointers fail with -EFAULT instead of crashing anything: a
    // kernel address, NULL, a buffer that runs off the end of the mmapped
    // pages, clock_gettime into the read-only code, nanosleep from a kernel
    // address.
    movl $5, %ebx
    movl $1, %edi
    movabsq $KERNEL_ADDR, %rsi
    movl $8, %edx
    SYSCALL(SYS_write)
    cmpq $-EFAULT, %rax
    jne hello_fail
    movl $1, %edi
    xorl %esi, %esi
    movl $8, %edx
    SYSCALL(SYS_write)
    cmpq $-EFAULT, %rax
    jne hello_fail
    movl $1, %edi
    leaq 8000(%r13), %rsi
    movl $1000, %edx
    SYSCALL(SYS_write)
    cmpq $-EFAULT, %rax
    jne hello_fail
    movl $CLOCK_MONOTONIC, %edi
    leaq user_hello(%rip), %rsi
    SYSCALL(SYS_clock_gettime)
    cmpq $-EFAULT, %rax
    jne hello_fail
    movabsq $KERNEL_ADDR, %rdi
    xorl %esi, %esi
    SYSCALL(SYS_nanosleep)
    cmpq $-EFAULT, %rax
    jne hello_fail

    // 6: other bad arguments: fd 5, mmap of 0 bytes and of a file, an
    // unknown system call.
    movl $6, %ebx
    movl $5, %edi
    leaq greeting(%rip), %rsi
    movl $1, %edx
    SYSCALL(SYS_write)
    cmpq $-EBADF, %rax
    jne hello_fail
    xorl %edi, %edi
    xorl %esi, %esi
    movl $PROT_READ, %edx
    movl $(MAP_PRIVATE | MAP_ANONYMOUS), %r10d
    movq $-1, %r8
    xorl %r9d, %r9d
    SYSCALL(SYS_mmap)
    cmpq $-EINVAL, %rax
    jne hello_fail
    xorl %edi, %edi
    movl $4096, %esi
    movl $PROT_READ, %edx
    movl $MAP_PRIVATE, %r10d
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    SYSCALL(SYS_mmap)
    cmpq $-EINVAL, %rax
    jne hello_fail
    movl $1000, %eax
    syscall
    cmpq $-ENOSYS, %rax
    jne hello_fail

    // 7: a system call keeps every register but rax, rcx and r11.
    movl $7, %ebx
    movq $0x1111, %rdx
    movq $0x2222, %rsi
    movq $0x3333, %rdi
    movq $0x4444, %rbp
    movq $0x5555, %r8
    movq $0x6666, %r9
    movq $0x7777, %r10
    movq $0x8888, %r14
    movq $0x9999, %r15
    movq %rsp, %r13                          // (the mmap address is no longer needed)
    SYSCALL(SYS_getpid)
    cmpq %r12, %rax
    jne hello_fail
    cmpq $0x1111, %rdx
    jne hello_fail
    cmpq $0x2222, %rsi
    jne hello_fail
    cmpq $0x3333, %rdi
    jne hello_fail
    cmpq $0x4444, %rbp
    jne hello_fail
    cmpq $0x5555, %r8
    jne hello_fail
    cmpq $0x6666, %r9
    jne hello_fail
    cmpq $0x7777, %r10
    jne hello_fail
    cmpq $0x8888, %r14
    jne hello_fail
    cmpq $0x9999, %r15
    jne hello_fail
    cmpq %r13, %rsp
    jne hello_fail
    cmpl $7, %ebx
    jne hello_fail

    xorl %edi, %edi
    SYSCALL(SYS_exit)
    ud2
hello_fail:
    movl %ebx, %edi
    SYSCALL(SYS_exit)
    ud2
greeting:
    .ascii "Hello from ring 3!\n"
greeting_end:
user_hello_end:

// kread: read a kernel address. The page is mapped, but not for user mode:
// the process must get killed by a page fault.
.global user_kread, user_kread_end
user_kread:
    movabsq $KERNEL_ADDR, %rax
    movq (%rax), %rax
    xorl %edi, %edi
    SYSCALL(SYS_exit)
    ud2
user_kread_end:

// priv: run `cli`, which ring 3 may not. Killed by a general protection
// fault.
.global user_priv, user_priv_end
user_priv:
    cli
    xorl %edi, %edi
    SYSCALL(SYS_exit)
    ud2
user_priv_end:

// spin: busy-loop in ring 3 for rdi TSC ticks, without a system call, so
// only the timer can take the CPU away. Its pid sits on its stack the whole
// time; exit(0) if it is still there at the end (no other process wrote to
// this one's memory, at the same addresses in its own address space), else
// exit(1).
.global user_spin, user_spin_end
user_spin:
    movq %rdi, %r12
    SYSCALL(SYS_getpid)
    movq %rax, %r13
    pushq %rax
    rdtsc
    shlq $32, %rdx
    orq %rdx, %rax
    addq %rax, %r12                          // the deadline
1:
    pause
    rdtsc
    shlq $32, %rdx
    orq %rdx, %rax
    cmpq %r12, %rax
    jb 1b
    movl $1, %edi
    cmpq %r13, (%rsp)
    jne 2f
    xorl %edi, %edi
2:
    SYSCALL(SYS_exit)
    ud2
user_spin_end:
