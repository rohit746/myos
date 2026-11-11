#pragma once

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

// The heap: blocks from memory mmap hands out, 16-byte aligned.
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void abort(void);

// "NAME=value" from the environment: the value, or NULL.
char *getenv(const char *name);
extern char **environ;

int abs(int n);
long labs(long n);
int atoi(const char *s);
long strtol(const char *restrict s, char **restrict end, int base);
unsigned long strtoul(const char *restrict s, char **restrict end, int base);
