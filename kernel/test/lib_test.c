// The kernel's own library functions: strings, formatting, CRC-32.

#include <crc32.h>
#include <kprintf.h>
#include <ktest.h>
#include <string.h>

KTEST(string_functions) {
    char buf[8];
    KEXPECT_EQ(strlen(""), 0);
    KEXPECT_EQ(strlen("myos"), 4);
    KEXPECT_EQ(strcmp("abc", "abc"), 0);
    KASSERT(strcmp("abc", "abd") < 0);
    KASSERT(strcmp("b", "abc") > 0);
    memset(buf, 'x', sizeof buf);
    memcpy(buf, "hello", 6);
    KEXPECT_EQ(memcmp(buf, "hello\0xx", 8), 0);
    memmove(buf + 1, buf, 5); // overlapping
    KEXPECT_EQ(memcmp(buf, "hhello", 6), 0);
}

KTEST(ksnprintf_formats) {
    char buf[32];
    KEXPECT_EQ(ksnprintf(buf, sizeof buf, "%d %u %x %s %c", -42, 7u, 0xbeef, "ok", '!'), 15);
    KEXPECT_EQ(strcmp(buf, "-42 7 beef ok !"), 0);
    ksnprintf(buf, sizeof buf, "[%5d|%-4s|%08lx]", 12, "ab", 0x1234ul);
    KEXPECT_EQ(strcmp(buf, "[   12|ab  |00001234]"), 0);
    // Truncated, still NUL-terminated, and the full length returned.
    KEXPECT_EQ(ksnprintf(buf, 4, "%s", "truncated"), 9);
    KEXPECT_EQ(strcmp(buf, "tru"), 0);
}

KTEST(crc32_check_value) {
    KEXPECT_EQ(crc32(0, "123456789", 9), 0xcbf43926);
    // In pieces, carrying the CRC over.
    KEXPECT_EQ(crc32(crc32(0, "1234", 4), "56789", 5), 0xcbf43926);
    KEXPECT_EQ(crc32(0, "", 0), 0);
}
