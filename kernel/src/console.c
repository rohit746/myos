// Framebuffer text console.
//
// The screen is kept in layers:
//   cells        the character grid: a glyph and two colors per cell
//   shadow       the grid's pixels in RAM, drawn from cells
//   shown        the cells the framebuffer shows right now
//   framebuffer  only ever written, never read
// Reading video memory back is very slow on real GPUs, and writes to it are
// fastest in long runs (Limine maps it write-combining). Output only updates
// cells. console_flush() draws text rows whose cells changed into the
// shadow, compares each screen row with `shown`, and copies the changed
// span of each changed row to the framebuffer.
//
// Scrolling moves no pixels in RAM: cells and shadow rows form a ring, and a
// scroll only moves the ring's top. The framebuffer has to be rewritten, but
// only where the new line differs from the one that was there.
//
// Until the heap is up there is no shadow, and flushes draw the changed
// cells straight onto the framebuffer.

#include <console.h>
#include <kheap.h>
#include <klog.h>
#include <kprintf.h>
#include <limine.h>
#include <stddef.h>
#include <string.h>

#define MAX_COLS 320 // enough for 7680 px wide at scale 3
#define MAX_ROWS 160

#define DEFAULT_FG 0xe6e6e6
#define DEFAULT_BG 0x101218

// The 16 ANSI colors: black, red, green, yellow, blue, magenta, cyan, white,
// then their bright versions.
static const uint32_t palette16[16] = {
    0x000000, 0xcd3131, 0x5fd75f, 0xffd75f, 0x5fafff, 0xd75fd7, 0x5fd7d7, 0xc0c0c0,
    0x8a8a8a, 0xff5f5f, 0x87ff87, 0xffff87, 0x87d7ff, 0xff87ff, 0x87ffff, 0xffffff,
};

// A color as SGR sets it: a palette index (0-255), an RGB value, or the
// default color.
#define COLOR_RGB     0x01000000u // flag; the low 24 bits are 0xRRGGBB
#define COLOR_DEFAULT 0x02000000u

struct attrs {
    uint32_t fg, bg;
    bool bold, reverse;
};

struct cell {
    uint16_t glyph;
    uint32_t fg, bg; // packed framebuffer pixels
};

// ---------------------------------------------------------------------------
// The font: a PSF2 file embedded by font.S.
// ---------------------------------------------------------------------------

#define PSF2_MAGIC             0x864ab572
#define PSF2_HAS_UNICODE_TABLE 0x1
#define PSF2_SEQUENCE          0xfe // in the Unicode table: a multi-code-point sequence follows
#define PSF2_END_OF_GLYPH      0xff
#define NO_GLYPH               0xffff

struct psf2_header {
    uint32_t magic, version, header_size, flags;
    uint32_t length, glyph_size, height, width;
};

extern const uint8_t console_font[], console_font_end[];

static struct {
    const uint8_t *glyphs;
    unsigned count, glyph_size, row_bytes, width, height;
    uint16_t latin1[256]; // code point -> glyph
    uint16_t unknown;     // glyph for code points the font lacks
} font;

// Decode one UTF-8 code point from [p, end) and return where the next one
// starts.
static const uint8_t *utf8_next(const uint8_t *p, const uint8_t *end, uint32_t *cp) {
    unsigned more = *p >= 0xf0 ? 3 : *p >= 0xe0 ? 2 : *p >= 0xc0 ? 1 : 0;
    uint32_t c = *p++ & (more ? 0x3f >> more : 0xff);
    while (more-- && p < end && (*p & 0xc0) == 0x80) c = c << 6 | (*p++ & 0x3f);
    *cp = c;
    return p;
}

static bool font_load(void) {
    const struct psf2_header *h = (const struct psf2_header *)console_font;
    size_t size = (size_t)(console_font_end - console_font);
    if (size < sizeof *h || h->magic != PSF2_MAGIC) return false;
    if (h->width == 0 || h->width > 32 || h->height == 0 || h->height > 64 || h->length == 0) return false;
    font.row_bytes = (h->width + 7) / 8;
    if (h->glyph_size != font.row_bytes * h->height) return false;
    if (h->header_size + (uint64_t)h->length * h->glyph_size > size) return false;

    font.glyphs = console_font + h->header_size;
    font.count = h->length;
    font.glyph_size = h->glyph_size;
    font.width = h->width;
    font.height = h->height;

    for (unsigned i = 0; i < 256; i++) font.latin1[i] = NO_GLYPH;
    if (h->flags & PSF2_HAS_UNICODE_TABLE) {
        // Each glyph's entry: its code points in UTF-8, then sequences
        // (0xFE + code points) that also draw as it, then 0xFF.
        const uint8_t *p = font.glyphs + font.count * font.glyph_size;
        for (unsigned g = 0; g < font.count && p < console_font_end; g++) {
            bool sequence = false;
            while (p < console_font_end && *p != PSF2_END_OF_GLYPH) {
                if (*p == PSF2_SEQUENCE) {
                    sequence = true;
                    p++;
                    continue;
                }
                uint32_t cp;
                p = utf8_next(p, console_font_end, &cp);
                if (!sequence && cp < 256 && font.latin1[cp] == NO_GLYPH) font.latin1[cp] = (uint16_t)g;
            }
            p++;
        }
    } else {
        for (unsigned i = 0; i < 256 && i < font.count; i++) font.latin1[i] = (uint16_t)i;
    }

    if (font.latin1[' '] == NO_GLYPH) return false; // blank cells are drawn as spaces
    font.unknown = font.latin1['?'] != NO_GLYPH ? font.latin1['?'] : font.latin1[' '];
    for (unsigned i = 0; i < 256; i++)
        if (font.latin1[i] == NO_GLYPH) font.latin1[i] = font.unknown;
    return true;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum parse_state { S_TEXT, S_ESC, S_CSI };

#define MAX_PARAMS 16

static struct {
    bool ready;
    uint32_t *fb;
    uint64_t width, height, fb_stride; // stride: pixels from one line to the next
    uint8_t rs, gs, bs; // bit positions of red/green/blue in a pixel
    uint8_t rl, gl, bl; // bit widths of red/green/blue
    uint32_t *shadow;   // rows x cell_h lines of cols x cell_w pixels, or NULL
    uint64_t shadow_stride;

    unsigned scale; // each font pixel becomes scale x scale pixels
    unsigned cell_w, cell_h;
    unsigned cols, rows;
    unsigned top; // the ring slot (row of cells[] and the shadow) on screen row 0

    unsigned cx, cy; // cursor; cx == cols means "wrap before the next character"
    bool cursor_on;
    struct attrs attr;
    uint32_t fg, bg; // attr as framebuffer pixels
    unsigned saved_cx, saved_cy;
    struct attrs saved_attr;

    // Where the shadow has the cursor drawn.
    int drawn_cursor_slot; // -1: nowhere
    unsigned drawn_cursor_col;

    // Escape sequence and UTF-8 decoding.
    enum parse_state state;
    unsigned params[MAX_PARAMS], nparams;
    bool private_mode; // ESC[? ...
    uint32_t utf8_cp;
    unsigned utf8_more;
} con;

static struct cell cells[MAX_ROWS][MAX_COLS]; // by ring slot
static bool dirty[MAX_ROWS];                   // by ring slot: the shadow row is out of date
static struct cell shown[MAX_ROWS][MAX_COLS];  // by screen row, the cursor drawn inverted

static inline unsigned slot(unsigned row) {
    unsigned i = con.top + row;
    return i >= con.rows ? i - con.rows : i;
}

static inline struct cell *row_cells(unsigned row) {
    return cells[slot(row)];
}

static inline uint32_t pack(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return ((r >> (8 - con.rl)) << con.rs) | ((g >> (8 - con.gl)) << con.gs) |
           ((b >> (8 - con.bl)) << con.bs);
}

// rep movs/stos: the CPU turns these into wide stores, which matters most
// for write-combined video memory. `n` must be even: a cell is a multiple
// of 8 pixels wide.
static inline void copy_pixels(uint32_t *dst, const uint32_t *src, size_t n) {
    n /= 2;
    __asm__ volatile("rep movsq" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

static inline void fill_pixels(uint32_t *dst, uint32_t px, size_t n) {
    __asm__ volatile("rep stosl" : "+D"(dst), "+c"(n) : "a"(px) : "memory");
}

static inline uint32_t *fb_line(uint64_t y) {
    return con.fb + y * con.fb_stride;
}

// ---------------------------------------------------------------------------
// Colors
// ---------------------------------------------------------------------------

static uint32_t palette256(unsigned n) {
    if (n < 16) return palette16[n];
    if (n < 232) { // 6x6x6 color cube
        n -= 16;
        unsigned r = n / 36, g = n / 6 % 6, b = n % 6;
        r = r ? 55 + 40 * r : 0;
        g = g ? 55 + 40 * g : 0;
        b = b ? 55 + 40 * b : 0;
        return r << 16 | g << 8 | b;
    }
    unsigned v = 8 + 10 * (n - 232); // grey ramp
    return v << 16 | v << 8 | v;
}

static uint32_t resolve(uint32_t color, bool is_fg) {
    if (color & COLOR_RGB) return color & 0xffffff;
    if (is_fg && con.attr.bold) { // bold shows as the bright color
        if (color == COLOR_DEFAULT) return palette16[15];
        if (color < 8) color += 8;
    }
    if (color == COLOR_DEFAULT) return is_fg ? DEFAULT_FG : DEFAULT_BG;
    return palette256(color);
}

static void update_colors(void) {
    uint32_t fg = pack(resolve(con.attr.fg, true)), bg = pack(resolve(con.attr.bg, false));
    con.fg = con.attr.reverse ? bg : fg;
    con.bg = con.attr.reverse ? fg : bg;
}

static void reset_attrs(void) {
    con.attr = (struct attrs){.fg = COLOR_DEFAULT, .bg = COLOR_DEFAULT};
    update_colors();
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Changed cells closer together than this are written out as one run.
#define RUN_GAP 4

static inline bool same_cell(const struct cell *a, const struct cell *b) {
    return a->glyph == b->glyph && a->fg == b->fg && a->bg == b->bg;
}

// Draw cells [from, to) of `line` with their top-left pixel row at `y0` of
// an image `stride` pixels wide. `cursor_col` is the column to draw the
// cursor on (a block: the cell in inverse colors), or MAX_COLS for none.
static void draw_cells(uint32_t *image, uint64_t stride, uint64_t y0, const struct cell *line, unsigned from,
                       unsigned to, unsigned cursor_col) {
    unsigned s = con.scale;
    for (unsigned py = 0; py < con.cell_h; py++) {
        uint32_t *out = image + (y0 + py) * stride + (uint64_t)from * con.cell_w;
        unsigned gy = py / s;
        for (unsigned col = from; col < to; col++) {
            const struct cell *c = &line[col];
            uint32_t fg = col == cursor_col ? c->bg : c->fg;
            uint32_t bg = col == cursor_col ? c->fg : c->bg;
            const uint8_t *bits = font.glyphs + (size_t)c->glyph * font.glyph_size + gy * font.row_bytes;
            for (unsigned gx = 0; gx < font.width; gx++) {
                uint32_t px = bits[gx >> 3] & (0x80 >> (gx & 7)) ? fg : bg;
                for (unsigned i = 0; i < s; i++) *out++ = px;
            }
        }
    }
}

void console_flush(void) {
    if (!con.ready) return;

    bool cursor = con.cursor_on;
    unsigned cursor_col = con.cx < con.cols ? con.cx : con.cols - 1;

    // Move the cursor in the shadow: redraw the rows it leaves and enters.
    if (con.shadow) {
        int want = cursor ? (int)slot(con.cy) : -1;
        if (want != con.drawn_cursor_slot || cursor_col != con.drawn_cursor_col) {
            if (con.drawn_cursor_slot >= 0) dirty[con.drawn_cursor_slot] = true;
            if (want >= 0) dirty[want] = true;
            con.drawn_cursor_slot = want;
            con.drawn_cursor_col = cursor_col;
        }
    }

    for (unsigned r = 0; r < con.rows; r++) {
        unsigned sl = slot(r);
        const struct cell *line = cells[sl];
        unsigned row_cursor = cursor && r == con.cy ? cursor_col : MAX_COLS;

        if (con.shadow && dirty[sl]) {
            draw_cells(con.shadow, con.shadow_stride, (uint64_t)sl * con.cell_h, line, 0, con.cols,
                       (int)sl == con.drawn_cursor_slot ? con.drawn_cursor_col : MAX_COLS);
            dirty[sl] = false;
        }

        // Find the runs of cells that differ from what the screen shows, and
        // write each run out. Runs a few cells apart are merged into one.
        unsigned c = 0;
        while (c < con.cols) {
            unsigned lo = con.cols, hi = 0;
            for (; c < con.cols && (hi == 0 || c < hi + RUN_GAP); c++) {
                struct cell want = line[c];
                if (c == row_cursor) want = (struct cell){want.glyph, want.bg, want.fg};
                if (same_cell(&want, &shown[r][c])) continue;
                shown[r][c] = want;
                if (lo == con.cols) lo = c;
                hi = c + 1;
            }
            if (lo >= hi) break;
            if (con.shadow) {
                for (unsigned py = 0; py < con.cell_h; py++) {
                    copy_pixels(fb_line((uint64_t)r * con.cell_h + py) + (uint64_t)lo * con.cell_w,
                                con.shadow + ((uint64_t)sl * con.cell_h + py) * con.shadow_stride +
                                    (uint64_t)lo * con.cell_w,
                                (uint64_t)(hi - lo) * con.cell_w);
                }
            } else {
                draw_cells(con.fb, con.fb_stride, (uint64_t)r * con.cell_h, line, lo, hi, row_cursor);
            }
        }
    }
}

void console_redraw(void) {
    if (!con.ready) return;
    for (unsigned r = 0; r < con.rows; r++) {
        dirty[r] = true;
        for (unsigned c = 0; c < con.cols; c++) shown[r][c].glyph = NO_GLYPH;
    }
    con.drawn_cursor_slot = -1;
    console_flush();
}

// ---------------------------------------------------------------------------
// The character grid
// ---------------------------------------------------------------------------

// Blank cells [from, to) of a row, in the current background color.
static void erase(unsigned row, unsigned from, unsigned to) {
    struct cell *line = row_cells(row);
    for (unsigned c = from; c < to; c++) line[c] = (struct cell){font.latin1[' '], con.fg, con.bg};
    dirty[slot(row)] = true;
}

// The top row's slot becomes the new bottom row.
static void scroll_up(void) {
    if (++con.top == con.rows) con.top = 0;
    erase(con.rows - 1, 0, con.cols);
}

static void line_feed(void) {
    if (con.cy + 1 < con.rows) con.cy++;
    else scroll_up();
}

static void put_glyph(uint16_t glyph) {
    if (con.cx >= con.cols) {
        con.cx = 0;
        line_feed();
    }
    row_cells(con.cy)[con.cx] = (struct cell){glyph, con.fg, con.bg};
    dirty[slot(con.cy)] = true;
    con.cx++;
}

static void put_codepoint(uint32_t cp) {
    put_glyph(cp < 256 ? font.latin1[cp] : font.unknown);
}

static void move_to(int col, int row) {
    if (col < 0) col = 0;
    if (col >= (int)con.cols) col = (int)con.cols - 1;
    if (row < 0) row = 0;
    if (row >= (int)con.rows) row = (int)con.rows - 1;
    con.cx = (unsigned)col;
    con.cy = (unsigned)row;
}

static void clear_screen(void) {
    for (unsigned r = 0; r < con.rows; r++) erase(r, 0, con.cols);
}

static void control(uint8_t c) {
    switch (c) {
    case '\n': // also a carriage return, like the serial port's \r\n
    case '\v':
    case '\f':
        con.cx = 0;
        line_feed();
        break;
    case '\r':
        con.cx = 0;
        break;
    case '\b':
        if (con.cx >= con.cols) con.cx = con.cols - 1;
        if (con.cx > 0) con.cx--;
        break;
    case '\t':
        if (con.cx < con.cols) {
            con.cx = (con.cx | 7) + 1;
            if (con.cx >= con.cols) con.cx = con.cols - 1;
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Escape sequences
// ---------------------------------------------------------------------------

static unsigned param(unsigned i, unsigned dflt) {
    return i < con.nparams && con.params[i] ? con.params[i] : dflt;
}

// SGR: ESC[...m
static void sgr(void) {
    if (con.nparams == 0) con.nparams = 1; // ESC[m is ESC[0m
    for (unsigned i = 0; i < con.nparams; i++) {
        unsigned p = con.params[i];
        if (p == 0) con.attr = (struct attrs){.fg = COLOR_DEFAULT, .bg = COLOR_DEFAULT};
        else if (p == 1) con.attr.bold = true;
        else if (p == 22) con.attr.bold = false;
        else if (p == 7) con.attr.reverse = true;
        else if (p == 27) con.attr.reverse = false;
        else if (p >= 30 && p <= 37) con.attr.fg = p - 30;
        else if (p == 39) con.attr.fg = COLOR_DEFAULT;
        else if (p >= 40 && p <= 47) con.attr.bg = p - 40;
        else if (p == 49) con.attr.bg = COLOR_DEFAULT;
        else if (p >= 90 && p <= 97) con.attr.fg = p - 90 + 8;
        else if (p >= 100 && p <= 107) con.attr.bg = p - 100 + 8;
        else if (p == 38 || p == 48) {
            uint32_t color;
            if (i + 2 < con.nparams && con.params[i + 1] == 5) {
                color = con.params[i + 2] & 0xff;
                i += 2;
            } else if (i + 4 < con.nparams && con.params[i + 1] == 2) {
                color = COLOR_RGB | (con.params[i + 2] & 0xff) << 16 | (con.params[i + 3] & 0xff) << 8 |
                        (con.params[i + 4] & 0xff);
                i += 4;
            } else {
                break;
            }
            if (p == 38) con.attr.fg = color;
            else con.attr.bg = color;
        }
    }
    update_colors();
}

static void save_cursor(void) {
    con.saved_cx = con.cx;
    con.saved_cy = con.cy;
    con.saved_attr = con.attr;
}

static void restore_cursor(void) {
    con.cx = con.saved_cx;
    con.cy = con.saved_cy;
    con.attr = con.saved_attr;
    update_colors();
}

static void reset(void) {
    reset_attrs();
    con.cursor_on = true;
    con.cx = con.cy = 0;
    save_cursor();
    clear_screen();
}

static void csi_dispatch(uint8_t final) {
    int x = (int)(con.cx < con.cols ? con.cx : con.cols - 1), y = (int)con.cy;
    int n = (int)param(0, 1);

    if (con.private_mode) {
        if ((final == 'h' || final == 'l') && param(0, 0) == 25) con.cursor_on = final == 'h';
        return;
    }
    switch (final) {
    case 'A': move_to(x, y - n); break;
    case 'B': move_to(x, y + n); break;
    case 'C': move_to(x + n, y); break;
    case 'D': move_to(x - n, y); break;
    case 'E': move_to(0, y + n); break;
    case 'F': move_to(0, y - n); break;
    case 'G': move_to(n - 1, y); break;
    case 'd': move_to(x, n - 1); break;
    case 'H':
    case 'f': move_to((int)param(1, 1) - 1, n - 1); break;
    case 'J':
        switch (param(0, 0)) {
        case 0:
            erase((unsigned)y, (unsigned)x, con.cols);
            for (unsigned r = (unsigned)y + 1; r < con.rows; r++) erase(r, 0, con.cols);
            break;
        case 1:
            for (unsigned r = 0; r < (unsigned)y; r++) erase(r, 0, con.cols);
            erase((unsigned)y, 0, (unsigned)x + 1);
            break;
        case 2:
        case 3: clear_screen(); break;
        }
        break;
    case 'K':
        switch (param(0, 0)) {
        case 0: erase((unsigned)y, (unsigned)x, con.cols); break;
        case 1: erase((unsigned)y, 0, (unsigned)x + 1); break;
        case 2: erase((unsigned)y, 0, con.cols); break;
        }
        break;
    case 'm': sgr(); break;
    case 's': save_cursor(); break;
    case 'u': restore_cursor(); break;
    }
}

static void csi_byte(uint8_t b) {
    if (b >= '0' && b <= '9') {
        if (con.nparams == 0) con.nparams = 1;
        unsigned *p = &con.params[con.nparams - 1];
        if (*p < 100000) *p = *p * 10 + (b - '0');
    } else if (b == ';' || b == ':') {
        if (con.nparams == 0) con.nparams = 1;
        if (con.nparams < MAX_PARAMS) con.params[con.nparams++] = 0;
    } else if (b >= 0x3c && b <= 0x3f) { // '<' '=' '>' '?': private parameters
        con.private_mode = true;
    } else if (b >= 0x40 && b <= 0x7e) {
        con.state = S_TEXT;
        csi_dispatch(b);
    } else if (b == 0x18 || b == 0x1a) { // CAN, SUB: abort the sequence
        con.state = S_TEXT;
    } else if (b < 0x20) {
        control(b); // control characters still work inside a sequence
    }
    // 0x20-0x2f (intermediate bytes) and anything else: ignored
}

static void esc_byte(uint8_t b) {
    con.state = S_TEXT;
    switch (b) {
    case '[':
        con.state = S_CSI;
        con.nparams = 0;
        con.params[0] = 0;
        con.private_mode = false;
        break;
    case '7': save_cursor(); break;
    case '8': restore_cursor(); break;
    case 'c': reset(); break;
    default:
        if (b < 0x20) {
            control(b);
            con.state = S_ESC;
        }
        break;
    }
}

static void text_byte(uint8_t b) {
    if (con.utf8_more) {
        if ((b & 0xc0) == 0x80) {
            con.utf8_cp = con.utf8_cp << 6 | (b & 0x3f);
            if (--con.utf8_more == 0) put_codepoint(con.utf8_cp);
            return;
        }
        con.utf8_more = 0;
        put_glyph(font.unknown); // a cut-off sequence
    }
    if (b < 0x20 || b == 0x7f) control(b);
    else if (b < 0x80) put_glyph(font.latin1[b]);
    else if (b >= 0xc2 && b <= 0xf4) {
        con.utf8_more = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : 1;
        con.utf8_cp = b & (0x3f >> con.utf8_more);
    } else {
        put_glyph(font.unknown);
    }
}

void console_putc(char ch) {
    if (!con.ready) return;
    uint8_t b = (uint8_t)ch;
    if (b == 0x1b) { // ESC always starts a new sequence, even inside another
        con.utf8_more = 0;
        con.state = S_ESC;
        return;
    }
    switch (con.state) {
    case S_TEXT: text_byte(b); break;
    case S_ESC: esc_byte(b); break;
    case S_CSI: csi_byte(b); break;
    }
}

// ---------------------------------------------------------------------------

bool console_init(struct limine_framebuffer *fb) {
    if (fb == NULL || fb->bpp != 32 || fb->memory_model != LIMINE_FRAMEBUFFER_RGB) return false;
    if (!font_load()) {
        kprintf("console: the built-in font is not a valid PSF2 font\n");
        return false;
    }

    con.fb = fb->address;
    con.width = fb->width;
    con.height = fb->height;
    con.fb_stride = fb->pitch / 4;
    con.rs = fb->red_mask_shift;   con.rl = fb->red_mask_size;
    con.gs = fb->green_mask_shift; con.gl = fb->green_mask_size;
    con.bs = fb->blue_mask_shift;  con.bl = fb->blue_mask_size;

    // Make text readable on high-resolution screens.
    con.scale = con.width >= 3200 ? 3 : con.width >= 1600 ? 2 : 1;
    con.cell_w = font.width * con.scale;
    con.cell_h = font.height * con.scale;
    con.cols = con.width / con.cell_w;
    con.rows = con.height / con.cell_h;
    if (con.cols > MAX_COLS) con.cols = MAX_COLS;
    if (con.rows > MAX_ROWS) con.rows = MAX_ROWS;
    if (con.cols == 0 || con.rows == 0) return false;

    // Paint the whole screen, margins included, with blank cells.
    reset();
    for (uint64_t y = 0; y < con.height; y++) fill_pixels(fb_line(y), con.bg, con.width);
    for (unsigned r = 0; r < con.rows; r++)
        for (unsigned c = 0; c < con.cols; c++) shown[r][c] = cells[r][c];
    con.drawn_cursor_slot = -1;
    con.ready = true;

    // Show what was printed before there was a screen.
    char buf[256];
    size_t n;
    for (size_t off = 0; (n = klog_read(off, buf, sizeof buf)) > 0; off += n)
        for (size_t i = 0; i < n; i++) console_putc(buf[i]);
    console_flush();
    return true;
}

bool console_use_shadow_buffer(void) {
    if (!con.ready) return false;
    if (con.shadow) return true;
    uint64_t stride = (uint64_t)con.cols * con.cell_w;
    uint32_t *shadow = kmalloc(stride * con.rows * con.cell_h * sizeof *shadow);
    if (shadow == NULL) return false;
    // The framebuffer already shows the right thing: fill the shadow in
    // without copying anything out.
    for (unsigned sl = 0; sl < con.rows; sl++) dirty[sl] = true;
    con.drawn_cursor_slot = -1;
    con.shadow = shadow;
    con.shadow_stride = stride;
    console_flush();
    return true;
}

struct console_info console_get_info(void) {
    return (struct console_info){
        .cols = con.cols,
        .rows = con.rows,
        .font_w = font.width,
        .font_h = font.height,
        .scale = con.scale,
        .shadow = con.shadow != NULL,
    };
}
