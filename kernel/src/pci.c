// PCI and PCI Express enumeration. See pci.h.

#include <acpi.h>
#include <apic.h>
#include <cpu.h>
#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <memory.h>
#include <pci.h>
#include <shell.h>
#include <string.h>
#include <sync.h>
#include <vmm.h>

// The linker puts every PCI_DRIVER between these (linker.ld).
extern const struct pci_driver pci_drivers_start[], pci_drivers_end[];

// ---------------------------------------------------------------------------
// Configuration space access
// ---------------------------------------------------------------------------

// One ECAM window from the MCFG: bus b, device d, function f's 4 KiB start
// at base + (b << 20 | d << 15 | f << 12). Each bus's 1 MiB is mapped the
// first time it is touched, so only buses that are scanned take room.
struct ecam {
    uint64_t base;
    uint16_t segment;
    uint8_t start_bus, end_bus;
    volatile uint8_t **bus_map; // indexed by bus - start_bus
};

static struct ecam *ecams;
static size_t ecam_count;
static bool legacy_ports; // 0xcf8/0xcfc work

#define LEGACY_ADDRESS 0xcf8
#define LEGACY_DATA    0xcfc
#define LEGACY_ENABLE  0x80000000u

// Guards the two-step legacy access and the lazy ECAM mapping.
static struct spinlock config_lock = SPINLOCK_INIT("pci config");

static struct pci_dev **devices;
static size_t device_count, device_capacity;

// Where (segment, bus, dev, func, offset) is in ECAM, or NULL.
static volatile uint8_t *ecam_address(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset) {
    for (size_t i = 0; i < ecam_count; i++) {
        struct ecam *e = &ecams[i];
        if (e->segment != seg || bus < e->start_bus || bus > e->end_bus) continue;
        volatile uint8_t **map = &e->bus_map[bus - e->start_bus];
        if (*map == NULL) *map = vmm_map_mmio(e->base + ((uint64_t)bus << 20), 1 << 20);
        return *map + ((uint32_t)dev << 15 | (uint32_t)func << 12 | offset);
    }
    return NULL;
}

static uint32_t legacy_address(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset) {
    return LEGACY_ENABLE | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)func << 8 | (offset & 0xfc);
}

// `width` is 2 or 4; `offset` is aligned to it.
static uint32_t config_read(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, int width) {
    uint32_t v = width == 4 ? 0xffffffffu : 0xffffu;
    spin_lock(&config_lock);
    volatile uint8_t *p = ecam_address(seg, bus, dev, func, offset);
    if (p) {
        v = width == 4 ? *(volatile uint32_t *)p : *(volatile uint16_t *)p;
    } else if (legacy_ports && seg == 0 && offset < 256) {
        outl(LEGACY_ADDRESS, legacy_address(bus, dev, func, offset));
        v = width == 4 ? inl(LEGACY_DATA) : inw(LEGACY_DATA + (offset & 2));
    }
    spin_unlock(&config_lock);
    return v;
}

static void config_write(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, int width,
                         uint32_t v) {
    spin_lock(&config_lock);
    volatile uint8_t *p = ecam_address(seg, bus, dev, func, offset);
    if (p) {
        if (width == 4) *(volatile uint32_t *)p = v;
        else *(volatile uint16_t *)p = (uint16_t)v;
    } else if (legacy_ports && seg == 0 && offset < 256) {
        outl(LEGACY_ADDRESS, legacy_address(bus, dev, func, offset));
        if (width == 4) outl(LEGACY_DATA, v);
        else outw(LEGACY_DATA + (offset & 2), (uint16_t)v);
    }
    spin_unlock(&config_lock);
}

uint32_t pci_read32(const struct pci_dev *d, uint16_t offset) {
    return config_read(d->segment, d->bus, d->dev, d->func, offset & ~3, 4);
}

uint16_t pci_read16(const struct pci_dev *d, uint16_t offset) {
    return (uint16_t)config_read(d->segment, d->bus, d->dev, d->func, offset & ~1, 2);
}

uint8_t pci_read8(const struct pci_dev *d, uint16_t offset) {
    return (uint8_t)(pci_read16(d, offset) >> (offset & 1) * 8);
}

void pci_write32(const struct pci_dev *d, uint16_t offset, uint32_t value) {
    config_write(d->segment, d->bus, d->dev, d->func, offset & ~3, 4, value);
}

void pci_write16(const struct pci_dev *d, uint16_t offset, uint16_t value) {
    config_write(d->segment, d->bus, d->dev, d->func, offset & ~1, 2, value);
}

// Whether 0xcf8 holds what is written to it, as Linux checks: the port is
// a register of the host bridge, not just open bus.
static bool probe_legacy_ports(void) {
    uint32_t saved = inl(LEGACY_ADDRESS);
    outl(LEGACY_ADDRESS, LEGACY_ENABLE);
    bool ok = inl(LEGACY_ADDRESS) == LEGACY_ENABLE;
    outl(LEGACY_ADDRESS, saved);
    return ok;
}

// ---------------------------------------------------------------------------
// BARs and capabilities
// ---------------------------------------------------------------------------

#define BAR_IO          (1u << 0)
#define BAR_TYPE_64     (2u << 1)
#define BAR_TYPE_MASK   (3u << 1)
#define BAR_PREFETCH    (1u << 3)
#define BAR_MEM_MASK    (~(uint64_t)0xf)
#define BAR_IO_MASK     (~(uint32_t)0x3)

// Size a BAR the standard way: write all ones, read back which address bits
// stick (the BAR's alignment, and so its size), and put the address back.
// The device stops decoding while that happens, so it doesn't answer at the
// all-ones address meanwhile; host bridges keep decoding, as on Linux, since
// some of them stop answering otherwise.
static void read_bars(struct pci_dev *d) {
    unsigned count = d->header_type == 0 ? 6 : d->header_type == 1 ? 2 : 0;
    if (count == 0) return;
    bool host_bridge = d->class_code == 0x06 && d->subclass == 0x00;
    uint16_t cmd = pci_read16(d, PCI_COMMAND);

    // Nothing else may touch the device (or the screen, if it is the GPU)
    // while it isn't decoding: no interrupts here, no printing elsewhere.
    bool held = kprintf_hold();
    uint64_t flags = read_rflags();
    cli();
    if (!host_bridge) pci_write16(d, PCI_COMMAND, cmd & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY));
    for (unsigned i = 0; i < count; i++) {
        uint16_t off = PCI_BAR0 + i * 4;
        uint32_t lo = pci_read32(d, off);
        pci_write32(d, off, 0xffffffffu);
        uint32_t lo_mask = pci_read32(d, off);
        pci_write32(d, off, lo);
        if (lo_mask == 0 || lo_mask == 0xffffffffu) continue; // not implemented (or nothing answers)

        struct pci_bar *b = &d->bars[i];
        if (lo & BAR_IO) {
            uint32_t mask = lo_mask & BAR_IO_MASK;
            if ((mask & 0xffff0000u) == 0) mask |= 0xffff0000u; // 16-bit decoder: the upper bits read as 0
            b->io = true;
            b->base = lo & BAR_IO_MASK;
            b->size = (uint32_t)(~mask + 1);
            continue;
        }
        uint64_t base = lo & BAR_MEM_MASK, mask = (uint64_t)lo_mask | 0xffffffff00000000ull;
        b->prefetchable = lo & BAR_PREFETCH;
        if ((lo & BAR_TYPE_MASK) == BAR_TYPE_64 && i + 1 < count) {
            uint32_t hi = pci_read32(d, off + 4);
            pci_write32(d, off + 4, 0xffffffffu);
            uint32_t hi_mask = pci_read32(d, off + 4);
            pci_write32(d, off + 4, hi);
            b->is64 = true;
            base |= (uint64_t)hi << 32;
            mask = (uint64_t)hi_mask << 32 | lo_mask;
            i++; // the next BAR is this one's upper half
        }
        b->base = base;
        b->size = ~(mask & BAR_MEM_MASK) + 1;
    }
    if (!host_bridge) pci_write16(d, PCI_COMMAND, cmd);
    if (flags & RFLAGS_IF) sti();
    kprintf_release(held);
}

// Walk the capability list. A broken list (a loop, or an offset into the
// header) ends the walk; 48 is how many fit in the 192 bytes after it.
static void read_caps(struct pci_dev *d) {
    if (!(pci_read16(d, PCI_STATUS) & PCI_STATUS_CAP_LIST)) return;
    uint8_t off = pci_read8(d, PCI_CAPABILITIES) & 0xfc;
    for (int n = 0; off >= 0x40 && n < 48; n++) {
        uint16_t head = pci_read16(d, off); // ID, then the next pointer
        uint8_t id = head & 0xff;
        if (id == 0xff) break; // the function vanished
        if (d->cap_count < PCI_MAX_CAPS) {
            d->cap_id[d->cap_count] = id;
            d->cap_offset[d->cap_count++] = off;
        }
        if (id == PCI_CAP_MSI && !d->msi_cap) d->msi_cap = off;
        if (id == PCI_CAP_MSIX && !d->msix_cap) d->msix_cap = off;
        if (id == PCI_CAP_EXPRESS && !d->express_cap) d->express_cap = off;
        off = (head >> 8) & 0xfc;
    }
}

uint8_t pci_find_cap(const struct pci_dev *d, uint8_t id) {
    for (unsigned i = 0; i < d->cap_count; i++)
        if (d->cap_id[i] == id) return d->cap_offset[i];
    return 0;
}

void pci_enable(struct pci_dev *d) {
    uint16_t cmd = pci_read16(d, PCI_COMMAND) | PCI_COMMAND_MASTER;
    for (unsigned i = 0; i < PCI_MAX_BARS; i++)
        if (d->bars[i].size) cmd |= d->bars[i].io ? PCI_COMMAND_IO : PCI_COMMAND_MEMORY;
    pci_write16(d, PCI_COMMAND, cmd);
}

volatile void *pci_map_bar(struct pci_dev *d, unsigned bar) {
    if (bar >= PCI_MAX_BARS) return NULL;
    struct pci_bar *b = &d->bars[bar];
    if (b->io || b->size == 0 || b->base == 0) return NULL;
    if (b->virt == NULL) b->virt = vmm_map_mmio(b->base, b->size);
    return b->virt;
}

// ---------------------------------------------------------------------------
// MSI and MSI-X
// ---------------------------------------------------------------------------

// MSI capability: message control at +2, then the address (+4, and +8 for
// the upper half when the function can do 64-bit addresses) and the data.
#define MSI_CONTROL       2
#define MSI_ADDRESS       4
#define MSI_CTRL_ENABLE   (1u << 0)
#define MSI_CTRL_MME      (7u << 4) // multiple message enable: log2 of the vectors in use
#define MSI_CTRL_64BIT    (1u << 7)
#define MSI_CTRL_MASKABLE (1u << 8) // per-vector mask bits after the data

// MSI-X capability: message control at +2, then the table's BAR (bits 0-2)
// and offset, and the pending bit array's. Each table entry is 16 bytes:
// address low, address high, data, vector control (bit 0: masked).
#define MSIX_CONTROL       2
#define MSIX_TABLE         4
#define MSIX_CTRL_SIZE     0x7ffu // table size - 1
#define MSIX_CTRL_MASK_ALL (1u << 14)
#define MSIX_CTRL_ENABLE   (1u << 15)
#define MSIX_ENTRY_SIZE    16
#define MSIX_VECTOR_MASKED (1u << 0)

static void disable_intx(struct pci_dev *d) {
    pci_write16(d, PCI_COMMAND, pci_read16(d, PCI_COMMAND) | PCI_COMMAND_INTX_DISABLE);
}

int pci_enable_msi(struct pci_dev *d, interrupt_handler_t handler) {
    uint8_t cap = d->msi_cap;
    if (cap == 0) return -ENODEV;
    struct msi_message msg;
    if (!msi_vector_alloc(handler, &msg)) return -EBUSY;

    uint16_t ctrl = pci_read16(d, cap + MSI_CONTROL);
    ctrl &= ~(MSI_CTRL_ENABLE | MSI_CTRL_MME); // one message
    pci_write16(d, cap + MSI_CONTROL, ctrl);
    pci_write32(d, cap + MSI_ADDRESS, (uint32_t)msg.address);
    uint16_t data = cap + MSI_ADDRESS + 4;
    if (ctrl & MSI_CTRL_64BIT) {
        pci_write32(d, cap + MSI_ADDRESS + 4, (uint32_t)(msg.address >> 32));
        data += 4;
    }
    pci_write16(d, data, (uint16_t)msg.data);
    if (ctrl & MSI_CTRL_MASKABLE) pci_write32(d, data + 4, 0); // unmask
    disable_intx(d);
    pci_write16(d, cap + MSI_CONTROL, ctrl | MSI_CTRL_ENABLE);
    return msg.vector;
}

int pci_enable_msix(struct pci_dev *d, unsigned entry, interrupt_handler_t handler) {
    uint8_t cap = d->msix_cap;
    if (cap == 0) return -ENODEV;
    uint16_t ctrl = pci_read16(d, cap + MSIX_CONTROL);
    if (entry > (ctrl & MSIX_CTRL_SIZE)) return -EINVAL;
    uint32_t table = pci_read32(d, cap + MSIX_TABLE);
    volatile uint8_t *bar = pci_map_bar(d, table & 7);
    if (bar == NULL) return -ENODEV;
    struct msi_message msg;
    if (!msi_vector_alloc(handler, &msg)) return -EBUSY;

    // Enable MSI-X with every vector masked, fill in the entry, then unmask.
    pci_write16(d, cap + MSIX_CONTROL, ctrl | MSIX_CTRL_ENABLE | MSIX_CTRL_MASK_ALL);
    volatile uint32_t *e = (volatile uint32_t *)(bar + (table & ~7u) + entry * MSIX_ENTRY_SIZE);
    e[3] |= MSIX_VECTOR_MASKED;
    e[0] = (uint32_t)msg.address;
    e[1] = (uint32_t)(msg.address >> 32);
    e[2] = msg.data;
    e[3] &= ~MSIX_VECTOR_MASKED;
    disable_intx(d);
    pci_write16(d, cap + MSIX_CONTROL, (ctrl | MSIX_CTRL_ENABLE) & ~MSIX_CTRL_MASK_ALL);
    return msg.vector;
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

static void add_device(struct pci_dev *d) {
    if (device_count == device_capacity) {
        size_t cap = device_capacity ? device_capacity * 2 : 32;
        struct pci_dev **n = krealloc(devices, cap * sizeof *n);
        if (n == NULL) panic("pci: out of memory");
        devices = n;
        device_capacity = cap;
    }
    devices[device_count++] = d;
}

static void scan_bus(uint16_t seg, uint8_t bus, uint8_t *visited);

static void scan_function(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t func, uint8_t *visited) {
    struct pci_dev probe = {.segment = seg, .bus = bus, .dev = dev, .func = func};
    uint32_t id = pci_read32(&probe, PCI_VENDOR_ID);
    if ((id & 0xffff) == 0xffff) return;

    struct pci_dev *d = kcalloc(1, sizeof *d);
    if (d == NULL) panic("pci: out of memory");
    *d = probe;
    d->vendor = id & 0xffff;
    d->device = id >> 16;
    uint32_t class = pci_read32(d, PCI_REVISION);
    d->revision = class & 0xff;
    d->prog_if = class >> 8 & 0xff;
    d->subclass = class >> 16 & 0xff;
    d->class_code = class >> 24;
    uint8_t header = pci_read8(d, PCI_HEADER_TYPE);
    d->header_type = header & 0x7f;
    d->multifunction = header & 0x80;
    uint16_t irq = pci_read16(d, PCI_INTERRUPT_LINE);
    d->irq_line = irq & 0xff;
    d->irq_pin = irq >> 8;
    if (d->header_type == 0) {
        uint32_t sub = pci_read32(d, PCI_SUBSYSTEM);
        d->subsys_vendor = sub & 0xffff;
        d->subsys_id = sub >> 16;
    }
    read_bars(d);
    read_caps(d);
    add_device(d);

    // A bridge: the buses behind it, as the firmware numbered them. Only
    // buses after this one, and each once, so a misprogrammed bridge can't
    // send the scan round in circles.
    if (d->header_type == 1 || d->header_type == 2) {
        uint32_t buses = pci_read32(d, PCI_PRIMARY_BUS);
        d->primary_bus = buses & 0xff;
        d->secondary_bus = buses >> 8 & 0xff;
        d->subordinate_bus = buses >> 16 & 0xff;
        if (d->header_type == 1 && d->secondary_bus > bus) scan_bus(seg, d->secondary_bus, visited);
    }
}

static void scan_bus(uint16_t seg, uint8_t bus, uint8_t *visited) {
    if (visited[bus / 8] & 1 << bus % 8) return;
    visited[bus / 8] |= 1 << bus % 8;
    for (uint8_t dev = 0; dev < 32; dev++) {
        struct pci_dev f0 = {.segment = seg, .bus = bus, .dev = dev};
        if (pci_read16(&f0, PCI_VENDOR_ID) == 0xffff) continue;
        scan_function(seg, bus, dev, 0, visited);
        if (!(pci_read8(&f0, PCI_HEADER_TYPE) & 0x80)) continue; // single function
        for (uint8_t func = 1; func < 8; func++) scan_function(seg, bus, dev, func, visited);
    }
}

bool pci_id_matches(const struct pci_id *id, const struct pci_dev *d) {
    uint32_t class = PCI_CLASS(d->class_code, d->subclass, d->prog_if);
    return (id->vendor == PCI_ANY_ID || id->vendor == d->vendor) &&
           (id->device == PCI_ANY_ID || id->device == d->device) && ((class ^ id->class_code) & id->class_mask) == 0;
}

static void probe_drivers(struct pci_dev *d) {
    for (const struct pci_driver *drv = pci_drivers_start; drv < pci_drivers_end; drv++)
        for (const struct pci_id *id = drv->ids; id->vendor; id++)
            if (pci_id_matches(id, d)) {
                if (drv->probe(d)) {
                    d->driver = drv;
                    return;
                }
                break; // this driver said no; try the next one
            }
}

size_t pci_device_count(void) { return device_count; }
struct pci_dev *pci_device(size_t i) { return i < device_count ? devices[i] : NULL; }

void pci_init(char *line, size_t size) {
    const struct acpi_info *acpi = acpi_get_info();
    ecam_count = acpi->mcfg_count;
    if (ecam_count) {
        ecams = kcalloc(ecam_count, sizeof *ecams);
        if (ecams == NULL) panic("pci: out of memory");
    }
    for (size_t i = 0; i < ecam_count; i++) {
        const struct acpi_mcfg_entry *m = &acpi->mcfg[i];
        ecams[i] = (struct ecam){.base = m->base, .segment = m->segment, .start_bus = m->start_bus,
                                 .end_bus = m->end_bus};
        ecams[i].bus_map = kcalloc(m->end_bus - m->start_bus + 1, sizeof *ecams[i].bus_map);
        if (ecams[i].bus_map == NULL) panic("pci: out of memory");
    }
    legacy_ports = probe_legacy_ports();

    // Each segment from its first bus; segment 0 from bus 0 when there is no
    // ECAM for it (the legacy ports).
    uint8_t visited[32];
    bool seg0 = false;
    for (size_t i = 0; i < ecam_count; i++) {
        bool seen = false;
        for (size_t j = 0; j < i; j++) seen |= ecams[j].segment == ecams[i].segment;
        if (seen) continue;
        memset(visited, 0, sizeof visited);
        scan_bus(ecams[i].segment, ecams[i].start_bus, visited);
        seg0 |= ecams[i].segment == 0;
    }
    if (!seg0 && legacy_ports) {
        memset(visited, 0, sizeof visited);
        scan_bus(0, 0, visited);
    }

    // In address order, as lspci lists them (the scan went depth first).
    for (size_t i = 1; i < device_count; i++) {
        struct pci_dev *d = devices[i];
        uint64_t key = (uint64_t)d->segment << 16 | d->bus << 8 | d->dev << 3 | d->func;
        size_t j = i;
        for (; j > 0; j--) {
            struct pci_dev *p = devices[j - 1];
            if (((uint64_t)p->segment << 16 | p->bus << 8 | p->dev << 3 | p->func) < key) break;
            devices[j] = p;
        }
        devices[j] = d;
    }

    size_t buses = 0;
    for (size_t i = 0; i < device_count; i++)
        buses += i == 0 || devices[i]->bus != devices[i - 1]->bus || devices[i]->segment != devices[i - 1]->segment;
    for (size_t i = 0; i < device_count; i++) probe_drivers(devices[i]);

    if (ecam_count)
        ksnprintf(line, size, "PCI: %lu devices on %lu bus%s, config space through ECAM at 0x%lx", device_count,
                  buses, buses == 1 ? "" : "es", ecams[0].base);
    else if (legacy_ports)
        ksnprintf(line, size, "PCI: %lu devices on %lu bus%s, config space through ports 0xcf8/0xcfc (no MCFG)",
                  device_count, buses, buses == 1 ? "" : "es");
    else
        ksnprintf(line, size, "PCI: no configuration space (no MCFG, and ports 0xcf8/0xcfc don't answer)");
}

// ---------------------------------------------------------------------------
// Names, and lspci
// ---------------------------------------------------------------------------

const char *pci_class_name(uint8_t class_code, uint8_t subclass) {
    static const struct {
        uint16_t code; // class << 8 | subclass; 0x..ff: the class with any subclass
        const char *name;
    } names[] = {
        {0x0000, "Non-VGA unclassified device"}, {0x0001, "VGA compatible unclassified device"},
        {0x0100, "SCSI storage controller"}, {0x0101, "IDE interface"}, {0x0102, "Floppy disk controller"},
        {0x0104, "RAID bus controller"}, {0x0105, "ATA controller"}, {0x0106, "SATA controller"},
        {0x0107, "Serial Attached SCSI controller"}, {0x0108, "Non-Volatile memory controller"},
        {0x01ff, "Mass storage controller"},
        {0x0200, "Ethernet controller"}, {0x02ff, "Network controller"},
        {0x0300, "VGA compatible controller"}, {0x0301, "XGA compatible controller"}, {0x0302, "3D controller"},
        {0x03ff, "Display controller"},
        {0x0400, "Multimedia video controller"}, {0x0401, "Multimedia audio controller"},
        {0x0403, "Audio device"}, {0x04ff, "Multimedia controller"},
        {0x0500, "RAM memory"}, {0x0501, "FLASH memory"}, {0x05ff, "Memory controller"},
        {0x0600, "Host bridge"}, {0x0601, "ISA bridge"}, {0x0602, "EISA bridge"}, {0x0604, "PCI bridge"},
        {0x0607, "CardBus bridge"}, {0x06ff, "Bridge"},
        {0x0700, "Serial controller"}, {0x0701, "Parallel controller"}, {0x0703, "Modem"},
        {0x0780, "Communication controller"}, {0x07ff, "Communication controller"},
        {0x0800, "PIC"}, {0x0801, "DMA controller"}, {0x0802, "Timer"}, {0x0803, "RTC"},
        {0x0805, "SD Host controller"}, {0x0806, "IOMMU"}, {0x0880, "System peripheral"},
        {0x08ff, "Generic system peripheral"},
        {0x0900, "Keyboard controller"}, {0x0902, "Mouse controller"}, {0x09ff, "Input device controller"},
        {0x0aff, "Docking station"}, {0x0bff, "Processor"},
        {0x0c00, "FireWire (IEEE 1394)"}, {0x0c03, "USB controller"}, {0x0c05, "SMBus"},
        {0x0c80, "Serial bus controller"}, {0x0cff, "Serial bus controller"},
        {0x0d11, "Bluetooth"}, {0x0dff, "Wireless controller"},
        {0x0eff, "Intelligent controller"}, {0x0fff, "Satellite communications controller"},
        {0x10ff, "Encryption controller"}, {0x1180, "Signal processing controller"},
        {0x11ff, "Signal processing controller"}, {0x12ff, "Processing accelerators"},
        {0x13ff, "Non-Essential Instrumentation"}, {0xffff, "Unassigned class"},
    };
    uint16_t code = class_code << 8 | subclass;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].code == code) return names[i].name;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].code == (class_code << 8 | 0xff)) return names[i].name;
    return NULL;
}

static const char *cap_name(uint8_t id) {
    switch (id) {
    case PCI_CAP_PM: return "Power Management";
    case 0x03: return "Vital Product Data";
    case PCI_CAP_MSI: return "MSI";
    case PCI_CAP_VENDOR: return "Vendor Specific";
    case 0x0a: return "Debug port";
    case PCI_CAP_SSVID: return "Subsystem";
    case PCI_CAP_EXPRESS: return "Express";
    case PCI_CAP_MSIX: return "MSI-X";
    case PCI_CAP_SATA: return "SATA HBA";
    case 0x13: return "PCI Advanced Features";
    case 0x14: return "Enhanced Allocation";
    default: return "?";
    }
}

static bool has_domains(void) {
    for (size_t i = 0; i < device_count; i++)
        if (devices[i]->segment) return true;
    return false;
}

// "00:1f.2", or "0000:00:1f.2" when there is more than segment 0, as lspci.
static void print_address(const struct pci_dev *d, bool domains) {
    if (domains) kprintf("%04x:", d->segment);
    kprintf("%02x:%02x.%x", d->bus, d->dev, d->func);
}

// "4K", "16M", "32" (bytes), as lspci prints sizes.
static void print_size(uint64_t size) {
    static const char units[] = " KMGT";
    unsigned u = 0;
    while (size >= 1024 && size % 1024 == 0 && u < 4) {
        size /= 1024;
        u++;
    }
    if (u) kprintf("%lu%c", size, units[u]);
    else kprintf("%lu", size);
}

static void print_verbose(struct pci_dev *d) {
    if (d->header_type == 0 && (d->subsys_vendor || d->subsys_id))
        kprintf("\tSubsystem: %04x:%04x\n", d->subsys_vendor, d->subsys_id);
    uint16_t cmd = pci_read16(d, PCI_COMMAND);
    kprintf("\tFlags:%s", cmd & PCI_COMMAND_MASTER ? " bus master," : "");
    if (d->irq_pin) kprintf(" INT%c, IRQ %u", 'A' + d->irq_pin - 1, d->irq_line);
    else kprintf(" no INTx");
    kprintf("\n");
    for (unsigned i = 0; i < PCI_MAX_BARS; i++) {
        struct pci_bar *b = &d->bars[i];
        if (b->size == 0) continue;
        if (b->io) kprintf("\tI/O ports at %04lx [size=", b->base);
        else kprintf("\tMemory at %08lx (%s, %sprefetchable) [size=", b->base, b->is64 ? "64-bit" : "32-bit",
                     b->prefetchable ? "" : "non-");
        print_size(b->size);
        kprintf("]%s\n", b->base ? "" : " [unassigned]");
    }
    if (d->header_type == 1 || d->header_type == 2)
        kprintf("\tBus: primary=%02x, secondary=%02x, subordinate=%02x\n", d->primary_bus, d->secondary_bus,
                d->subordinate_bus);
    for (unsigned i = 0; i < d->cap_count; i++) {
        uint8_t off = d->cap_offset[i];
        kprintf("\tCapabilities: [%02x] %s", off, cap_name(d->cap_id[i]));
        if (d->cap_id[i] == PCI_CAP_MSI) {
            uint16_t ctrl = pci_read16(d, off + MSI_CONTROL);
            kprintf(": Enable%c Count=%u/%u 64bit%c", ctrl & MSI_CTRL_ENABLE ? '+' : '-', 1u << (ctrl >> 4 & 7),
                    1u << (ctrl >> 1 & 7), ctrl & MSI_CTRL_64BIT ? '+' : '-');
        } else if (d->cap_id[i] == PCI_CAP_MSIX) {
            uint16_t ctrl = pci_read16(d, off + MSIX_CONTROL);
            kprintf(": Enable%c Count=%u Masked%c", ctrl & MSIX_CTRL_ENABLE ? '+' : '-', (ctrl & MSIX_CTRL_SIZE) + 1,
                    ctrl & MSIX_CTRL_MASK_ALL ? '+' : '-');
        } else if (d->cap_id[i] == PCI_CAP_EXPRESS) {
            static const char *const types[] = {"Endpoint", "Legacy Endpoint", "?", "?", "Root Port",
                                                "Upstream Port", "Downstream Port", "PCI-Express to PCI/PCI-X Bridge",
                                                "PCI/PCI-X to PCI-Express Bridge", "Root Complex Integrated Endpoint",
                                                "Root Complex Event Collector"};
            uint8_t type = pci_read16(d, off + 2) >> 4 & 0xf;
            kprintf(" %s", type < sizeof types / sizeof types[0] ? types[type] : "?");
        }
        kprintf("\n");
    }
    if (d->driver) kprintf("\tKernel driver in use: %s\n", d->driver->name);
    kprintf("\n");
}

SHELL_COMMAND(lspci, "[-n|-v]", "list PCI devices (-n: numbers only, as Linux's lspci -n; -v: details)");
static void cmd_lspci(int argc, char **argv) {
    bool numeric = false, verbose = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) numeric = true;
        else if (strcmp(argv[i], "-v") == 0) verbose = true;
        else {
            kprintf("usage: lspci [-n|-v]\n");
            return;
        }
    }
    if (device_count == 0) {
        kprintf("lspci: no PCI devices found\n");
        return;
    }
    bool domains = has_domains();
    for (size_t i = 0; i < device_count; i++) {
        struct pci_dev *d = devices[i];
        print_address(d, domains);
        const char *name = pci_class_name(d->class_code, d->subclass);
        if (numeric) kprintf(" %02x%02x:", d->class_code, d->subclass);
        else if (name) kprintf(" %s [%02x%02x]:", name, d->class_code, d->subclass);
        else kprintf(" Class [%02x%02x]:", d->class_code, d->subclass);
        kprintf(" %04x:%04x", d->vendor, d->device);
        if (d->revision) kprintf(" (rev %02x)", d->revision);
        if (verbose && d->prog_if) kprintf(" (prog-if %02x)", d->prog_if);
        kprintf("\n");
        if (verbose) print_verbose(d);
    }
}
