#pragma once
// PCI and PCI Express: find every device, read and write its configuration
// space, decode its BARs, route its interrupts as MSI or MSI-X, and hand it
// to a driver.
//
// Configuration space is reached through ECAM, the memory-mapped window the
// ACPI MCFG table describes (4 KiB per function, so the PCIe extended space
// above 256 bytes too), or, on machines without an MCFG, through the legacy
// ports 0xcf8/0xcfc (the first 256 bytes only, segment 0 only).
//
// Enumeration starts at the first bus of each segment and follows PCI-to-PCI
// bridges to the buses behind them, as the firmware numbered them. Root
// buses that only the DSDT knows about (PNP0A08 devices with their own _BBN,
// on some servers) need an AML interpreter and are not found.
//
// Drivers add themselves to a table with PCI_DRIVER, like shell commands:
//
//   static const struct pci_id ahci_ids[] = {
//       {PCI_ANY_ID, PCI_ANY_ID, PCI_CLASS(0x01, 0x06, 0x01), PCI_CLASS_MASK_ALL},
//       {0},
//   };
//   static bool ahci_probe(struct pci_dev *dev) { ... }
//   PCI_DRIVER(ahci, ahci_ids, ahci_probe);
//
// pci_init() offers each device to the drivers in link order; the first
// whose table matches and whose probe returns true gets it.

#include <arch.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Configuration space registers (type 0 and type 1 headers).
#define PCI_VENDOR_ID      0x00
#define PCI_DEVICE_ID      0x02
#define PCI_COMMAND        0x04
#define PCI_STATUS         0x06
#define PCI_REVISION       0x08 // then prog-if, subclass, class
#define PCI_HEADER_TYPE    0x0e
#define PCI_BAR0           0x10
#define PCI_PRIMARY_BUS    0x18 // type 1: then secondary, subordinate
#define PCI_SUBSYSTEM      0x2c // type 0: subsystem vendor, then subsystem ID
#define PCI_CAPABILITIES   0x34
#define PCI_INTERRUPT_LINE 0x3c
#define PCI_INTERRUPT_PIN  0x3d

#define PCI_COMMAND_IO           (1u << 0)
#define PCI_COMMAND_MEMORY       (1u << 1)
#define PCI_COMMAND_MASTER       (1u << 2)
#define PCI_COMMAND_INTX_DISABLE (1u << 10)

#define PCI_STATUS_CAP_LIST (1u << 4)

// Capability IDs.
#define PCI_CAP_PM      0x01
#define PCI_CAP_MSI     0x05
#define PCI_CAP_VENDOR  0x09
#define PCI_CAP_SSVID   0x0d
#define PCI_CAP_EXPRESS 0x10
#define PCI_CAP_MSIX    0x11
#define PCI_CAP_SATA    0x12

#define PCI_MAX_BARS 6
#define PCI_MAX_CAPS 16

struct pci_bar {
    uint64_t base; // physical address, or I/O port; 0 if the firmware left it unassigned
    uint64_t size; // 0: no BAR here (or the upper half of a 64-bit one)
    bool io;
    bool is64;
    bool prefetchable;
    volatile void *virt; // where pci_map_bar() mapped it
};

struct pci_driver;

struct pci_dev {
    uint16_t segment;
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if, revision;
    uint8_t header_type; // 0 device, 1 PCI-to-PCI bridge, 2 CardBus bridge
    bool multifunction;
    uint16_t subsys_vendor, subsys_id; // type 0 only
    uint8_t irq_pin;     // 1-4 for INTA-INTD, 0 for none
    uint8_t irq_line;    // what the firmware wrote there (a PIC IRQ, informational)
    struct pci_bar bars[PCI_MAX_BARS];
    uint8_t primary_bus, secondary_bus, subordinate_bus; // bridges only

    // The capability list, in order: IDs and their offsets.
    uint8_t cap_count;
    uint8_t cap_id[PCI_MAX_CAPS], cap_offset[PCI_MAX_CAPS];
    uint8_t msi_cap, msix_cap, express_cap; // offsets, 0 if absent

    const struct pci_driver *driver;
};

// A driver's match table entry. A device matches when its vendor and device
// IDs equal these (or they are PCI_ANY_ID) and its 24-bit class code
// (class << 16 | subclass << 8 | prog-if) equals `class_code` in the bits set
// in `class_mask`. A table ends with an entry whose vendor is 0.
struct pci_id {
    uint16_t vendor, device;
    uint32_t class_code, class_mask;
};

#define PCI_ANY_ID 0xffff
#define PCI_CLASS(c, s, p) ((uint32_t)(c) << 16 | (uint32_t)(s) << 8 | (p))
#define PCI_CLASS_MASK_ALL      0xffffffu // class, subclass and prog-if
#define PCI_CLASS_MASK_SUBCLASS 0xffff00u // class and subclass, any prog-if

struct pci_driver {
    const char *name;
    const struct pci_id *ids;
    // Take the device: true if the driver now owns it.
    bool (*probe)(struct pci_dev *dev);
};

#define PCI_DRIVER(name_, ids_, probe_)                                                                        \
    __attribute__((used, section(".pci_drivers"), aligned(8))) static const struct pci_driver                  \
        pci_driver_##name_ = {#name_, ids_, probe_}

// Find the configuration space (MCFG, else the legacy ports), enumerate
// every bus, decode the BARs and capabilities, and probe the drivers. Needs
// ACPI, the VMM and the heap; drivers that set up interrupts need them on.
// Writes a one-line summary to `line`.
void pci_init(char *line, size_t size);

// The devices, in the order they were found (bus, then device, function).
size_t pci_device_count(void);
struct pci_dev *pci_device(size_t i);

// Configuration space access. Offsets above 255 need ECAM; without it, reads
// return all ones and writes are dropped. Reads are naturally aligned.
uint8_t pci_read8(const struct pci_dev *d, uint16_t offset);
uint16_t pci_read16(const struct pci_dev *d, uint16_t offset);
uint32_t pci_read32(const struct pci_dev *d, uint16_t offset);
void pci_write16(const struct pci_dev *d, uint16_t offset, uint16_t value);
void pci_write32(const struct pci_dev *d, uint16_t offset, uint32_t value);

// Turn on the command register bits a driver needs: memory and I/O decoding
// for the BARs it has, and bus mastering (DMA, and MSI/MSI-X messages, which
// are memory writes by the device).
void pci_enable(struct pci_dev *d);

// Map a memory BAR uncached (once; later calls return the same address).
// NULL if the BAR is an I/O BAR, absent or unassigned.
volatile void *pci_map_bar(struct pci_dev *d, unsigned bar);

// The offset of the first capability with this ID, or 0.
uint8_t pci_find_cap(const struct pci_dev *d, uint8_t id);

// Route the device's interrupt to `handler` as an MSI (a single message) or
// as MSI-X table entry `entry`, and turn legacy INTx off. Call pci_enable()
// first: the MSI-X table is in a memory BAR, and a message is a memory write
// the device makes as bus master. They return the
// vector, or a negative errno: -ENODEV without the capability, -EINVAL for an
// entry past the table, -EBUSY with no vector left.
int pci_enable_msi(struct pci_dev *d, interrupt_handler_t handler);
int pci_enable_msix(struct pci_dev *d, unsigned entry, interrupt_handler_t handler);

// Whether driver match entry `id` matches the device.
bool pci_id_matches(const struct pci_id *id, const struct pci_dev *d);

// "SATA controller", "Host bridge", ... (the names Linux's lspci uses), or
// NULL for a class it doesn't know.
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

// Boot self-test (pci_test.c): driver matching, and, on QEMU with its edu
// and igb devices, a mapped BAR, an MSI and an MSI-X that arrive. Run after
// pci_init(). False, with the reason in `line`, if something failed.
bool pci_self_test(char *line, size_t size);
