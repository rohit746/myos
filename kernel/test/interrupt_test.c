// Interrupts: the IDT's handlers run and return.

#include <arch.h>
#include <ktest.h>
#include <stddef.h>

static volatile bool breakpoint_hit;

static void breakpoint_handler(struct interrupt_frame *f) {
    (void)f;
    breakpoint_hit = true;
}

// A breakpoint exception reaches its handler, and execution resumes after it.
KTEST(int3_handled_and_returns) {
    breakpoint_hit = false;
    idt_set_handler(3, breakpoint_handler);
    __asm__ volatile("int3");
    idt_set_handler(3, NULL);
    KASSERT(breakpoint_hit);
}
