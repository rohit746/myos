#pragma once
// x86-64 descriptor tables, interrupt plumbing and per-CPU data.
// The #defines are also for assembly (isr.S, syscall.S).

#ifndef __ASSEMBLER__
#include <stdint.h>
#endif

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

// IST slot used for the double-fault handler, so a stack overflow
// produces a readable crash instead of a silent reboot.
#define IST_DOUBLE_FAULT 1

// Offsets in struct cpu_local, for assembly.
#define CPU_LOCAL_KERNEL_RSP 8
#define CPU_LOCAL_USER_RSP   16

#ifndef __ASSEMBLER__

// This CPU's own data. While the CPU runs kernel code, the GS base points
// here; while it runs user code, the GS base is the user's (0) and the
// kernel's value waits in MSR_KERNEL_GS_BASE. `swapgs` swaps the two, on
// every way into the kernel from user mode and back out (isr.S,
// syscall.S), so `%gs:` always reaches this struct in the kernel.
struct cpu_local {
    struct cpu_local *self;
    uint64_t kernel_rsp; // top of the running thread's kernel stack (= TSS.rsp0)
    uint64_t user_rsp;   // scratch: the user stack pointer during syscall entry
};

// What the interrupt stubs in isr.S push, lowest address first.
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    // Pushed by the CPU:
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);

void gdt_init(void);
void idt_init(void);

// The stack the CPU switches to when an interrupt (TSS.rsp0) or system call
// (cpu_local.kernel_rsp) comes from user mode. The scheduler points it at
// the running thread's kernel stack.
void tss_set_rsp0(uint64_t rsp0);
uint64_t tss_get_rsp0(void);

// Install a handler for vector 32..255 (hardware IRQs, syscalls, ...).
void idt_set_handler(uint8_t vector, interrupt_handler_t handler);

// "Page fault", for vectors 0..31.
const char *exception_name(uint64_t vector);

#endif
