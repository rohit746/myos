#pragma once
// x86-64 descriptor tables and interrupt plumbing.

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

// IST slot used for the double-fault handler, so a stack overflow
// produces a readable crash instead of a silent reboot.
#define IST_DOUBLE_FAULT 1

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

// Install a handler for vector 32..255 (hardware IRQs, syscalls, ...).
void idt_set_handler(uint8_t vector, interrupt_handler_t handler);
