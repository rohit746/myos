#pragma once
// Text console drawn straight onto the framebuffer the firmware set up.

#include <stdbool.h>
#include <stdint.h>

struct limine_framebuffer;

#define COLOR_BLACK   0x000000
#define COLOR_WHITE   0xe6e6e6
#define COLOR_GREY    0x8a8a8a
#define COLOR_RED     0xff5f5f
#define COLOR_GREEN   0x5fd75f
#define COLOR_YELLOW  0xffd75f
#define COLOR_BLUE    0x5fafff
#define COLOR_CYAN    0x5fd7d7
#define COLOR_BG      0x101218
#define COLOR_PANIC   0x7a0000

bool console_init(struct limine_framebuffer *fb);
void console_putc(char c);
void console_set_color(uint32_t fg, uint32_t bg);
void console_clear(void);
