// ACPI table discovery.
//
// The RSDP (from Limine) points to the RSDT (32-bit table addresses) and,
// on ACPI 2.0 and later, the XSDT (64-bit addresses). Each address there is
// a table that starts with a struct acpi_sdt_header. We keep a list of them
// for acpi_find_table() and parse the ones the kernel needs at boot.
//
// Limine (base revision 4 and later) guarantees that the RSDP, RSDT, XSDT
// and every table they list sit in memory-map regions that go in the HHDM,
// and vmm_init() maps those, so tables are read through phys_to_virt().
// map_phys() still checks every page, so a bad address from the firmware is
// reported instead of page-faulting.
//
// Table checksums are checked. A table with a bad one is still used, with a
// warning, as Linux does: firmware gets them wrong more often than it ships
// a table that is otherwise broken. The RSDP is the exception, since its
// checksum is how we know it is real.

#include <acpi.h>
#include <cpu.h>
#include <kheap.h>
#include <kprintf.h>
#include <memory.h>
#include <stddef.h>
#include <string.h>
#include <vmm.h>

struct rsdp {
    char signature[8]; // "RSD PTR "
    uint8_t checksum;  // first 20 bytes sum to 0
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    // ACPI 2.0 and later:
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum; // all `length` bytes sum to 0
    uint8_t reserved[3];
} __attribute__((packed));

#define RSDP_V1_SIZE 20

struct madt {
    struct acpi_sdt_header h;
    uint32_t lapic_address;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed));

#define MADT_PCAT_COMPAT (1u << 0)

// MADT entry types.
#define MADT_LAPIC          0
#define MADT_IOAPIC         1
#define MADT_OVERRIDE       2
#define MADT_NMI_SOURCE     3
#define MADT_LAPIC_NMI      4
#define MADT_LAPIC_ADDRESS  5
#define MADT_X2APIC         9
#define MADT_X2APIC_NMI     10

// Processor entry flags.
#define CPU_ENABLED        (1u << 0)
#define CPU_ONLINE_CAPABLE (1u << 1)

struct madt_lapic {
    uint8_t type, length;
    uint8_t uid;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed));

struct madt_ioapic {
    uint8_t type, length;
    uint8_t id;
    uint8_t reserved;
    uint32_t address;
    uint32_t gsi_base;
} __attribute__((packed));

struct madt_override {
    uint8_t type, length;
    uint8_t bus; // always 0, ISA
    uint8_t source;
    uint32_t gsi;
    uint16_t flags; // MPS INTI flags
} __attribute__((packed));

struct madt_nmi_source {
    uint8_t type, length;
    uint16_t flags;
    uint32_t gsi;
} __attribute__((packed));

struct madt_lapic_nmi {
    uint8_t type, length;
    uint8_t uid; // 0xff: all processors
    uint16_t flags;
    uint8_t lint;
} __attribute__((packed));

struct madt_lapic_address {
    uint8_t type, length;
    uint16_t reserved;
    uint64_t address;
} __attribute__((packed));

struct madt_x2apic {
    uint8_t type, length;
    uint16_t reserved;
    uint32_t x2apic_id;
    uint32_t flags;
    uint32_t uid;
} __attribute__((packed));

struct madt_x2apic_nmi {
    uint8_t type, length;
    uint16_t flags;
    uint32_t uid; // 0xffffffff: all processors
    uint8_t lint;
    uint8_t reserved[3];
} __attribute__((packed));

// MPS INTI flags: polarity in bits 0-1, trigger mode in bits 2-3.
// 0 means "conforms to the bus", 1 active high / edge, 3 active low / level.
#define INTI_POLARITY(f) ((f) & 3)
#define INTI_TRIGGER(f)  (((f) >> 2) & 3)
#define INTI_CONFORMS    0
#define INTI_LOW_LEVEL   3

// The FADT up to X_PM_TMR_BLK, the last field we read. Older (shorter)
// FADTs are copied into a zeroed one, so missing fields read as 0.
struct fadt {
    struct acpi_sdt_header h;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable, acpi_disable, s4bios_req, pstate_cnt;
    uint32_t pm1a_evt_blk, pm1b_evt_blk, pm1a_cnt_blk, pm1b_cnt_blk, pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk, gpe1_blk;
    uint8_t pm1_evt_len, pm1_cnt_len, pm2_cnt_len, pm_tmr_len;
    uint8_t gpe0_blk_len, gpe1_blk_len, gpe1_base, cst_cnt;
    uint16_t p_lvl2_lat, p_lvl3_lat, flush_size, flush_stride;
    uint8_t duty_offset, duty_width, day_alrm, mon_alrm, century;
    uint16_t iapc_boot_arch;
    uint8_t reserved1;
    uint32_t flags;
    struct acpi_gas reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t minor_version;
    uint64_t x_firmware_ctrl, x_dsdt;
    struct acpi_gas x_pm1a_evt_blk, x_pm1b_evt_blk, x_pm1a_cnt_blk, x_pm1b_cnt_blk, x_pm2_cnt_blk;
    struct acpi_gas x_pm_tmr_blk;
} __attribute__((packed));

_Static_assert(offsetof(struct fadt, iapc_boot_arch) == 109, "FADT layout");
_Static_assert(offsetof(struct fadt, reset_reg) == 116, "FADT layout");
_Static_assert(offsetof(struct fadt, x_pm_tmr_blk) == 208, "FADT layout");

#define FADT_TMR_VAL_EXT   (1u << 8)
#define FADT_RESET_REG_SUP (1u << 10)
#define FADT_HW_REDUCED    (1u << 20)

#define BOOT_ARCH_LEGACY_DEVICES (1u << 0)
#define BOOT_ARCH_8042           (1u << 1)
#define BOOT_ARCH_VGA_ABSENT     (1u << 2)
#define BOOT_ARCH_MSI_ABSENT     (1u << 3)
#define BOOT_ARCH_CMOS_ABSENT    (1u << 5)

// IAPC_BOOT_ARCH exists from FADT revision 3 (ACPI 2.0) on.
#define FADT_BOOT_ARCH_REVISION 3

struct hpet_table {
    struct acpi_sdt_header h;
    uint32_t block_id; // bits 8-12: number of comparators - 1
    struct acpi_gas base;
    uint8_t number;
    uint16_t min_tick;
    uint8_t page_protection;
} __attribute__((packed));

struct mcfg_table {
    struct acpi_sdt_header h;
    uint8_t reserved[8];
    struct {
        uint64_t base;
        uint16_t segment;
        uint8_t start_bus, end_bus;
        uint32_t reserved;
    } __attribute__((packed)) entries[];
} __attribute__((packed));

static struct acpi_info info;
static const struct acpi_sdt_header **tables;

// ---------------------------------------------------------------------------
// Reaching tables.
// ---------------------------------------------------------------------------

static void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kprintf("  acpi: warning: ");
    kvprintf(fmt, ap);
    kprintf("\n");
    va_end(ap);
}

// [phys, phys + len) through the HHDM, or NULL if any of it is not mapped.
static const void *map_phys(uint64_t phys, uint64_t len) {
    if (len == 0 || phys + len < phys) return NULL;
    for (uint64_t page = ALIGN_DOWN(phys, PAGE_SIZE); page < phys + len; page += PAGE_SIZE) {
        uint64_t got;
        if (!vmm_virt_to_phys(&kernel_space, (uint64_t)phys_to_virt(page), &got) || got != page) return NULL;
    }
    return phys_to_virt(phys);
}

static uint8_t checksum(const void *p, size_t len) {
    const uint8_t *b = p;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += b[i];
    return sum;
}

// A NUL-terminated copy of a fixed-size, space-padded name.
static void copy_name(char *dst, const char *src, size_t len) {
    memcpy(dst, src, len);
    dst[len] = '\0';
    for (size_t i = len; i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\0'); i--) dst[i - 1] = '\0';
}

// The table at `phys`, or NULL if it can't be read. `expect` is a signature
// to check for, or NULL for any.
static const struct acpi_sdt_header *map_table(uint64_t phys, const char *expect) {
    const struct acpi_sdt_header *h = map_phys(phys, sizeof *h);
    if (h == NULL) {
        warn("table at %p is not in mapped memory", (void *)phys);
        return NULL;
    }
    char sig[5];
    copy_name(sig, h->signature, 4);
    if (expect && memcmp(h->signature, expect, 4) != 0) {
        warn("table at %p is \"%s\", expected %s", (void *)phys, sig, expect);
        return NULL;
    }
    if (h->length < sizeof *h) {
        warn("%s table at %p has a bad length (%u)", sig, (void *)phys, h->length);
        return NULL;
    }
    if (map_phys(phys, h->length) == NULL) {
        warn("%s table at %p (%u bytes) is not all in mapped memory", sig, (void *)phys, h->length);
        return NULL;
    }
    if (checksum(h, h->length) != 0) warn("%s table at %p has a bad checksum, using it anyway", sig, (void *)phys);
    return h;
}

// Fill `tables` from the RSDT (4-byte entries) or XSDT (8-byte entries).
static void load_root(const struct acpi_sdt_header *root, size_t entry_size) {
    size_t n = (root->length - sizeof *root) / entry_size;
    tables = kcalloc(n ? n : 1, sizeof *tables);
    if (tables == NULL) panic("acpi: out of memory");
    const uint8_t *entries = (const uint8_t *)(root + 1);
    for (size_t i = 0; i < n; i++) {
        uint64_t phys = 0;
        memcpy(&phys, entries + i * entry_size, entry_size); // little-endian, maybe unaligned
        if (phys == 0) continue;
        const struct acpi_sdt_header *t = map_table(phys, NULL);
        if (t) tables[info.table_count++] = t;
    }
}

const struct acpi_sdt_header *acpi_find_table_nth(const char *signature, size_t n) {
    for (size_t i = 0; i < info.table_count; i++)
        if (memcmp(tables[i]->signature, signature, 4) == 0 && n-- == 0) return tables[i];
    return NULL;
}

const struct acpi_sdt_header *acpi_find_table(const char *signature) {
    return acpi_find_table_nth(signature, 0);
}

const struct acpi_info *acpi_get_info(void) { return &info; }

// ---------------------------------------------------------------------------
// FADT
// ---------------------------------------------------------------------------

static bool usable_gas(const struct acpi_gas *g) {
    return g->address != 0 && (g->space == ACPI_SPACE_MEMORY || g->space == ACPI_SPACE_IO);
}

static void parse_fadt(const struct acpi_sdt_header *h) {
    struct fadt f;
    memset(&f, 0, sizeof f);
    memcpy(&f, h, h->length < sizeof f ? h->length : sizeof f);

    info.have_fadt = true;
    info.fadt_revision = h->revision;
    info.hw_reduced = f.flags & FADT_HW_REDUCED;
    info.sci_irq = f.sci_int;

    if ((f.flags & FADT_RESET_REG_SUP) && f.reset_reg.address != 0 &&
        (usable_gas(&f.reset_reg) || f.reset_reg.space == ACPI_SPACE_PCI)) {
        info.reset_supported = true;
        info.reset_reg = f.reset_reg;
        info.reset_value = f.reset_value;
    }

    if (!info.hw_reduced) {
        if (usable_gas(&f.x_pm_tmr_blk)) {
            info.pm_timer = true;
            info.pm_timer_reg = f.x_pm_tmr_blk;
        } else if (f.pm_tmr_blk != 0 && f.pm_tmr_len >= 4) {
            info.pm_timer = true;
            info.pm_timer_reg = (struct acpi_gas){.space = ACPI_SPACE_IO, .bit_width = 32,
                                                  .address = f.pm_tmr_blk};
        }
        info.pm_timer_32bit = f.flags & FADT_TMR_VAL_EXT;
    }

    // Before ACPI 2.0 there were no flags: assume a PC with the usual
    // legacy hardware, as Linux does.
    uint16_t arch = BOOT_ARCH_LEGACY_DEVICES | BOOT_ARCH_8042;
    if (h->revision >= FADT_BOOT_ARCH_REVISION && h->length >= offsetof(struct fadt, iapc_boot_arch) + 2) {
        info.boot_arch_known = true;
        arch = f.iapc_boot_arch;
    }
    info.legacy_devices = arch & BOOT_ARCH_LEGACY_DEVICES;
    info.has_8042 = arch & BOOT_ARCH_8042;
    info.vga_absent = arch & BOOT_ARCH_VGA_ABSENT;
    info.msi_unsupported = arch & BOOT_ARCH_MSI_ABSENT;
    info.cmos_rtc_absent = arch & BOOT_ARCH_CMOS_ABSENT;
}

// ---------------------------------------------------------------------------
// MADT
// ---------------------------------------------------------------------------

// "Conforms to the bus" means edge and active high for ISA, except for the
// SCI, which the spec makes level-triggered and active low.
static enum acpi_polarity inti_polarity(uint16_t flags, bool sci) {
    if (INTI_POLARITY(flags) == INTI_CONFORMS) return sci ? ACPI_ACTIVE_LOW : ACPI_ACTIVE_HIGH;
    return INTI_POLARITY(flags) == INTI_LOW_LEVEL ? ACPI_ACTIVE_LOW : ACPI_ACTIVE_HIGH;
}

static enum acpi_trigger inti_trigger(uint16_t flags, bool sci) {
    if (INTI_TRIGGER(flags) == INTI_CONFORMS) return sci ? ACPI_LEVEL : ACPI_EDGE;
    return INTI_TRIGGER(flags) == INTI_LOW_LEVEL ? ACPI_LEVEL : ACPI_EDGE;
}

static uint32_t boot_apic_id(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 0xb) {
        cpuid(0xb, 0, &a, &b, &c, &d);
        if (b != 0) return d; // x2APIC ID
    }
    cpuid(1, 0, &a, &b, &c, &d);
    return b >> 24;
}

static void add_cpu(bool fill, uint32_t apic_id, uint32_t uid, uint32_t flags) {
    if (!(flags & (CPU_ENABLED | CPU_ONLINE_CAPABLE))) return; // can never be used
    if (!fill) {
        info.cpu_count++; // an upper bound; duplicates are dropped below
        return;
    }
    // Firmware may list a CPU both as a local APIC and as an x2APIC.
    for (size_t i = 0; i < info.cpu_count; i++)
        if (info.cpus[i].apic_id == apic_id) return;
    info.cpus[info.cpu_count++] = (struct acpi_cpu){
        .apic_id = apic_id, .uid = uid, .enabled = flags & CPU_ENABLED};
    if (flags & CPU_ENABLED) info.cpus_enabled++;
}

static void add_lapic_nmi(bool fill, uint32_t uid, uint8_t lint, uint16_t flags) {
    if (fill)
        info.lapic_nmis[info.lapic_nmi_count] = (struct acpi_lapic_nmi){
            .uid = uid, .lint = lint, .polarity = inti_polarity(flags, false),
            .trigger = inti_trigger(flags, false)};
    info.lapic_nmi_count++;
}

// One pass over the entries. The first (fill = false) only counts them, to
// size the arrays; the second fills them in.
static void walk_madt(const struct madt *m, bool fill) {
    info.cpu_count = info.cpus_enabled = info.ioapic_count = info.override_count = 0;
    info.lapic_nmi_count = info.nmi_source_count = 0;

    const uint8_t *p = m->entries;
    const uint8_t *end = (const uint8_t *)m + m->h.length;
    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > end) {
            if (fill) warn("MADT entry at offset %lu has a bad length (%u)", (uint64_t)(p - (const uint8_t *)m), len);
            break;
        }
        switch (type) {
        case MADT_LAPIC: {
            const struct madt_lapic *e = (const void *)p;
            if (len < sizeof *e) break;
            if (e->apic_id != 0xff) add_cpu(fill, e->apic_id, e->uid, e->flags);
            break;
        }
        case MADT_X2APIC: {
            const struct madt_x2apic *e = (const void *)p;
            if (len < sizeof *e) break;
            add_cpu(fill, e->x2apic_id, e->uid, e->flags);
            break;
        }
        case MADT_IOAPIC: {
            const struct madt_ioapic *e = (const void *)p;
            if (len < sizeof *e) break;
            if (fill)
                info.ioapics[info.ioapic_count] = (struct acpi_ioapic){
                    .id = e->id, .address = e->address, .gsi_base = e->gsi_base};
            info.ioapic_count++;
            break;
        }
        case MADT_OVERRIDE: {
            const struct madt_override *e = (const void *)p;
            if (len < sizeof *e) break;
            bool sci = info.have_fadt && e->source == info.sci_irq;
            if (fill)
                info.overrides[info.override_count] = (struct acpi_override){
                    .irq = e->source, .gsi = e->gsi, .polarity = inti_polarity(e->flags, sci),
                    .trigger = inti_trigger(e->flags, sci)};
            info.override_count++;
            break;
        }
        case MADT_NMI_SOURCE: {
            const struct madt_nmi_source *e = (const void *)p;
            if (len < sizeof *e) break;
            if (fill)
                info.nmi_sources[info.nmi_source_count] = (struct acpi_nmi_source){
                    .gsi = e->gsi, .polarity = inti_polarity(e->flags, false),
                    .trigger = inti_trigger(e->flags, false)};
            info.nmi_source_count++;
            break;
        }
        case MADT_LAPIC_NMI: {
            const struct madt_lapic_nmi *e = (const void *)p;
            if (len < sizeof *e) break;
            add_lapic_nmi(fill, e->uid == 0xff ? ACPI_ALL_CPUS : e->uid, e->lint, e->flags);
            break;
        }
        case MADT_X2APIC_NMI: {
            const struct madt_x2apic_nmi *e = (const void *)p;
            if (len < sizeof *e) break;
            add_lapic_nmi(fill, e->uid, e->lint, e->flags);
            break;
        }
        case MADT_LAPIC_ADDRESS: {
            const struct madt_lapic_address *e = (const void *)p;
            if (len < sizeof *e) break;
            info.lapic_address = e->address;
            break;
        }
        default:
            break; // other interrupt controllers, wakeup mailboxes, ...
        }
        p += len;
    }
}

static void *alloc_array(size_t count, size_t size) {
    if (count == 0) return NULL;
    void *p = kcalloc(count, size);
    if (p == NULL) panic("acpi: out of memory");
    return p;
}

static void parse_madt(const struct acpi_sdt_header *h) {
    if (h->length < sizeof(struct madt)) panic("acpi: MADT is too short (%u bytes)", h->length);
    const struct madt *m = (const void *)h;
    info.lapic_address = m->lapic_address;
    info.pcat_compat = m->flags & MADT_PCAT_COMPAT;

    walk_madt(m, false);
    info.cpus = alloc_array(info.cpu_count, sizeof *info.cpus);
    info.ioapics = alloc_array(info.ioapic_count, sizeof *info.ioapics);
    info.overrides = alloc_array(info.override_count, sizeof *info.overrides);
    info.lapic_nmis = alloc_array(info.lapic_nmi_count, sizeof *info.lapic_nmis);
    info.nmi_sources = alloc_array(info.nmi_source_count, sizeof *info.nmi_sources);
    walk_madt(m, true);

    if (info.cpus_enabled == 0) panic("acpi: the MADT lists no usable CPUs");
    uint32_t bsp = boot_apic_id();
    bool found = false;
    for (size_t i = 0; i < info.cpu_count; i++) {
        if (info.cpus[i].apic_id == bsp) {
            info.cpus[i].boot_cpu = true;
            found = true;
        }
    }
    if (!found) warn("this CPU (APIC ID %u) is not in the MADT", bsp);
    if (info.ioapic_count == 0) warn("the MADT lists no I/O APICs");
}

// ---------------------------------------------------------------------------
// HPET and MCFG (used by later drivers; for now we just record them).
// ---------------------------------------------------------------------------

static void parse_hpet(const struct acpi_sdt_header *h) {
    if (h->length < sizeof(struct hpet_table)) {
        warn("HPET table is too short (%u bytes)", h->length);
        return;
    }
    const struct hpet_table *t = (const void *)h;
    if (t->base.space != ACPI_SPACE_MEMORY || t->base.address == 0) {
        warn("HPET is not memory-mapped (space %u, address %p), ignoring it", t->base.space,
             (void *)t->base.address);
        return;
    }
    info.have_hpet = true;
    info.hpet_address = t->base.address;
    info.hpet_comparators = ((t->block_id >> 8) & 0x1f) + 1;
    info.hpet_min_tick = t->min_tick;
}

static void parse_mcfg(const struct acpi_sdt_header *h) {
    if (h->length < sizeof(struct mcfg_table)) {
        warn("MCFG table is too short (%u bytes)", h->length);
        return;
    }
    const struct mcfg_table *t = (const void *)h;
    size_t n = (h->length - sizeof *t) / sizeof t->entries[0];
    info.mcfg = alloc_array(n, sizeof *info.mcfg);
    for (size_t i = 0; i < n; i++) {
        if (t->entries[i].end_bus < t->entries[i].start_bus) {
            warn("MCFG entry %lu has buses %u-%u, ignoring it", i, t->entries[i].start_bus, t->entries[i].end_bus);
            continue;
        }
        info.mcfg[info.mcfg_count++] = (struct acpi_mcfg_entry){
            .base = t->entries[i].base, .segment = t->entries[i].segment,
            .start_bus = t->entries[i].start_bus, .end_bus = t->entries[i].end_bus};
    }
}

// ---------------------------------------------------------------------------

void acpi_init(void *rsdp_ptr) {
    if (rsdp_ptr == NULL) panic("acpi: the bootloader found no RSDP (no ACPI?)");
    uint64_t rsdp_phys = virt_to_phys(rsdp_ptr);
    const struct rsdp *rsdp = map_phys(rsdp_phys, RSDP_V1_SIZE);
    if (rsdp == NULL) panic("acpi: RSDP at %p is not in mapped memory", (void *)rsdp_phys);
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) panic("acpi: no RSDP signature at %p", (void *)rsdp_phys);
    if (checksum(rsdp, RSDP_V1_SIZE) != 0) panic("acpi: RSDP at %p has a bad checksum", (void *)rsdp_phys);
    info.revision = rsdp->revision;
    copy_name(info.oem_id, rsdp->oem_id, 6);

    // Prefer the XSDT (ACPI 2.0+), but fall back to the RSDT if it is broken.
    const struct acpi_sdt_header *root = NULL;
    if (rsdp->revision >= 2) {
        if (map_phys(rsdp_phys, sizeof *rsdp) == NULL || rsdp->length < sizeof *rsdp ||
            map_phys(rsdp_phys, rsdp->length) == NULL)
            warn("ACPI 2.0 RSDP has a bad length (%u), using the RSDT", rsdp->length);
        else if (checksum(rsdp, rsdp->length) != 0)
            warn("ACPI 2.0 RSDP has a bad extended checksum, using the RSDT");
        else if (rsdp->xsdt_address != 0 && (root = map_table(rsdp->xsdt_address, "XSDT")) != NULL)
            info.xsdt = true;
    }
    if (root == NULL) {
        if (rsdp->rsdt_address == 0 || (root = map_table(rsdp->rsdt_address, "RSDT")) == NULL)
            panic("acpi: neither the XSDT nor the RSDT can be read");
    }
    load_root(root, info.xsdt ? 8 : 4);

    // The FADT first: the MADT needs to know which IRQ is the SCI.
    const struct acpi_sdt_header *t;
    if ((t = acpi_find_table("FACP")) != NULL) parse_fadt(t);
    else warn("no FADT");
    if ((t = acpi_find_table("APIC")) == NULL) panic("acpi: no MADT, so no APIC information");
    parse_madt(t);
    if ((t = acpi_find_table("HPET")) != NULL) parse_hpet(t);
    if ((t = acpi_find_table("MCFG")) != NULL) parse_mcfg(t);
}

// ---------------------------------------------------------------------------
// Boot summary.
// ---------------------------------------------------------------------------

static const char *space_name(uint8_t space) {
    switch (space) {
    case ACPI_SPACE_MEMORY: return "memory";
    case ACPI_SPACE_IO: return "I/O port";
    case ACPI_SPACE_PCI: return "PCI config";
    default: return "?";
    }
}

static const char *irq_mode(enum acpi_trigger t, enum acpi_polarity p) {
    if (t == ACPI_EDGE) return p == ACPI_ACTIVE_HIGH ? "edge, active high" : "edge, active low";
    return p == ACPI_ACTIVE_HIGH ? "level, active high" : "level, active low";
}

#define MAX_LISTED_CPUS 16

void acpi_print_summary(void) {
    kprintf("    tables:");
    for (size_t i = 0; i < info.table_count; i++) {
        char sig[5];
        copy_name(sig, tables[i]->signature, 4);
        kprintf(" %s", sig);
    }
    kprintf("\n");

    kprintf("    cpus:   %lu", info.cpus_enabled);
    if (info.cpu_count > info.cpus_enabled) kprintf(" (+%lu hot-pluggable)", info.cpu_count - info.cpus_enabled);
    kprintf(", APIC IDs");
    size_t listed = 0;
    for (size_t i = 0; i < info.cpu_count && listed < MAX_LISTED_CPUS; i++) {
        if (!info.cpus[i].enabled) continue;
        kprintf(" %u%s", info.cpus[i].apic_id, info.cpus[i].boot_cpu ? "*" : "");
        listed++;
    }
    if (listed < info.cpus_enabled) kprintf(" ...");
    kprintf(" (* = boot CPU), local APIC at 0x%lx\n", info.lapic_address);

    for (size_t i = 0; i < info.ioapic_count; i++)
        kprintf("    I/O APIC %u at 0x%lx, GSIs from %u\n", info.ioapics[i].id, info.ioapics[i].address,
                info.ioapics[i].gsi_base);
    for (size_t i = 0; i < info.override_count; i++) {
        const struct acpi_override *o = &info.overrides[i];
        kprintf("    IRQ %u -> GSI %u, %s%s\n", o->irq, o->gsi, irq_mode(o->trigger, o->polarity),
                info.have_fadt && o->irq == info.sci_irq ? " (ACPI SCI)" : "");
    }
    for (size_t i = 0; i < info.nmi_source_count; i++)
        kprintf("    NMI on GSI %u, %s\n", info.nmi_sources[i].gsi,
                irq_mode(info.nmi_sources[i].trigger, info.nmi_sources[i].polarity));
    // Real machines often list one LINT NMI per CPU; print one line per pin.
    for (int lint = 0; lint < 2; lint++) {
        size_t n = 0;
        bool all = false;
        for (size_t i = 0; i < info.lapic_nmi_count; i++) {
            if (info.lapic_nmis[i].lint != lint) continue;
            n++;
            if (info.lapic_nmis[i].uid == ACPI_ALL_CPUS) all = true;
        }
        if (all) kprintf("    NMI on LINT%d of every CPU\n", lint);
        else if (n) kprintf("    NMI on LINT%d of %lu CPU%s\n", lint, n, n == 1 ? "" : "s");
    }
    if (info.pcat_compat) kprintf("    legacy 8259 PICs present (mask them before using the APICs)\n");

    if (info.have_fadt) {
        kprintf("    FADT rev %u:%s", info.fadt_revision, info.hw_reduced ? " hardware-reduced," : "");
        if (info.reset_supported)
            kprintf(" reset by writing 0x%x to %s 0x%lx,", info.reset_value, space_name(info.reset_reg.space),
                    info.reset_reg.address);
        else
            kprintf(" no reset register,");
        if (info.pm_timer)
            kprintf(" PM timer at %s 0x%lx (%d-bit)\n", space_name(info.pm_timer_reg.space),
                    info.pm_timer_reg.address, info.pm_timer_32bit ? 32 : 24);
        else
            kprintf(" no PM timer\n");
        kprintf("    boot flags%s: 8042 PS/2 %s, legacy devices %s, VGA %s, MSI %s, CMOS RTC %s\n",
                info.boot_arch_known ? "" : " (assumed, FADT predates them)", info.has_8042 ? "yes" : "no",
                info.legacy_devices ? "yes" : "no", info.vga_absent ? "no" : "yes",
                info.msi_unsupported ? "no" : "yes", info.cmos_rtc_absent ? "no" : "yes");
    }
    if (info.have_hpet)
        kprintf("    HPET at 0x%lx, %u comparators\n", info.hpet_address, info.hpet_comparators);
    for (size_t i = 0; i < info.mcfg_count; i++)
        kprintf("    PCIe config space at 0x%lx (segment %u, buses %u-%u)\n", info.mcfg[i].base,
                info.mcfg[i].segment, info.mcfg[i].start_bus, info.mcfg[i].end_bus);
}
