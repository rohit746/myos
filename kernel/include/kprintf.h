#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

// printf for the kernel. Writes to the serial port and the screen. One
// call's output stays together: CPUs take turns (an interrupt handler on the
// CPU that is printing prints in the middle).
// Supports %d %i %u %x %X %p %s %c %%, the flags '-' and '0', a field width,
// and the length modifiers l, ll, z.
__attribute__((format(printf, 1, 2))) void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);

// Write `size` bytes as they are (NUL bytes included), like kprintf("%s").
void kwrite(const char *buf, size_t size);

// Format into a buffer. Always NUL-terminates when size > 0.
// Returns the length the full output would have had.
__attribute__((format(printf, 3, 4))) int ksnprintf(char *buf, size_t size, const char *fmt, ...);

// Keep every other CPU from printing until kprintf_release(), which gets
// what kprintf_hold() returned (while the screen's device is switched off,
// say). Turns preemption off meanwhile.
bool kprintf_hold(void);
void kprintf_release(bool held);

// From now on print without taking turns: this CPU is panicking and has
// stopped the others, one of which may have been printing (smp.c).
void kprintf_break_lock(void);

// Print a message with a red banner and stop the machine (every CPU).
__attribute__((noreturn, format(printf, 1, 2))) void panic(const char *fmt, ...);
