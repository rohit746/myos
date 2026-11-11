#pragma once
// The auxiliary vector: what the kernel tells a program about itself, on
// its stack after the environment.

#define AT_NULL   0
#define AT_PHDR   3 // the program headers in memory
#define AT_PHENT  4 // the size of one
#define AT_PHNUM  5 // how many
#define AT_PAGESZ 6
#define AT_ENTRY  9 // the program's entry point

// The value for `type`, or 0 if the kernel gave none.
unsigned long getauxval(unsigned long type);
