#pragma once
// Keyboard input: key codes, modifier state, a US keymap and a queue of key
// events. Keyboard drivers (ps2.c, usb_kbd.c) report raw key presses and
// releases with keyboard_report(); consumers take events with
// keyboard_read().
//
// Key codes are scancode set 1 make codes, with KEY_EXT (0x80) added for the
// keys that send an 0xE0 prefix. Every key has one code, whatever the
// modifiers.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEY_EXT 0x80

enum {
    KEY_ESC = 0x01,
    KEY_1 = 0x02, // ... KEY_0 = 0x0b
    KEY_MINUS = 0x0c,
    KEY_EQUAL = 0x0d,
    KEY_BACKSPACE = 0x0e,
    KEY_TAB = 0x0f,
    KEY_Q = 0x10,
    KEY_ENTER = 0x1c,
    KEY_LCTRL = 0x1d,
    KEY_A = 0x1e,
    KEY_LSHIFT = 0x2a,
    KEY_Z = 0x2c,
    KEY_RSHIFT = 0x36,
    KEY_KP_STAR = 0x37,
    KEY_LALT = 0x38,
    KEY_SPACE = 0x39,
    KEY_CAPSLOCK = 0x3a,
    KEY_F1 = 0x3b, // ... KEY_F10 = 0x44
    KEY_NUMLOCK = 0x45,
    KEY_SCROLLLOCK = 0x46,
    KEY_KP7 = 0x47, KEY_KP8, KEY_KP9, KEY_KP_MINUS,
    KEY_KP4, KEY_KP5, KEY_KP6, KEY_KP_PLUS,
    KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP0, KEY_KP_DOT, // 0x53
    KEY_F11 = 0x57,
    KEY_F12 = 0x58,

    KEY_KP_ENTER = KEY_EXT | 0x1c,
    KEY_RCTRL = KEY_EXT | 0x1d,
    KEY_KP_SLASH = KEY_EXT | 0x35,
    KEY_PRINTSCREEN = KEY_EXT | 0x37,
    KEY_RALT = KEY_EXT | 0x38,
    KEY_PAUSE = KEY_EXT | 0x45, // sent as E1 1D 45 E1 9D C5, never released
    KEY_HOME = KEY_EXT | 0x47,
    KEY_UP = KEY_EXT | 0x48,
    KEY_PAGEUP = KEY_EXT | 0x49,
    KEY_LEFT = KEY_EXT | 0x4b,
    KEY_RIGHT = KEY_EXT | 0x4d,
    KEY_END = KEY_EXT | 0x4f,
    KEY_DOWN = KEY_EXT | 0x50,
    KEY_PAGEDOWN = KEY_EXT | 0x51,
    KEY_INSERT = KEY_EXT | 0x52,
    KEY_DELETE = KEY_EXT | 0x53,
    KEY_LGUI = KEY_EXT | 0x5b,
    KEY_RGUI = KEY_EXT | 0x5c,
    KEY_MENU = KEY_EXT | 0x5d,
};

// Modifier and lock state, in key_event.mods.
#define KMOD_SHIFT  (1u << 0) // either Shift key is down
#define KMOD_CTRL   (1u << 1)
#define KMOD_ALT    (1u << 2)
#define KMOD_CAPS   (1u << 3) // Caps Lock is on
#define KMOD_NUM    (1u << 4) // Num Lock is on
#define KMOD_SCROLL (1u << 5) // Scroll Lock is on
#define KMOD_LOCKS  (KMOD_CAPS | KMOD_NUM | KMOD_SCROLL)

struct key_event {
    uint8_t key;  // KEY_*
    bool pressed; // false: released. Held keys repeat as more presses.
    uint8_t mods; // KMOD_*, after this event
    char ch;      // what the key types with the US layout, or 0 (always 0 on release).
                  // Ctrl+letter gives 1-26; Enter '\n', Backspace '\b', Tab '\t', Esc 0x1b.
};

// For drivers: key `key` was pressed or released. Updates the modifiers and
// queues an event. Call with interrupts off (from an interrupt handler, say),
// so reports from different keyboards don't interleave.
void keyboard_report(uint8_t key, bool pressed);

// The current KMOD_* state.
uint8_t keyboard_mods(void);

// Take the oldest key event. False if there is none. Never blocks.
bool keyboard_read(struct key_event *ev);

// Events dropped because the queue was full.
uint64_t keyboard_dropped(void);

enum ps2_status {
    PS2_READY,       // the keyboard on port 1 is sending keys through IRQ 1
    PS2_ABSENT,      // no controller (the FADT says so, or nothing answers)
    PS2_NO_KEYBOARD, // a controller, but nothing on port 1
    PS2_FAILED,      // the controller misbehaved during setup
};

// Find the i8042 PS/2 controller, set it up and start taking keys from its
// first port on IRQ 1. Skipped when the FADT says there is no 8042. Needs
// ACPI, the APIC and time_init(). Writes a line for the boot log to `line`.
enum ps2_status ps2_init(char *line, size_t size);
