#pragma once
// Function names for code addresses, and backtraces, for the crash screens
// (panic() and CPU exceptions).
//
// The names come from a table of the kernel's own symbols that the build
// links into it (scripts/mksyms.py), so a photo of a crashed machine's
// screen says which functions it was in: kmain+0x1a3, not a bare address.

#include <stdint.h>

// The function `addr` is in, and how far into it: its name, with *offset
// set, or NULL when `addr` isn't in the kernel's code (or the kernel was
// built without the table).
const char *symbolize(uint64_t addr, uint64_t *offset);

// Print "    #N <address> <function>+0x<offset>" for each frame: `rip`
// first, when it isn't 0, then the return address in each frame on the
// chain of saved frame pointers from `rbp` (the kernel is built with
// -fno-omit-frame-pointer). Stops at the end of the chain, and with a line
// saying why at a frame pointer that can't be read (not in the kernel half,
// not aligned, not mapped), so a corrupt stack doesn't fault again.
void backtrace_print(uint64_t rip, uint64_t rbp);
