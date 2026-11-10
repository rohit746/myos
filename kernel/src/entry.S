// Kernel entry point. Limine jumps here in 64-bit long mode with paging on,
// interrupts off and a valid stack.

.section .text
.global _start
.type _start, @function
_start:
    // Zero the frame pointer so stack traces know where to stop.
    xorl %ebp, %ebp
    call kmain
    // kmain never returns; if it does, stop here.
1:  cli
    hlt
    jmp 1b
.size _start, . - _start
