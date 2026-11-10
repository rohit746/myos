// Interrupt entry stubs.
//
// One 16-byte stub per vector (0..255), laid out back to back starting at
// `isr_stubs`, so the stub for vector N is at isr_stubs + N * 16.
// Each stub makes the stack look the same (a dummy error code is pushed
// when the CPU doesn't push one), pushes its vector number, and jumps to
// isr_common, which saves registers and calls isr_dispatch() in C.

.section .text

.global isr_stubs
.balign 16
isr_stubs:
.set vec, 0
.rept 256
    .balign 16
    // These exceptions push an error code themselves.
    .if (vec == 8) || (vec == 10) || (vec == 11) || (vec == 12) || (vec == 13) || (vec == 14) || (vec == 17) || (vec == 21) || (vec == 29) || (vec == 30)
    .else
        pushq $0
    .endif
    pushq $vec
    jmp isr_common
    .set vec, vec + 1
.endr

isr_common:
    cld
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

    // Stack is 16-byte aligned here (CPU frame + 2 + 15 pushes = 176 bytes).
    movq %rsp, %rdi
    call isr_dispatch

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
    popq %rax
    addq $16, %rsp // drop vector + error code
    iretq
