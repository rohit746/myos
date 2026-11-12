// xHCI: USB host controllers (PCI class 0C:03:30), following the eXtensible
// Host Controller Interface specification, revision 1.2, and USB 2.0 for
// the standard requests and hubs.
//
// At probe time the driver takes the controller from the firmware (the USB
// Legacy Support capability's BIOS/OS semaphore, and on Intel 7- to 9-series
// chipsets, routing the USB 2 ports away from the EHCI controllers), resets
// it, and gives it a device context array, scratchpad buffers, a command
// ring and an event ring on interrupter 0. Then it looks at every root
// port: a USB 2 port with something on it is reset (a USB 3 port enables
// itself), and the device gets a slot and an address and is asked for its
// descriptors. Hubs are configured, their ports powered and reset, and the
// devices behind them found the same way; keyboards that offer the boot
// protocol get their interrupt IN endpoint configured and a transfer queued
// on it. Other devices keep their address and show in `lsusb`.
//
// Every ring is one page: 255 TRBs and a link TRB back to the start. Commands
// and control transfers go one at a time per controller (the mutex), and the
// caller waits for the completion. Interrupt transfers (keyboard reports,
// hub port changes) complete in the interrupt handler, which hands a report
// to the keyboard and queues the next transfer at once.
//
// Events come in through MSI-X table entry 0 (or MSI). The handler reads
// the event ring and wakes whoever waits: the thread that ran a command or
// control transfer, or the controller's own thread, which handles ports
// whose status changed (a device plugged in or pulled out, on a root port
// or behind a hub), endpoints that stopped with an error, key repeat and
// the keyboards' LEDs (checked every LED_CHECK_MS too, since a lock key on
// another keyboard changes them). A controller whose interrupt never arrives is polled
// every 10 ms instead. A command that doesn't complete within
// COMMAND_TIMEOUT_MS leaves the controller unused from then on.
//
// SuperSpeed hubs are not configured (a keyboard plugged into a USB 3 hub is
// behind its USB 2 half, which works); devices further than 15 ports down a
// hub can't be addressed by the route string and are left alone.

#include <console.h>
#include <keyboard.h>
#include <kheap.h>
#include <kprintf.h>
#include <memory.h>
#include <pci.h>
#include <pmm.h>
#include <sched.h>
#include <shell.h>
#include <stdarg.h>
#include <string.h>
#include <sync.h>
#include <time.h>
#include <usb.h>

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

// Capability registers (BAR 0).
#define CAP_LENGTH     0x00 // CAPLENGTH in bits 7:0, HCIVERSION in 31:16
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF      0x14
#define CAP_RTSOFF     0x18

#define HCS1_MAX_SLOTS(p)   ((p) & 0xff)
#define HCS1_MAX_PORTS(p)   ((p) >> 24 & 0xff)
#define HCS2_SCRATCHPADS(p) (((p) >> 27 & 0x1f) | ((p) >> 21 & 0x1f) << 5)
#define HCC1_AC64           (1u << 0) // 64-bit addresses
#define HCC1_CSZ            (1u << 2) // 64-byte contexts
#define HCC1_PPC            (1u << 3) // port power control
#define HCC1_XECP(p)        ((p) >> 16) // first extended capability, in dwords

// Operational registers (at CAPLENGTH).
#define OP_USBCMD    0x00
#define OP_USBSTS    0x04
#define OP_PAGESIZE  0x08
#define OP_CRCR      0x18 // 64 bits
#define OP_DCBAAP    0x30 // 64 bits
#define OP_CONFIG    0x38
#define OP_PORTSC(n) (0x400 + 0x10 * ((n) - 1)) // ports count from 1

#define USBCMD_RUN   (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBCMD_INTE  (1u << 2)

#define USBSTS_HCH  (1u << 0)  // halted
#define USBSTS_HSE  (1u << 2)  // host system error
#define USBSTS_EINT (1u << 3)
#define USBSTS_CNR  (1u << 11) // controller not ready
#define USBSTS_HCE  (1u << 12) // host controller error

#define PORTSC_CCS      (1u << 0) // something is connected
#define PORTSC_PED      (1u << 1) // enabled (write 1 to disable: never written back)
#define PORTSC_PR       (1u << 4) // reset
#define PORTSC_PP       (1u << 9) // powered
#define PORTSC_SPEED(s) ((s) >> 10 & 0xf)
#define PORTSC_CSC      (1u << 17) // connect status change
#define PORTSC_PRC      (1u << 21) // port reset change
#define PORTSC_CHANGES  (0x7fu << 17) // CSC, PEC, WRC, OCC, PRC, PLC, CEC: write 1 to clear
#define PORTSC_KEEP     (PORTSC_PP | 3u << 14 | 7u << 25) // power, indicator and wake bits

// Interrupter 0's registers (at RTSOFF).
#define RT_IMAN   0x20
#define RT_IMOD   0x24
#define RT_ERSTSZ 0x28
#define RT_ERSTBA 0x30 // 64 bits
#define RT_ERDP   0x38 // 64 bits

#define IMAN_IP  (1u << 0) // interrupt pending (write 1 to clear)
#define IMAN_IE  (1u << 1)
#define ERDP_EHB (1u << 3) // event handler busy (write 1 to clear)
#define IMOD_40US 160      // in 250 ns units

// Extended capabilities.
#define XCAP_LEGACY   1
#define XCAP_PROTOCOL 2

#define LEGACY_BIOS_OWNED (1u << 16)
#define LEGACY_OS_OWNED   (1u << 24)
#define LEGACY_CTLSTS     4            // USBLEGCTLSTS, after USBLEGSUP
#define LEGACY_SMI_ENABLES 0x0000e011u // SMIs on USB events, host system error, ownership, PCI command, BAR
#define LEGACY_SMI_EVENTS  0xe0000000u // write 1 to clear

// Intel 7- to 9-series chipsets: which ports the xHCI controller gets (the
// rest stay with the EHCI controllers), and the masks of the ones it can.
#define INTEL_XUSB2PR    0xd0
#define INTEL_USB2PRM    0xd4
#define INTEL_USB3_PSSEN 0xd8
#define INTEL_USB3PRM    0xdc

// ---------------------------------------------------------------------------
// TRBs and contexts
// ---------------------------------------------------------------------------

struct trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
};
_Static_assert(sizeof(struct trb) == 16, "TRBs are 16 bytes");

#define TRB_CYCLE    (1u << 0)
#define TRB_TC       (1u << 1)  // link TRB: toggle the cycle bit
#define TRB_ISP      (1u << 2)  // interrupt on a short packet
#define TRB_IOC      (1u << 5)  // interrupt on completion
#define TRB_IDT      (1u << 6)  // immediate data (setup stage)
#define TRB_DIR_IN   (1u << 16) // data and status stages
#define TRB_TYPE(t)  ((uint32_t)(t) << 10)
#define TRB_TYPE_OF(c) ((c) >> 10 & 0x3f)
#define TRB_SLOT(s)  ((uint32_t)(s) << 24)
#define TRB_EP(dci)  ((uint32_t)(dci) << 16)
#define TRB_TRT(t)   ((uint32_t)(t) << 16) // setup stage: 0 no data, 2 OUT, 3 IN

enum {
    TRB_NORMAL = 1,
    TRB_SETUP = 2,
    TRB_DATA = 3,
    TRB_STATUS = 4,
    TRB_LINK = 6,
    TRB_NOOP = 8,
    TRB_ENABLE_SLOT = 9,
    TRB_DISABLE_SLOT = 10,
    TRB_ADDRESS_DEVICE = 11,
    TRB_CONFIGURE_EP = 12,
    TRB_EVALUATE_CONTEXT = 13,
    TRB_RESET_EP = 14,
    TRB_STOP_EP = 15,
    TRB_SET_TR_DEQUEUE = 16,
    TRB_NOOP_COMMAND = 23,
    TRB_TRANSFER_EVENT = 32,
    TRB_COMMAND_COMPLETION = 33,
    TRB_PORT_STATUS_CHANGE = 34,
};

// Completion codes.
#define CC_SUCCESS     1
#define CC_SHORT       13
#define CC_STOPPED     26
#define CC_STOPPED_LEN 27

#define RING_TRBS  (PAGE_SIZE / sizeof(struct trb)) // the last one links back
#define EVENT_TRBS (PAGE_SIZE / sizeof(struct trb))

// Endpoint types (endpoint context).
#define EP_CONTROL 4
#define EP_INT_IN  7

// Speeds, as PORTSC and the slot context give them.
#define SPEED_FULL  1
#define SPEED_LOW   2
#define SPEED_HIGH  3
#define SPEED_SUPER 4

// ---------------------------------------------------------------------------
// USB requests and descriptors
// ---------------------------------------------------------------------------

#define REQ_IN        0x80
#define REQ_CLASS     0x20
#define REQ_INTERFACE 0x01
#define REQ_OTHER     0x03 // hub requests to a port

#define REQ_GET_STATUS     0
#define REQ_CLEAR_FEATURE  1
#define REQ_SET_FEATURE    3
#define REQ_GET_DESCRIPTOR 6
#define REQ_SET_CONFIG     9
#define HID_SET_REPORT     0x09
#define HID_SET_IDLE       0x0a
#define HID_SET_PROTOCOL   0x0b

#define DESC_DEVICE     1
#define DESC_CONFIG     2
#define DESC_STRING     3
#define DESC_INTERFACE  4
#define DESC_ENDPOINT   5
#define DESC_HUB        0x29
#define DESC_SS_COMPANION 0x30

#define CLASS_HID 3
#define CLASS_HUB 9

// Hub port features and status bits (USB 2.0, 11.24.2).
#define PORT_RESET    4
#define PORT_POWER    8
#define C_PORT_CONNECTION 16 // the change bits' features are 16 + their bit

#define PS_CONNECTION (1u << 0)
#define PS_ENABLE     (1u << 1)
#define PS_LOW_SPEED  (1u << 9)
#define PS_HIGH_SPEED (1u << 10)
#define PC_CONNECTION (1u << 0)
#define PC_RESET      (1u << 4)

// ---------------------------------------------------------------------------

#define MAX_CONTROLLERS 8
#define MAX_HUB_PORTS   15 // what one route string nibble can address
#define MAX_TIERS       5  // hubs below a root port

#define COMMAND_TIMEOUT_MS 5000
#define CONTROL_TIMEOUT_MS 2000
#define POLL_MS            10
#define LED_CHECK_MS       100 // how soon a USB keyboard's LEDs follow a lock key pressed on another keyboard
#define PORT_RESET_MS      500 // most resets take 10-20 ms
#define RESET_RECOVERY_MS  10
#define DEBOUNCE_MS        100
#define MAX_ENDPOINT_ERRORS 3  // in a row, before an interrupt endpoint is given up

// Each device's DMA memory, page by page.
#define DEV_OUT_CTX 0 // the device context the controller writes
#define DEV_IN_CTX  1 // input contexts for commands
#define DEV_EP0     2 // endpoint 0's ring
#define DEV_INT     3 // the interrupt IN endpoint's ring
#define DEV_BUF     4 // control transfer data, then the interrupt buffer
#define DEV_PAGES   5
#define CTRL_BUF    2048
#define INT_BUF     2048 // offset in DEV_BUF; up to 1024 bytes

struct ring {
    volatile struct trb *trbs;
    uint64_t phys;
    unsigned enq;
    uint32_t cycle;
};

enum dev_kind { DEV_OTHER, DEV_HUB, DEV_KEYBOARD };

struct xhci;

struct usb_device {
    struct xhci *c;
    struct usb_device *parent; // the hub it is on, or NULL on a root port
    uint8_t port;              // on the parent hub, or the root port
    uint8_t root_port;
    uint8_t depth;             // hubs between it and the root port
    uint32_t route;            // route string: a port number per hub, 4 bits each
    uint8_t speed;             // SPEED_*
    uint8_t slot;
    uint8_t tt_slot, tt_port;  // low/full speed behind a high-speed hub: its transaction translator
    uint16_t mps0;             // endpoint 0's max packet size
    uint64_t dma;              // DEV_PAGES pages
    uint8_t *buf;
    uint64_t buf_phys;
    struct ring ep0, intr;

    uint16_t vendor, product;
    uint8_t class_code;        // the device's class, or its first interface's
    char name[48];             // the product string, or a description
    enum dev_kind kind;

    // The interrupt IN endpoint (hubs and keyboards).
    uint8_t iface;
    uint8_t int_dci;
    uint16_t int_mps;
    bool int_halted;    // stopped with an error; the thread restarts it
    uint8_t int_errors; // errors in a row
    bool int_stopped;   // given up on

    // Hubs.
    uint8_t ports;
    uint8_t ttt; // think time of its transaction translator, for the slot context
    bool hub_changed;
    struct usb_device *children[MAX_HUB_PORTS + 1];

    // Keyboards.
    struct usb_kbd kbd;
    uint8_t leds; // what its LEDs were last set to
};

// A command or control transfer being waited for.
struct pending {
    bool active, done, command;
    uint64_t trb;      // command: the TRB's address
    uint8_t slot, dci; // control transfer: the endpoint
    uint8_t code;      // completion code
    uint8_t result_slot; // command: the slot ID in the completion
};

struct xhci {
    struct pci_dev *pci;
    volatile uint8_t *base;
    volatile uint32_t *op, *rt, *db;
    unsigned index; // usb0, usb1, ...
    uint16_t version;
    uint8_t max_slots, max_ports;
    unsigned ctx_size; // 32 or 64
    uint64_t dma_limit;
    bool ppc;

    uint64_t dma; // DCBAA and ERST, command ring, event ring
    volatile uint64_t *dcbaa;
    struct ring cmd;
    volatile struct trb *events;
    uint64_t events_phys;
    unsigned evt_deq;
    uint32_t evt_cycle;
    uint64_t *scratchpads; // the array, or NULL
    unsigned scratchpad_count;

    uint8_t port_major[256];     // USB major version of each root port (2 or 3), 0 if no protocol says
    uint8_t port_slot_type[256];
    struct usb_device *root[256]; // device on each root port

    struct mutex lock;         // commands, control transfers and the device tree
    struct spinlock event_lock; // the event ring, `pending`, slots[], the interrupt rings, the work flags
    struct wait_queue waiters; // for `pending`
    struct wait_queue thread_wq;
    struct pending pending;
    struct usb_device *slots[256];
    uint32_t port_changes[8]; // root ports with a Port Status Change event
    bool work;                // something for the thread: port changes, hubs, halted endpoints, LEDs

    bool dead;
    bool use_irq;
    int vector;
    uint64_t irq_count;
};

static struct xhci *controllers[MAX_CONTROLLERS];
static size_t controller_count;

static uint32_t cap_read(struct xhci *c, unsigned reg) { return *(volatile uint32_t *)(c->base + reg); }
static uint32_t op_read(struct xhci *c, unsigned reg) { return c->op[reg / 4]; }
static void op_write(struct xhci *c, unsigned reg, uint32_t v) { c->op[reg / 4] = v; }
static void rt_write(struct xhci *c, unsigned reg, uint32_t v) { c->rt[reg / 4] = v; }

// 64-bit registers, written as two dwords, low first.
static void op_write64(struct xhci *c, unsigned reg, uint64_t v) {
    op_write(c, reg, (uint32_t)v);
    op_write(c, reg + 4, (uint32_t)(v >> 32));
}

static void rt_write64(struct xhci *c, unsigned reg, uint64_t v) {
    rt_write(c, reg, (uint32_t)v);
    rt_write(c, reg + 4, (uint32_t)(v >> 32));
}

static void warn(struct xhci *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kprintf(ANSI_YELLOW "  [warn] xHCI %02x:%02x.%x: ", c->pci->bus, c->pci->dev, c->pci->func);
    kvprintf(fmt, ap);
    kprintf(ANSI_RESET "\n");
    va_end(ap);
}

// "1.1.3": the root port, then the port on each hub.
static void port_path(const struct usb_device *d, char *buf, size_t size) {
    if (d->parent == NULL) {
        ksnprintf(buf, size, "%u", d->port);
        return;
    }
    char up[24];
    port_path(d->parent, up, sizeof up);
    ksnprintf(buf, size, "%s.%u", up, d->port);
}

static void dev_warn(struct usb_device *d, const char *fmt, ...) {
    char path[24];
    port_path(d, path, sizeof path);
    va_list ap;
    va_start(ap, fmt);
    kprintf(ANSI_YELLOW "  [warn] usb%u port %s: ", d->c->index, path);
    kvprintf(fmt, ap);
    kprintf(ANSI_RESET "\n");
    va_end(ap);
}

// Physically contiguous, zeroed pages the controller can reach.
static uint64_t dma_alloc(struct xhci *c, size_t pages) {
    uint64_t phys = pmm_alloc_contiguous_below(pages, c->dma_limit);
    if (phys) memset(phys_to_virt(phys), 0, pages * PAGE_SIZE);
    return phys;
}

static void dma_free(uint64_t phys, size_t pages) { pmm_free_contiguous(phys, pages); }

// Wait up to `ms` for (reg & mask) == want. Sleeps between reads.
static bool wait_op(struct xhci *c, unsigned reg, uint32_t mask, uint32_t want, uint64_t ms) {
    uint64_t deadline = clock_monotonic_ns() + ms * NS_PER_MS;
    for (;;) {
        uint32_t v = op_read(c, reg);
        if (v == 0xffffffff) return false; // gone
        if ((v & mask) == want) return true;
        if (clock_monotonic_ns() > deadline) return false;
        thread_sleep(1);
    }
}

// ---------------------------------------------------------------------------
// Rings
// ---------------------------------------------------------------------------

static void ring_init(struct ring *r, uint64_t phys) {
    r->phys = phys;
    r->trbs = phys_to_virt(phys);
    memset((void *)r->trbs, 0, PAGE_SIZE);
    r->trbs[RING_TRBS - 1].param = phys;
    r->enq = 0;
    r->cycle = 1;
}

static uint64_t ring_enqueue_ptr(const struct ring *r) { return r->phys + r->enq * sizeof(struct trb); }

// Put a TRB on the ring; returns its address. With `hold`, its cycle bit is
// left wrong, so the controller doesn't start on it yet (ring_release).
static uint64_t ring_push(struct ring *r, uint64_t param, uint32_t status, uint32_t control, bool hold) {
    volatile struct trb *t = &r->trbs[r->enq];
    uint64_t at = ring_enqueue_ptr(r);
    t->param = param;
    t->status = status;
    __asm__ volatile("" ::: "memory"); // the cycle bit last: it hands the TRB over
    t->control = (control & ~TRB_CYCLE) | (hold ? r->cycle ^ 1 : r->cycle);
    if (++r->enq == RING_TRBS - 1) {
        volatile struct trb *link = &r->trbs[RING_TRBS - 1];
        link->control = TRB_TYPE(TRB_LINK) | TRB_TC | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return at;
}

static void ring_release(struct ring *r, uint64_t trb) {
    volatile struct trb *t = &r->trbs[(trb - r->phys) / sizeof(struct trb)];
    __asm__ volatile("" ::: "memory");
    t->control ^= TRB_CYCLE;
}

// Make room for `n` TRBs before the link TRB, so a transfer's TRBs never
// straddle it: fill the rest of the ring with no-ops.
static void ring_reserve(struct ring *r, unsigned n) {
    while (r->enq + n > RING_TRBS - 1) ring_push(r, 0, 0, TRB_TYPE(TRB_NOOP), false);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static void queue_interrupt_transfer(struct usb_device *d) {
    ring_push(&d->intr, d->buf_phys + INT_BUF, d->int_mps, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP, false);
    d->c->db[d->slot] = d->int_dci;
}

static void wake_thread(struct xhci *c) {
    c->work = true;
    wait_queue_wake_all(&c->thread_wq);
}

static void interrupt_transfer_done(struct xhci *c, struct usb_device *d, uint8_t code, uint32_t residual) {
    if (code == CC_STOPPED || code == CC_STOPPED_LEN) return;
    if (code != CC_SUCCESS && code != CC_SHORT) {
        d->int_halted = true;
        wake_thread(c);
        return;
    }
    d->int_errors = 0;
    if (d->kind == DEV_KEYBOARD) {
        uint32_t len = residual < d->int_mps ? d->int_mps - residual : 0;
        usb_kbd_report(&d->kbd, d->buf + INT_BUF, len, clock_monotonic_ns());
        queue_interrupt_transfer(d);
        // The thread repeats held keys and sets the LEDs.
        if (usb_kbd_leds(keyboard_mods()) != d->leds) c->work = true;
        wait_queue_wake_all(&c->thread_wq);
    } else if (d->kind == DEV_HUB) {
        d->hub_changed = true; // the thread reads the ports, then queues the next transfer
        wake_thread(c);
    }
}

static void handle_event(struct xhci *c, uint64_t param, uint32_t status, uint32_t control) {
    uint8_t code = status >> 24;
    switch (TRB_TYPE_OF(control)) {
    case TRB_COMMAND_COMPLETION:
        if (c->pending.active && c->pending.command && !c->pending.done && param == c->pending.trb) {
            c->pending.code = code;
            c->pending.result_slot = control >> 24;
            c->pending.done = true;
            wait_queue_wake_all(&c->waiters);
        }
        break;
    case TRB_TRANSFER_EVENT: {
        uint8_t slot = control >> 24, dci = control >> 16 & 0x1f;
        struct usb_device *d = c->slots[slot];
        if (d == NULL) break;
        if (c->pending.active && !c->pending.command && !c->pending.done && c->pending.slot == slot &&
            c->pending.dci == dci) {
            if (code == CC_STOPPED || code == CC_STOPPED_LEN) break;
            c->pending.code = code;
            c->pending.done = true;
            wait_queue_wake_all(&c->waiters);
        } else if (dci == d->int_dci && d->int_dci) {
            interrupt_transfer_done(c, d, code, status & 0xffffff);
        }
        break;
    }
    case TRB_PORT_STATUS_CHANGE: {
        uint8_t port = param >> 24 & 0xff;
        c->port_changes[port / 32] |= 1u << (port % 32);
        wake_thread(c);
        break;
    }
    }
}

// Handle every new event. Call with event_lock held.
static void process_events(struct xhci *c) {
    for (;;) {
        volatile struct trb *e = &c->events[c->evt_deq];
        uint32_t control = e->control;
        if ((control & TRB_CYCLE) != c->evt_cycle) break;
        __asm__ volatile("" ::: "memory"); // the cycle bit first, then the rest of the TRB
        handle_event(c, e->param, e->status, control);
        if (++c->evt_deq == EVENT_TRBS) {
            c->evt_deq = 0;
            c->evt_cycle ^= 1;
        }
    }
    // Clear Event Handler Busy even with nothing new: the interrupt for
    // events already handled may come after they were.
    rt_write64(c, RT_ERDP, (c->events_phys + c->evt_deq * sizeof(struct trb)) | ERDP_EHB);
}

static void xhci_interrupt(struct interrupt_frame *f) {
    for (size_t i = 0; i < controller_count; i++) {
        struct xhci *c = controllers[i];
        if (c->vector < 0 || (uint64_t)c->vector != f->vector) continue;
        spin_lock(&c->event_lock);
        c->irq_count++;
        op_write(c, OP_USBSTS, USBSTS_EINT);
        rt_write(c, RT_IMAN, IMAN_IE | IMAN_IP);
        process_events(c);
        spin_unlock(&c->event_lock);
    }
}

// Wait for the pending command or control transfer. Sleeps until the
// interrupt handler wakes it, but looks at the event ring every POLL_MS too,
// in case the interrupt doesn't come. Call with the mutex held.
static bool wait_pending(struct xhci *c, uint64_t ms) {
    uint64_t deadline = clock_monotonic_ns() + ms * NS_PER_MS;
    uint64_t flags = spin_lock_irqsave(&c->event_lock);
    for (;;) {
        process_events(c);
        if (c->pending.done) break;
        uint64_t now = clock_monotonic_ns();
        if (now >= deadline) break;
        uint64_t until = now + POLL_MS * NS_PER_MS;
        wait_queue_sleep_until(&c->waiters, &c->event_lock, until < deadline ? until : deadline);
    }
    bool done = c->pending.done;
    c->pending.active = false;
    spin_unlock_irqrestore(&c->event_lock, flags);
    return done;
}

// Run a command and wait for it. Returns its completion code, or 0 if it
// timed out (and the controller is not used again). `slot` gets the slot ID
// of the completion. Call with the mutex held.
static uint8_t command(struct xhci *c, uint64_t param, uint32_t control, uint8_t *slot) {
    if (c->dead) return 0;
    uint64_t flags = spin_lock_irqsave(&c->event_lock);
    uint64_t trb = ring_push(&c->cmd, param, 0, control, false);
    c->pending = (struct pending){.active = true, .command = true, .trb = trb};
    spin_unlock_irqrestore(&c->event_lock, flags);
    c->db[0] = 0;
    if (!wait_pending(c, COMMAND_TIMEOUT_MS)) {
        warn(c, "command %u timed out (USBSTS %08x); the controller is not used any more", TRB_TYPE_OF(control),
             op_read(c, OP_USBSTS));
        c->dead = true;
        return 0;
    }
    if (slot) *slot = c->pending.result_slot;
    return c->pending.code;
}

// ---------------------------------------------------------------------------
// Devices: contexts, control transfers
// ---------------------------------------------------------------------------

// Input context `i`: 0 is the input control context, 1 the slot context,
// 1 + DCI an endpoint's. Output (device) context `i`: 0 the slot, DCI an
// endpoint.
static volatile uint32_t *in_ctx(struct usb_device *d, unsigned i) {
    return (volatile uint32_t *)((uint8_t *)phys_to_virt(d->dma + DEV_IN_CTX * PAGE_SIZE) + i * d->c->ctx_size);
}

static volatile uint32_t *out_ctx(struct usb_device *d, unsigned i) {
    return (volatile uint32_t *)((uint8_t *)phys_to_virt(d->dma + DEV_OUT_CTX * PAGE_SIZE) + i * d->c->ctx_size);
}

static void clear_in_ctx(struct usb_device *d) { memset(phys_to_virt(d->dma + DEV_IN_CTX * PAGE_SIZE), 0, PAGE_SIZE); }

// Point the controller past whatever is on the ring, after an endpoint
// stopped (`stop`: stop it first) or halted with an error.
static void endpoint_recover(struct usb_device *d, uint8_t dci, struct ring *r, bool stop) {
    struct xhci *c = d->c;
    command(c, 0, TRB_TYPE(stop ? TRB_STOP_EP : TRB_RESET_EP) | TRB_SLOT(d->slot) | TRB_EP(dci), NULL);
    command(c, ring_enqueue_ptr(r) | r->cycle, TRB_TYPE(TRB_SET_TR_DEQUEUE) | TRB_SLOT(d->slot) | TRB_EP(dci), NULL);
}

// A control transfer on endpoint 0: `len` bytes (at most CTRL_BUF) in or out
// of `data`. Returns the completion code (CC_SUCCESS, or CC_SHORT when the
// device sent less: the rest of `data` is zeros), or 0 if it timed out.
// Call with the mutex held.
static uint8_t control(struct usb_device *d, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                       uint16_t len, void *data) {
    struct xhci *c = d->c;
    if (c->dead) return 0;
    if (len > CTRL_BUF) len = CTRL_BUF;
    bool in = type & REQ_IN;
    if (in) memset(d->buf, 0, len);
    else if (len) memcpy(d->buf, data, len);

    uint64_t setup = type | (uint64_t)request << 8 | (uint64_t)value << 16 | (uint64_t)index << 32 |
                     (uint64_t)len << 48;
    struct ring *r = &d->ep0;
    ring_reserve(r, 3);
    uint64_t first = ring_push(r, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | TRB_TRT(len == 0 ? 0 : in ? 3 : 2), true);
    if (len) ring_push(r, d->buf_phys, len, TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0), false);
    ring_push(r, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (len && in ? 0 : TRB_DIR_IN), false);

    uint64_t flags = spin_lock_irqsave(&c->event_lock);
    c->pending = (struct pending){.active = true, .slot = d->slot, .dci = 1};
    spin_unlock_irqrestore(&c->event_lock, flags);
    ring_release(r, first);
    c->db[d->slot] = 1;

    if (!wait_pending(c, CONTROL_TIMEOUT_MS)) {
        endpoint_recover(d, 1, r, true);
        return 0;
    }
    uint8_t code = c->pending.code;
    if (code != CC_SUCCESS && code != CC_SHORT) endpoint_recover(d, 1, r, false); // a stall halts it
    else if (in && len) memcpy(data, d->buf, len);
    return code;
}

static bool control_ok(uint8_t code) { return code == CC_SUCCESS || code == CC_SHORT; }

static uint8_t get_descriptor(struct usb_device *d, uint8_t type, uint8_t index, uint16_t lang, uint16_t len,
                              void *buf) {
    return control(d, REQ_IN, REQ_GET_DESCRIPTOR, (uint16_t)(type << 8 | index), lang, len, buf);
}

// String descriptor `index` in the first language the device lists, as
// ASCII ('?' for anything else). False if there is none.
static bool get_string(struct usb_device *d, uint8_t index, char *out, size_t size) {
    uint8_t buf[256];
    char s[64];
    if (index == 0 || !control_ok(get_descriptor(d, DESC_STRING, 0, 0, 4, buf)) || buf[0] < 4) return false;
    uint16_t lang = buf[2] | buf[3] << 8;
    if (!control_ok(get_descriptor(d, DESC_STRING, index, lang, 255, buf)) || buf[1] != DESC_STRING) return false;
    size_t n = 0;
    for (unsigned i = 2; i + 1 < buf[0] && n + 1 < sizeof s; i += 2) {
        uint16_t ch = buf[i] | buf[i + 1] << 8;
        s[n++] = ch >= 0x20 && ch < 0x7f ? (char)ch : '?';
    }
    while (n && s[n - 1] == ' ') n--;
    s[n] = '\0';
    if (n == 0) return false;
    ksnprintf(out, size, "%s", s);
    return true;
}

// Give the device on a slot its address, with endpoint 0 set up.
static uint8_t address_device(struct usb_device *d) {
    clear_in_ctx(d);
    volatile uint32_t *icc = in_ctx(d, 0), *slot = in_ctx(d, 1), *ep0 = in_ctx(d, 2);
    icc[1] = 1u << 0 | 1u << 1; // add the slot and endpoint 0
    slot[0] = d->route | (uint32_t)d->speed << 20 | 1u << 27; // one context entry
    slot[1] = (uint32_t)d->root_port << 16;
    slot[2] = d->tt_slot | (uint32_t)d->tt_port << 8;
    ep0[1] = 3u << 1 | EP_CONTROL << 3 | (uint32_t)d->mps0 << 16; // 3 errors allowed
    ep0[2] = (uint32_t)d->ep0.phys | d->ep0.cycle;
    ep0[3] = (uint32_t)(d->ep0.phys >> 32);
    ep0[4] = 8; // average TRB length
    return command(d->c, d->dma + DEV_IN_CTX * PAGE_SIZE, TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_SLOT(d->slot), NULL);
}

// Tell the controller endpoint 0's real max packet size.
static uint8_t update_mps0(struct usb_device *d) {
    clear_in_ctx(d);
    in_ctx(d, 0)[1] = 1u << 1;
    volatile uint32_t *ep0 = in_ctx(d, 2), *cur = out_ctx(d, 1);
    for (int i = 0; i < 5; i++) ep0[i] = cur[i];
    ep0[1] = (ep0[1] & 0xffff) | (uint32_t)d->mps0 << 16;
    return command(d->c, d->dma + DEV_IN_CTX * PAGE_SIZE, TRB_TYPE(TRB_EVALUATE_CONTEXT) | TRB_SLOT(d->slot), NULL);
}

// What a configuration descriptor says about the interface a driver wants.
struct iface_info {
    bool found;
    uint8_t number;
    uint8_t ep_addr; // its interrupt IN endpoint, 0 if none
    uint16_t ep_mps;
    uint8_t ep_interval;
    uint8_t ep_burst; // SuperSpeed: from the companion descriptor
};

static bool is_keyboard_iface(const uint8_t *p) { return p[5] == CLASS_HID && p[6] == 1 && p[7] == 1; }

// Find the first interface (alternate setting 0) that `match` accepts, and
// its first interrupt IN endpoint.
static struct iface_info find_iface(const uint8_t *cfg, size_t len, bool (*match)(const uint8_t *iface)) {
    struct iface_info f = {0};
    bool in_it = false;
    for (size_t off = 0; off + 2 <= len;) {
        const uint8_t *p = cfg + off;
        if (p[0] < 2 || off + p[0] > len) break;
        if (p[1] == DESC_INTERFACE && p[0] >= 9) {
            if (f.found) {
                in_it = false;
                if (f.ep_addr) break;
            }
            if (!f.found && p[3] == 0 && match(p)) {
                f.found = in_it = true;
                f.number = p[2];
            }
        } else if (p[1] == DESC_ENDPOINT && p[0] >= 7 && in_it && f.ep_addr == 0) {
            if ((p[3] & 3) == 3 && (p[2] & 0x80)) {
                f.ep_addr = p[2];
                f.ep_mps = p[4] | p[5] << 8;
                f.ep_interval = p[6];
            }
        } else if (p[1] == DESC_SS_COMPANION && p[0] >= 6 && in_it && f.ep_addr && f.ep_burst == 0) {
            f.ep_burst = p[2];
        }
        off += p[0];
    }
    return f;
}

static bool is_hub_iface(const uint8_t *p) { return p[5] == CLASS_HUB; }

// Configure the interrupt IN endpoint `f` (and for a hub, tell the slot
// context it is one).
static bool configure_interrupt_endpoint(struct usb_device *d, const struct iface_info *f) {
    unsigned ep = f->ep_addr & 0xf;
    uint8_t dci = (uint8_t)(ep * 2 + 1);
    uint16_t mps = f->ep_mps & 0x7ff;
    uint32_t burst = d->speed >= SPEED_SUPER ? f->ep_burst : d->speed == SPEED_HIGH ? (f->ep_mps >> 11 & 3) : 0;
    if (mps == 0 || mps > 1024) return false;

    // Interval as 2^n * 125 us: high and SuperSpeed devices give n + 1,
    // low and full speed ones milliseconds.
    unsigned interval;
    if (d->speed >= SPEED_HIGH) {
        unsigned b = f->ep_interval < 1 ? 1 : f->ep_interval > 16 ? 16 : f->ep_interval;
        interval = b - 1;
    } else {
        unsigned frames = f->ep_interval ? f->ep_interval : 1;
        interval = 31 - (unsigned)__builtin_clz(frames * 8);
        if (interval < 3) interval = 3;
        if (interval > 10) interval = 10;
    }
    uint32_t esit = mps * (burst + 1);

    clear_in_ctx(d);
    volatile uint32_t *icc = in_ctx(d, 0), *slot = in_ctx(d, 1), *epc = in_ctx(d, 1 + dci), *cur = out_ctx(d, 0);
    icc[1] = 1u << 0 | 1u << dci;
    for (int i = 0; i < 4; i++) slot[i] = cur[i];
    uint32_t entries = slot[0] >> 27 > dci ? slot[0] >> 27 : dci;
    slot[0] = (slot[0] & 0x07ffffff) | entries << 27;
    if (d->kind == DEV_HUB) {
        slot[0] |= 1u << 26; // a hub
        slot[1] = (slot[1] & 0x00ffffff) | (uint32_t)d->ports << 24;
        slot[2] = (slot[2] & ~(3u << 16)) | (uint32_t)d->ttt << 16;
    }
    slot[3] = 0;
    epc[0] = interval << 16 | (esit >> 16) << 24;
    epc[1] = 3u << 1 | EP_INT_IN << 3 | burst << 8 | (uint32_t)mps << 16;
    epc[2] = (uint32_t)d->intr.phys | d->intr.cycle;
    epc[3] = (uint32_t)(d->intr.phys >> 32);
    epc[4] = esit | (esit & 0xffff) << 16; // average TRB length, max ESIT payload
    uint8_t code = command(d->c, d->dma + DEV_IN_CTX * PAGE_SIZE, TRB_TYPE(TRB_CONFIGURE_EP) | TRB_SLOT(d->slot), NULL);
    if (code != CC_SUCCESS) {
        dev_warn(d, "Configure Endpoint failed (completion code %u)", code);
        return false;
    }
    d->int_dci = dci;
    d->int_mps = mps;
    d->iface = f->number;
    return true;
}

static void start_interrupt_transfers(struct usb_device *d) {
    uint64_t flags = spin_lock_irqsave(&d->c->event_lock);
    queue_interrupt_transfer(d);
    spin_unlock_irqrestore(&d->c->event_lock, flags);
}

// ---------------------------------------------------------------------------
// Device tree
// ---------------------------------------------------------------------------

static struct usb_device *attach(struct xhci *c, struct usb_device *parent, uint8_t port, uint8_t speed);
static void detach(struct usb_device *d);

static const char *speed_name(uint8_t speed) {
    switch (speed) {
    case SPEED_FULL: return "full speed";
    case SPEED_LOW: return "low speed";
    case SPEED_HIGH: return "high speed";
    case SPEED_SUPER: return "SuperSpeed";
    case 5: return "SuperSpeed+";
    default: return "unknown speed";
    }
}

static const char *class_name(uint8_t class_code) {
    switch (class_code) {
    case 1: return "audio";
    case 2: return "communications";
    case 3: return "HID";
    case 6: return "imaging";
    case 7: return "printer";
    case 8: return "mass storage";
    case CLASS_HUB: return "hub";
    case 0x0a: return "CDC data";
    case 0x0b: return "smart card";
    case 0x0e: return "video";
    case 0xe0: return "wireless";
    default: return NULL;
    }
}

// "QEMU USB Keyboard (0627:0001, full speed), keyboard"
static void describe(struct usb_device *d, char *buf, size_t size) {
    char what[40];
    if (d->kind == DEV_KEYBOARD) ksnprintf(what, sizeof what, "keyboard");
    else if (d->kind == DEV_HUB) ksnprintf(what, sizeof what, "hub, %u ports", d->ports);
    else if (class_name(d->class_code)) ksnprintf(what, sizeof what, "%s, no driver", class_name(d->class_code));
    else ksnprintf(what, sizeof what, "class %02x, no driver", d->class_code);
    ksnprintf(buf, size, "%s (%04x:%04x, %s), %s", d->name, d->vendor, d->product, speed_name(d->speed), what);
}

static void print_device(struct usb_device *d, const char *suffix) {
    char path[24], desc[128];
    port_path(d, path, sizeof path);
    describe(d, desc, sizeof desc);
    kprintf("  usb%u port %s: %s%s\n", d->c->index, path, desc, suffix);
}

static bool setup_keyboard(struct usb_device *d, uint8_t config, const struct iface_info *f) {
    if (!configure_interrupt_endpoint(d, f)) return false;
    if (!control_ok(control(d, 0, REQ_SET_CONFIG, config, 0, 0, NULL))) {
        dev_warn(d, "the keyboard refused SET_CONFIGURATION");
        return false;
    }
    if (!control_ok(control(d, REQ_CLASS | REQ_INTERFACE, HID_SET_PROTOCOL, 0, d->iface, 0, NULL))) {
        dev_warn(d, "the keyboard refused the boot protocol");
        return false;
    }
    // Reports only when something changes. Some keyboards stall this; fine.
    control(d, REQ_CLASS | REQ_INTERFACE, HID_SET_IDLE, 0, d->iface, 0, NULL);
    d->leds = usb_kbd_leds(keyboard_mods());
    control(d, REQ_CLASS | REQ_INTERFACE, HID_SET_REPORT, 0x0200, d->iface, 1, &d->leds);
    d->kind = DEV_KEYBOARD;
    start_interrupt_transfers(d);
    return true;
}

static uint8_t hub_port_status(struct usb_device *hub, unsigned port, uint16_t *status, uint16_t *change) {
    uint8_t s[4];
    uint8_t code = control(hub, REQ_IN | REQ_CLASS | REQ_OTHER, REQ_GET_STATUS, 0, (uint16_t)port, 4, s);
    *status = s[0] | s[1] << 8;
    *change = s[2] | s[3] << 8;
    return code;
}

static void hub_port_feature(struct usb_device *hub, uint8_t request, uint16_t feature, unsigned port) {
    control(hub, REQ_CLASS | REQ_OTHER, request, feature, (uint16_t)port, 0, NULL);
}

// Look at a hub's port: a device that went away is detached, a new one is
// reset and attached. `initial`: the first look, when every device there is
// new.
static void hub_port(struct usb_device *hub, unsigned port, bool initial) {
    uint16_t status, change;
    if (!control_ok(hub_port_status(hub, port, &status, &change))) return;
    for (unsigned bit = 0; bit < 5; bit++)
        if (change & (1u << bit)) hub_port_feature(hub, REQ_CLEAR_FEATURE, (uint16_t)(C_PORT_CONNECTION + bit), port);
    bool connected = status & PS_CONNECTION;
    struct usb_device *child = hub->children[port];
    if (child && (!connected || (change & PC_CONNECTION) || !(status & PS_ENABLE))) {
        detach(child);
        child = NULL;
    }
    if (!connected || child || !(initial || (change & PC_CONNECTION))) return;
    if (!initial) {
        thread_sleep(DEBOUNCE_MS);
        if (!control_ok(hub_port_status(hub, port, &status, &change)) || !(status & PS_CONNECTION)) return;
        if (change & PC_CONNECTION) hub_port_feature(hub, REQ_CLEAR_FEATURE, C_PORT_CONNECTION, port);
    }

    hub_port_feature(hub, REQ_SET_FEATURE, PORT_RESET, port);
    uint64_t deadline = clock_monotonic_ns() + PORT_RESET_MS * NS_PER_MS;
    for (;;) {
        thread_sleep(10);
        if (!control_ok(hub_port_status(hub, port, &status, &change))) return;
        if ((change & PC_RESET) || clock_monotonic_ns() > deadline) break;
    }
    hub_port_feature(hub, REQ_CLEAR_FEATURE, C_PORT_CONNECTION + 4, port); // C_PORT_RESET
    char path[24];
    port_path(hub, path, sizeof path);
    if (!(status & PS_ENABLE)) {
        if (status & PS_CONNECTION)
            kprintf(ANSI_YELLOW "  [warn] usb%u port %s.%u: the port did not enable after a reset" ANSI_RESET "\n",
                    hub->c->index, path, port);
        return;
    }
    uint8_t speed = status & PS_LOW_SPEED ? SPEED_LOW : status & PS_HIGH_SPEED ? SPEED_HIGH : SPEED_FULL;
    thread_sleep(RESET_RECOVERY_MS);
    hub->children[port] = attach(hub->c, hub, (uint8_t)port, speed);
}

static bool setup_hub(struct usb_device *d, uint8_t config, const struct iface_info *f) {
    if (d->speed >= SPEED_SUPER) return false; // not supported: shows as "hub, no driver"
    if (d->depth >= MAX_TIERS) {
        dev_warn(d, "hubs nested this deep are not supported");
        return false;
    }
    uint8_t desc[16];
    uint8_t code = control(d, REQ_IN | REQ_CLASS, REQ_GET_DESCRIPTOR, DESC_HUB << 8, 0, sizeof desc, desc);
    if (!control_ok(code) || desc[1] != DESC_HUB) {
        dev_warn(d, "no hub descriptor");
        return false;
    }
    uint16_t characteristics = desc[3] | desc[4] << 8;
    unsigned power_ms = desc[5] * 2u;
    d->ports = desc[2] > MAX_HUB_PORTS ? MAX_HUB_PORTS : desc[2];
    d->ttt = d->speed == SPEED_HIGH ? (characteristics >> 5 & 3) : 0;
    d->kind = DEV_HUB;
    if (!configure_interrupt_endpoint(d, f)) return false;
    if (!control_ok(control(d, 0, REQ_SET_CONFIG, config, 0, 0, NULL))) {
        dev_warn(d, "the hub refused SET_CONFIGURATION");
        return false;
    }
    print_device(d, "");

    for (unsigned p = 1; p <= d->ports; p++) hub_port_feature(d, REQ_SET_FEATURE, PORT_POWER, p);
    thread_sleep(power_ms > DEBOUNCE_MS ? power_ms : DEBOUNCE_MS);
    for (unsigned p = 1; p <= d->ports; p++) hub_port(d, p, true);
    start_interrupt_transfers(d);
    return true;
}

static void free_device(struct usb_device *d) {
    struct xhci *c = d->c;
    if (d->slot) {
        uint64_t flags = spin_lock_irqsave(&c->event_lock);
        c->slots[d->slot] = NULL;
        if (d->kind == DEV_KEYBOARD) usb_kbd_release_all(&d->kbd);
        spin_unlock_irqrestore(&c->event_lock, flags);
        if (command(c, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(d->slot), NULL) == 0) return; // keep its memory
        c->dcbaa[d->slot] = 0;
    }
    if (d->dma) dma_free(d->dma, DEV_PAGES);
    kfree(d);
}

// Give the device on a port that was just reset a slot and an address, read
// its descriptors and start its driver. NULL if that fails.
static struct usb_device *attach(struct xhci *c, struct usb_device *parent, uint8_t port, uint8_t speed) {
    struct usb_device *d = kcalloc(1, sizeof *d);
    if (d == NULL) return NULL;
    d->c = c;
    d->parent = parent;
    d->port = port;
    d->speed = speed;
    if (parent) {
        d->depth = (uint8_t)(parent->depth + 1);
        d->route = parent->route | (uint32_t)port << (4 * parent->depth);
        d->root_port = parent->root_port;
        if (speed == SPEED_LOW || speed == SPEED_FULL) {
            if (parent->speed == SPEED_HIGH) {
                d->tt_slot = parent->slot;
                d->tt_port = port;
            } else {
                d->tt_slot = parent->tt_slot;
                d->tt_port = parent->tt_port;
            }
        }
    } else {
        d->root_port = port;
    }
    ksnprintf(d->name, sizeof d->name, "USB device");

    d->dma = dma_alloc(c, DEV_PAGES);
    if (d->dma == 0) {
        dev_warn(d, "no memory");
        kfree(d);
        return NULL;
    }
    ring_init(&d->ep0, d->dma + DEV_EP0 * PAGE_SIZE);
    ring_init(&d->intr, d->dma + DEV_INT * PAGE_SIZE);
    d->buf_phys = d->dma + DEV_BUF * PAGE_SIZE;
    d->buf = phys_to_virt(d->buf_phys);

    uint8_t slot = 0;
    uint8_t code = command(c, 0, TRB_TYPE(TRB_ENABLE_SLOT) | (uint32_t)c->port_slot_type[d->root_port] << 16, &slot);
    if (code != CC_SUCCESS || slot == 0 || slot > c->max_slots) {
        dev_warn(d, "no device slot (completion code %u)", code);
        free_device(d);
        return NULL;
    }
    d->slot = slot;
    c->dcbaa[slot] = d->dma + DEV_OUT_CTX * PAGE_SIZE;
    uint64_t flags = spin_lock_irqsave(&c->event_lock);
    c->slots[slot] = d;
    spin_unlock_irqrestore(&c->event_lock, flags);

    d->mps0 = speed >= SPEED_SUPER ? 512 : speed == SPEED_HIGH ? 64 : 8;
    if ((code = address_device(d)) != CC_SUCCESS) {
        dev_warn(d, "Address Device failed (completion code %u)", code);
        free_device(d);
        return NULL;
    }

    uint8_t dev[18];
    if (!control_ok(get_descriptor(d, DESC_DEVICE, 0, 0, 8, dev))) {
        dev_warn(d, "the device did not send its descriptor");
        free_device(d);
        return NULL;
    }
    uint16_t mps0 = speed >= SPEED_SUPER ? (dev[7] < 16 ? 1u << dev[7] : 0) : dev[7];
    if (mps0 >= 8 && mps0 != d->mps0) {
        d->mps0 = mps0;
        update_mps0(d);
    }
    if (!control_ok(get_descriptor(d, DESC_DEVICE, 0, 0, sizeof dev, dev))) {
        dev_warn(d, "the device did not send its descriptor");
        free_device(d);
        return NULL;
    }
    d->vendor = dev[8] | dev[9] << 8;
    d->product = dev[10] | dev[11] << 8;
    d->class_code = dev[4];
    if (!get_string(d, dev[15], d->name, sizeof d->name)) get_string(d, dev[14], d->name, sizeof d->name);

    // The first configuration. If it is too long for the buffer, what fits
    // is enough: drivers look at the first interfaces.
    uint8_t *cfg = kmalloc(CTRL_BUF);
    if (cfg == NULL || !control_ok(get_descriptor(d, DESC_CONFIG, 0, 0, 9, cfg)) || cfg[1] != DESC_CONFIG) {
        if (cfg) dev_warn(d, "the device did not send its configuration");
        kfree(cfg);
        free_device(d);
        return NULL;
    }
    uint16_t total = cfg[2] | cfg[3] << 8;
    if (total > CTRL_BUF) total = CTRL_BUF;
    if (total < 9 || !control_ok(get_descriptor(d, DESC_CONFIG, 0, 0, total, cfg))) {
        dev_warn(d, "the device did not send its configuration");
        kfree(cfg);
        free_device(d);
        return NULL;
    }
    uint8_t config = cfg[5];
    if (d->class_code == 0 && total >= 18 && cfg[10] == DESC_INTERFACE) d->class_code = cfg[14];

    struct iface_info hub = find_iface(cfg, total, is_hub_iface);
    struct iface_info kbd = find_iface(cfg, total, is_keyboard_iface);
    kfree(cfg);
    if (d->class_code == CLASS_HUB && hub.found && hub.ep_addr) {
        if (setup_hub(d, config, &hub)) return d; // printed itself, before the devices behind it
        d->kind = DEV_OTHER;
    } else if (kbd.found && kbd.ep_addr && !setup_keyboard(d, config, &kbd)) {
        d->kind = DEV_OTHER;
    }
    print_device(d, "");
    return d;
}

// The device went away (or its port was disabled): forget it, and every
// device behind it.
static void detach(struct usb_device *d) {
    struct xhci *c = d->c;
    if (d->kind == DEV_HUB)
        for (unsigned p = 1; p <= d->ports; p++)
            if (d->children[p]) detach(d->children[p]);
    char path[24];
    port_path(d, path, sizeof path);
    kprintf("  usb%u port %s: %s disconnected\n", c->index, path, d->name);
    if (d->parent) d->parent->children[d->port] = NULL;
    else c->root[d->port] = NULL;
    free_device(d);
}

static uint32_t portsc_read(struct xhci *c, unsigned port) { return op_read(c, OP_PORTSC(port)); }

// Look at a root port: a device that went away is detached, a new one is
// attached (a USB 2 port is reset first). `initial`: the first look, when
// every device there is new.
static void root_port(struct xhci *c, unsigned port, bool initial) {
    uint32_t sc = portsc_read(c, port);
    if (sc == 0xffffffff) return;
    op_write(c, OP_PORTSC(port), (sc & PORTSC_KEEP) | (sc & PORTSC_CHANGES));
    bool connected = sc & PORTSC_CCS;
    struct usb_device *d = c->root[port];
    if (d && (!connected || (sc & PORTSC_CSC) || !(sc & PORTSC_PED))) {
        detach(d);
        d = NULL;
    }
    if (!connected || d || !(initial || (sc & PORTSC_CSC))) return;
    if (!initial) {
        thread_sleep(DEBOUNCE_MS);
        sc = portsc_read(c, port);
        if (!(sc & PORTSC_CCS)) return;
        op_write(c, OP_PORTSC(port), (sc & PORTSC_KEEP) | (sc & PORTSC_CHANGES));
    }

    uint64_t deadline = clock_monotonic_ns() + PORT_RESET_MS * NS_PER_MS;
    if (c->port_major[port] != 3) {
        // USB 2: a reset enables the port, then it says what speed it got.
        op_write(c, OP_PORTSC(port), (sc & PORTSC_KEEP) | PORTSC_PR);
        while (!((sc = portsc_read(c, port)) & PORTSC_PRC) && clock_monotonic_ns() < deadline) thread_sleep(1);
        op_write(c, OP_PORTSC(port), (sc & PORTSC_KEEP) | (sc & PORTSC_CHANGES));
    } else {
        // USB 3 ports enable themselves once the link is up.
        while (!((sc = portsc_read(c, port)) & PORTSC_PED) && (sc & PORTSC_CCS) && clock_monotonic_ns() < deadline)
            thread_sleep(1);
    }
    sc = portsc_read(c, port);
    if (!(sc & PORTSC_PED)) {
        if (sc & PORTSC_CCS)
            kprintf(ANSI_YELLOW "  [warn] usb%u port %u: the port did not enable (PORTSC %08x)" ANSI_RESET "\n",
                    c->index, port, sc);
        return;
    }
    thread_sleep(RESET_RECOVERY_MS);
    c->root[port] = attach(c, NULL, (uint8_t)port, (uint8_t)PORTSC_SPEED(sc));
}

// ---------------------------------------------------------------------------
// The controller's thread
// ---------------------------------------------------------------------------

static void hub_changes(struct usb_device *hub) {
    uint8_t s[4];
    // Hub-wide changes (local power, over-current): clear them so the hub
    // stops reporting them.
    if (control_ok(control(hub, REQ_IN | REQ_CLASS, REQ_GET_STATUS, 0, 0, 4, s)))
        for (unsigned bit = 0; bit < 2; bit++)
            if (s[2] & (1u << bit)) control(hub, REQ_CLASS, REQ_CLEAR_FEATURE, (uint16_t)bit, 0, 0, NULL);
    for (unsigned p = 1; p <= hub->ports; p++) hub_port(hub, p, false);
    start_interrupt_transfers(hub);
}

// An interrupt endpoint stopped with an error: reset it and start again,
// unless it keeps failing.
static void restart_interrupt_endpoint(struct usb_device *d) {
    if (++d->int_errors > MAX_ENDPOINT_ERRORS) {
        dev_warn(d, "its interrupt endpoint keeps failing; not using it any more");
        d->int_stopped = true;
        return;
    }
    endpoint_recover(d, d->int_dci, &d->intr, false);
    start_interrupt_transfers(d);
}

// The thread's work on one device and the ones behind it.
static void service(struct usb_device *d) {
    if (d == NULL) return;
    struct xhci *c = d->c;
    uint64_t flags = spin_lock_irqsave(&c->event_lock);
    bool changed = d->hub_changed, halted = d->int_halted && !d->int_stopped;
    d->hub_changed = d->int_halted = false;
    spin_unlock_irqrestore(&c->event_lock, flags);

    if (halted) restart_interrupt_endpoint(d);
    if (d->kind == DEV_HUB) {
        if (changed) hub_changes(d);
        for (unsigned p = 1; p <= d->ports; p++) service(d->children[p]);
    } else if (d->kind == DEV_KEYBOARD) {
        uint8_t leds = usb_kbd_leds(keyboard_mods());
        if (leds != d->leds) {
            d->leds = leds;
            control(d, REQ_CLASS | REQ_INTERFACE, HID_SET_REPORT, 0x0200, d->iface, 1, &leds);
        }
    }
}

// Repeat held keys whose time has come, and note keyboards whose LEDs don't
// show the lock state (another keyboard may have changed it). Returns when
// to look again: when the next repeat is due, LED_CHECK_MS from now with a
// keyboard attached, or UINT64_MAX. Call with event_lock held.
static uint64_t keyboards_tick(struct xhci *c, uint64_t now) {
    uint64_t next = UINT64_MAX;
    uint8_t leds = usb_kbd_leds(keyboard_mods());
    for (unsigned s = 1; s <= c->max_slots; s++) {
        struct usb_device *d = c->slots[s];
        if (d == NULL || d->kind != DEV_KEYBOARD) continue;
        if (d->leds != leds) c->work = true;
        uint64_t t = usb_kbd_repeat(&d->kbd, now);
        if (t && t < next) next = t;
        if (now + LED_CHECK_MS * NS_PER_MS < next) next = now + LED_CHECK_MS * NS_PER_MS;
    }
    return next;
}

static void xhci_thread(void *arg) {
    struct xhci *c = arg;
    while (!c->dead) {
        uint32_t ports[8];
        uint64_t flags = spin_lock_irqsave(&c->event_lock);
        for (;;) {
            process_events(c);
            uint64_t now = clock_monotonic_ns();
            uint64_t next = keyboards_tick(c, now);
            if (c->work) break;
            if (!c->use_irq && next > now + POLL_MS * NS_PER_MS) next = now + POLL_MS * NS_PER_MS;
            if (next == UINT64_MAX) wait_queue_sleep(&c->thread_wq, &c->event_lock);
            else wait_queue_sleep_until(&c->thread_wq, &c->event_lock, next);
        }
        memcpy(ports, c->port_changes, sizeof ports);
        memset(c->port_changes, 0, sizeof c->port_changes);
        c->work = false;
        spin_unlock_irqrestore(&c->event_lock, flags);

        mutex_lock(&c->lock);
        for (unsigned p = 1; p <= c->max_ports; p++)
            if (ports[p / 32] & (1u << (p % 32))) root_port(c, p, false);
        for (unsigned p = 1; p <= c->max_ports; p++) service(c->root[p]);
        mutex_unlock(&c->lock);
    }
}

// ---------------------------------------------------------------------------
// Bringing the controller up
// ---------------------------------------------------------------------------

// Take the controller from the firmware (USB Legacy Support), and note each
// root port's protocol (Supported Protocol). The firmware gets a second to
// let go, then it is cut off: its SMIs are turned off either way.
static void read_ext_caps(struct xhci *c) {
    uint32_t off = HCC1_XECP(cap_read(c, CAP_HCCPARAMS1)) * 4;
    for (unsigned n = 0; off && n < 64; n++) {
        volatile uint32_t *cap = (volatile uint32_t *)(c->base + off);
        uint32_t v = cap[0];
        if ((v & 0xff) == XCAP_LEGACY) {
            if (v & LEGACY_BIOS_OWNED) {
                ((volatile uint8_t *)cap)[3] = 1; // OS owned
                uint64_t deadline = clock_monotonic_ns() + 1000 * NS_PER_MS;
                while ((cap[0] & LEGACY_BIOS_OWNED) && clock_monotonic_ns() < deadline) thread_sleep(1);
                if (cap[0] & LEGACY_BIOS_OWNED) {
                    warn(c, "the firmware did not let go of the controller; taking it anyway");
                    ((volatile uint8_t *)cap)[2] = 0;
                }
            }
            cap[LEGACY_CTLSTS / 4] = (cap[LEGACY_CTLSTS / 4] & ~LEGACY_SMI_ENABLES) | LEGACY_SMI_EVENTS;
        } else if ((v & 0xff) == XCAP_PROTOCOL) {
            uint8_t major = v >> 24;
            uint32_t ports = cap[2];
            unsigned first = ports & 0xff, count = ports >> 8 & 0xff;
            for (unsigned p = first; p < first + count && p <= c->max_ports; p++) {
                if (p == 0) continue;
                c->port_major[p] = major;
                c->port_slot_type[p] = cap[3] & 0x1f;
            }
        }
        uint32_t next = v >> 8 & 0xff;
        off = next ? off + next * 4 : 0;
    }
}

// Intel's 7- to 9-series chipsets start with every port on their EHCI
// controllers; hand the ones the xHCI controller can take to it (as Linux
// does).
static void intel_route_ports(struct pci_dev *d) {
    static const uint16_t switchable[] = {0x1e31, 0x8c31, 0x9c31, 0x8cb1, 0x9cb1};
    if (d->vendor != 0x8086) return;
    for (size_t i = 0; i < sizeof switchable / sizeof switchable[0]; i++) {
        if (d->device != switchable[i]) continue;
        pci_write32(d, INTEL_USB3_PSSEN, pci_read32(d, INTEL_USB3PRM));
        pci_write32(d, INTEL_XUSB2PR, pci_read32(d, INTEL_USB2PRM));
        return;
    }
}

static bool setup_scratchpads(struct xhci *c) {
    unsigned n = HCS2_SCRATCHPADS(cap_read(c, CAP_HCSPARAMS2));
    if (n == 0) return true;
    size_t array_pages = (n * sizeof(uint64_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t array = dma_alloc(c, array_pages);
    if (array == 0) return false;
    c->scratchpads = phys_to_virt(array);
    for (unsigned i = 0; i < n; i++) {
        uint64_t page = dma_alloc(c, 1);
        if (page == 0) return false; // the controller is not used, so this memory just stays allocated
        c->scratchpads[i] = page;
    }
    c->scratchpad_count = n;
    return true;
}

// Reset the controller, give it its data structures and start it.
static bool start_controller(struct xhci *c) {
    if (!(op_read(c, OP_USBSTS) & USBSTS_HCH)) {
        op_write(c, OP_USBCMD, op_read(c, OP_USBCMD) & ~USBCMD_RUN);
        if (!wait_op(c, OP_USBSTS, USBSTS_HCH, USBSTS_HCH, 100)) {
            warn(c, "the controller did not stop (USBSTS %08x)", op_read(c, OP_USBSTS));
            return false;
        }
    }
    op_write(c, OP_USBCMD, USBCMD_HCRST);
    thread_sleep(1); // some Intel controllers hang if read right after a reset
    if (!wait_op(c, OP_USBCMD, USBCMD_HCRST, 0, 1000) || !wait_op(c, OP_USBSTS, USBSTS_CNR, 0, 1000)) {
        warn(c, "the controller did not reset (USBSTS %08x)", op_read(c, OP_USBSTS));
        return false;
    }
    if (!(op_read(c, OP_PAGESIZE) & 1)) {
        warn(c, "the controller does not support 4 KiB pages");
        return false;
    }

    op_write(c, OP_CONFIG, c->max_slots);
    op_write64(c, OP_DCBAAP, c->dma);
    op_write64(c, OP_CRCR, c->cmd.phys | c->cmd.cycle);

    // One event ring segment, for interrupter 0.
    volatile uint64_t *erst = (volatile uint64_t *)((uint8_t *)phys_to_virt(c->dma) + PAGE_SIZE / 2);
    erst[0] = c->events_phys;
    erst[1] = EVENT_TRBS;
    rt_write(c, RT_ERSTSZ, 1);
    rt_write64(c, RT_ERDP, c->events_phys);
    rt_write64(c, RT_ERSTBA, c->dma + PAGE_SIZE / 2);
    rt_write(c, RT_IMOD, IMOD_40US);
    rt_write(c, RT_IMAN, IMAN_IE | IMAN_IP);

    op_write(c, OP_USBCMD, USBCMD_RUN | USBCMD_INTE);
    if (!wait_op(c, OP_USBSTS, USBSTS_HCH, 0, 100)) {
        warn(c, "the controller did not start (USBSTS %08x)", op_read(c, OP_USBSTS));
        return false;
    }
    return true;
}

static bool xhci_probe(struct pci_dev *pd) {
    if (controller_count == MAX_CONTROLLERS) return false;
    volatile uint8_t *base = pci_map_bar(pd, 0);
    if (base == NULL || pd->bars[0].size < 0x1000) {
        kprintf(ANSI_YELLOW "  [warn] xHCI %02x:%02x.%x: BAR 0 is not a mapped memory BAR" ANSI_RESET "\n", pd->bus,
                pd->dev, pd->func);
        return false;
    }
    struct xhci *c = kcalloc(1, sizeof *c);
    if (c == NULL) return false;
    c->pci = pd;
    c->base = base;
    c->vector = -1;
    uint32_t caplength = cap_read(c, CAP_LENGTH);
    c->version = caplength >> 16;
    c->op = (volatile uint32_t *)(base + (caplength & 0xff));
    c->rt = (volatile uint32_t *)(base + (cap_read(c, CAP_RTSOFF) & ~0x1fu));
    c->db = (volatile uint32_t *)(base + (cap_read(c, CAP_DBOFF) & ~0x3u));
    uint32_t hcs1 = cap_read(c, CAP_HCSPARAMS1), hcc1 = cap_read(c, CAP_HCCPARAMS1);
    c->max_slots = HCS1_MAX_SLOTS(hcs1);
    c->max_ports = HCS1_MAX_PORTS(hcs1);
    c->ctx_size = hcc1 & HCC1_CSZ ? 64 : 32;
    c->ppc = hcc1 & HCC1_PPC;
    c->dma_limit = hcc1 & HCC1_AC64 ? UINT64_MAX : (uint64_t)1 << 32;
    mutex_init(&c->lock, "xhci controller");
    spin_init(&c->event_lock, "xhci events");

    pci_enable(pd);
    read_ext_caps(c);
    intel_route_ports(pd);

    // DCBAA (2 KiB) and the event ring segment table, then the command ring
    // and the event ring.
    c->dma = dma_alloc(c, 3);
    if (c->dma == 0 || !setup_scratchpads(c)) {
        warn(c, "no memory");
        return false;
    }
    c->dcbaa = phys_to_virt(c->dma);
    if (c->scratchpad_count) c->dcbaa[0] = virt_to_phys(c->scratchpads);
    ring_init(&c->cmd, c->dma + PAGE_SIZE);
    c->events_phys = c->dma + 2 * PAGE_SIZE;
    c->events = phys_to_virt(c->events_phys);
    c->evt_cycle = 1;

    const char *irq_kind = "MSI-X";
    int vector = pci_enable_msix(pd, 0, xhci_interrupt);
    if (vector < 0) {
        irq_kind = "MSI";
        vector = pci_enable_msi(pd, xhci_interrupt);
    }
    c->vector = vector;
    c->use_irq = vector >= 0;
    c->index = (unsigned)controller_count;
    controllers[controller_count++] = c; // the interrupt handler looks here

    if (!start_controller(c)) {
        c->dead = true;
        return true;
    }

    // A no-op command shows whether the interrupt arrives: give it 100 ms
    // before looking at the event ring.
    mutex_lock(&c->lock);
    uint64_t flags = spin_lock_irqsave(&c->event_lock);
    ring_push(&c->cmd, 0, 0, TRB_TYPE(TRB_NOOP_COMMAND), false);
    spin_unlock_irqrestore(&c->event_lock, flags);
    c->db[0] = 0;
    uint64_t deadline = clock_monotonic_ns() + 100 * NS_PER_MS;
    while (c->use_irq && c->irq_count == 0 && clock_monotonic_ns() < deadline) thread_sleep(1);
    uint8_t code = command(c, 0, TRB_TYPE(TRB_NOOP_COMMAND), NULL);
    mutex_unlock(&c->lock);
    if (code != CC_SUCCESS) {
        if (!c->dead) warn(c, "a no-op command failed (completion code %u)", code);
        c->dead = true;
        return true;
    }
    bool irq_works = c->use_irq && c->irq_count > 0;

    unsigned usb2 = 0, usb3 = 0;
    for (unsigned p = 1; p <= c->max_ports; p++) {
        if (c->port_major[p] == 2) usb2++;
        if (c->port_major[p] == 3) usb3++;
    }
    kprintf("  xHCI %02x:%02x.%x: usb%u, version %x.%02x, %u ports (%u USB 2, %u USB 3), %u slots, ", pd->bus,
            pd->dev, pd->func, c->index, c->version >> 8, c->version & 0xff, c->max_ports, usb2, usb3, c->max_slots);
    if (irq_works) kprintf("%s on vector 0x%x\n", irq_kind, vector);
    else if (vector >= 0) kprintf("polled (its %s did not arrive)\n", irq_kind);
    else kprintf("polled (no MSI-X or MSI)\n");
    c->use_irq = irq_works;

    if (c->ppc) {
        for (unsigned p = 1; p <= c->max_ports; p++) {
            uint32_t sc = portsc_read(c, p);
            if (!(sc & PORTSC_PP)) op_write(c, OP_PORTSC(p), (sc & PORTSC_KEEP) | PORTSC_PP);
        }
        thread_sleep(20);
    }
    mutex_lock(&c->lock);
    for (unsigned p = 1; p <= c->max_ports && !c->dead; p++) root_port(c, p, true);
    mutex_unlock(&c->lock);

    char name[THREAD_NAME_MAX];
    ksnprintf(name, sizeof name, "usb%u", c->index);
    if (thread_create(name, xhci_thread, c) == NULL) warn(c, "no thread: devices plugged in later are not found");
    return true;
}

static const struct pci_id xhci_ids[] = {
    {PCI_ANY_ID, PCI_ANY_ID, PCI_CLASS(0x0c, 0x03, 0x30), PCI_CLASS_MASK_ALL},
    {0},
};
PCI_DRIVER(xhci, xhci_ids, xhci_probe);

// ---------------------------------------------------------------------------
// Summaries and `lsusb`
// ---------------------------------------------------------------------------

struct kbd_list {
    char *line;
    size_t size, len, count;
};

static void list_keyboards(struct usb_device *d, struct kbd_list *l) {
    if (d == NULL) return;
    if (d->kind == DEV_HUB)
        for (unsigned p = 1; p <= d->ports; p++) list_keyboards(d->children[p], l);
    if (d->kind != DEV_KEYBOARD) return;
    char path[24];
    port_path(d, path, sizeof path);
    if (l->len < l->size)
        l->len += (size_t)ksnprintf(l->line + l->len, l->size - l->len, "%s%s (usb%u port %s)", l->count ? ", " : "",
                                    d->name, d->c->index, path);
    l->count++;
}

size_t usb_keyboard_summary(char *line, size_t size) {
    char list[192] = "";
    struct kbd_list l = {list, sizeof list, 0, 0};
    for (size_t i = 0; i < controller_count; i++) {
        struct xhci *c = controllers[i];
        mutex_lock(&c->lock);
        for (unsigned p = 1; p <= c->max_ports; p++) list_keyboards(c->root[p], &l);
        mutex_unlock(&c->lock);
    }
    if (l.count) ksnprintf(line, size, "USB keyboard%s ready: %s", l.count == 1 ? "" : "s", list);
    else if (size) line[0] = '\0';
    return l.count;
}

static void list_device(struct usb_device *d) {
    if (d == NULL) return;
    char path[24], desc[128];
    port_path(d, path, sizeof path);
    describe(d, desc, sizeof desc);
    kprintf("usb%u port %s: %s\n", d->c->index, path, desc);
    if (d->kind == DEV_HUB)
        for (unsigned p = 1; p <= d->ports; p++) list_device(d->children[p]);
}

SHELL_COMMAND(lsusb, "", "list USB devices");
static void cmd_lsusb(int argc, char **argv) {
    if (controller_count == 0) {
        kprintf("lsusb: no USB controllers\n");
        return;
    }
    for (size_t i = 0; i < controller_count; i++) {
        struct xhci *c = controllers[i];
        mutex_lock(&c->lock);
        struct pci_dev *pd = c->pci;
        kprintf("usb%u: xHCI %x.%02x at %02x:%02x.%x%s\n", c->index, c->version >> 8, c->version & 0xff, pd->bus,
                pd->dev, pd->func, c->dead ? " (not working)" : "");
        for (unsigned p = 1; p <= c->max_ports; p++) list_device(c->root[p]);
        mutex_unlock(&c->lock);
    }
}
