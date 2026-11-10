#include <cpu.h>
#include <serial.h>

#define COM1 0x3f8

static bool serial_present;

bool serial_init(void) {
    outb(COM1 + 1, 0x00); // no UART interrupts
    outb(COM1 + 3, 0x80); // enable divisor latch
    outb(COM1 + 0, 0x01); // divisor 1 => 115200 baud
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); // 8 data bits, no parity, 1 stop bit
    outb(COM1 + 2, 0xc7); // enable and clear FIFOs
    outb(COM1 + 4, 0x1e); // loopback mode for a self-test

    // A missing port reads back 0xff, so this also detects "no serial port".
    outb(COM1 + 0, 0xae);
    if (inb(COM1 + 0) != 0xae) {
        serial_present = false;
        return false;
    }

    outb(COM1 + 4, 0x0f); // normal operation
    serial_present = true;
    return true;
}

void serial_putc(char c) {
    if (!serial_present) return;
    if (c == '\n') serial_putc('\r');
    // Wait for the transmit buffer to empty, but never hang forever.
    for (int spins = 0; spins < 100000 && !(inb(COM1 + 5) & 0x20); spins++) {}
    outb(COM1, (uint8_t)c);
}
