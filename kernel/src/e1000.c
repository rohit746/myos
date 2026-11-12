// Intel's gigabit Ethernet controllers that share the e1000 register set:
// the 82540EM and 82545EM (QEMU's `-device e1000`), the 82574L (QEMU's
// `-device e1000e`, a q35 machine's default card), and the I217, I218 and
// I219 that most Intel desktops and laptops since 2013 have on board.
// Only QEMU's have been tried; docs/HARDWARE.md records real ones.
//
// At probe time the driver resets the MAC, reads the MAC address the
// card loaded from its NVM (RAL0/RAH0, or the EEPROM on an 8254x that left
// them empty), sets the link up with autonegotiation, and gives the card a
// receive and a transmit ring of RING_SIZE legacy descriptors, each with a
// 2 KiB buffer in memory of its own: frames are copied in and out of
// those. Received frames raise an MSI where the card has one (not QEMU's
// e1000); without one the net thread polls the receive ring.
//
// The receive ring is only touched by the net thread (poll). Transmitting
// can happen from any thread, under a spinlock: the frame goes into the
// next descriptor's buffer if the card is done with it (DD), and TDT moves
// past it.

#include <console.h>
#include <kheap.h>
#include <kprintf.h>
#include <memory.h>
#include <net.h>
#include <pci.h>
#include <pmm.h>
#include <sched.h>
#include <string.h>
#include <sync.h>
#include <time.h>

// Registers (offsets in BAR 0).
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00c0
#define REG_IMS    0x00d0
#define REG_IMC    0x00d8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_RDTR   0x2820
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200 // 128 registers
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404

#define CTRL_FD      (1u << 0)
#define CTRL_LRST    (1u << 3)
#define CTRL_ASDE    (1u << 5)
#define CTRL_SLU     (1u << 6)
#define CTRL_ILOS    (1u << 7)
#define CTRL_RST     (1u << 26)
#define CTRL_VME     (1u << 30)
#define CTRL_PHY_RST (1u << 31)

#define STATUS_FD       (1u << 0)
#define STATUS_LU       (1u << 1)
#define STATUS_SPEED(s) ((s) >> 6 & 3) // 0: 10, 1: 100, 2 or 3: 1000 Mbit/s

#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15) // accept broadcasts
#define RCTL_SECRC (1u << 26) // strip the FCS
// BSIZE 00 with BSEX 0: 2048-byte buffers.

#define TCTL_EN   (1u << 1)
#define TCTL_PSP  (1u << 3) // pad short packets
#define TCTL_CT   (0x0fu << 4)
#define TCTL_COLD (0x3fu << 12)
#define TCTL_RTLC (1u << 24)

#define TIPG_COPPER (8u | 8u << 10 | 6u << 20)

// Interrupt causes.
#define ICR_LSC    (1u << 2) // link status change
#define ICR_RXSEQ  (1u << 3)
#define ICR_RXDMT0 (1u << 4) // receive ring getting empty
#define ICR_RXO    (1u << 6) // receive overrun
#define ICR_RXT0   (1u << 7) // a frame came in
#define IMS_WANTED (ICR_LSC | ICR_RXSEQ | ICR_RXDMT0 | ICR_RXO | ICR_RXT0)

#define RAH_AV (1u << 31) // the address is valid

#define RX_DD  (1u << 0)
#define RX_EOP (1u << 1)
#define TX_EOP  (1u << 0)
#define TX_IFCS (1u << 1)
#define TX_RS   (1u << 3)
#define TX_DD   (1u << 0)

#define RING_SIZE 64 // descriptors per ring: 1 KiB, a page holds it
#define BUF_SIZE  2048

#define RESET_MS 20

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
};

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
};

_Static_assert(sizeof(struct rx_desc) == 16 && sizeof(struct tx_desc) == 16, "descriptors are 16 bytes");

// DMA memory, page by page: the two rings, then the buffers.
#define DMA_RX_RING 0
#define DMA_TX_RING 1
#define DMA_RX_BUFS 2
#define DMA_TX_BUFS (DMA_RX_BUFS + RING_SIZE * BUF_SIZE / PAGE_SIZE)
#define DMA_PAGES   (DMA_TX_BUFS + RING_SIZE * BUF_SIZE / PAGE_SIZE)

#define MAX_CARDS 4

struct model {
    uint16_t device;
    const char *name;
    bool e1000e; // 82574L and the PCH ones: else an 8254x, whose EEPROM EERD reads
};

static const struct model models[] = {
    {0x100e, "82540EM", false}, {0x100f, "82545EM", false}, {0x10d3, "82574L", true},
    {0x153a, "I217-LM", true},  {0x153b, "I217-V", true},   {0x155a, "I218-LM", true},
    {0x1559, "I218-V", true},   {0x15a0, "I218-LM", true},  {0x15a1, "I218-V", true},
    {0x15a2, "I218-LM", true},  {0x15a3, "I218-V", true},   {0x156f, "I219-LM", true},
    {0x1570, "I219-V", true},   {0x15b7, "I219-LM", true},  {0x15b8, "I219-V", true},
    {0x15b9, "I219-LM", true},  {0x15d7, "I219-LM", true},  {0x15d8, "I219-V", true},
    {0x15e3, "I219-LM", true},  {0x15d6, "I219-V", true},   {0x15bd, "I219-LM", true},
    {0x15be, "I219-V", true},   {0x15bb, "I219-LM", true},  {0x15bc, "I219-V", true},
    {0x15df, "I219-LM", true},  {0x15e0, "I219-V", true},   {0x15e1, "I219-LM", true},
    {0x15e2, "I219-V", true},   {0x0d4e, "I219-LM", true},  {0x0d4f, "I219-V", true},
    {0x0d4c, "I219-LM", true},  {0x0d4d, "I219-V", true},   {0x0d53, "I219-LM", true},
    {0x0d55, "I219-V", true},   {0x15fb, "I219-LM", true},  {0x15fc, "I219-V", true},
    {0x15f9, "I219-LM", true},  {0x15fa, "I219-V", true},   {0x15f4, "I219-LM", true},
    {0x15f5, "I219-V", true},   {0x1a1e, "I219-LM", true},  {0x1a1f, "I219-V", true},
    {0x1a1c, "I219-LM", true},  {0x1a1d, "I219-V", true},
};

#define MODELS (sizeof models / sizeof models[0])

struct card {
    struct netif nif;
    struct pci_dev *pci;
    const struct model *model;
    volatile uint32_t *regs;
    int vector; // MSI, or -1: polled
    volatile struct rx_desc *rx;
    volatile struct tx_desc *tx;
    uint8_t *rx_bufs, *tx_bufs;
    unsigned rx_next; // the next descriptor the card fills
    bool rx_partial;  // in the middle of a frame too long for one buffer: dropping it
    unsigned tx_next; // the next descriptor to fill
    struct spinlock tx_lock;
};

static struct card *cards[MAX_CARDS];
static unsigned card_count;

static uint32_t rd(struct card *c, unsigned reg) { return c->regs[reg / 4]; }
static void wr(struct card *c, unsigned reg, uint32_t v) { c->regs[reg / 4] = v; }

static void e1000_interrupt(struct interrupt_frame *f) {
    for (unsigned i = 0; i < card_count; i++) {
        struct card *c = cards[i];
        if (c->vector < 0 || (uint64_t)c->vector != f->vector) continue;
        if (rd(c, REG_ICR)) net_wake(); // reading ICR clears it
    }
}

static bool e1000_transmit(struct netif *nif, const void *frame, size_t len) {
    struct card *c = nif->priv;
    if (len > BUF_SIZE) return false;
    uint64_t flags = spin_lock_irqsave(&c->tx_lock);
    unsigned i = c->tx_next;
    volatile struct tx_desc *d = &c->tx[i];
    bool free = d->status & TX_DD;
    if (free) {
        memcpy(c->tx_bufs + (size_t)i * BUF_SIZE, frame, len);
        d->length = (uint16_t)len;
        d->cmd = TX_EOP | TX_IFCS | TX_RS;
        d->status = 0;
        c->tx_next = (i + 1) % RING_SIZE;
        __asm__ volatile("" ::: "memory"); // the descriptor is written before the card is told
        wr(c, REG_TDT, c->tx_next);
    }
    spin_unlock_irqrestore(&c->tx_lock, flags);
    return free;
}

static void e1000_poll(struct netif *nif) {
    struct card *c = nif->priv;
    for (unsigned n = 0; n < RING_SIZE; n++) {
        unsigned i = c->rx_next;
        volatile struct rx_desc *d = &c->rx[i];
        uint8_t status = d->status;
        if (!(status & RX_DD)) break;
        __asm__ volatile("" ::: "memory"); // DD first, then the rest of what the card wrote
        uint16_t len = d->length;
        if (!(status & RX_EOP)) {
            c->rx_partial = true; // more than 2 KiB: no jumbo frames
        } else if (c->rx_partial || d->errors) {
            c->rx_partial = false;
            nif->rx_dropped++;
        } else {
            net_receive(nif, c->rx_bufs + (size_t)i * BUF_SIZE, len);
        }
        d->status = 0;
        c->rx_next = (i + 1) % RING_SIZE;
        // Hand it back: the card owns RDH up to, not including, RDT.
        wr(c, REG_RDT, i);
    }
}

static bool e1000_link(struct netif *nif, unsigned *mbps, bool *full_duplex) {
    struct card *c = nif->priv;
    uint32_t s = rd(c, REG_STATUS);
    static const unsigned speeds[] = {10, 100, 1000, 1000};
    *mbps = speeds[STATUS_SPEED(s)];
    *full_duplex = s & STATUS_FD;
    return s & STATUS_LU;
}

static const struct netif_ops e1000_ops = {.transmit = e1000_transmit, .poll = e1000_poll, .link = e1000_link};

// A word of an 8254x's EEPROM, or 0xffff if the read never finishes.
static uint16_t eeprom_read(struct card *c, unsigned word) {
    wr(c, REG_EERD, 1u | word << 8); // start, address
    for (int i = 0; i < 10000; i++) {
        uint32_t v = rd(c, REG_EERD);
        if (v & (1u << 4)) return (uint16_t)(v >> 16); // done
    }
    return 0xffff;
}

static bool read_mac(struct card *c, uint8_t mac[ETH_ALEN]) {
    uint32_t ral = rd(c, REG_RAL0), rah = rd(c, REG_RAH0);
    if ((rah & RAH_AV) && (ral || (rah & 0xffff))) {
        for (int i = 0; i < 4; i++) mac[i] = (uint8_t)(ral >> (8 * i));
        mac[4] = (uint8_t)rah;
        mac[5] = (uint8_t)(rah >> 8);
        return true;
    }
    if (c->model->e1000e) return false;
    for (unsigned w = 0; w < 3; w++) {
        uint16_t v = eeprom_read(c, w);
        mac[2 * w] = (uint8_t)v;
        mac[2 * w + 1] = (uint8_t)(v >> 8);
    }
    return (mac[0] & 1) == 0 && memcmp(mac, "\xff\xff\xff\xff\xff\xff", ETH_ALEN) != 0;
}

static bool e1000_probe(struct pci_dev *d) {
    const struct model *model = NULL;
    for (size_t i = 0; i < MODELS; i++)
        if (models[i].device == d->device) model = &models[i];
    if (model == NULL || card_count == MAX_CARDS) return false;

    pci_enable(d);
    volatile uint32_t *regs = pci_map_bar(d, 0);
    if (regs == NULL) {
        kprintf(ANSI_YELLOW "  [warn] e1000 %02x:%02x.%x: BAR 0 is not a memory BAR" ANSI_RESET "\n", d->bus, d->dev,
                d->func);
        return false;
    }
    uint64_t dma = pmm_alloc_contiguous(DMA_PAGES);
    struct card *c = kmalloc(sizeof *c);
    if (dma == 0 || c == NULL) {
        if (dma) pmm_free_contiguous(dma, DMA_PAGES);
        kfree(c);
        kprintf(ANSI_YELLOW "  [warn] e1000 %02x:%02x.%x: no memory" ANSI_RESET "\n", d->bus, d->dev, d->func);
        return false;
    }
    memset(phys_to_virt(dma), 0, DMA_PAGES * PAGE_SIZE);
    *c = (struct card){.pci = d, .model = model, .regs = regs, .vector = -1};
    spin_init(&c->tx_lock, "e1000 tx");

    // Reset the MAC, with the receiver, transmitter and interrupts off.
    wr(c, REG_IMC, ~0u);
    wr(c, REG_RCTL, 0);
    wr(c, REG_TCTL, 0);
    thread_sleep(1);
    wr(c, REG_CTRL, rd(c, REG_CTRL) | CTRL_RST);
    thread_sleep(RESET_MS);
    for (int i = 0; i < 100 && (rd(c, REG_CTRL) & CTRL_RST); i++) thread_sleep(1);
    wr(c, REG_IMC, ~0u);
    (void)rd(c, REG_ICR);

    // Link up, speed and duplex from autonegotiation.
    uint32_t ctrl = rd(c, REG_CTRL);
    ctrl &= ~(CTRL_LRST | CTRL_ILOS | CTRL_VME | CTRL_PHY_RST | CTRL_FD);
    wr(c, REG_CTRL, ctrl | CTRL_SLU | CTRL_ASDE);

    uint8_t *mac = c->nif.mac;
    if (!read_mac(c, mac)) {
        kprintf(ANSI_YELLOW "  [warn] e1000 %02x:%02x.%x: no MAC address" ANSI_RESET "\n", d->bus, d->dev, d->func);
        pmm_free_contiguous(dma, DMA_PAGES);
        kfree(c);
        return false;
    }
    wr(c, REG_RAL0, (uint32_t)mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24);
    wr(c, REG_RAH0, (uint32_t)mac[4] | (uint32_t)mac[5] << 8 | RAH_AV);
    for (unsigned i = 0; i < 128; i++) wr(c, REG_MTA + 4 * i, 0);

    c->rx = phys_to_virt(dma + DMA_RX_RING * PAGE_SIZE);
    c->tx = phys_to_virt(dma + DMA_TX_RING * PAGE_SIZE);
    c->rx_bufs = phys_to_virt(dma + DMA_RX_BUFS * PAGE_SIZE);
    c->tx_bufs = phys_to_virt(dma + DMA_TX_BUFS * PAGE_SIZE);
    for (unsigned i = 0; i < RING_SIZE; i++) {
        c->rx[i].addr = dma + DMA_RX_BUFS * PAGE_SIZE + (uint64_t)i * BUF_SIZE;
        c->tx[i].addr = dma + DMA_TX_BUFS * PAGE_SIZE + (uint64_t)i * BUF_SIZE;
        c->tx[i].status = TX_DD; // free
    }
    uint64_t rx_ring = dma + DMA_RX_RING * PAGE_SIZE, tx_ring = dma + DMA_TX_RING * PAGE_SIZE;
    wr(c, REG_RDBAL, (uint32_t)rx_ring);
    wr(c, REG_RDBAH, (uint32_t)(rx_ring >> 32));
    wr(c, REG_RDLEN, RING_SIZE * sizeof(struct rx_desc));
    wr(c, REG_RDH, 0);
    wr(c, REG_RDT, RING_SIZE - 1);
    wr(c, REG_RDTR, 0);
    wr(c, REG_TDBAL, (uint32_t)tx_ring);
    wr(c, REG_TDBAH, (uint32_t)(tx_ring >> 32));
    wr(c, REG_TDLEN, RING_SIZE * sizeof(struct tx_desc));
    wr(c, REG_TDH, 0);
    wr(c, REG_TDT, 0);
    wr(c, REG_TIPG, TIPG_COPPER);
    wr(c, REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD | TCTL_RTLC);
    wr(c, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    c->vector = pci_enable_msi(d, e1000_interrupt);
    cards[card_count++] = c;
    if (c->vector >= 0) wr(c, REG_IMS, IMS_WANTED);

    struct netif *nif = &c->nif;
    nif->ops = &e1000_ops;
    nif->priv = c;
    nif->polled = c->vector < 0;
    ksnprintf(nif->driver, sizeof nif->driver, "%s", model->e1000e ? "e1000e" : "e1000");
    char irq[32];
    if (c->vector >= 0) ksnprintf(irq, sizeof irq, "MSI on vector 0x%x", c->vector);
    else ksnprintf(irq, sizeof irq, "polled (no MSI)");
    ksnprintf(nif->desc, sizeof nif->desc, "Intel %s (%s) at %02x:%02x.%x, %s", model->name, nif->driver, d->bus,
              d->dev, d->func, irq);
    bool registered = net_register(nif);
    kprintf("  %s %02x:%02x.%x: Intel %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, %s%s%s\n", nif->driver, d->bus, d->dev,
            d->func, model->name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], irq, registered ? ", " : "",
            registered ? nif->name : " (too many cards: not used)");
    return true;
}

// Every model is matched in the probe, against `models`.
static const struct pci_id e1000_ids[] = {
    {0x8086, PCI_ANY_ID, PCI_CLASS(0x02, 0x00, 0x00), PCI_CLASS_MASK_SUBCLASS},
    {0},
};
PCI_DRIVER(e1000, e1000_ids, e1000_probe);
