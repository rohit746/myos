#pragma once
// Signals (Linux's numbers). There is no way to send or catch them yet: a
// process killed for a fault (a bad memory access, an invalid instruction)
// ends with the one a Unix would have sent, as waitpid shows.

#define SIGILL  4
#define SIGTRAP 5
#define SIGBUS  7
#define SIGFPE  8
#define SIGSEGV 11
