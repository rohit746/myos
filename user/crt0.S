// _start: where every user program begins. The kernel leaves rsp pointing
// at argc, with argv, envp and the auxiliary vector above it (see
// kernel/include/proc.h), and every other register zero.

.section .text
.global _start
.type _start, @function
_start:
    xorl %ebp, %ebp          // backtraces stop here
    movq %rsp, %rdi          // __libc_start(sp)
    andq $-16, %rsp          // the ABI wants rsp 16-byte aligned at a call
    call __libc_start
    ud2                      // __libc_start never returns
.size _start, . - _start
