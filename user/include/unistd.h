#pragma once

#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// File descriptors (open is in fcntl.h). A process the kernel starts has
// the console as 0, 1 and 2; a child gets copies of its parent's. Reading
// the console waits for a key, then gives the bytes typed so far, raw: no
// echo, no line editing.
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
int dup(int fd);
int dup2(int fd, int new_fd);

pid_t getpid(void);
pid_t getppid(void); // 0 for a process the kernel started
__attribute__((noreturn)) void _exit(int status);

// myos's own: run the program at `path` in a new process, a child of this
// one with copies of its file descriptors. Its pid, for waitpid
// (sys/wait.h); -1 with errno set if it can't be started.
pid_t spawn(const char *path, char *const argv[], char *const envp[]);

// myos's own, for the shell: run a command of the kernel's built-in shell
// (`help`, `mem`, `reboot`, ...), which prints to the console. 0 once it is
// done; -1 with ENOENT if there is no such command.
int kernel_command(char *const argv[]);

// The raw system call: the number, then up to six arguments. Returns what
// the kernel returned, a negative errno value on failure (errno untouched).
long __syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5);
