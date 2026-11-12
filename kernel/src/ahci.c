// AHCI: SATA disks through an AHCI host bus adapter (PCI class 01:06:01),
// following the Serial ATA AHCI 1.3.1 specification.
//
// At probe time the driver takes the HBA from the firmware (the BIOS/OS
// handoff, when the HBA has one), resets it, and then, on every implemented
// port: points it at its command list and received-FIS area, resets the link
// (COMRESET) and waits for a device. A port whose device answers with the
// ATA signature gets IDENTIFY DEVICE and becomes a block device, "sda" on;
// ATAPI (CD/DVD) and the rest are only reported.
//
// Commands go one at a time per port, in command slot 0: READ DMA EXT and
// WRITE DMA EXT (48-bit LBA), through a 64 KiB DMA buffer per port that
// data is copied through. All DMA memory sits below 4 GiB unless the HBA
// says it can address 64 bits (CAP.S64A).
//
// Completion is interrupt driven: the HBA raises an MSI, the handler
// collects each port's interrupt status and wakes the thread waiting on that
// port. A command that hasn't finished after COMMAND_TIMEOUT_MS fails with
// EIO, and the port is restarted. An HBA without MSI is polled instead
// (legacy INTx needs the DSDT's _PRT, which needs an AML interpreter).

#include <block.h>
#include <console.h>
#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <memory.h>
#include <pci.h>
#include <pmm.h>
#include <sched.h>
#include <stdarg.h>
#include <string.h>
#include <sync.h>
#include <time.h>

// HBA registers (offsets in ABAR, BAR 5).
#define HBA_CAP  0x00
#define HBA_GHC  0x04
#define HBA_IS   0x08 // a bit per port with an interrupt pending
#define HBA_PI   0x0c // ports implemented
#define HBA_VS   0x10
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28

#define CAP_SCLO (1u << 24) // supports command list override
#define CAP_SAM  (1u << 18) // AHCI only: GHC.AE is read-only 1
#define CAP_SSS  (1u << 27) // staggered spin-up: ports need PxCMD.SUD
#define CAP_S64A (1u << 31) // 64-bit DMA addresses

#define GHC_HR (1u << 0)  // HBA reset
#define GHC_IE (1u << 1)  // interrupts on
#define GHC_AE (1u << 31) // AHCI mode (not legacy IDE)

#define CAP2_BOH  (1u << 0) // BIOS/OS handoff
#define BOHC_BOS  (1u << 0) // the BIOS owns the HBA
#define BOHC_OOS  (1u << 1) // the OS wants it
#define BOHC_BB   (1u << 4) // the BIOS is busy finishing commands

// Port registers, at 0x100 + port * 0x80.
#define PX_CLB  0x00 // command list base (1 KiB aligned), then the upper half
#define PX_CLBU 0x04
#define PX_FB   0x08 // received-FIS area (256 bytes, 256-byte aligned), then the upper half
#define PX_FBU  0x0c
#define PX_IS   0x10
#define PX_IE   0x14
#define PX_CMD  0x18
#define PX_TFD  0x20 // the device's status (bits 7:0) and error (15:8) registers
#define PX_SIG  0x24
#define PX_SSTS 0x28 // SATA status: DET in bits 3:0
#define PX_SCTL 0x2c // SATA control: DET in bits 3:0, IPM in 11:8
#define PX_SERR 0x30
#define PX_CI   0x38 // command issue: a bit per slot, cleared when the command is done

#define PXCMD_ST  (1u << 0)  // process the command list
#define PXCMD_SUD (1u << 1)  // spin up the device
#define PXCMD_POD (1u << 2)  // power the device on
#define PXCMD_CLO (1u << 3)  // command list override: clear BSY and DRQ
#define PXCMD_FRE (1u << 4)  // accept FISes from the device
#define PXCMD_FR  (1u << 14) // FIS receive running
#define PXCMD_CR  (1u << 15) // command list running

#define PXIS_DHRS (1u << 0)  // D2H Register FIS (a command finished)
#define PXIS_PSS  (1u << 1)  // PIO Setup FIS (IDENTIFY's data)
#define PXIS_DSS  (1u << 2)  // DMA Setup FIS
#define PXIS_SDBS (1u << 3)  // Set Device Bits FIS
#define PXIS_DPS  (1u << 5)  // a PRD with its I bit was done
#define PXIS_OFS  (1u << 24) // overflow
#define PXIS_INFS (1u << 26) // interface non-fatal error
#define PXIS_IFS  (1u << 27) // interface fatal error
#define PXIS_HBDS (1u << 28) // host bus data error
#define PXIS_HBFS (1u << 29) // host bus fatal error
#define PXIS_TFES (1u << 30) // task file error: the device reported an error
#define PXIS_ERRORS (PXIS_OFS | PXIS_IFS | PXIS_HBDS | PXIS_HBFS | PXIS_TFES)
#define PXIS_WANTED (PXIS_DHRS | PXIS_PSS | PXIS_DSS | PXIS_SDBS | PXIS_DPS | PXIS_INFS | PXIS_ERRORS)

#define TFD_ERR 0x01
#define TFD_DRQ 0x08
#define TFD_BSY 0x80

#define SSTS_DET(s)       ((s) & 0xf)
#define DET_NONE          0
#define DET_PHY           3 // a device is there and the link is up
#define SCTL_IPM_NO_SLEEP 0x300 // no partial or slumber power states
#define SCTL_DET_COMRESET 0x1

// PxSIG, from the device's first D2H Register FIS.
#define SIG_ATA   0x00000101
#define SIG_ATAPI 0xeb140101
#define SIG_SEMB  0xc33c0101
#define SIG_PM    0x96690101

// ATA commands.
#define ATA_IDENTIFY       0xec
#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_DEVICE_LBA     0x40

#define FIS_REG_H2D 0x27

// A command header in the command list (32 of them, one per slot).
struct cmd_header {
    uint16_t flags; // CFL (FIS length in dwords) in bits 4:0, W (write) bit 6
    uint16_t prdtl; // PRD table entries
    volatile uint32_t prdbc; // bytes transferred, written by the HBA
    uint32_t ctba, ctbau; // command table (128-byte aligned)
    uint32_t reserved[4];
};

#define CMD_HEADER_WRITE (1u << 6)

// A physical region descriptor: one contiguous piece of the data buffer.
struct prd {
    uint32_t dba, dbau;
    uint32_t reserved;
    uint32_t dbc; // byte count - 1 (bit 0 set: an even count) in bits 21:0, I (interrupt) bit 31
};

// The command table for a slot: the command FIS, then the PRDs.
struct cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct prd prdt[1];
};

// Each port's DMA memory: one page for the command list, received FISes and
// slot 0's command table, then the data buffer.
#define DMA_CL_OFFSET    0x000 // 1 KiB, 32 headers
#define DMA_FIS_OFFSET   0x400 // 256 bytes
#define DMA_TABLE_OFFSET 0x500 // 128-byte aligned
#define DATA_PAGES       16
#define DATA_BYTES       (DATA_PAGES * PAGE_SIZE)

#define MAX_CONTROLLERS 8
#define MAX_PORTS       32

#define COMMAND_TIMEOUT_MS 5000
#define LINK_DETECT_MS     100  // after a COMRESET, for a device to be seen at all
#define LINK_UP_MS         1000 // then for its link to come up
#define READY_MS           10000 // and for it to clear BSY (spinning disks spin up)
#define STOP_MS            500

struct ahci_controller;

struct ahci_port {
    struct ahci_controller *hba;
    unsigned index;
    volatile uint32_t *regs;
    uint64_t dma_phys;      // the page with the command list, FIS area and command table
    uint64_t data_phys;     // DATA_BYTES of data buffer
    uint8_t *data;          // the same, through the HHDM
    struct cmd_header *cl;
    struct cmd_table *table;
    struct mutex lock;      // one command at a time
    bool dead;              // its command engine would not stop: its DMA memory may still be in use
    struct spinlock irq_lock; // irq_status and waiters
    uint32_t irq_status;    // PxIS bits the interrupt handler collected
    struct wait_queue waiters;
    struct block_device disk;
};

struct ahci_controller {
    struct pci_dev *pci;
    volatile uint32_t *abar;
    uint32_t cap;
    bool use_irq;
    int vector;
    struct ahci_port *ports[MAX_PORTS];
};

static struct ahci_controller *controllers[MAX_CONTROLLERS];
static size_t controller_count;
static unsigned disk_letters; // disks named so far: sda, sdb, ...

static uint32_t hba_read(struct ahci_controller *c, unsigned reg) { return c->abar[reg / 4]; }
static void hba_write(struct ahci_controller *c, unsigned reg, uint32_t v) { c->abar[reg / 4] = v; }
static uint32_t port_read(struct ahci_port *p, unsigned reg) { return p->regs[reg / 4]; }
static void port_write(struct ahci_port *p, unsigned reg, uint32_t v) { p->regs[reg / 4] = v; }

// Wait up to `ms` for (register & mask) == want, sleeping between reads.
static bool wait_reg(volatile uint32_t *reg, uint32_t mask, uint32_t want, uint64_t ms) {
    uint64_t deadline = clock_monotonic_ns() + ms * NS_PER_MS;
    for (;;) {
        if ((*reg & mask) == want) return true;
        if (clock_monotonic_ns() > deadline) return false;
        thread_sleep(1);
    }
}

static void warn(struct ahci_controller *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kprintf(ANSI_YELLOW "  [warn] AHCI %02x:%02x.%x: ", c->pci->bus, c->pci->dev, c->pci->func);
    kvprintf(fmt, ap);
    kprintf(ANSI_RESET "\n");
    va_end(ap);
}

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

// Stop the command list engine and FIS receive (PxCMD.ST and FRE off, and
// wait for CR and FR to follow).
static bool port_stop(struct ahci_port *p) {
    uint32_t cmd = port_read(p, PX_CMD);
    if (cmd & PXCMD_ST) port_write(p, PX_CMD, cmd & ~PXCMD_ST);
    if (!wait_reg(&p->regs[PX_CMD / 4], PXCMD_CR, 0, STOP_MS)) return false;
    cmd = port_read(p, PX_CMD);
    if (cmd & PXCMD_FRE) port_write(p, PX_CMD, cmd & ~PXCMD_FRE);
    return wait_reg(&p->regs[PX_CMD / 4], PXCMD_FR, 0, STOP_MS);
}

static void port_start(struct ahci_port *p) {
    wait_reg(&p->regs[PX_CMD / 4], PXCMD_CR, 0, STOP_MS);
    port_write(p, PX_CMD, port_read(p, PX_CMD) | PXCMD_FRE | PXCMD_ST);
}

// After an error or a timeout (AHCI 6.2.2.1): stop the port, clear its
// errors, clear BSY/DRQ if the device left them set, and start it again.
// A port whose engine won't stop may still be doing DMA into slot 0's
// memory, so it is given up on.
static void port_recover(struct ahci_port *p) {
    port_write(p, PX_CMD, port_read(p, PX_CMD) & ~PXCMD_ST);
    if (!wait_reg(&p->regs[PX_CMD / 4], PXCMD_CR, 0, STOP_MS)) {
        warn(p->hba, "port %u: the command engine did not stop; the port is not used any more", p->index);
        p->dead = true;
        return;
    }
    port_write(p, PX_SERR, ~0u);
    port_write(p, PX_IS, ~0u);
    if ((port_read(p, PX_TFD) & (TFD_BSY | TFD_DRQ)) && (p->hba->cap & CAP_SCLO)) {
        port_write(p, PX_CMD, port_read(p, PX_CMD) | PXCMD_CLO);
        wait_reg(&p->regs[PX_CMD / 4], PXCMD_CLO, 0, STOP_MS);
    }
    port_start(p);
}

// Run one command in slot 0 and wait for it: `fis` is the 20-byte Register
// H2D FIS, `bytes` the data moved through the port's data buffer (0 for
// none). Call with the port's mutex held. 0 or -EIO.
static int port_exec(struct ahci_port *p, const uint8_t *fis, bool write, uint32_t bytes) {
    if (p->dead) return -EIO;
    if (!wait_reg(&p->regs[PX_TFD / 4], TFD_BSY | TFD_DRQ, 0, COMMAND_TIMEOUT_MS)) {
        port_recover(p);
        if (p->dead || (port_read(p, PX_TFD) & (TFD_BSY | TFD_DRQ))) return -EIO;
    }

    struct cmd_header *h = &p->cl[0];
    h->flags = 5 | (write ? CMD_HEADER_WRITE : 0); // a Register H2D FIS is 5 dwords
    h->prdtl = bytes ? 1 : 0;
    h->prdbc = 0;
    memset(p->table->cfis, 0, sizeof p->table->cfis);
    memcpy(p->table->cfis, fis, 20);
    if (bytes) {
        p->table->prdt[0] = (struct prd){
            .dba = (uint32_t)p->data_phys, .dbau = (uint32_t)(p->data_phys >> 32), .dbc = (bytes - 1) | 1u << 31};
    }

    uint64_t flags = spin_lock_irqsave(&p->irq_lock);
    p->irq_status = 0;
    spin_unlock_irqrestore(&p->irq_lock, flags);
    __asm__ volatile("" ::: "memory"); // the command is in memory before the HBA is told
    port_write(p, PX_CI, 1);

    uint64_t deadline = clock_monotonic_ns() + COMMAND_TIMEOUT_MS * NS_PER_MS;
    uint32_t status = 0;
    bool done = false;
    if (p->hba->use_irq) {
        flags = spin_lock_irqsave(&p->irq_lock);
        for (;;) {
            status = p->irq_status;
            done = !(port_read(p, PX_CI) & 1);
            if ((done && status) || (status & PXIS_ERRORS)) break;
            if (!wait_queue_sleep_until(&p->waiters, &p->irq_lock, deadline)) {
                status = p->irq_status;
                done = !(port_read(p, PX_CI) & 1);
                break;
            }
        }
        spin_unlock_irqrestore(&p->irq_lock, flags);
        if (done && status == 0) {
            // It finished, but the interrupt never came: poll from now on.
            warn(p->hba, "port %u: a command finished without an interrupt; polling from now on", p->index);
            p->hba->use_irq = false;
            hba_write(p->hba, HBA_GHC, hba_read(p->hba, HBA_GHC) & ~GHC_IE);
        }
    } else {
        for (;;) {
            status = port_read(p, PX_IS);
            done = !(port_read(p, PX_CI) & 1);
            if (done || (status & PXIS_ERRORS) || clock_monotonic_ns() > deadline) break;
            thread_yield();
        }
        port_write(p, PX_IS, status);
    }

    uint32_t tfd = port_read(p, PX_TFD);
    if (done && !(status & PXIS_ERRORS) && !(tfd & TFD_ERR)) return 0;
    if (!done && !(status & PXIS_ERRORS))
        warn(p->hba, "port %u: command %02x timed out (status %02x)", p->index, fis[2], tfd & 0xff);
    else
        warn(p->hba, "port %u: command %02x failed (status %02x, error %02x, PxIS %08x, PxSERR %08x)", p->index,
             fis[2], tfd & 0xff, tfd >> 8 & 0xff, status, port_read(p, PX_SERR));
    port_recover(p);
    return -EIO;
}

static void build_fis(uint8_t fis[20], uint8_t command, uint64_t lba, uint16_t count) {
    memset(fis, 0, 20);
    fis[0] = FIS_REG_H2D;
    fis[1] = 0x80; // C: this is a command
    fis[2] = command;
    fis[4] = lba;
    fis[5] = lba >> 8;
    fis[6] = lba >> 16;
    fis[7] = ATA_DEVICE_LBA;
    fis[8] = lba >> 24;
    fis[9] = lba >> 32;
    fis[10] = lba >> 40;
    fis[12] = count;
    fis[13] = count >> 8;
}

static int port_rw(struct block_device *dev, uint64_t lba, uint32_t count, void *buf, bool write) {
    struct ahci_port *p = dev->priv;
    uint32_t per_command = DATA_BYTES / dev->sector_size;
    uint8_t *b = buf;
    mutex_lock(&p->lock);
    int err = 0;
    while (count && err == 0) {
        uint32_t n = count < per_command ? count : per_command;
        uint32_t bytes = n * dev->sector_size;
        uint8_t fis[20];
        build_fis(fis, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, lba, (uint16_t)n);
        if (p->dead) { // its engine may still be using p->data
            err = -EIO;
            break;
        }
        if (write) memcpy(p->data, b, bytes);
        err = port_exec(p, fis, write, bytes);
        if (err == 0 && !write) memcpy(b, p->data, bytes);
        lba += n;
        count -= n;
        b += bytes;
    }
    mutex_unlock(&p->lock);
    return err;
}

static int ahci_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    return port_rw(dev, lba, count, buf, false);
}

static int ahci_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    return port_rw(dev, lba, count, (void *)buf, true);
}

static const struct block_ops ahci_ops = {.read = ahci_read, .write = ahci_write};

// IDENTIFY DEVICE strings are byte-swapped words, padded with spaces.
static void ata_string(char *out, const uint16_t *words, unsigned nwords) {
    unsigned n = 0;
    for (unsigned i = 0; i < nwords; i++) {
        out[n++] = (char)(words[i] >> 8);
        out[n++] = (char)words[i];
    }
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\0')) n--;
    out[n] = '\0';
    unsigned start = 0;
    while (out[start] == ' ') start++;
    memmove(out, out + start, n - start + 1);
}

// IDENTIFY the device on a port and register it as a disk. False, with a
// warning, if it can't be used.
static bool port_identify(struct ahci_port *p) {
    uint8_t fis[20];
    build_fis(fis, ATA_IDENTIFY, 0, 0);
    fis[7] = 0;
    mutex_lock(&p->lock);
    int err = port_exec(p, fis, false, 512);
    uint16_t id[256];
    memcpy(id, p->data, sizeof id);
    mutex_unlock(&p->lock);
    if (err) {
        warn(p->hba, "port %u: IDENTIFY DEVICE failed", p->index);
        return false;
    }

    struct block_device *d = &p->disk;
    ata_string(d->model, &id[27], 20);
    if (!(id[83] & (1u << 10))) {
        warn(p->hba, "port %u: %s has no 48-bit LBA (not supported)", p->index, d->model);
        return false;
    }
    d->sector_count = (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 |
                      (uint64_t)id[103] << 48;
    d->sector_size = 512;
    // Word 106: bit 14 set, 15 clear means it is valid; bit 12, the logical
    // sector is longer than 256 words, and words 117-118 say how many.
    if ((id[106] & 0xc000) == 0x4000 && (id[106] & (1u << 12)))
        d->sector_size = 2 * ((uint32_t)id[117] | (uint32_t)id[118] << 16);
    if (d->sector_size < 512 || d->sector_size > DATA_BYTES || d->sector_count == 0) {
        warn(p->hba, "port %u: %s reports %lu sectors of %u bytes", p->index, d->model, d->sector_count,
             d->sector_size);
        return false;
    }
    if (disk_letters >= 26) {
        warn(p->hba, "port %u: more than 26 disks, %s left out", p->index, d->model);
        return false;
    }
    ksnprintf(d->name, sizeof d->name, "sd%c", 'a' + disk_letters++);
    struct pci_dev *pci = p->hba->pci;
    ksnprintf(d->where, sizeof d->where, "AHCI %02x:%02x.%x port %u", pci->bus, pci->dev, pci->func, p->index);
    d->ops = &ahci_ops;
    d->priv = p;
    block_register(d);
    return true;
}

// ---------------------------------------------------------------------------
// The HBA
// ---------------------------------------------------------------------------

static void ahci_interrupt(struct interrupt_frame *f) {
    for (size_t i = 0; i < controller_count; i++) {
        struct ahci_controller *c = controllers[i];
        if (!c->use_irq) continue;
        uint32_t pending = hba_read(c, HBA_IS);
        if (pending == 0) continue;
        // Each port's status first, then the HBA's (AHCI 10.7.2.1).
        for (unsigned n = 0; n < MAX_PORTS; n++) {
            if (!(pending & (1u << n))) continue;
            struct ahci_port *p = c->ports[n];
            if (p == NULL) continue;
            uint32_t status = port_read(p, PX_IS);
            port_write(p, PX_IS, status);
            spin_lock(&p->irq_lock);
            p->irq_status |= status;
            wait_queue_wake_all(&p->waiters);
            spin_unlock(&p->irq_lock);
        }
        hba_write(c, HBA_IS, pending);
    }
}

// Take the HBA from the firmware (AHCI 10.6.3).
static void bios_handoff(struct ahci_controller *c) {
    if (!(hba_read(c, HBA_CAP2) & CAP2_BOH)) return;
    hba_write(c, HBA_BOHC, hba_read(c, HBA_BOHC) | BOHC_OOS);
    thread_sleep(25);
    uint64_t ms = hba_read(c, HBA_BOHC) & BOHC_BB ? 2000 : 25;
    if (!wait_reg(&c->abar[HBA_BOHC / 4], BOHC_BOS, 0, ms))
        warn(c, "the firmware did not let go of the HBA; taking it anyway");
}

// Turn on AHCI mode (unless the HBA is AHCI-only, CAP.SAM).
static void enable_ahci(struct ahci_controller *c) {
    if (!(c->cap & CAP_SAM)) hba_write(c, HBA_GHC, hba_read(c, HBA_GHC) | GHC_AE);
}

static const char *signature_name(uint32_t sig) {
    switch (sig) {
    case SIG_ATAPI: return "an ATAPI device (CD/DVD)";
    case SIG_SEMB: return "an enclosure management bridge";
    case SIG_PM: return "a port multiplier";
    default: return "a device it doesn't know";
    }
}

static bool ahci_probe(struct pci_dev *d) {
    if (controller_count == MAX_CONTROLLERS) return false;
    volatile uint32_t *abar = pci_map_bar(d, 5);
    if (abar == NULL) {
        kprintf(ANSI_YELLOW "  [warn] AHCI %02x:%02x.%x: BAR 5 is not a mapped memory BAR" ANSI_RESET "\n", d->bus,
                d->dev, d->func);
        return false;
    }
    struct ahci_controller *c = kcalloc(1, sizeof *c);
    if (c == NULL) return false;
    c->pci = d;
    c->abar = abar;
    c->vector = -1;
    pci_enable(d);

    bios_handoff(c);

    // Reset the HBA. CAP and PI are written by the firmware on some
    // chipsets and lost in the reset, so they are put back (as Linux does).
    c->cap = hba_read(c, HBA_CAP);
    uint32_t pi = hba_read(c, HBA_PI);
    enable_ahci(c);
    hba_write(c, HBA_GHC, hba_read(c, HBA_GHC) | GHC_HR);
    if (!wait_reg(&c->abar[HBA_GHC / 4], GHC_HR, 0, 1000)) {
        warn(c, "the HBA did not come out of reset");
        kfree(c);
        return false;
    }
    enable_ahci(c);
    hba_write(c, HBA_CAP, c->cap);
    hba_write(c, HBA_PI, pi);
    c->cap = hba_read(c, HBA_CAP);
    if (hba_read(c, HBA_PI)) pi = hba_read(c, HBA_PI);
    controllers[controller_count++] = c;

    uint64_t limit = c->cap & CAP_S64A ? UINT64_MAX : (uint64_t)1 << 32;
    unsigned ports = 0;
    for (unsigned n = 0; n < MAX_PORTS; n++) {
        if (!(pi & (1u << n))) continue;
        struct ahci_port *p = kcalloc(1, sizeof *p);
        uint64_t dma = pmm_alloc_contiguous_below(1 + DATA_PAGES, limit);
        if (p == NULL || dma == 0) {
            warn(c, "port %u: no memory", n);
            kfree(p);
            if (dma) pmm_free_contiguous(dma, 1 + DATA_PAGES);
            continue;
        }
        p->hba = c;
        p->index = n;
        p->regs = c->abar + (0x100 + n * 0x80) / 4;
        p->dma_phys = dma;
        p->data_phys = dma + PAGE_SIZE;
        p->data = phys_to_virt(p->data_phys);
        memset(phys_to_virt(dma), 0, PAGE_SIZE);
        p->cl = phys_to_virt(dma + DMA_CL_OFFSET);
        p->table = phys_to_virt(dma + DMA_TABLE_OFFSET);
        p->cl[0].ctba = (uint32_t)(dma + DMA_TABLE_OFFSET);
        p->cl[0].ctbau = (uint32_t)((dma + DMA_TABLE_OFFSET) >> 32);
        mutex_init(&p->lock, "ahci port");
        spin_init(&p->irq_lock, "ahci port irq");

        if (!port_stop(p)) warn(c, "port %u: the command engine did not stop", n);
        port_write(p, PX_CLB, (uint32_t)(dma + DMA_CL_OFFSET));
        port_write(p, PX_CLBU, (uint32_t)((dma + DMA_CL_OFFSET) >> 32));
        port_write(p, PX_FB, (uint32_t)(dma + DMA_FIS_OFFSET));
        port_write(p, PX_FBU, (uint32_t)((dma + DMA_FIS_OFFSET) >> 32));
        port_write(p, PX_SERR, ~0u);
        port_write(p, PX_IS, ~0u);
        port_write(p, PX_IE, 0);
        port_write(p, PX_CMD, port_read(p, PX_CMD) | PXCMD_FRE | PXCMD_POD | (c->cap & CAP_SSS ? PXCMD_SUD : 0));
        // COMRESET: DET = 1 for at least 1 ms, then 0.
        port_write(p, PX_SCTL, SCTL_IPM_NO_SLEEP | SCTL_DET_COMRESET);
        c->ports[n] = p;
        ports++;
    }
    thread_sleep(2);
    for (unsigned n = 0; n < MAX_PORTS; n++)
        if (c->ports[n]) port_write(c->ports[n], PX_SCTL, SCTL_IPM_NO_SLEEP);

    // Wait for the links, every port at once: a port with nothing seen on it
    // after LINK_DETECT_MS is empty.
    uint64_t t0 = clock_monotonic_ns();
    for (;;) {
        bool waiting = false;
        uint64_t elapsed = clock_monotonic_ns() - t0;
        for (unsigned n = 0; n < MAX_PORTS; n++) {
            if (c->ports[n] == NULL) continue;
            uint32_t det = SSTS_DET(port_read(c->ports[n], PX_SSTS));
            if (det != DET_PHY && (det != DET_NONE || elapsed < LINK_DETECT_MS * NS_PER_MS)) waiting = true;
        }
        if (!waiting || elapsed > LINK_UP_MS * NS_PER_MS) break;
        thread_sleep(1);
    }
    // Then for the devices to be ready (BSY and DRQ clear).
    t0 = clock_monotonic_ns();
    for (;;) {
        bool waiting = false;
        for (unsigned n = 0; n < MAX_PORTS; n++) {
            struct ahci_port *p = c->ports[n];
            if (p && SSTS_DET(port_read(p, PX_SSTS)) == DET_PHY) {
                port_write(p, PX_SERR, ~0u); // the link coming up set some
                if (port_read(p, PX_TFD) & (TFD_BSY | TFD_DRQ)) waiting = true;
            }
        }
        if (!waiting || clock_monotonic_ns() - t0 > READY_MS * NS_PER_MS) break;
        thread_sleep(1);
    }

    int vector = pci_enable_msi(d, ahci_interrupt);
    c->use_irq = vector >= 0;
    c->vector = vector;
    hba_write(c, HBA_IS, ~0u);
    if (c->use_irq) hba_write(c, HBA_GHC, hba_read(c, HBA_GHC) | GHC_IE);

    // VS: major in bits 31:16, minor in 15:8, and a third digit in 7:0 (1.3.1).
    uint32_t vs = hba_read(c, HBA_VS);
    kprintf("  AHCI %02x:%02x.%x: version %u.%u", d->bus, d->dev, d->func, vs >> 16, vs >> 8 & 0xff);
    if (vs & 0xff) kprintf(".%u", vs & 0xff);
    kprintf(", %u ports, %u command slots, %s DMA, ", ports, (c->cap >> 8 & 0x1f) + 1,
            c->cap & CAP_S64A ? "64-bit" : "32-bit");
    if (c->use_irq) kprintf("MSI on vector 0x%x\n", vector);
    else kprintf("polled (no MSI)\n");

    for (unsigned n = 0; n < MAX_PORTS; n++) {
        struct ahci_port *p = c->ports[n];
        if (p == NULL || SSTS_DET(port_read(p, PX_SSTS)) != DET_PHY) continue;
        uint32_t sig = port_read(p, PX_SIG);
        if (port_read(p, PX_TFD) & (TFD_BSY | TFD_DRQ)) {
            warn(c, "port %u: the device stayed busy", n);
            continue;
        }
        port_write(p, PX_IS, ~0u);
        port_write(p, PX_IE, c->use_irq ? PXIS_WANTED : 0);
        port_start(p);
        if (sig != SIG_ATA) {
            kprintf("  AHCI %02x:%02x.%x: port %u has %s (signature %08x), not used\n", d->bus, d->dev, d->func, n,
                    signature_name(sig), sig);
            continue;
        }
        port_identify(p);
    }
    return true;
}

static const struct pci_id ahci_ids[] = {
    {PCI_ANY_ID, PCI_ANY_ID, PCI_CLASS(0x01, 0x06, 0x01), PCI_CLASS_MASK_ALL},
    {0},
};
PCI_DRIVER(ahci, ahci_ids, ahci_probe);
