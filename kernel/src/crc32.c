// CRC-32, a table of 256 entries built on first use. See crc32.h.

#include <crc32.h>
#include <stdbool.h>

static uint32_t table[256];
static bool have_table;

uint32_t crc32(uint32_t crc, const void *data, size_t n) {
    if (!have_table) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        have_table = true; // a race only builds the same table twice
    }
    const uint8_t *p = data;
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}
