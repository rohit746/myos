#include <console.h>
#include <cpu.h>
#include <kprintf.h>
#include <serial.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// The formatter writes through a sink so the same code serves kprintf
// (screen + serial) and ksnprintf (memory buffer).
struct sink {
    void (*put)(struct sink *s, char c);
    char *buf;
    size_t size, len;
};

static void put_out(struct sink *s, char c) {
    serial_putc(c);
    console_putc(c);
    s->len++;
}

static void put_buf(struct sink *s, char c) {
    if (s->len + 1 < s->size) s->buf[s->len] = c;
    s->len++;
}

static void emit_padded(struct sink *s, const char *str, size_t len, int width, bool left, char pad) {
    int fill = width > (int)len ? width - (int)len : 0;
    if (!left) while (fill-- > 0) s->put(s, pad);
    for (size_t i = 0; i < len; i++) s->put(s, str[i]);
    if (left) while (fill-- > 0) s->put(s, ' ');
}

static void format(struct sink *s, const char *fmt, va_list ap) {
    char tmp[32];

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            s->put(s, *fmt);
            continue;
        }
        fmt++;

        bool left = false, zero = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else break;
        }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');

        int lng = 0; // 0 = int, 1 = long, 2 = long long / size_t
        while (*fmt == 'l' || *fmt == 'z') {
            lng += *fmt == 'z' ? 2 : 1;
            fmt++;
        }

        char pad = zero && !left ? '0' : ' ';
        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            bool neg = v < 0;
            uint64_t u = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            int n = 0;
            do tmp[sizeof tmp - 1 - n++] = '0' + u % 10; while (u /= 10);
            if (neg) {
                if (pad == '0') { s->put(s, '-'); width--; }
                else tmp[sizeof tmp - 1 - n++] = '-';
            }
            emit_padded(s, tmp + sizeof tmp - n, n, width, left, pad);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'p': {
            uint64_t u;
            unsigned base = *fmt == 'u' ? 10 : 16;
            const char *digits = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            if (*fmt == 'p') {
                u = (uint64_t)va_arg(ap, void *);
                s->put(s, '0');
                s->put(s, 'x');
                width = 16;
                pad = '0';
            } else {
                u = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            }
            int n = 0;
            do tmp[sizeof tmp - 1 - n++] = digits[u % base]; while (u /= base);
            emit_padded(s, tmp + sizeof tmp - n, n, width, left, pad);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            emit_padded(s, str, strlen(str), width, left, ' ');
            break;
        }
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            emit_padded(s, tmp, 1, width, left, ' ');
            break;
        case '%':
            s->put(s, '%');
            break;
        case '\0':
            return;
        default: // unknown conversion: print it verbatim
            s->put(s, '%');
            s->put(s, *fmt);
            break;
        }
    }
}

void kvprintf(const char *fmt, va_list ap) {
    struct sink s = {.put = put_out};
    format(&s, fmt, ap);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    struct sink s = {.put = put_buf, .buf = buf, .size = size};
    va_list ap;
    va_start(ap, fmt);
    format(&s, fmt, ap);
    va_end(ap);
    if (size) buf[s.len < size ? s.len : size - 1] = '\0';
    return (int)s.len;
}

void panic(const char *fmt, ...) {
    cli();
    console_set_color(COLOR_WHITE, COLOR_PANIC);
    kprintf("\n *** KERNEL PANIC *** ");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf(" \n");
    halt_forever();
}
