#pragma once
// Freestanding replacements for the libc memory/string functions.
// The compiler may emit calls to memcpy/memset/memmove/memcmp on its own,
// so these must always exist.

#include <stddef.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
