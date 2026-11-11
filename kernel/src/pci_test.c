// PCI self-test: drivers for two of QEMU's devices that check, end to end,
// what pci.c sets up. `make test` gives QEMU both (scripts/boot-test.sh).
//
//   edu (1234:11e8), QEMU's teaching device: its BAR 0 is mapped and read
//   (an ID register, and a register that returns what is written to it,
//   inverted), then it is told to raise an interrupt, which must arrive as
//   an MSI.
//
//   igb (8086:10c9), QEMU's Intel 82576 NIC, the only QEMU device whose
//   MSI-X vectors the guest can fire directly: in MSI-X mode, writing bit n
//   of its Extended Interrupt Cause Set register raises vector n. A real
//   82576 is left alone: only QEMU's carries Red Hat's subsystem ID.
//
// pci_self_test() also checks driver matching against made-up devices.

#include <errno.h>
#include <kprintf.h>
#include <pci.h>
#include <string.h>
#include <time.h>

#define WAIT_MS 200

#define QEMU_SUBSYS_VENDOR 0x1af4 // Red Hat, Inc.
#define QEMU_SUBSYS_ID     0x1100

static volatile uint64_t msi_count, msix_count;
static int msi_vector = -1, msix_vector = -1;
static char edu_error[64], igb_error[64];

// Wait (with interrupts on) for `*count` to become non-zero.
static bool wait_for(volatile uint64_t *count) {
    uint64_t deadline = clock_monotonic_ns() + WAIT_MS * NS_PER_MS;
    while (*count == 0 && clock_monotonic_ns() < deadline) __asm__ volatile("pause");
    return *count != 0;
}

// ---------------------------------------------------------------------------
// edu: MMIO and MSI
// ---------------------------------------------------------------------------

#define EDU_ID         0x00 // 0xRRrr00ed: major.minor version, then 0xed
#define EDU_LIVENESS   0x04 // reads back the bitwise NOT of what was written
#define EDU_IRQ_STATUS 0x24
#define EDU_IRQ_RAISE  0x60 // OR the value into the status and raise the interrupt
#define EDU_IRQ_ACK    0x64 // clear these status bits

static volatile uint32_t *edu_regs;

static void edu_interrupt(struct interrupt_frame *f) {
    uint32_t status = edu_regs[EDU_IRQ_STATUS / 4];
    edu_regs[EDU_IRQ_ACK / 4] = status;
    msi_count++;
}

static bool edu_probe(struct pci_dev *d) {
    edu_regs = pci_map_bar(d, 0);
    if (edu_regs == NULL) {
        ksnprintf(edu_error, sizeof edu_error, "edu: BAR 0 is not a mapped memory BAR");
        return false;
    }
    pci_enable(d);
    uint32_t id = edu_regs[EDU_ID / 4];
    edu_regs[EDU_LIVENESS / 4] = 0x12345678;
    uint32_t live = edu_regs[EDU_LIVENESS / 4];
    if ((id & 0xff) != 0xed || live != ~0x12345678u) {
        ksnprintf(edu_error, sizeof edu_error, "edu: BAR 0 reads id %x, liveness %x", id, live);
        return false;
    }
    msi_vector = pci_enable_msi(d, edu_interrupt);
    if (msi_vector < 0) {
        ksnprintf(edu_error, sizeof edu_error, "edu: no MSI: %s", strerror(-msi_vector));
        return false;
    }
    edu_regs[EDU_IRQ_RAISE / 4] = 1;
    if (!wait_for(&msi_count)) ksnprintf(edu_error, sizeof edu_error, "edu: no MSI on vector 0x%x", msi_vector);
    return true;
}

static const struct pci_id edu_ids[] = {
    {0x1234, 0x11e8, 0, 0},
    {0},
};
PCI_DRIVER(edu, edu_ids, edu_probe);

// ---------------------------------------------------------------------------
// igb: MSI-X
// ---------------------------------------------------------------------------

#define IGB_GPIE 0x1514 // General Purpose Interrupt Enable
#define IGB_EICS 0x1520 // Extended Interrupt Cause Set
#define IGB_EIMS 0x1524 // Extended Interrupt Mask Set
#define IGB_EIMC 0x1528 // Extended Interrupt Mask Clear
#define IGB_EICR 0x1580 // Extended Interrupt Cause Read (reading clears it)

#define GPIE_MSIX_MODE (1u << 4)

static volatile uint32_t *igb_regs;

static void igb_interrupt(struct interrupt_frame *f) {
    (void)igb_regs[IGB_EICR / 4];
    msix_count++;
}

static bool igb_probe(struct pci_dev *d) {
    if (d->subsys_vendor != QEMU_SUBSYS_VENDOR || d->subsys_id != QEMU_SUBSYS_ID) return false;
    igb_regs = pci_map_bar(d, 0);
    if (igb_regs == NULL) {
        ksnprintf(igb_error, sizeof igb_error, "igb: BAR 0 is not a mapped memory BAR");
        return false;
    }
    pci_enable(d);
    msix_vector = pci_enable_msix(d, 0, igb_interrupt);
    if (msix_vector < 0) {
        ksnprintf(igb_error, sizeof igb_error, "igb: no MSI-X: %s", strerror(-msix_vector));
        return false;
    }
    igb_regs[IGB_GPIE / 4] = GPIE_MSIX_MODE;
    igb_regs[IGB_EIMS / 4] = 1; // vector 0
    igb_regs[IGB_EICS / 4] = 1;
    bool ok = wait_for(&msix_count);
    igb_regs[IGB_EIMC / 4] = ~0u; // nothing else from it
    if (!ok) ksnprintf(igb_error, sizeof igb_error, "igb: no MSI-X on vector 0x%x", msix_vector);
    return true;
}

static const struct pci_id igb_ids[] = {
    {0x8086, 0x10c9, 0, 0},
    {0},
};
PCI_DRIVER(igb_test, igb_ids, igb_probe);

// ---------------------------------------------------------------------------

// Driver match entries against made-up devices: by ID, by full class code,
// by class and subclass.
static bool match_self_test(void) {
    struct pci_dev xhci = {.vendor = 0x8086, .device = 0xa36d, .class_code = 0x0c, .subclass = 0x03, .prog_if = 0x30};
    struct pci_dev ehci = {.vendor = 0x8086, .device = 0xa36d, .class_code = 0x0c, .subclass = 0x03, .prog_if = 0x20};
    const struct pci_id by_id = {0x8086, 0xa36d, 0, 0};
    const struct pci_id other_id = {0x8086, 0xa36e, 0, 0};
    const struct pci_id xhci_class = {PCI_ANY_ID, PCI_ANY_ID, PCI_CLASS(0x0c, 0x03, 0x30), PCI_CLASS_MASK_ALL};
    const struct pci_id usb_class = {PCI_ANY_ID, PCI_ANY_ID, PCI_CLASS(0x0c, 0x03, 0), PCI_CLASS_MASK_SUBCLASS};
    return pci_id_matches(&by_id, &xhci) && !pci_id_matches(&other_id, &xhci) &&
           pci_id_matches(&xhci_class, &xhci) && !pci_id_matches(&xhci_class, &ehci) &&
           pci_id_matches(&usb_class, &xhci) && pci_id_matches(&usb_class, &ehci);
}

bool pci_self_test(char *line, size_t size) {
    if (!match_self_test()) {
        ksnprintf(line, size, "PCI self-test: driver matching is wrong");
        return false;
    }
    if (edu_error[0] || igb_error[0]) {
        ksnprintf(line, size, "PCI self-test: %s%s%s", edu_error, edu_error[0] && igb_error[0] ? "; " : "",
                  igb_error);
        return false;
    }
    int n = ksnprintf(line, size, "PCI self-test passed (driver matching");
    if (msi_count && n >= 0 && (size_t)n < size)
        n += ksnprintf(line + n, size - n, "; edu: BAR mapped, MSI on vector 0x%x", msi_vector);
    if (msix_count && n >= 0 && (size_t)n < size)
        n += ksnprintf(line + n, size - n, "; igb: MSI-X on vector 0x%x", msix_vector);
    if (n >= 0 && (size_t)n < size) ksnprintf(line + n, size - n, ")");
    return true;
}
