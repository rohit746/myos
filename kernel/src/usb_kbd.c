// HID boot-protocol keyboards (USB HID 1.11, appendix B): boot reports in,
// key presses and releases out, through keyboard_report() like the PS/2
// keyboard's.
//
// A USB keyboard only says which keys are down; it neither sends a key's
// press again while it is held nor lets the host know when to. So held keys
// repeat here, as a PS/2 keyboard's do by default: after 500 ms, then about
// 30 times a second. Modifiers and lock keys don't repeat.

#include <keyboard.h>
#include <string.h>
#include <time.h>
#include <usb.h>

#define REPEAT_DELAY_MS  500
#define REPEAT_PERIOD_MS 33

// HID usages (keyboard page 0x07) 0x04-0x65 as key codes (scancode set 1,
// keyboard.h); 0 for usages with no key code.
static const uint8_t usage_keys[0x66] = {
    [0x04] = 0x1e, [0x05] = 0x30, [0x06] = 0x2e, [0x07] = 0x20, [0x08] = 0x12, [0x09] = 0x21, // a-f
    [0x0a] = 0x22, [0x0b] = 0x23, [0x0c] = 0x17, [0x0d] = 0x24, [0x0e] = 0x25, [0x0f] = 0x26, // g-l
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19, [0x14] = 0x10, [0x15] = 0x13, // m-r
    [0x16] = 0x1f, [0x17] = 0x14, [0x18] = 0x16, [0x19] = 0x2f, [0x1a] = 0x11, [0x1b] = 0x2d, // s-x
    [0x1c] = 0x15, [0x1d] = 0x2c,                                                             // y, z
    [0x1e] = 0x02, [0x1f] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06,                // 1-5
    [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09, [0x26] = 0x0a, [0x27] = 0x0b,                // 6-9, 0
    [0x28] = KEY_ENTER, [0x29] = KEY_ESC, [0x2a] = KEY_BACKSPACE, [0x2b] = KEY_TAB, [0x2c] = KEY_SPACE,
    [0x2d] = KEY_MINUS, [0x2e] = KEY_EQUAL,
    [0x2f] = 0x1a, [0x30] = 0x1b, [0x31] = 0x2b, // [ ] backslash
    [0x32] = 0x2b,                               // the ISO keyboard's # key, where US keyboards have backslash
    [0x33] = 0x27, [0x34] = 0x28, [0x35] = 0x29, // ; ' `
    [0x36] = 0x33, [0x37] = 0x34, [0x38] = 0x35, // , . /
    [0x39] = KEY_CAPSLOCK,
    [0x3a] = 0x3b, [0x3b] = 0x3c, [0x3c] = 0x3d, [0x3d] = 0x3e, [0x3e] = 0x3f, // F1-F5
    [0x3f] = 0x40, [0x40] = 0x41, [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44, // F6-F10
    [0x44] = KEY_F11, [0x45] = KEY_F12,
    [0x46] = KEY_PRINTSCREEN, [0x47] = KEY_SCROLLLOCK, [0x48] = KEY_PAUSE,
    [0x49] = KEY_INSERT, [0x4a] = KEY_HOME, [0x4b] = KEY_PAGEUP,
    [0x4c] = KEY_DELETE, [0x4d] = KEY_END, [0x4e] = KEY_PAGEDOWN,
    [0x4f] = KEY_RIGHT, [0x50] = KEY_LEFT, [0x51] = KEY_DOWN, [0x52] = KEY_UP,
    [0x53] = KEY_NUMLOCK, [0x54] = KEY_KP_SLASH, [0x55] = KEY_KP_STAR, [0x56] = KEY_KP_MINUS,
    [0x57] = KEY_KP_PLUS, [0x58] = KEY_KP_ENTER,
    [0x59] = KEY_KP1, [0x5a] = KEY_KP2, [0x5b] = KEY_KP3, [0x5c] = KEY_KP4, [0x5d] = KEY_KP5,
    [0x5e] = KEY_KP6, [0x5f] = KEY_KP7, [0x60] = KEY_KP8, [0x61] = KEY_KP9, [0x62] = KEY_KP0,
    [0x63] = KEY_KP_DOT,
    [0x64] = 0x56, // the key next to left Shift on ISO keyboards
    [0x65] = KEY_MENU,
};

// The modifier byte, bit by bit: left Ctrl, Shift, Alt, GUI, then the right ones.
static const uint8_t modifier_keys[8] = {KEY_LCTRL, KEY_LSHIFT, KEY_LALT, KEY_LGUI,
                                         KEY_RCTRL, KEY_RSHIFT, KEY_RALT, KEY_RGUI};

// HID's ErrorRollOver, POSTFail and ErrorUndefined: a report full of these
// means "too many keys down", not which ones.
#define USAGE_FIRST_KEY 0x04

static uint8_t usage_key(uint8_t usage) { return usage < sizeof usage_keys ? usage_keys[usage] : 0; }

static bool held(const uint8_t *keys, uint8_t usage) {
    for (int i = 0; i < 6; i++)
        if (keys[i] == usage) return true;
    return false;
}

static bool repeats(uint8_t key) {
    return key != KEY_CAPSLOCK && key != KEY_NUMLOCK && key != KEY_SCROLLLOCK && key != KEY_PAUSE;
}

void usb_kbd_report(struct usb_kbd *k, const uint8_t *report, size_t len, uint64_t now_ns) {
    if (len < 3) return;
    uint8_t keys[6] = {0};
    size_t n = len - 2 < 6 ? len - 2 : 6;
    for (size_t i = 0; i < n; i++) {
        if (report[2 + i] && report[2 + i] < USAGE_FIRST_KEY) return; // rollover: keep what we have
        keys[i] = report[2 + i];
    }
    uint8_t mods = report[0];

    // Releases first, then the modifiers, then presses: when Shift and a
    // letter go down in one report, the letter comes out shifted.
    for (int i = 0; i < 6; i++) {
        uint8_t key = usage_key(k->keys[i]);
        if (k->keys[i] == 0 || held(keys, k->keys[i]) || key == 0) continue;
        keyboard_report(key, false);
        if (key == k->repeat) k->repeat = 0;
    }
    for (int bit = 0; bit < 8; bit++)
        if ((mods ^ k->mods) & (1u << bit)) keyboard_report(modifier_keys[bit], mods & (1u << bit));
    for (int i = 0; i < 6; i++) {
        uint8_t key = usage_key(keys[i]);
        if (keys[i] == 0 || held(k->keys, keys[i]) || key == 0) continue;
        keyboard_report(key, true);
        k->repeat = repeats(key) ? key : 0;
        k->repeat_ns = now_ns + REPEAT_DELAY_MS * NS_PER_MS;
    }
    k->mods = mods;
    memcpy(k->keys, keys, sizeof keys);
}

uint64_t usb_kbd_repeat(struct usb_kbd *k, uint64_t now_ns) {
    if (k->repeat == 0) return 0;
    if (now_ns >= k->repeat_ns) {
        keyboard_report(k->repeat, true);
        k->repeat_ns += REPEAT_PERIOD_MS * NS_PER_MS;
        if (k->repeat_ns <= now_ns) k->repeat_ns = now_ns + REPEAT_PERIOD_MS * NS_PER_MS; // fell behind
    }
    return k->repeat_ns;
}

void usb_kbd_release_all(struct usb_kbd *k) {
    for (int i = 0; i < 6; i++)
        if (k->keys[i] && usage_key(k->keys[i])) keyboard_report(usage_key(k->keys[i]), false);
    for (int bit = 0; bit < 8; bit++)
        if (k->mods & (1u << bit)) keyboard_report(modifier_keys[bit], false);
    memset(k, 0, sizeof *k);
}

uint8_t usb_kbd_leds(uint8_t kmods) {
    return (kmods & KMOD_NUM ? 1u << 0 : 0) | (kmods & KMOD_CAPS ? 1u << 1 : 0) | (kmods & KMOD_SCROLL ? 1u << 2 : 0);
}
