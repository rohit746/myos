// exit, the environment and number conversions.

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void exit(int status) {
    // Nothing is buffered (stdio.h), so there is nothing to flush.
    _exit(status);
}

void abort(void) {
    write(STDERR_FILENO, "abort()\n", 8);
    _exit(127);
}

char *getenv(const char *name) {
    size_t n = strlen(name);
    for (char **e = environ; e && *e; e++)
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return *e + n + 1;
    return NULL;
}

int abs(int n) { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }

// Digits of `base` (2..36; 0 picks from a 0x or 0 prefix) after optional
// spaces and a sign. Sets *neg, and *end to just past the last digit (or
// `s` if there were none). Saturates at ULONG_MAX with ERANGE.
static unsigned long parse(const char *s, char **end, int base, int *neg) {
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    *neg = 0;
    if (*p == '+' || *p == '-') *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        // Only a prefix if a hex digit follows; "0x" alone is the number 0.
        char c = p[2];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            p += 2;
            base = 16;
        }
    }
    if (base == 0) base = p[0] == '0' ? 8 : 10;
    unsigned long v = 0;
    int any = 0, overflow = 0;
    for (;; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;
        any = 1;
        if (v > (ULONG_MAX - (unsigned long)d) / (unsigned long)base) overflow = 1;
        else v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end) *end = (char *)(any ? p : s);
    if (overflow) {
        errno = ERANGE;
        return ULONG_MAX;
    }
    return v;
}

long strtol(const char *restrict s, char **restrict end, int base) {
    int neg;
    unsigned long v = parse(s, end, base, &neg);
    if (neg) {
        if (v > (unsigned long)LONG_MAX + 1) {
            errno = ERANGE;
            return LONG_MIN;
        }
        return (long)(0 - v);
    }
    if (v > LONG_MAX) {
        errno = ERANGE;
        return LONG_MAX;
    }
    return (long)v;
}

unsigned long strtoul(const char *restrict s, char **restrict end, int base) {
    int neg;
    unsigned long v = parse(s, end, base, &neg);
    return neg ? 0 - v : v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
