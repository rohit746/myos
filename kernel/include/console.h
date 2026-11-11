#pragma once
// Text console drawn onto the framebuffer the firmware set up.
//
// It understands the common ANSI escape codes, so the same bytes color the
// screen and a terminal on the serial port:
//   SGR  ESC[...m   0 reset, 1 bold (bright colors), 22, 7 reverse, 27,
//                   30-37/90-97 and 40-47/100-107, 39/49 default,
//                   38;5;n / 48;5;n (256 colors), 38;2;r;g;b / 48;2;r;g;b
//   cursor          ESC[nA/B/C/D, ESC[nE/F, ESC[nG, ESC[r;cH (and f), ESC[nd,
//                   ESC[s / ESC[u and ESC 7 / ESC 8 (save, restore)
//   erase           ESC[nJ (0 to end, 1 to cursor, 2 and 3 whole screen),
//                   ESC[nK (same for the line)
//   ESC[?25l / ESC[?25h hide or show the cursor; ESC c resets everything
// Text is UTF-8; code points the font lacks show as '?'.

#include <stdbool.h>

struct limine_framebuffer;

// Escape codes for kprintf: kprintf(ANSI_GREEN "ok" ANSI_RESET "\n").
#define ANSI_RESET  "\033[0m"
#define ANSI_GREEN  "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_CYAN   "\033[36m"
#define ANSI_GREY   "\033[90m"
#define ANSI_PANIC  "\033[97;41m" // white on red

// Start drawing on `fb`, and show everything kprintf has logged so far.
// False if there is no 32-bit RGB framebuffer.
bool console_init(struct limine_framebuffer *fb);

// Draw into a copy of the screen in RAM from now on, and copy only the
// rows that changed to the framebuffer. Needs the kernel heap. False if
// the console isn't up or there's no memory; it then keeps drawing
// straight to the framebuffer.
bool console_use_shadow_buffer(void);

// Feed one byte of output. It shows up on screen at the next flush.
void console_putc(char c);

// Put everything written so far on the screen.
void console_flush(void);

// Draw the whole screen again (to measure how long that takes).
void console_redraw(void);

struct console_info {
    unsigned cols, rows;
    unsigned font_w, font_h, scale;
    bool shadow;
};

struct console_info console_get_info(void);
