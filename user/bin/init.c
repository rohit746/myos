// /bin/init: the first program the kernel loads from a file.
//
// There is nothing for it to start yet (no fork or exec), so it checks that
// a C program works in user mode: what the kernel put on its stack (argv,
// the environment, the auxiliary vector), its segments (.data, .rodata,
// .bss), and the libc (strings, printf, malloc, the system calls). It
// prints a line for each check that fails and exits with how many did, so
// the kernel's boot log shows "exited with status 0" when all is well.

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
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

int main(int argc, char **argv, char **envp) {
    printf("init: hello from %s, pid %d, loaded from the initramfs\n", argv[0], getpid());
    check_startup(argc, argv, envp);
    check_segments();
    check_strings();
    check_numbers();
    check_printf();
    check_malloc();
    check_syscalls();
    if (failed) printf("init: %d checks failed\n", failed);
    else printf("init: libc self-test passed (argv/envp/auxv, segments, strings, printf, malloc, syscalls)\n");
    return failed;
}
