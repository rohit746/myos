// /bin/init: the first program the kernel runs, from the initramfs.
//
// First it checks that a C program works in user mode: what the kernel put
// on its stack (argv, the environment, the auxiliary vector), its segments
// (.data, .rodata, .bss), the libc (strings, printf, malloc, the system
// calls), file descriptors, and child processes (spawn, waitpid, a child
// that crashes). It prints a line for each check that fails.
//
// Then it runs the shell, /bin/sh, and waits for it; when the shell ends it
// starts a new one. Run from the shell instead (it has a parent then), it
// only does the checks, and exits with how many failed.

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void _start(void);

static int failed;

#define CHECK(cond)                                                                                               \
    do {                                                                                                          \
        if (!(cond)) {                                                                                            \
            printf("init: check failed, line %d: %s\n", __LINE__, #cond);                                        \
            failed++;                                                                                             \
        }                                                                                                         \
    } while (0)

// snprintf(...) must give `want`, and return its length.
#define CHECK_FORMAT(want, ...)                                                                                   \
    do {                                                                                                          \
        char got_[96];                                                                                            \
        int n_ = snprintf(got_, sizeof got_, __VA_ARGS__);                                                        \
        if (strcmp(got_, want) != 0 || n_ != (int)strlen(want)) {                                                 \
            printf("init: check failed, line %d: snprintf(%s) gave \"%s\" (%d), not \"%s\"\n", __LINE__,          \
                   #__VA_ARGS__, got_, n_, want);                                                                 \
            failed++;                                                                                             \
        }                                                                                                         \
    } while (0)

// An ELF program header, as AT_PHDR points to them.
struct phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

static int answer = 42;                     // .data
static const char greeting[] = "rodata";    // .rodata
static unsigned char zeroes[3 * 4096 + 5];  // .bss, over several pages

static void check_startup(int argc, char **argv, char **envp) {
    CHECK(argc >= 1);
    CHECK(argv[argc] == NULL);
    const char *slash = strrchr(argv[0], '/');
    CHECK(strcmp(slash ? slash + 1 : argv[0], "init") == 0);
    CHECK(envp == environ);
    CHECK(getenv("PATH") && strcmp(getenv("PATH"), "/bin") == 0);
    CHECK(getenv("HOME") && strcmp(getenv("HOME"), "/") == 0);
    CHECK(getenv("PAT") == NULL && getenv("NOPE") == NULL);

    CHECK(getauxval(AT_PAGESZ) == 4096);
    CHECK(getauxval(AT_ENTRY) == (unsigned long)_start);
    CHECK(getauxval(AT_PHENT) == sizeof(struct phdr));
    CHECK(getauxval(12345) == 0);
    // The program headers in memory: one of them loads the code.
    const struct phdr *ph = (const struct phdr *)getauxval(AT_PHDR);
    unsigned long phnum = getauxval(AT_PHNUM);
    CHECK(ph != NULL && phnum >= 3);
    bool found = false;
    for (unsigned long i = 0; ph && i < phnum; i++)
        if (ph[i].type == 1 && (ph[i].flags & 1) && (uintptr_t)_start - ph[i].vaddr < ph[i].memsz) found = true;
    CHECK(found);
}

static void check_segments(void) {
    CHECK(answer == 42);
    answer++;
    CHECK(answer == 43);
    CHECK(strcmp(greeting, "rodata") == 0);
    bool zero = true;
    for (size_t i = 0; i < sizeof zeroes; i++)
        if (zeroes[i]) zero = false;
    CHECK(zero);
    zeroes[0] = 1;
    zeroes[sizeof zeroes - 1] = 2;
    CHECK(zeroes[0] == 1 && zeroes[sizeof zeroes - 1] == 2);
}

static void check_strings(void) {
    char buf[32];
    CHECK(strlen("") == 0 && strlen("hello") == 5);
    CHECK(strnlen("hello", 3) == 3);
    CHECK(strcmp("abc", "abc") == 0 && strcmp("abc", "abd") < 0 && strcmp("b", "abc") > 0);
    CHECK(strcmp("\xff", "a") > 0); // compared as unsigned char
    CHECK(strncmp("abcx", "abcy", 3) == 0 && strncmp("abcx", "abcy", 4) < 0);
    CHECK(memcmp("ab\0c", "ab\0d", 4) < 0);
    CHECK(strcpy(buf, "foo") == buf && strcmp(strcat(buf, "bar"), "foobar") == 0);
    const char *h = "hello", *hay = "haystack";
    CHECK(strchr(h, 'l') == h + 2 && strrchr(h, 'l') == h + 3);
    CHECK(strchr(h, '\0') == h + 5 && strchr(h, 'z') == NULL);
    CHECK(strstr(hay, "st") == hay + 3 && strstr(hay, "x") == NULL && strstr(hay, "") == hay);
    memset(buf, 'x', sizeof buf);
    CHECK(buf[0] == 'x' && buf[31] == 'x');
    strcpy(buf, "0123456789");
    memmove(buf + 2, buf, 5); // overlapping, forwards
    CHECK(strcmp(buf, "0101234789") == 0);
    memmove(buf, buf + 3, 5); // overlapping, backwards
    CHECK(strcmp(buf, "1234734789") == 0);
    CHECK(memchr(buf, '4', 10) == buf + 3 && memchr(buf, 'z', 10) == NULL);
    char *d = strdup("copy");
    CHECK(d && strcmp(d, "copy") == 0);
    free(d);
    strncpy(buf, "ab", 4);
    CHECK(buf[0] == 'a' && buf[2] == '\0' && buf[3] == '\0');
    CHECK(strcmp(strerror(ENOENT), "No such file or directory") == 0);
}

static void check_numbers(void) {
    char *end;
    CHECK(atoi("  -123x") == -123);
    CHECK(strtol("0x1F", &end, 0) == 31 && *end == '\0');
    CHECK(strtol("0755", NULL, 0) == 0755);
    CHECK(strtol("z", &end, 36) == 35);
    CHECK(strtoul("ffffffffffffffff", NULL, 16) == ULONG_MAX);
    errno = 0;
    CHECK(strtol("99999999999999999999", NULL, 10) == LONG_MAX && errno == ERANGE);
    CHECK(strtol("-9223372036854775808", NULL, 10) == LONG_MIN);
    CHECK(strtol("abc", &end, 10) == 0 && *end == 'a');
    CHECK(abs(-5) == 5 && labs(-5L) == 5);
}

static void check_printf(void) {
    CHECK_FORMAT("-42 0 4000000000", "%d %i %u", -42, 0, 4000000000u);
    CHECK_FORMAT("   42|42   |00042|-0042", "%5d|%-5d|%05d|%05d", 42, 42, 42, -42);
    CHECK_FORMAT("ff FF 0xff 0XFF 377 0377 0 0", "%x %X %#x %#X %o %#o %#x %#o", 255, 255, 255, 255, 255, 255, 0, 0);
    CHECK_FORMAT("-9223372036854775808 18446744073709551615", "%lld %lu", LLONG_MIN, ULONG_MAX);
    CHECK_FORMAT("deadbeefcafe 7 -3", "%lx %zu %td", 0xdeadbeefcafeUL, (size_t)7, (ptrdiff_t)-3);
    CHECK_FORMAT("hello|hel|   hello|hello   |(null)", "%s|%.3s|%8s|%-8s|%s", "hello", "hello", "hello", "hello",
                 (char *)NULL);
    CHECK_FORMAT("ok|  x|y  |%", "%c%c|%3c|%-3c|%%", 'o', 'k', 'x', 'y');
    CHECK_FORMAT("0x1234 0x0", "%p %p", (void *)0x1234, (void *)0);
    CHECK_FORMAT("00042 +5  5 -5", "%.5d %+d % d %+d", 42, 5, 5, -5);
    CHECK_FORMAT("    42|42    |  042", "%*d|%-*d|%*.*d", 6, 42, 6, 42, 5, 3, 42);
    CHECK_FORMAT("44 4464 -1", "%hhu %hd %hhd", 300, 70000, 255);
    CHECK_FORMAT("|", "%.0d|", 0);

    char small[5];
    CHECK(snprintf(small, sizeof small, "hello world") == 11 && strcmp(small, "hell") == 0);
    CHECK(snprintf(NULL, 0, "%d", 12345) == 5);
    // printf counts what it wrote; a long one goes out in several writes.
    char line[400];
    memset(line, '.', sizeof line - 1);
    line[sizeof line - 1] = '\0';
    CHECK(snprintf(NULL, 0, "%s%s", line, line) == 2 * (int)(sizeof line - 1));
}

static void check_malloc(void) {
    enum { N = 64 };
    unsigned char *blocks[N];
    size_t sizes[N];
    for (int i = 0; i < N; i++) {
        sizes[i] = (size_t)(i * 37 % 500) + 1;
        blocks[i] = malloc(sizes[i]);
        CHECK(blocks[i] != NULL && (uintptr_t)blocks[i] % 16 == 0);
        if (blocks[i]) memset(blocks[i], i, sizes[i]);
    }
    for (int i = 0; i < N; i += 2) free(blocks[i]); // every other one
    for (int i = 0; i < N; i += 2) {
        blocks[i] = malloc(sizes[i]);
        CHECK(blocks[i] != NULL);
        if (blocks[i]) memset(blocks[i], i, sizes[i]);
    }
    bool intact = true;
    for (int i = 0; i < N; i++)
        for (size_t j = 0; blocks[i] && j < sizes[i]; j++)
            if (blocks[i][j] != (unsigned char)i) intact = false;
    CHECK(intact);

    // realloc keeps the contents; calloc zeroes (even reused memory).
    unsigned char *r = realloc(blocks[1], 5000);
    CHECK(r != NULL && r[0] == 1 && r[sizes[1] - 1] == 1);
    blocks[1] = r;
    for (int i = 0; i < N; i++) free(blocks[i]);
    int *z = calloc(1000, sizeof *z);
    bool zero = z != NULL;
    for (int i = 0; z && i < 1000; i++)
        if (z[i]) zero = false;
    CHECK(zero);
    free(z);
    CHECK(calloc(SIZE_MAX / 2, 4) == NULL && errno == ENOMEM);

    // Bigger than an arena: its own mmap.
    size_t big = 1 << 20;
    unsigned char *b = malloc(big);
    CHECK(b != NULL);
    if (b) {
        b[0] = 1;
        b[big - 1] = 2;
        CHECK(b[0] == 1 && b[big - 1] == 2);
    }
    free(b);
    // Freed memory gets used again: 1 GiB in all, in a machine with less.
    bool reused = true;
    for (int i = 0; i < 1024 && reused; i++) {
        void *m = malloc(big);
        if (m == NULL) reused = false;
        free(m);
    }
    CHECK(reused);
}

static void check_syscalls(void) {
    CHECK(getpid() > 0);
    struct timespec t0, t1, ms = {0, 1000000};
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t0) == 0);
    CHECK(nanosleep(&ms, NULL) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t1) == 0);
    CHECK((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec) >= 1000000);
    errno = 0;
    CHECK(write(99, "x", 1) == -1 && errno == EBADF);
    CHECK(write(STDOUT_FILENO, (void *)0xffffffff80000000, 1) == -1 && errno == EFAULT);
    CHECK(clock_gettime(7, &t0) == -1 && errno == EINVAL);
}

#define SCRATCH "/init.out" // a file the checks write (tmpfs: gone at the next boot)

static void check_files(void) {
    char buf[64];
    int fd = open("/etc/motd", O_RDONLY);
    CHECK(fd >= 3); // 0, 1 and 2 are the console
    CHECK(read(fd, buf, 7) == 7 && memcmp(buf, "Welcome", 7) == 0);
    CHECK(read(fd, buf, 0) == 0);
    CHECK(close(fd) == 0);
    CHECK(close(fd) == -1 && errno == EBADF);
    CHECK(read(fd, buf, 1) == -1 && errno == EBADF);
    CHECK(open("/nope", O_RDONLY) == -1 && errno == ENOENT);
    CHECK(open("/etc", O_WRONLY) == -1 && errno == EISDIR);
    CHECK(open("/etc/motd", O_RDONLY | 0x80000) == -1 && errno == EINVAL); // O_CLOEXEC: not yet
    CHECK(open((char *)0xffffffff80000000, O_RDONLY) == -1 && errno == EFAULT);
    CHECK(write(STDIN_FILENO, "", 0) == 0); // the console, open for reading and writing

    // dup and dup2 share the open file, and its offset.
    fd = open(SCRATCH, O_RDWR | O_CREAT | O_TRUNC);
    CHECK(fd >= 0);
    int fd2 = dup(fd);
    CHECK(fd2 > fd);
    CHECK(write(fd, "ab", 2) == 2 && write(fd2, "cd", 2) == 2);
    CHECK(dup2(fd, 9) == 9 && dup2(9, 9) == 9);
    CHECK(close(fd) == 0 && close(fd2) == 0);
    CHECK(write(9, "e", 1) == 1); // still open through 9
    CHECK(close(9) == 0);
    CHECK(dup(42) == -1 && errno == EBADF);
    CHECK(dup2(STDOUT_FILENO, 99) == -1 && errno == EBADF);
    fd = open(SCRATCH, O_RDONLY);
    CHECK(fd >= 0 && read(fd, buf, sizeof buf) == 5 && memcmp(buf, "abcde", 5) == 0);
    close(fd);

    // Open every descriptor there is.
    int fds[32], n = 0;
    while (n < 32 && (fds[n] = open("/dev/null", O_RDONLY)) >= 0) n++;
    CHECK(n > 0 && n < 32 && errno == EMFILE);
    while (n > 0) close(fds[--n]);
}

// Point stdout and stderr at `path`; restore() puts them back.
static int saved_out, saved_err;

static void redirect(const char *path) {
    saved_out = dup(STDOUT_FILENO);
    saved_err = dup(STDERR_FILENO);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0 && dup2(fd, STDOUT_FILENO) == STDOUT_FILENO && dup2(fd, STDERR_FILENO) == STDERR_FILENO);
    close(fd);
}

static void restore(void) {
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);
}

// Start `argv` as a child with stdout and stderr sent to `out`, and wait for
// it: its wait status, or -1.
static int run_child(char *const argv[], const char *out) {
    redirect(out);
    pid_t pid = spawn(argv[0], argv, environ);
    restore();
    CHECK(pid > 0 && pid != getpid());
    if (pid <= 0) return -1;
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(waitpid(pid, &status, 0) == -1 && errno == ECHILD); // reaped already
    return status;
}

static void check_processes(void) {
    CHECK(getppid() != getpid());
    CHECK(waitpid(-1, NULL, 0) == -1 && errno == ECHILD); // no children yet

    // A child that prints, into a file through its inherited stdout.
    char *echo[] = {"/bin/echo", "from", "a child", NULL};
    int status = run_child(echo, SCRATCH);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char buf[64] = "";
    int fd = open(SCRATCH, O_RDONLY);
    CHECK(fd >= 0 && read(fd, buf, sizeof buf) == 13 && memcmp(buf, "from a child\n", 13) == 0);
    close(fd);

    // One that exits with a status, and one that crashes: the kernel says
    // why on its stderr.
    char *cat[] = {"/bin/cat", "/nope", NULL};
    status = run_child(cat, "/dev/null");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    char *crash[] = {"/bin/crash", NULL};
    status = run_child(crash, SCRATCH);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    char report[256];
    fd = open(SCRATCH, O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, report, sizeof report - 1) : -1;
    report[n > 0 ? n : 0] = '\0';
    CHECK(strstr(report, "(crash) killed: page fault at 0x0000000000000000 (write, not mapped)") != NULL);
    close(fd);

    // Two at once, reaped in either order; WNOHANG doesn't wait.
    char *quiet[] = {"/bin/echo", NULL};
    redirect("/dev/null");
    pid_t a = spawn(quiet[0], quiet, environ), b = spawn(quiet[0], quiet, environ);
    restore();
    CHECK(a > 0 && b > 0 && a != b);
    pid_t r = waitpid(b, &status, WNOHANG);
    CHECK(r == 0 || r == b);
    if (r == 0) CHECK(waitpid(b, &status, 0) == b);
    CHECK(waitpid(-1, &status, 0) == a && WIFEXITED(status));
    CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);

    // Programs that can't be started.
    CHECK(spawn("/nope", quiet, environ) == -1 && errno == ENOENT);
    CHECK(spawn("/etc/motd", quiet, environ) == -1 && errno == ENOEXEC);
    CHECK(spawn("/bin", quiet, environ) == -1 && errno == EACCES);
    CHECK(spawn("/bin/echo", (char **)0xffffffff80000000, environ) == -1 && errno == EFAULT);
    CHECK(waitpid(0, &status, 0) == -1 && errno == EINVAL);
}

// Run the shell, and a new one whenever it ends.
static void run_shell(void) {
    char *argv[] = {"sh", NULL};
    for (;;) {
        pid_t pid = spawn("/bin/sh", argv, environ);
        if (pid < 0) {
            printf("init: can't start /bin/sh: %s\n", strerror(errno));
            return;
        }
        printf("init: started /bin/sh, pid %d\n", pid);
        int status;
        while (waitpid(pid, &status, 0) != pid) {}
        if (WIFSIGNALED(status)) printf("init: /bin/sh was killed by signal %d; starting a new one\n", WTERMSIG(status));
        else printf("init: /bin/sh exited with status %d; starting a new one\n", WEXITSTATUS(status));
        // A second first, so a shell that keeps failing at once doesn't flood the screen.
        struct timespec second = {1, 0};
        nanosleep(&second, NULL);
    }
}

int main(int argc, char **argv, char **envp) {
    printf("init: hello from %s, pid %d, loaded from the initramfs\n", argv[0], getpid());
    check_startup(argc, argv, envp);
    check_segments();
    check_strings();
    check_numbers();
    check_printf();
    check_malloc();
    check_syscalls();
    check_files();
    check_processes();
    if (failed) printf("init: %d checks failed\n", failed);
    else printf("init: libc self-test passed (argv/envp/auxv, segments, strings, printf, malloc, syscalls, "
                "files, processes)\n");
    if (getppid() != 0) return failed; // run from the shell: just the checks
    run_shell();
    return 1;
}
