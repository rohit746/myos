// Framebuffer text console.
//
// Every character on screen is also kept in `cells`, so scrolling can
// redraw from memory. Reading pixels back from video memory is very slow on
// real hardware.

#include <console.h>
#include <limine.h>
#include <stddef.h>
#include <string.h>

#include "font8x8/font8x8_basic.h"

#define GLYPH_W 8
#define GLYPH_H 8
#define LINE_GAP 2 // empty pixel rows between text lines (scaled)

#define MAX_COLS 480 // enough for 3840 px wide at the smallest scale
#define MAX_ROWS 270

struct cell {
    char ch;
    uint32_t fg, bg;
};

static struct {
    bool ready;
    uint8_t *base;
    uint64_t width, height, pitch;
    uint8_t rs, gs, bs; // bit positions of red/green/blue in a pixel
    uint8_t rl, gl, bl; // bit widths of red/green/blue
    unsigned scale;     // each font pixel becomes scale x scale pixels
    unsigned cols, rows;
    unsigned cx, cy;    // cursor
    uint32_t fg, bg;
} con;

static struct cell cells[MAX_ROWS][MAX_COLS];

static inline uint32_t pack(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return ((r >> (8 - con.rl)) << con.rs) | ((g >> (8 - con.gl)) << con.gs) |
           ((b >> (8 - con.bl)) << con.bs);
}

static void fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t rgb) {
    uint32_t px = pack(rgb);
    for (uint64_t j = 0; j < h; j++) {
        uint32_t *row = (uint32_t *)(con.base + (y + j) * con.pitch) + x;
        for (uint64_t i = 0; i < w; i++) row[i] = px;
    }
}

static void draw_cell(unsigned col, unsigned row) {
    const struct cell *c = &cells[row][col];
    unsigned s = con.scale;
    unsigned cw = GLYPH_W * s, ch = (GLYPH_H + LINE_GAP) * s;
    uint64_t x0 = (uint64_t)col * cw, y0 = (uint64_t)row * ch;
    uint32_t fg = pack(c->fg), bg = pack(c->bg);
    const uint8_t *glyph = (const uint8_t *)font8x8_basic[(unsigned char)c->ch & 0x7f];

    for (unsigned py = 0; py < ch; py++) {
        // Glyph rows sit in the middle of the cell, with LINE_GAP/2 blank
        // rows above and below.
        int gy = (int)(py / s) - LINE_GAP / 2;
        uint8_t bits = (gy >= 0 && gy < GLYPH_H) ? glyph[gy] : 0;
        uint32_t *line = (uint32_t *)(con.base + (y0 + py) * con.pitch) + x0;
        for (unsigned px = 0; px < cw; px++) {
            line[px] = (bits >> (px / s)) & 1 ? fg : bg;
        }
    }
}

bool console_init(struct limine_framebuffer *fb) {
    if (fb == NULL || fb->bpp != 32 || fb->memory_model != LIMINE_FRAMEBUFFER_RGB) return false;

    con.base = fb->address;
    con.width = fb->width;
    con.height = fb->height;
    con.pitch = fb->pitch;
    con.rs = fb->red_mask_shift;   con.rl = fb->red_mask_size;
    con.gs = fb->green_mask_shift; con.gl = fb->green_mask_size;
    con.bs = fb->blue_mask_shift;  con.bl = fb->blue_mask_size;

    // Make text readable on high-resolution screens.
    con.scale = con.width >= 3200 ? 3 : con.width >= 1600 ? 2 : 1;
    con.cols = con.width / (GLYPH_W * con.scale);
    con.rows = con.height / ((GLYPH_H + LINE_GAP) * con.scale);
    if (con.cols > MAX_COLS) con.cols = MAX_COLS;
    if (con.rows > MAX_ROWS) con.rows = MAX_ROWS;

    con.fg = COLOR_WHITE;
    con.bg = COLOR_BG;
    con.ready = true;
    console_clear();
    return true;
}

void console_set_color(uint32_t fg, uint32_t bg) {
    con.fg = fg;
    con.bg = bg;
}

void console_clear(void) {
    if (!con.ready) return;
    fill_rect(0, 0, con.width, con.height, con.bg);
    for (unsigned r = 0; r < con.rows; r++)
        for (unsigned c = 0; c < con.cols; c++)
            cells[r][c] = (struct cell){' ', con.fg, con.bg};
    con.cx = con.cy = 0;
}

static void scroll(void) {
    // Shift every row up by one and redraw only the cells that changed.
    for (unsigned r = 0; r + 1 < con.rows; r++) {
        for (unsigned c = 0; c < con.cols; c++) {
            struct cell next = cells[r + 1][c];
            struct cell *cur = &cells[r][c];
            if (next.ch != cur->ch || next.fg != cur->fg || next.bg != cur->bg) {
                *cur = next;
                draw_cell(c, r);
            }
        }
    }
    unsigned last = con.rows - 1;
    for (unsigned c = 0; c < con.cols; c++) {
        cells[last][c] = (struct cell){' ', con.fg, con.bg};
        draw_cell(c, last);
    }
}

static void newline(void) {
    con.cx = 0;
    if (++con.cy >= con.rows) {
        scroll();
        con.cy = con.rows - 1;
    }
}

void console_putc(char c) {
    if (!con.ready) return;
    switch (c) {
    case '\n':
        newline();
        return;
    case '\r':
        con.cx = 0;
        return;
    case '\t':
        do console_putc(' '); while (con.cx % 4);
        return;
    case '\b':
        if (con.cx > 0) {
            con.cx--;
            cells[con.cy][con.cx] = (struct cell){' ', con.fg, con.bg};
            draw_cell(con.cx, con.cy);
        }
        return;
    }
    if (con.cx >= con.cols) newline();
    cells[con.cy][con.cx] = (struct cell){c, con.fg, con.bg};
    draw_cell(con.cx, con.cy);
    con.cx++;
}
