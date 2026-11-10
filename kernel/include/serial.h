#pragma once
// COM1 serial port. QEMU shows it on your terminal; on real hardware it's
// usually absent, and then every call here does nothing.

#include <stdbool.h>

bool serial_init(void);
void serial_putc(char c);
