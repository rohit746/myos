// Thread switching.

.section .text

// void switch_context(uint64_t *save_rsp, uint64_t new_rsp)
//
// Push the callee-saved registers on the current stack, store the stack
// pointer in *save_rsp, load new_rsp and pop the registers the new thread
// pushed when it was switched away from. `ret` then returns into the new
// thread, where it called switch_context. The caller-saved registers are the
// compiler's to save; RFLAGS is not switched (interrupts are off here, and
// each thread puts its own flags back afterwards).
.global switch_context
.type switch_context, @function
switch_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size switch_context, . - switch_context

// Where a new thread starts: thread_create's stack makes switch_context
// "return" here with the entry point in r12 and its argument in r13, and
// rbp 0 so backtraces stop.
.global thread_start
.type thread_start, @function
thread_start:
    movq %r12, %rdi
    movq %r13, %rsi
    andq $-16, %rsp // the ABI's alignment at a call
    call thread_main
    ud2             // thread_main never returns
.size thread_start, . - thread_start
