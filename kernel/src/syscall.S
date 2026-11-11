// The way into the kernel for system calls, and the first way out to user
// mode.

#include <arch.h>

.section .text

// `syscall` lands here (MSR_LSTAR) in ring 0, with:
//   rcx  the user's rip (the instruction after `syscall`)
//   r11  the user's rflags; FMASK has cleared IF, DF, TF, AC and NT
//   rsp  still the user's stack, and GS base still the user's
// Switch to the thread's kernel stack, save the user's registers in the
// layout of struct interrupt_frame (so a system call and an interrupt look
// the same to C), and call syscall_dispatch with interrupts on.
.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    swapgs                                   // GS base: struct cpu_local
    movq %rsp, %gs:CPU_LOCAL_USER_RSP
    movq %gs:CPU_LOCAL_KERNEL_RSP, %rsp

    pushq $(GDT_USER_DATA | 3)               // ss
    pushq %gs:CPU_LOCAL_USER_RSP             // rsp
    pushq %r11                               // rflags
    pushq $(GDT_USER_CODE | 3)               // cs
    pushq %rcx                               // rip
    pushq $0                                 // error code
    pushq $0                                 // vector (none: not an interrupt)
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    // 22 pushes from a 16-byte aligned stack top: still aligned.
    xorl %ebp, %ebp // backtraces stop here
    sti
    movq %rsp, %rdi
    call syscall_dispatch
    cli

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax                                // the result
    addq $16, %rsp                           // vector, error code

    // sysret takes rip from rcx and rflags from r11. The rip is canonical:
    // nothing is mapped in the last page of the lower half (see proc.h), so
    // no `syscall` can sit at its very end.
    movq 0(%rsp), %rcx
    movq 16(%rsp), %r11
    movq 24(%rsp), %rsp                      // the user's stack, interrupts still off
    swapgs                                   // GS base: the user's
    sysretq
.size syscall_entry, . - syscall_entry

// void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg)
//
// Leave the kernel for good: start running user code at `rip` with stack
// `rsp`, `arg` in rdi, interrupts on and every other register zero, so
// nothing of the kernel's is left in them.
.global enter_user
.type enter_user, @function
enter_user:
    cli
    pushq $(GDT_USER_DATA | 3)               // ss
    pushq %rsi                               // rsp
    pushq $0x202                             // rflags: IF
    pushq $(GDT_USER_CODE | 3)               // cs
    pushq %rdi                               // rip
    movq %rdx, %rdi
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    swapgs                                   // GS base: the user's
    iretq
.size enter_user, . - enter_user
