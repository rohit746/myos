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

// Offsets in struct cpu_local, for assembly (and this_cpu()/thread_current()).
#define CPU_LOCAL_KERNEL_RSP 8
#define CPU_LOCAL_USER_RSP   16
#define CPU_LOCAL_CURRENT    24

// CPUs the kernel runs on at most. Any more stay parked in the bootloader.
// 64 so a set of CPUs fits in a uint64_t.
#define MAX_CPUS 64

#ifndef __ASSEMBLER__

#include <stdbool.h>

struct thread;

// Each CPU's own data, cpus[index]. While a CPU runs kernel code, its GS
// base points at its entry; while it runs user code, the GS base is the
// user's (0) and the kernel's value waits in MSR_KERNEL_GS_BASE. `swapgs`
// swaps the two, on every way into the kernel from user mode and back out
// (isr.S, syscall.S), so `%gs:` always reaches this CPU's entry in the
// kernel.
//
// Code that may move to another CPU halfway (interrupts on, preemption on)
// must not keep the pointer this_cpu() returned: it may be another CPU's by
// the time it is used. thread_current() is always right, since whichever
// CPU a thread is on, that CPU's `current` is that thread.
struct cpu_local {
    struct cpu_local *self;
    uint64_t kernel_rsp;     // top of the running thread's kernel stack (= TSS.rsp0)
    uint64_t user_rsp;       // scratch: the user stack pointer during syscall entry
    struct thread *current;  // the thread on this CPU; NULL until it joins the scheduler
    unsigned index;          // 0 is the bootstrap processor (BSP)
    uint32_t lapic_id;
    uint32_t acpi_uid;       // ACPI processor UID (what the MADT's NMI entries name)
    volatile bool online;    // runs threads and answers IPIs
    struct thread *idle;     // runs `hlt` when nothing else is ready
    struct thread *zombie;   // exited here; freed once the switch away from it is done
    volatile uint64_t cr3;   // the page tables loaded now (sched.c), for TLB shootdowns
};

extern struct cpu_local cpus[MAX_CPUS];

// This CPU's entry in cpus[]. See struct cpu_local for when it can be kept.
static inline struct cpu_local *this_cpu(void) {
    struct cpu_local *c;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(c));
    return c;
}

// What the interrupt stubs in isr.S push, lowest address first.
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    // Pushed by the CPU:
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);

// Point the GS base at cpus[0] (the BSP's), so this_cpu() works from the
// first line of kmain on.
void cpu_early_init(void);

// Load the BSP's GDT and TSS, then an application processor's: its own
// GDT, TSS (double faults go to `df_stack_top`) and GS base.
void gdt_init(void);
void gdt_init_ap(struct cpu_local *c, uint64_t df_stack_top);

// Fill the IDT and load it; idt_load() loads it on an application processor.
void idt_init(void);
void idt_load(void);

// The stack this CPU switches to when an interrupt (TSS.rsp0) or system
// call (cpu_local.kernel_rsp) comes from user mode. The scheduler points it
// at the running thread's kernel stack. Interrupts must be off.
void tss_set_rsp0(uint64_t rsp0);
uint64_t tss_get_rsp0(void);

// Install a handler for vector 32..255 (hardware IRQs, syscalls, ...).
void idt_set_handler(uint8_t vector, interrupt_handler_t handler);

// "Page fault", for vectors 0..31.
const char *exception_name(uint64_t vector);

#endif
