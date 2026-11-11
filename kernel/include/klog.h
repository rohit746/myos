#pragma once
// Kernel log: the last KLOG_SIZE bytes kprintf printed, escape codes and
// all, kept from the very first message. The console shows what was logged
// before it came up, and a `dmesg` can print it again later.

#include <stddef.h>

#define KLOG_SIZE (64 * 1024)

// Append one byte, dropping the oldest byte once the log is full.
void klog_putc(char c);

// Copy up to `size` bytes into `buf`, starting `offset` bytes after the
// oldest byte still kept. Returns how many were copied: 0 at the end.
size_t klog_read(size_t offset, char *buf, size_t size);

// Bytes kept: at most KLOG_SIZE.
size_t klog_size(void);
