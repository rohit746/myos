// printf and friends. See stdio.h.
//
// One formatter (format) writes through a `struct out`: into a caller's
// buffer for snprintf, or into a small buffer that goes to a file
// descriptor whenever it fills up, and at the end, for printf.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct out {
    char *buf;
    size_t size;  // bytes `buf` holds
    size_t len;   // bytes in it now
    size_t total; // bytes formatted so far, whether they fit or not
    int fd;       // flush to this when full; -1: keep what fits, drop the rest
    bool failed;  // a write failed
};

static void flush(struct out *o) {
    for (size_t done = 0; done < o->len;) {
        ssize_t n = write(o->fd, o->buf + done, o->len - done);
        if (n <= 0) {
            o->failed = true;
            break;
        }
        done += (size_t)n;
    }
    o->len = 0;
}

static void put(struct out *o, const char *s, size_t n) {
    o->total += n;
    while (n > 0) {
        if (o->len == o->size) {
            if (o->fd < 0) return;
            flush(o);
        }
        size_t room = o->size - o->len, k = n < room ? n : room;
        memcpy(o->buf + o->len, s, k);
        o->len += k;
        s += k;
        n -= k;
    }
}

static void pad(struct out *o, char c, int n) {
    for (; n > 0; n--) put(o, &c, 1);
}

// One conversion's flags, width and precision (-1: none given).
struct spec {
    bool left, zero, plus, space, alt;
    int width, prec;
};

// `prefix` (a sign, "0x"), then the digits, padded to the width.
static void emit_number(struct out *o, const struct spec *sp, const char *prefix, const char *digits, int ndigits) {
    int plen = (int)strlen(prefix);
    int zeros = sp->prec > ndigits ? sp->prec - ndigits : 0;
    int fill = sp->width - plen - zeros - ndigits;
    if (fill < 0) fill = 0;
    if (!sp->left && !(sp->zero && sp->prec < 0)) pad(o, ' ', fill);
    put(o, prefix, (size_t)plen);
    if (!sp->left && sp->zero && sp->prec < 0) pad(o, '0', fill);
    pad(o, '0', zeros);
    put(o, digits, (size_t)ndigits);
    if (sp->left) pad(o, ' ', fill);
}

static void emit_unsigned(struct out *o, const struct spec *sp, unsigned long long v, unsigned base, bool upper,
                          const char *sign, char conv) {
    char tmp[24]; // 64 bits in octal: 22 digits
    int n = 0;
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v != 0 || sp->prec != 0) { // %.0d of 0 prints no digits
        do {
            tmp[sizeof tmp - 1 - n++] = d[v % base];
            v /= base;
        } while (v);
    }
    const char *prefix = sign;
    if (sp->alt && conv == 'o' && (n == 0 || tmp[sizeof tmp - n] != '0')) prefix = "0";
    else if ((sp->alt && (conv == 'x' || conv == 'X') && n > 0 && !(n == 1 && tmp[sizeof tmp - 1] == '0')) ||
             conv == 'p')
        prefix = conv == 'X' ? "0X" : "0x";
    emit_number(o, sp, prefix, tmp + sizeof tmp - n, n);
}

static void emit_string(struct out *o, const struct spec *sp, const char *s) {
    if (s == NULL) s = "(null)";
    size_t n = sp->prec >= 0 ? strnlen(s, (size_t)sp->prec) : strlen(s);
    int fill = sp->width > (int)n ? sp->width - (int)n : 0;
    if (!sp->left) pad(o, ' ', fill);
    put(o, s, n);
    if (sp->left) pad(o, ' ', fill);
}

enum length { LEN_HH, LEN_H, LEN_NONE, LEN_L, LEN_LL };

static void format(struct out *o, const char *fmt, va_list ap) {
    while (*fmt) {
        if (*fmt != '%') {
            const char *start = fmt;
            while (*fmt && *fmt != '%') fmt++;
            put(o, start, (size_t)(fmt - start));
            continue;
        }
        const char *start = fmt++;
        struct spec sp = {.width = 0, .prec = -1};
        for (;; fmt++) {
            if (*fmt == '-') sp.left = true;
            else if (*fmt == '0') sp.zero = true;
            else if (*fmt == '+') sp.plus = true;
            else if (*fmt == ' ') sp.space = true;
            else if (*fmt == '#') sp.alt = true;
            else break;
        }
        if (*fmt == '*') {
            sp.width = va_arg(ap, int);
            if (sp.width < 0) {
                sp.left = true;
                sp.width = -sp.width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') sp.width = sp.width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            sp.prec = 0;
            if (*fmt == '*') {
                sp.prec = va_arg(ap, int);
                if (sp.prec < 0) sp.prec = -1;
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') sp.prec = sp.prec * 10 + (*fmt++ - '0');
            }
        }
        enum length len = LEN_NONE;
        if (fmt[0] == 'h' && fmt[1] == 'h') len = LEN_HH, fmt += 2;
        else if (fmt[0] == 'h') len = LEN_H, fmt++;
        else if (fmt[0] == 'l' && fmt[1] == 'l') len = LEN_LL, fmt += 2;
        else if (fmt[0] == 'l' || fmt[0] == 'z' || fmt[0] == 'j' || fmt[0] == 't') len = LEN_L, fmt++;

        char conv = *fmt;
        if (conv == '\0') { // a lone % at the end: print it as it is
            put(o, start, (size_t)(fmt - start));
            break;
        }
        fmt++;
        switch (conv) {
        case 'd':
        case 'i': {
            long long v;
            if (len >= LEN_L) v = len == LEN_LL ? va_arg(ap, long long) : va_arg(ap, long);
            else v = va_arg(ap, int);
            if (len == LEN_HH) v = (signed char)v;
            else if (len == LEN_H) v = (short)v;
            unsigned long long mag = v < 0 ? 0 - (unsigned long long)v : (unsigned long long)v;
            emit_unsigned(o, &sp, mag, 10, false, v < 0 ? "-" : sp.plus ? "+" : sp.space ? " " : "", conv);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long v;
            if (len >= LEN_L) v = len == LEN_LL ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned);
            if (len == LEN_HH) v = (unsigned char)v;
            else if (len == LEN_H) v = (unsigned short)v;
            emit_unsigned(o, &sp, v, conv == 'u' ? 10 : conv == 'o' ? 8 : 16, conv == 'X', "", conv);
            break;
        }
        case 'p':
            emit_unsigned(o, &sp, (uintptr_t)va_arg(ap, void *), 16, false, "", 'p');
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            int fill = sp.width > 1 ? sp.width - 1 : 0;
            if (!sp.left) pad(o, ' ', fill);
            put(o, &c, 1);
            if (sp.left) pad(o, ' ', fill);
            break;
        }
        case 's': emit_string(o, &sp, va_arg(ap, const char *)); break;
        case '%': put(o, "%", 1); break;
        default: put(o, start, (size_t)(fmt - start)); break; // unknown: print it as it is
        }
    }
}

int vsnprintf(char *restrict buf, size_t size, const char *restrict fmt, va_list ap) {
    struct out o = {.buf = buf, .size = size ? size - 1 : 0, .fd = -1};
    format(&o, fmt, ap);
    if (size) buf[o.len] = '\0';
    return (int)o.total;
}

int snprintf(char *restrict buf, size_t size, const char *restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int vdprintf(int fd, const char *restrict fmt, va_list ap) {
    char buf[256];
    struct out o = {.buf = buf, .size = sizeof buf, .fd = fd};
    format(&o, fmt, ap);
    flush(&o);
    return o.failed ? -1 : (int)o.total;
}

int dprintf(int fd, const char *restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *restrict fmt, va_list ap) { return vdprintf(STDOUT_FILENO, fmt, ap); }

int printf(const char *restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s) { return printf("%s\n", s) < 0 ? EOF : 0; }

int putchar(int c) {
    char ch = (char)c;
    return write(STDOUT_FILENO, &ch, 1) == 1 ? (unsigned char)ch : EOF;
}
