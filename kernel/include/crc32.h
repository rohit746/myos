#pragma once
// CRC-32 (IEEE 802.3, the one zlib, gzip, PNG and GPT use).

#include <stddef.h>
#include <stdint.h>

// Continue a CRC over `n` more bytes: start with crc = 0, and feed it the
// result of the previous call to checksum data that comes in pieces.
uint32_t crc32(uint32_t crc, const void *data, size_t n);
