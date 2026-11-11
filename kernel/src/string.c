#include <errno.h>
#include <stdint.h>
#include <string.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    for (; n--; p++, q++) {
        if (*p != *q) return *p < *q ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

const char *strerror(int err) {
    switch (err < 0 ? -err : err) {
    case 0: return "Success";
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case EIO: return "Input/output error";
    case E2BIG: return "Argument list too long";
    case ENOEXEC: return "Exec format error";
    case EBADF: return "Bad file descriptor";
    case ECHILD: return "No child processes";
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
    case ENAMETOOLONG: return "File name too long";
    case ENOSYS: return "Function not implemented";
    default: return "Unknown error";
    }
}
