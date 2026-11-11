#pragma once

#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// fd 1 and 2 write to the console; there are no other files yet.
ssize_t write(int fd, const void *buf, size_t count);
pid_t getpid(void);
__attribute__((noreturn)) void _exit(int status);

// The raw system call: the number, then up to six arguments. Returns what
// the kernel returned, a negative errno value on failure (errno untouched).
long __syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5);
