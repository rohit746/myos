#pragma once
// ACPI table discovery: find the firmware's tables and read the static ones
// that describe the machine (MADT, FADT, HPET, MCFG).
//
// Only tables are parsed. Anything that needs AML (the DSDT and SSDTs:
// power button, sleep, shutdown) is left for an AML interpreter.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The header every ACPI table (System Description Table) starts with.
struct acpi_sdt_header {
    char signature[4];
    uint32_t length; // whole table, header included
    uint8_t revision;
    uint8_t checksum; // all `length` bytes sum to 0
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

// Generic Address Structure: where a register is and how to reach it.
struct acpi_gas {
    uint8_t space; // ACPI_SPACE_*
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed));

#define ACPI_SPACE_MEMORY 0
#define ACPI_SPACE_IO     1
#define ACPI_SPACE_PCI    2

// Interrupt polarity and trigger mode, with "conforms to the bus" already
// resolved (ISA: active high, edge).
enum acpi_polarity { ACPI_ACTIVE_HIGH, ACPI_ACTIVE_LOW };
enum acpi_trigger { ACPI_EDGE, ACPI_LEVEL };

struct acpi_cpu {
    uint32_t apic_id;  // local APIC ID (x2APIC ID when above 254)
    uint32_t uid;      // ACPI processor UID, what NMI entries refer to
    bool enabled;      // usable now; false: may be hot-added later
    bool boot_cpu;     // the CPU running this code
};

struct acpi_ioapic {
    uint8_t id;
    uint64_t address;  // physical address of its registers
    uint32_t gsi_base; // first global system interrupt it handles
};

// An ISA IRQ that is wired to a different GSI, or not edge/active high.
struct acpi_override {
    uint8_t irq; // ISA IRQ (source)
    uint32_t gsi;
    enum acpi_polarity polarity;
    enum acpi_trigger trigger;
};

// A local APIC LINT pin that is wired to NMI.
struct acpi_lapic_nmi {
    uint32_t uid; // ACPI processor UID; ACPI_ALL_CPUS for every CPU
    uint8_t lint; // 0 or 1
    enum acpi_polarity polarity;
    enum acpi_trigger trigger;
};

// A GSI that is wired to NMI.
struct acpi_nmi_source {
    uint32_t gsi;
    enum acpi_polarity polarity;
    enum acpi_trigger trigger;
};

#define ACPI_ALL_CPUS 0xffffffffu

// One PCIe ECAM window (memory-mapped configuration space).
struct acpi_mcfg_entry {
    uint64_t base; // physical address of bus 0's configuration space
    uint16_t segment;
    uint8_t start_bus, end_bus;
};

struct acpi_info {
    uint8_t revision;  // RSDP revision: 0 for ACPI 1.0, 2 for 2.0 and later
    char oem_id[7];
    bool xsdt;         // tables were found through the XSDT (else the RSDT)
    size_t table_count;

    // MADT ("APIC")
    uint64_t lapic_address;
    bool pcat_compat; // there are also dual 8259 PICs, to mask before using the APICs
    struct acpi_cpu *cpus;
    size_t cpu_count;     // entries in `cpus`, enabled or not
    size_t cpus_enabled;
    struct acpi_ioapic *ioapics;
    size_t ioapic_count;
    struct acpi_override *overrides;
    size_t override_count;
    struct acpi_lapic_nmi *lapic_nmis;
    size_t lapic_nmi_count;
    struct acpi_nmi_source *nmi_sources;
    size_t nmi_source_count;

    // FADT ("FACP")
    bool have_fadt;
    uint8_t fadt_revision;
    bool hw_reduced;          // hardware-reduced ACPI: no PM timer, no fixed hardware
    bool reset_supported;     // writing `reset_value` to `reset_reg` resets the machine
    struct acpi_gas reset_reg;
    uint8_t reset_value;
    bool pm_timer;            // ACPI PM timer, 3.579545 MHz
    struct acpi_gas pm_timer_reg;
    bool pm_timer_32bit;      // else 24 bits wide
    uint16_t sci_irq;
    bool boot_arch_known;     // the flags below come from the FADT (ACPI 2.0+)
    bool legacy_devices;      // LPC/ISA devices such as the serial ports
    bool has_8042;            // PS/2 keyboard controller
    bool vga_absent;
    bool msi_unsupported;
    bool cmos_rtc_absent;

    // HPET
    bool have_hpet;
    uint64_t hpet_address;
    uint8_t hpet_comparators;
    uint16_t hpet_min_tick;

    // MCFG
    struct acpi_mcfg_entry *mcfg;
    size_t mcfg_count;
};

// Find and check the tables, then parse the MADT, FADT, HPET and MCFG.
// `rsdp` is Limine's RSDP pointer (an HHDM address). Needs the VMM and the
// heap. Panics if there is no valid RSDP or MADT.
void acpi_init(void *rsdp);

// The parsed tables. Valid after acpi_init().
const struct acpi_info *acpi_get_info(void);

// The first table with this 4-character signature ("APIC", "FACP", ...),
// or NULL. The pointer stays valid: ACPI tables are never freed.
const struct acpi_sdt_header *acpi_find_table(const char *signature);

// Like acpi_find_table, for the n-th table (from 0) with that signature.
const struct acpi_sdt_header *acpi_find_table_nth(const char *signature, size_t n);

// Print the boot summary: tables, CPUs, I/O APICs, overrides, FADT details.
void acpi_print_summary(void);
