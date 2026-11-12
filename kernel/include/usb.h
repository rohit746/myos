#pragma once
// USB: xHCI host controllers and the devices on them (xhci.c), and HID
// boot-protocol keyboards (usb_kbd.c).
//
// The xHCI driver finds the devices on each controller's root ports, goes
// through hubs to the devices behind them, and drives every keyboard that
// offers the boot protocol (HID class, subclass 1, protocol 1). Its key
// presses go into the same queue as the PS/2 keyboard's (keyboard.h). A
// thread per controller ("usb0", ...) handles devices that are plugged in or
// pulled out later, repeats held keys and sets the keyboards' lock LEDs.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The USB keyboards that are ready, for the boot log: "USB keyboard ready:
// QEMU USB Keyboard (usb0 port 1.1)". Returns how many there are; with none,
// `line` is left empty.
size_t usb_keyboard_summary(char *line, size_t size);

// ---------------------------------------------------------------------------
// For xhci.c: one HID boot-protocol keyboard (usb_kbd.c). A boot report is
// 8 bytes: a modifier bitmap, a reserved byte and up to 6 keys held (HID
// usage IDs). The calls that report keys are made with interrupts off,
// like the PS/2 keyboard's interrupt handler.
// ---------------------------------------------------------------------------

struct usb_kbd {
    uint8_t mods;       // modifier bitmap of the last report
    uint8_t keys[6];    // usages held in the last report (0: none)
    uint8_t repeat;     // the key (KEY_*) that repeats while it is held, or 0
    uint64_t repeat_ns; // when it repeats next (clock_monotonic_ns)
};

// A report came in: queue presses and releases for what changed since the
// last one.
void usb_kbd_report(struct usb_kbd *k, const uint8_t *report, size_t len, uint64_t now_ns);

// Repeat the held key if its time has come. Returns when it repeats next, or
// 0 if no key repeats.
uint64_t usb_kbd_repeat(struct usb_kbd *k, uint64_t now_ns);

// The keyboard is gone: release every key it held.
void usb_kbd_release_all(struct usb_kbd *k);

// The LED byte of a boot keyboard's output report (Num, Caps, Scroll Lock)
// for the lock state in `kmods` (KMOD_*).
uint8_t usb_kbd_leds(uint8_t kmods);
