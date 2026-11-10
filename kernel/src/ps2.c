// i8042 PS/2 controller and the keyboard on its first port.
//
// The controller sits at I/O ports 0x60 (data) and 0x64 (status/command).
// On laptops it is usually emulated by the embedded controller, and on
// desktops the firmware may emulate it for a USB keyboard ("USB legacy
// support") until the OS takes over the USB controller.
//
// The keyboard sends scancode set 2; the controller translates it to set 1,
// which is what the key codes in keyboard.h are made of. Port 2 (the mouse)
// stays off.

#include <acpi.h>
#include <apic.h>
#include <cpu.h>
#include <keyboard.h>
#include <kprintf.h>
#include <time.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64 // read
#define PS2_COMMAND 0x64 // write

#define STATUS_OUT_FULL (1u << 0) // a byte is waiting at PS2_DATA
#define STATUS_IN_FULL  (1u << 1) // the controller hasn't taken the last byte written yet
#define STATUS_AUX      (1u << 5) // the waiting byte came from port 2

#define CMD_READ_CONFIG   0x20
#define CMD_WRITE_CONFIG  0x60
#define CMD_DISABLE_PORT2 0xa7
#define CMD_SELF_TEST     0xaa // answers 0x55
#define CMD_TEST_PORT1    0xab // answers 0x00
#define CMD_DISABLE_PORT1 0xad
#define CMD_ENABLE_PORT1  0xae

#define SELF_TEST_OK 0x55

#define CONFIG_PORT1_IRQ       (1u << 0)
#define CONFIG_PORT2_IRQ       (1u << 1)
#define CONFIG_PORT1_CLOCK_OFF (1u << 4)
#define CONFIG_PORT2_CLOCK_OFF (1u << 5)
#define CONFIG_TRANSLATE       (1u << 6) // set 2 -> set 1

#define KBD_SET_LEDS        0xed
#define KBD_ENABLE_SCANNING 0xf4
#define KBD_ACK             0xfa
#define KBD_RESEND          0xfe

#define LED_SCROLL (1u << 0)
#define LED_NUM    (1u << 1)
#define LED_CAPS   (1u << 2)

#define TIMEOUT_MS     500 // per byte; slow embedded controllers need a while
#define IRQ_TIMEOUT_MS 10  // waiting to write from the interrupt handler
#define LED_TIMEOUT_MS 100 // for the keyboard to answer an LED update
#define MAX_TRIES      3   // a device asking for a byte to be resent
#define FLUSH_MAX      16  // bytes a sane controller can have buffered

static bool wait_status(uint8_t bit, bool set, uint64_t timeout_ms) {
    uint64_t deadline = clock_monotonic_ns() + timeout_ms * NS_PER_MS;
    while (!!(inb(PS2_STATUS) & bit) != set) {
        if (clock_monotonic_ns() > deadline) return false;
        pause();
    }
    return true;
}

static bool write_port(uint16_t port, uint8_t b) {
    if (!wait_status(STATUS_IN_FULL, false, TIMEOUT_MS)) return false;
    outb(port, b);
    return true;
}

static bool read_data(uint8_t *b) {
    if (!wait_status(STATUS_OUT_FULL, true, TIMEOUT_MS)) return false;
    *b = inb(PS2_DATA);
    return true;
}

// Throw away whatever the controller has buffered. False if it never empties.
static bool flush(void) {
    for (int i = 0; i < FLUSH_MAX; i++) {
        if (!(inb(PS2_STATUS) & STATUS_OUT_FULL)) return true;
        inb(PS2_DATA);
    }
    return !(inb(PS2_STATUS) & STATUS_OUT_FULL);
}

static bool command_reply(uint8_t cmd, uint8_t *reply) {
    return write_port(PS2_COMMAND, cmd) && read_data(reply);
}

// Send a byte to the keyboard and wait for its ACK. Before IRQ 1 is on.
static bool kbd_send(uint8_t b) {
    for (int tries = 0; tries < MAX_TRIES; tries++) {
        uint8_t reply;
        if (!write_port(PS2_DATA, b) || !read_data(&reply)) return false;
        if (reply == KBD_ACK) return true;
        if (reply != KBD_RESEND) return false;
    }
    return false;
}

static uint8_t led_bits(uint8_t mods) {
    return (mods & KMOD_SCROLL ? LED_SCROLL : 0) | (mods & KMOD_NUM ? LED_NUM : 0) |
           (mods & KMOD_CAPS ? LED_CAPS : 0);
}

// ---------------------------------------------------------------------------
// Interrupt side: scancodes in, LED updates out.
//
// An LED update is two bytes, 0xED then the LED mask, and the keyboard ACKs
// each one. The ACKs come in through IRQ 1 like scancodes, so the update
// runs as a small state machine driven by the interrupt handler.
// ---------------------------------------------------------------------------

static enum { LED_IDLE, LED_WAIT_CMD_ACK, LED_WAIT_VALUE_ACK } led_step;
static uint8_t leds_shown;  // KMOD_LOCKS bits the LEDs show
static uint8_t led_target;  // ... and the ones being sent
static uint8_t led_last;    // last byte sent, in case the keyboard asks again
static uint8_t led_tries;
static uint64_t led_deadline;

static bool e0_prefix;   // the last byte was 0xE0: the next key is extended
static uint8_t e1_skip;  // bytes of the Pause key's sequence still to come

static void led_send(uint8_t b) {
    led_last = b;
    led_deadline = clock_monotonic_ns() + LED_TIMEOUT_MS * NS_PER_MS;
    if (wait_status(STATUS_IN_FULL, false, IRQ_TIMEOUT_MS)) outb(PS2_DATA, b);
}

// Start sending the lock state to the LEDs if they don't show it yet.
static void leds_update(void) {
    if (led_step != LED_IDLE) {
        if (clock_monotonic_ns() < led_deadline) return; // the reply will continue it
        led_step = LED_IDLE;                              // no reply: start over
    }
    uint8_t want = keyboard_mods() & KMOD_LOCKS;
    if (want == leds_shown) return;
    led_target = want;
    led_tries = 0;
    led_step = LED_WAIT_CMD_ACK;
    led_send(KBD_SET_LEDS);
}

static void led_reply(uint8_t b) {
    if (b == KBD_RESEND) {
        if (++led_tries < MAX_TRIES) led_send(led_last);
        else led_step = LED_IDLE;
        return;
    }
    led_tries = 0;
    if (led_step == LED_WAIT_CMD_ACK) {
        led_step = LED_WAIT_VALUE_ACK;
        led_send(led_bits(led_target));
    } else {
        led_step = LED_IDLE;
        leds_shown = led_target;
        leds_update(); // a lock key may have changed meanwhile
    }
}

static void scancode(uint8_t b) {
    if (led_step != LED_IDLE && (b == KBD_ACK || b == KBD_RESEND)) {
        led_reply(b);
        return;
    }
    if (e1_skip) { // E1 1D 45 E1 9D C5: only Pause sends it, with no release
        e1_skip--;
        return;
    }
    if (b == 0xe1) {
        e1_skip = 5;
        keyboard_report(KEY_PAUSE, true);
        keyboard_report(KEY_PAUSE, false);
        return;
    }
    if (b == 0xe0) {
        e0_prefix = true;
        return;
    }
    bool ext = e0_prefix;
    e0_prefix = false;
    uint8_t code = b & 0x7f;
    // 0x00 and 0xFF are keyboard errors. E0 2A / E0 36 are fake Shift
    // presses some keys (Print Screen, the arrows with Num Lock on) wrap
    // themselves in.
    if (code == 0 || b == 0xff || (ext && (code == KEY_LSHIFT || code == KEY_RSHIFT))) return;
    keyboard_report(ext ? KEY_EXT | code : code, !(b & 0x80));
    leds_update();
}

static void ps2_irq(struct interrupt_frame *f) {
    (void)f;
    uint8_t status = inb(PS2_STATUS);
    if (!(status & STATUS_OUT_FULL)) return;
    uint8_t b = inb(PS2_DATA);
    if (status & STATUS_AUX) return; // port 2 is off; ignore anything from it
    scancode(b);
}

// ---------------------------------------------------------------------------

enum ps2_status ps2_init(char *line, size_t size) {
    const struct acpi_info *acpi = acpi_get_info();
    if (acpi->have_fadt && !acpi->has_8042) {
        ksnprintf(line, size, "PS/2 controller: none (the FADT says there is no 8042)");
        return PS2_ABSENT;
    }
    if (inb(PS2_STATUS) == 0xff) { // nothing decodes the port
        ksnprintf(line, size, "PS/2 controller: none (its status port reads 0xff)");
        return PS2_ABSENT;
    }

    // Stop both ports so nothing new arrives, and drop what is buffered.
    uint8_t config, reply;
    if (!write_port(PS2_COMMAND, CMD_DISABLE_PORT1) || !write_port(PS2_COMMAND, CMD_DISABLE_PORT2) || !flush()) {
        ksnprintf(line, size, "PS/2 controller: none (it doesn't take commands)");
        return PS2_ABSENT;
    }

    // Interrupts off while we poll; translation on.
    if (!command_reply(CMD_READ_CONFIG, &config)) goto timeout;
    config = (config & ~(CONFIG_PORT1_IRQ | CONFIG_PORT2_IRQ)) | CONFIG_TRANSLATE | CONFIG_PORT2_CLOCK_OFF;
    if (!write_port(PS2_COMMAND, CMD_WRITE_CONFIG) || !write_port(PS2_DATA, config)) goto timeout;

    if (!command_reply(CMD_SELF_TEST, &reply)) goto timeout;
    if (reply != SELF_TEST_OK) {
        ksnprintf(line, size, "PS/2 controller failed its self-test (answered 0x%02x, not 0x55)", reply);
        return PS2_FAILED;
    }
    // The self-test can reset the controller, configuration byte included.
    if (!write_port(PS2_COMMAND, CMD_WRITE_CONFIG) || !write_port(PS2_DATA, config)) goto timeout;

    if (!command_reply(CMD_TEST_PORT1, &reply)) goto timeout;
    if (reply != 0) {
        ksnprintf(line, size, "PS/2 port 1 failed its interface test (error 0x%02x)", reply);
        return PS2_FAILED;
    }

    if (!write_port(PS2_COMMAND, CMD_ENABLE_PORT1)) goto timeout;
    if (!kbd_send(KBD_ENABLE_SCANNING)) {
        flush();
        ksnprintf(line, size, "PS/2 controller found, but no keyboard answered on port 1");
        return PS2_NO_KEYBOARD;
    }
    // All lock LEDs off, matching keyboard_mods(). Not fatal if it fails.
    leds_shown = 0;
    if (!kbd_send(KBD_SET_LEDS) || !kbd_send(led_bits(leds_shown))) flush();

    // Route IRQ 1 first, then let the controller raise it, so no edge is
    // lost on a masked pin. Bytes that came in meanwhile are dropped.
    // `config` was read with port 1 disabled, so turn its clock back on too.
    config = (config & ~CONFIG_PORT1_CLOCK_OFF) | CONFIG_PORT1_IRQ;
    uint64_t rflags = read_rflags();
    cli();
    uint32_t gsi = irq_register(1, ps2_irq);
    bool ok = write_port(PS2_COMMAND, CMD_WRITE_CONFIG) && write_port(PS2_DATA, config);
    flush();
    if (rflags & RFLAGS_IF) sti();
    if (!ok) {
        irq_unregister(1);
        goto timeout;
    }

    ksnprintf(line, size, "PS/2 keyboard ready (IRQ 1 -> GSI %u, scancode set 2 translated to set 1)", gsi);
    return PS2_READY;

timeout:
    ksnprintf(line, size, "PS/2 controller stopped answering during setup");
    return PS2_FAILED;
}
