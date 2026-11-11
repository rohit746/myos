// The string and memory functions.

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        while (n--) d[n] = s[n];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (; n--; x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = s;
    for (; n--; p++)
        if (*p == (uint8_t)c) return (void *)p;
    return NULL;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

size_t strnlen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    for (; *a && *a == *b; a++, b++) {}
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n && *a && *a == *b; n--, a++, b++) {}
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

char *strcpy(char *restrict dst, const char *restrict src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

// Copy at most `n` bytes, and pad with NULs to `n` (not NUL-terminated if
// `src` is that long, as the standard says).
char *strncpy(char *restrict dst, const char *restrict src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *restrict dst, const char *restrict src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (*s == '\0') return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (*s == '\0') return (char *)last;
    }
}

char *strstr(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    for (; *haystack; haystack++)
        if (strncmp(haystack, needle, n) == 0) return (char *)haystack;
    return n == 0 ? (char *)haystack : NULL;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    return d ? memcpy(d, s, n) : NULL;
}

char *strerror(int err) {
    switch (err) {
    case 0: return "Success";
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case EIO: return "Input/output error";
    case E2BIG: return "Argument list too long";
    case ENOEXEC: return "Exec format error";
    case EBADF: return "Bad file descriptor";
    case ENOMEM: return "Out of memory";
    case EACCES: return "Permission denied";
    case EFAULT: return "Bad address";
    case EBUSY: return "Device or resource busy";
    case EEXIST: return "File exists";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case EMFILE: return "Too many open files";
    case EFBIG: return "File too large";
    case ENOSPC: return "No space left on device";
    case EROFS: return "Read-only file system";
    case ERANGE: return "Result out of range";
    case ENAMETOOLONG: return "File name too long";
    case ENOSYS: return "Function not implemented";
    default: return "Unknown error";
    }
}
