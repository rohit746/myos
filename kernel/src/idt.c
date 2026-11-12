// Interrupt Descriptor Table, plus the C side of interrupt handling.

#include <arch.h>
#include <console.h>
#include <cpu.h>
#include <kprintf.h>
#include <proc.h>
#include <smp.h>
#include <stddef.h>
#include <stdint.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define STUB_SIZE 16
#define GATE_INTERRUPT 0x8e // present, ring 0, 64-bit interrupt gate

extern char isr_stubs[];

static struct idt_entry idt[256];
static interrupt_handler_t handlers[256];

static const char *const exception_names[32] = {
    "Divide error", "Debug", "Non-maskable interrupt", "Breakpoint",
    "Overflow", "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS", "Segment not present",
    "Stack-segment fault", "General protection fault", "Page fault", "Reserved",
    "x87 floating-point exception", "Alignment check", "Machine check", "SIMD floating-point exception",
    "Virtualization exception", "Control protection exception", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection exception", "VMM communication exception", "Security exception", "Reserved",
};

static void set_gate(uint8_t vec, uint64_t handler, uint8_t ist) {
    idt[vec] = (struct idt_entry){
        .offset_low = handler & 0xffff,
        .selector = GDT_KERNEL_CODE,
        .ist = ist,
        .type_attr = GATE_INTERRUPT,
        .offset_mid = (handler >> 16) & 0xffff,
        .offset_high = handler >> 32,
    };
}

void idt_init(void) {
    for (int v = 0; v < 256; v++) {
        uint8_t ist = v == 8 ? IST_DOUBLE_FAULT : 0;
        set_gate((uint8_t)v, (uint64_t)(isr_stubs + v * STUB_SIZE), ist);
    }
    idt_load();
}

// Every CPU shares the one IDT (and handler table).
void idt_load(void) {
    struct idtr idtr = {.limit = sizeof idt - 1, .base = (uint64_t)idt};
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}

void idt_set_handler(uint8_t vector, interrupt_handler_t handler) {
    handlers[vector] = handler;
}

const char *exception_name(uint64_t vector) { return vector < 32 ? exception_names[vector] : "Interrupt"; }

// Walk the chain of saved frame pointers (the kernel is built with
// -fno-omit-frame-pointer) and print return addresses.
static void backtrace(uint64_t rbp) {
    kprintf("  backtrace:\n");
    for (int depth = 0; depth < 16; depth++) {
        if (rbp == 0 || (rbp & 7) || rbp < 0xffff800000000000) break;
        uint64_t *frame = (uint64_t *)rbp;
        uint64_t ret = frame[1];
        if (ret == 0) break;
        kprintf("    #%d %p\n", depth, (void *)ret);
        rbp = frame[0];
    }
}

static void __attribute__((noreturn)) exception_panic(struct interrupt_frame *f) {
    cli();
    smp_panic_begin();
    kprintf("\n" ANSI_PANIC " *** CPU EXCEPTION %lu: %s *** " ANSI_RESET "\n", f->vector, exception_names[f->vector]);

    kprintf("  error code: 0x%lx\n", f->error_code);
    if (f->vector == 14) {
        uint64_t e = f->error_code;
        kprintf("  faulting address (cr2): %p  [%s, %s, %s%s]\n", (void *)read_cr2(),
                e & 1 ? "protection violation" : "page not present",
                e & 2 ? "write" : "read",
                e & 4 ? "user mode" : "kernel mode",
                e & 16 ? ", instruction fetch" : "");
    }
    kprintf("  rip=%p cs=%04lx rflags=%016lx\n", (void *)f->rip, f->cs, f->rflags);
    kprintf("  rsp=%p ss=%04lx\n", (void *)f->rsp, f->ss);
    kprintf("  rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx\n", f->rax, f->rbx, f->rcx, f->rdx);
    kprintf("  rsi=%016lx rdi=%016lx rbp=%016lx\n", f->rsi, f->rdi, f->rbp);
    kprintf("  r8 =%016lx r9 =%016lx r10=%016lx r11=%016lx\n", f->r8, f->r9, f->r10, f->r11);
    kprintf("  r12=%016lx r13=%016lx r14=%016lx r15=%016lx\n", f->r12, f->r13, f->r14, f->r15);
    kprintf("  cr0=%016lx cr3=%016lx cr4=%016lx\n", read_cr0(), read_cr3(), read_cr4());
    backtrace(f->rbp);
    kprintf("\n  System halted.\n");
    halt_forever();
}

// Called from isr_common in isr.S for every interrupt and exception.
void isr_dispatch(struct interrupt_frame *f) {
    // Another CPU panicked and stopped this one with an NMI (smp.c). This
    // may be before the entry code's swapgs, so nothing here uses %gs.
    if (f->vector == 2 && smp_stopping()) smp_stop_this_cpu();
    // An exception in user code is the process's problem, not the kernel's:
    // it gets killed. NMIs, double faults and machine checks are about the
    // machine, whatever was running.
    if (f->vector < 32 && (f->cs & 3) && f->vector != 2 && f->vector != 8 && f->vector != 18)
        process_fault(f);
    if (handlers[f->vector]) {
        handlers[f->vector](f);
        return;
    }
    if (f->vector < 32) exception_panic(f);
    kprintf("[irq] unhandled interrupt vector %lu\n", f->vector);
}
