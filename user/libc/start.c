// __libc_start: called by _start (crt0.S) with the stack pointer the
// kernel gave the program. Finds argc, argv, envp and the auxiliary vector
// there, then runs main and exits with what it returns.

#include <stdint.h>
#include <stdlib.h>
#include <sys/auxv.h>

int main(int argc, char **argv, char **envp);

char **environ;
static unsigned long *auxv;

unsigned long getauxval(unsigned long type) {
    for (unsigned long *a = auxv; a && a[0] != AT_NULL; a += 2)
        if (a[0] == type) return a[1];
    return 0;
}

__attribute__((noreturn)) void __libc_start(uint64_t *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    char **e = envp;
    while (*e) e++;
    environ = envp;
    auxv = (unsigned long *)(e + 1);
    exit(main(argc, argv, envp));
}
