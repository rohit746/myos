// long __syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5)
//
// The C calling convention passes n, a0..a4 in rdi, rsi, rdx, rcx, r8, r9
// and a5 on the stack; `syscall` wants the number in rax and the arguments
// in rdi, rsi, rdx, r10, r8, r9. It clobbers rcx and r11, which the C
// convention lets a call clobber anyway.

.section .text
.global __syscall
.type __syscall, @function
__syscall:
    movq %rdi, %rax
    movq %rsi, %rdi
    movq %rdx, %rsi
    movq %rcx, %rdx
    movq %r8, %r10
    movq %r9, %r8
    movq 8(%rsp), %r9
    syscall
    ret
.size __syscall, . - __syscall
