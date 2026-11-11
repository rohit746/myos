#pragma once
// Formatted output to stdout (fd 1), or another descriptor with dprintf. No
// FILE streams: printf writes what it formatted when it returns, so nothing
// needs flushing.
//
// Conversions: d i u x X o c s p %, with the flags - 0 + space #, a width
// and a precision (numbers or *), and the lengths hh h l ll z j t. No
// floating point: user programs are built without SSE.

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

__attribute__((format(printf, 1, 2))) int printf(const char *restrict fmt, ...);
__attribute__((format(printf, 1, 0))) int vprintf(const char *restrict fmt, va_list ap);
__attribute__((format(printf, 2, 3))) int dprintf(int fd, const char *restrict fmt, ...);
__attribute__((format(printf, 2, 0))) int vdprintf(int fd, const char *restrict fmt, va_list ap);
__attribute__((format(printf, 3, 4))) int snprintf(char *restrict buf, size_t size, const char *restrict fmt, ...);
__attribute__((format(printf, 3, 0))) int vsnprintf(char *restrict buf, size_t size, const char *restrict fmt,
                                                     va_list ap);
int puts(const char *s);
int putchar(int c);
