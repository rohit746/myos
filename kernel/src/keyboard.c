// Keyboard input shared by all keyboard drivers: modifier and lock state,
// the US keymap and the event queue.
//
// The queue has one producer, the keyboard interrupt, and one consumer, so
// it needs no lock: only the interrupt moves `head` and only the reader
// moves `tail`.

#include <keyboard.h>
#include <stddef.h>

#define QUEUE_SIZE 256 // a power of two

static struct key_event queue[QUEUE_SIZE];
static volatile uint32_t head, tail; // free-running; index with % QUEUE_SIZE
static volatile uint64_t dropped;

static uint8_t down[256 / 8]; // keys held right now, one bit per key code
static volatile uint8_t mods;

// US layout for key codes 0x00-0x39, without and with Shift. (The string
// literals end in a NUL that is not a key.)
static const char plain[] =
    "\0\x1b" "1234567890-=" "\b\t" "qwertyuiop[]" "\n\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./" "\0*\0 ";
static const char shifted[] =
    "\0\x1b" "!@#$%^&*()_+" "\b\t" "QWERTYUIOP{}" "\n\0" "ASDFGHJKL:\"~" "\0|" "ZXCVBNM<>?" "\0*\0 ";

// The keypad, 0x47-0x53. Digits and the dot type with Num Lock on, or with
// Num Lock off and Shift held.
static const char keypad[] = "789-456+1230.";

_Static_assert(sizeof plain == 0x3a + 1 && sizeof shifted == sizeof plain, "keymap covers 0x00-0x39");
_Static_assert(sizeof keypad == KEY_KP_DOT - KEY_KP7 + 2, "keypad covers 0x47-0x53");

#define KEY_102ND 0x56 // the extra key next to left Shift on ISO keyboards

static bool is_down(uint8_t key) { return down[key / 8] & (1u << (key % 8)); }

static char key_char(uint8_t key, uint8_t m) {
    bool shift = m & KMOD_SHIFT;
    char c = 0;
    if (key < sizeof plain - 1) {
        c = shift ? shifted[key] : plain[key];
        // Caps Lock flips Shift for letters only.
        if ((m & KMOD_CAPS) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c ^= 0x20;
    } else if (key >= KEY_KP7 && key <= KEY_KP_DOT) {
        c = keypad[key - KEY_KP7];
        if (c != '-' && c != '+' && !(m & KMOD_NUM) == !shift) c = 0; // Shift flips Num Lock
    } else if (key == KEY_102ND) {
        c = shift ? '|' : '\\';
    } else if (key == KEY_KP_SLASH) {
        c = '/';
    } else if (key == KEY_KP_ENTER) {
        c = '\n';
    }
    if ((m & KMOD_CTRL) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c &= 0x1f;
    return c;
}

void keyboard_report(uint8_t key, bool pressed) {
    bool was_down = is_down(key);
    if (pressed)
        down[key / 8] |= 1u << (key % 8);
    else
        down[key / 8] &= ~(1u << (key % 8));

    uint8_t m = mods & KMOD_LOCKS;
    if (pressed && !was_down) { // not a typematic repeat
        if (key == KEY_CAPSLOCK) m ^= KMOD_CAPS;
        if (key == KEY_NUMLOCK) m ^= KMOD_NUM;
        if (key == KEY_SCROLLLOCK) m ^= KMOD_SCROLL;
    }
    if (is_down(KEY_LSHIFT) || is_down(KEY_RSHIFT)) m |= KMOD_SHIFT;
    if (is_down(KEY_LCTRL) || is_down(KEY_RCTRL)) m |= KMOD_CTRL;
    if (is_down(KEY_LALT) || is_down(KEY_RALT)) m |= KMOD_ALT;
    mods = m;

    if (head - tail >= QUEUE_SIZE) {
        dropped++;
        return;
    }
    queue[head % QUEUE_SIZE] = (struct key_event){
        .key = key, .pressed = pressed, .mods = m, .ch = pressed ? key_char(key, m) : 0};
    __atomic_store_n(&head, head + 1, __ATOMIC_RELEASE); // publish the event after writing it
}

uint8_t keyboard_mods(void) { return mods; }

bool keyboard_read(struct key_event *ev) {
    uint32_t t = tail;
    if (t == __atomic_load_n(&head, __ATOMIC_ACQUIRE)) return false;
    *ev = queue[t % QUEUE_SIZE];
    __atomic_store_n(&tail, t + 1, __ATOMIC_RELEASE);
    return true;
}

uint64_t keyboard_dropped(void) { return dropped; }
