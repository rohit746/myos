// crash: write to address 0, which is never mapped. The kernel kills the
// process for the page fault; whoever started it (the shell) carries on,
// and waitpid says it was killed by SIGSEGV.

#include <stdint.h>
#include <stdio.h>

int main(void) {
    printf("crash: writing to address 0...\n");
    volatile uintptr_t zero = 0; // read at run time, so the compiler can't see the null pointer
    *(volatile int *)zero = 1;
    printf("crash: the write did not fault\n");
    return 1;
}
