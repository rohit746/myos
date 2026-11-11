// The system call wrappers: a failure returns -1 (MAP_FAILED for mmap)
// and sets errno, the way POSIX says.

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int errno;

// A raw result: -4095..-1 are errors.
static long check(long r) {
    if (r < 0 && r >= -4095) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

ssize_t read(int fd, void *buf, size_t count) {
    return check(__syscall(SYS_read, fd, (long)buf, (long)count, 0, 0, 0));
}

ssize_t write(int fd, const void *buf, size_t count) {
    return check(__syscall(SYS_write, fd, (long)buf, (long)count, 0, 0, 0));
}

int open(const char *path, int flags, ...) { return (int)check(__syscall(SYS_open, (long)path, flags, 0, 0, 0, 0)); }
int close(int fd) { return (int)check(__syscall(SYS_close, fd, 0, 0, 0, 0, 0)); }
int dup(int fd) { return (int)check(__syscall(SYS_dup, fd, 0, 0, 0, 0, 0)); }
int dup2(int fd, int new_fd) { return (int)check(__syscall(SYS_dup2, fd, new_fd, 0, 0, 0, 0)); }

pid_t getpid(void) { return (pid_t)__syscall(SYS_getpid, 0, 0, 0, 0, 0, 0); }
pid_t getppid(void) { return (pid_t)__syscall(SYS_getppid, 0, 0, 0, 0, 0, 0); }

pid_t spawn(const char *path, char *const argv[], char *const envp[]) {
    return (pid_t)check(__syscall(SYS_spawn, (long)path, (long)argv, (long)envp, 0, 0, 0));
}

pid_t waitpid(pid_t pid, int *status, int options) {
    return (pid_t)check(__syscall(SYS_wait4, pid, (long)status, options, 0, 0, 0));
}

int kernel_command(char *const argv[]) { return (int)check(__syscall(SYS_kernel_command, (long)argv, 0, 0, 0, 0, 0)); }

void _exit(int status) {
    __syscall(SYS_exit, status, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    return (void *)check(__syscall(SYS_mmap, (long)addr, (long)length, prot, flags, fd, offset));
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    return (int)check(__syscall(SYS_nanosleep, (long)req, (long)rem, 0, 0, 0, 0));
}

int clock_gettime(clockid_t clock, struct timespec *ts) {
    return (int)check(__syscall(SYS_clock_gettime, clock, (long)ts, 0, 0, 0, 0));
}
