// Local APIC, I/O APIC and the legacy 8259 PICs.
//
// PC/AT machines start with interrupts going through two cascaded 8259 PICs,
// which by default deliver IRQ 0-7 on vectors 8-15, on top of the CPU
// exceptions. We remap them to 0x20-0x2f and mask every line, so a stray
// legacy IRQ can never look like an exception, and use the APICs instead:
//
//   device --> I/O APIC pin (a GSI) --> redirection entry: vector, CPU,
//              polarity, trigger --> that CPU's local APIC --> the IDT
//
// ISA IRQ n is on GSI n unless the MADT has an interrupt source override,
// which can also change its polarity and trigger mode (QEMU and most PCs
// wire the PIT, IRQ 0, to GSI 2).
//
// The local APIC is in x2APIC mode when the CPU supports it: its registers
// are MSRs 0x800 + offset / 16 instead of a 4 KiB MMIO page. Both modes are
// handled by lapic_read()/lapic_write().

#include <acpi.h>
#include <apic.h>
#include <cpu.h>
#include <kprintf.h>
#include <memory.h>
#include <smp.h>
#include <stddef.h>
#include <vmm.h>

// 8259 PIC ports and commands.
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xa0
#define PIC2_DATA 0xa1
#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01

#define MSR_APIC_BASE      0x1b
#define APIC_BASE_X2APIC   (1ull << 10)
#define APIC_BASE_ENABLE   (1ull << 11)
#define APIC_BASE_ADDR     0x000ffffffffff000ull
#define MSR_X2APIC_BASE    0x800

#define CPUID1_EDX_APIC    (1u << 9)
#define CPUID1_ECX_X2APIC  (1u << 21)

// Local APIC registers (xAPIC MMIO offsets).
#define LAPIC_ID       0x020
#define LAPIC_VERSION  0x030
#define LAPIC_TPR      0x080
#define LAPIC_EOI      0x0b0
#define LAPIC_SVR      0x0f0
#define LAPIC_ESR      0x280
#define LAPIC_ICR_LOW  0x300
#define LAPIC_ICR_HIGH 0x310
#define LAPIC_LVT_CMCI 0x2f0
#define LAPIC_LVT_TIMER   0x320
#define LAPIC_LVT_THERMAL 0x330
#define LAPIC_LVT_PERF    0x340
#define LAPIC_LVT_LINT0   0x350
#define LAPIC_LVT_LINT1   0x360
#define LAPIC_LVT_ERROR   0x370
#define LAPIC_TIMER_INITIAL 0x380
#define LAPIC_TIMER_CURRENT 0x390
#define LAPIC_TIMER_DIVIDE  0x3e0

#define SVR_ENABLE       (1u << 8)
#define LVT_DELIVERY_NMI (4u << 8)
#define LVT_ACTIVE_LOW   (1u << 13)
#define LVT_MASKED       (1u << 16)
#define LVT_TIMER_PERIODIC (1u << 17)

#define ICR_DELIVERY_NMI (4u << 8)
#define ICR_PENDING      (1u << 12) // xAPIC: the last IPI hasn't been sent yet
#define ICR_ASSERT       (1u << 14)

#define TIMER_DIVIDE_BY_16 0x3 // divide configuration register encoding
_Static_assert(LAPIC_TIMER_DIVIDER == 16, "TIMER_DIVIDE_BY_16 must match LAPIC_TIMER_DIVIDER");

// I/O APIC: an index register and a data window.
#define IOAPIC_REGSEL  0x00
#define IOAPIC_WINDOW  0x10
#define IOAPIC_VERSION 0x01
#define IOAPIC_REDIR(n) (0x10 + 2 * (n))

#define REDIR_ACTIVE_LOW (1u << 13)
#define REDIR_LEVEL      (1u << 15)
#define REDIR_MASKED     (1u << 16)

#define MAX_IOAPICS 16

struct ioapic {
    volatile uint32_t *regs;
    uint32_t gsi_base;
    uint32_t pins;
};

static bool x2apic;
static volatile uint32_t *lapic_regs; // xAPIC mode only
static struct ioapic ioapics[MAX_IOAPICS];
static size_t ioapic_count;
static interrupt_handler_t irq_handlers[ISA_IRQ_COUNT];
static interrupt_handler_t msi_handlers[MSI_VECTOR_END - MSI_VECTOR_BASE];
static struct raw_spinlock msi_lock; // msi_handlers, from any CPU
static struct apic_info info;

// ---------------------------------------------------------------------------
// 8259 PICs
// ---------------------------------------------------------------------------

static void pic_remap_and_mask(void) {
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC1_DATA, PIC_VECTOR_BASE); io_wait();     // ICW2: vector offsets
    outb(PIC2_DATA, PIC_VECTOR_BASE + 8); io_wait();
    outb(PIC1_DATA, 1 << 2); io_wait();              // ICW3: the slave is on IRQ 2
    outb(PIC2_DATA, 2); io_wait();                   //       ...and its cascade ID
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();
    outb(PIC1_DATA, 0xff);                           // mask every line
    outb(PIC2_DATA, 0xff);
}

// ---------------------------------------------------------------------------
// Local APIC
// ---------------------------------------------------------------------------

static uint32_t lapic_read(uint32_t reg) {
    if (x2apic) return (uint32_t)rdmsr(MSR_X2APIC_BASE + (reg >> 4));
    return lapic_regs[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t val) {
    if (x2apic) wrmsr(MSR_X2APIC_BASE + (reg >> 4), val);
    else lapic_regs[reg / 4] = val;
}

void lapic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

uint32_t lapic_id(void) {
    uint32_t id = lapic_read(LAPIC_ID);
    return x2apic ? id : id >> 24;
}

void lapic_timer_start(uint32_t count, bool periodic, bool interrupt) {
    lapic_write(LAPIC_TIMER_INITIAL, 0); // stop it while it is reprogrammed
    lapic_write(LAPIC_TIMER_DIVIDE, TIMER_DIVIDE_BY_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (periodic ? LVT_TIMER_PERIODIC : 0) |
                                     (interrupt ? 0 : LVT_MASKED));
    lapic_write(LAPIC_TIMER_INITIAL, count); // starts counting
}

uint32_t lapic_timer_count(void) { return lapic_read(LAPIC_TIMER_CURRENT); }

static void spurious_handler(struct interrupt_frame *f) {
    (void)f;
    __atomic_add_fetch(&info.spurious, 1, __ATOMIC_RELAXED); // no EOI: a spurious interrupt never set an ISR bit
}

// The BSP's ACPI processor UID, for the MADT's LINT NMIs.
static bool boot_cpu_uid(uint32_t *uid) {
    const struct acpi_info *a = acpi_get_info();
    for (size_t i = 0; i < a->cpu_count; i++)
        if (a->cpus[i].boot_cpu) {
            *uid = a->cpus[i].uid;
            return true;
        }
    return false;
}

// Turn on this CPU's local APIC: on the BSP, in x2APIC mode if the CPU
// has it; on an AP, in the mode the BSP chose (lapic_read and lapic_write
// assume they all match).
static void lapic_enable(bool bsp) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & CPUID1_EDX_APIC)) panic("apic: this CPU has no local APIC");

    // Valid IA32_APIC_BASE transitions are disabled -> xAPIC -> x2APIC, so
    // enable it first. Firmware (or the bootloader) may already have
    // switched it to x2APIC, which can't be undone without disabling it.
    uint64_t base = rdmsr(MSR_APIC_BASE);
    if (!(base & APIC_BASE_ENABLE)) {
        base |= APIC_BASE_ENABLE;
        wrmsr(MSR_APIC_BASE, base);
    }
    bool want_x2apic = bsp ? (c & CPUID1_ECX_X2APIC) != 0 : x2apic;
    if (!(base & APIC_BASE_X2APIC) && want_x2apic) {
        base |= APIC_BASE_X2APIC;
        wrmsr(MSR_APIC_BASE, base);
    }
    if (!bsp) {
        if (!!(base & APIC_BASE_X2APIC) != x2apic)
            panic("apic: CPU %u's local APIC is in %s mode, the BSP's isn't", lapic_id(), x2apic ? "xAPIC" : "x2APIC");
        return;
    }
    x2apic = base & APIC_BASE_X2APIC;
    info.lapic_phys = base & APIC_BASE_ADDR;
    if (!x2apic) lapic_regs = vmm_map_mmio(info.lapic_phys, PAGE_SIZE);
}

// Mask every local interrupt source but the MADT's NMIs for `uid`, and
// accept interrupts.
static void lapic_setup(bool have_uid, uint32_t uid) {
    // Mask every local interrupt source. LINT0 is where the 8259s would
    // come in (ExtINT); they are masked too.
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED);
    if (info.lvt_count > 4) lapic_write(LAPIC_LVT_PERF, LVT_MASKED);
    if (info.lvt_count > 5) lapic_write(LAPIC_LVT_THERMAL, LVT_MASKED);
    if (info.lvt_count > 6) lapic_write(LAPIC_LVT_CMCI, LVT_MASKED);

    // Then wire up the LINT pins the MADT says carry NMIs (usually LINT1).
    // NMIs are always edge-triggered; only the polarity can be chosen.
    const struct acpi_info *acpi = acpi_get_info();
    for (size_t i = 0; i < acpi->lapic_nmi_count; i++) {
        const struct acpi_lapic_nmi *n = &acpi->lapic_nmis[i];
        if (n->uid != ACPI_ALL_CPUS && !(have_uid && n->uid == uid)) continue;
        if (n->lint > 1) continue;
        uint32_t lvt = LVT_DELIVERY_NMI | (n->polarity == ACPI_ACTIVE_LOW ? LVT_ACTIVE_LOW : 0);
        lapic_write(n->lint ? LAPIC_LVT_LINT1 : LAPIC_LVT_LINT0, lvt);
    }

    // Clear any errors latched before now (xAPIC wants a write first).
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    lapic_write(LAPIC_TPR, 0); // accept every priority
    lapic_write(LAPIC_SVR, SVR_ENABLE | APIC_SPURIOUS_VECTOR);
}

static void lapic_init(void) {
    lapic_enable(true);
    uint32_t version = lapic_read(LAPIC_VERSION);
    info.x2apic = x2apic;
    info.lapic_id = lapic_id();
    info.lapic_version = version & 0xff;
    info.lvt_count = ((version >> 16) & 0xff) + 1;
    idt_set_handler(APIC_SPURIOUS_VECTOR, spurious_handler);
    uint32_t uid;
    bool have_uid = boot_cpu_uid(&uid);
    lapic_setup(have_uid, uid);
}

void apic_init_ap(uint32_t acpi_uid) {
    lapic_enable(false);
    lapic_setup(true, acpi_uid);
}

static void send_icr(uint32_t apic_id, uint32_t low) {
    // An x2APIC ICR write isn't serializing: make the memory the handler
    // reads visible first.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (x2apic) {
        wrmsr(MSR_X2APIC_BASE + (LAPIC_ICR_LOW >> 4), (uint64_t)apic_id << 32 | low);
        return;
    }
    // xAPIC: two writes, which an interrupt handler sending its own IPI
    // must not come between.
    uint64_t flags = read_rflags();
    cli();
    while (lapic_read(LAPIC_ICR_LOW) & ICR_PENDING) pause();
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, low);
    if (flags & RFLAGS_IF) sti();
}

void lapic_send_ipi(uint32_t apic_id, uint8_t vector) { send_icr(apic_id, ICR_ASSERT | vector); }

void lapic_send_nmi(uint32_t apic_id) { send_icr(apic_id, ICR_ASSERT | ICR_DELIVERY_NMI); }

// ---------------------------------------------------------------------------
// I/O APICs
// ---------------------------------------------------------------------------

static uint32_t ioapic_read(const struct ioapic *io, uint32_t reg) {
    io->regs[IOAPIC_REGSEL / 4] = reg;
    return io->regs[IOAPIC_WINDOW / 4];
}

static void ioapic_write(const struct ioapic *io, uint32_t reg, uint32_t val) {
    io->regs[IOAPIC_REGSEL / 4] = reg;
    io->regs[IOAPIC_WINDOW / 4] = val;
}

static void ioapic_init(void) {
    const struct acpi_info *acpi = acpi_get_info();
    if (acpi->ioapic_count == 0) panic("apic: the MADT lists no I/O APIC");
    if (acpi->ioapic_count > MAX_IOAPICS) panic("apic: %lu I/O APICs, at most %d supported", acpi->ioapic_count, MAX_IOAPICS);

    for (size_t i = 0; i < acpi->ioapic_count; i++) {
        struct ioapic *io = &ioapics[ioapic_count++];
        io->regs = vmm_map_mmio(acpi->ioapics[i].address, 0x20);
        io->gsi_base = acpi->ioapics[i].gsi_base;
        io->pins = ((ioapic_read(io, IOAPIC_VERSION) >> 16) & 0xff) + 1;
        info.ioapic_pins += io->pins;
        for (uint32_t pin = 0; pin < io->pins; pin++) {
            ioapic_write(io, IOAPIC_REDIR(pin), REDIR_MASKED);
            ioapic_write(io, IOAPIC_REDIR(pin) + 1, 0);
        }
    }
}

static const struct ioapic *ioapic_for_gsi(uint32_t gsi) {
    for (size_t i = 0; i < ioapic_count; i++)
        if (gsi >= ioapics[i].gsi_base && gsi - ioapics[i].gsi_base < ioapics[i].pins) return &ioapics[i];
    return NULL;
}

// Where ISA IRQ `irq` comes in, after the MADT's overrides. False if
// another IRQ has been moved onto its GSI (IRQ 2, when IRQ 0 is on GSI 2).
static bool isa_route(uint8_t irq, uint32_t *gsi, enum acpi_polarity *pol, enum acpi_trigger *trig) {
    const struct acpi_info *acpi = acpi_get_info();
    for (size_t i = 0; i < acpi->override_count; i++)
        if (acpi->overrides[i].irq == irq) {
            *gsi = acpi->overrides[i].gsi;
            *pol = acpi->overrides[i].polarity;
            *trig = acpi->overrides[i].trigger;
            return true;
        }
    for (size_t i = 0; i < acpi->override_count; i++)
        if (acpi->overrides[i].gsi == irq) return false;
    *gsi = irq;
    *pol = ACPI_ACTIVE_HIGH;
    *trig = ACPI_EDGE;
    return true;
}

static void irq_dispatch(struct interrupt_frame *f) {
    uint8_t irq = f->vector - IRQ_VECTOR_BASE;
    if (irq_handlers[irq]) irq_handlers[irq](f);
    lapic_eoi();
}

uint32_t irq_register(uint8_t isa_irq, interrupt_handler_t handler) {
    if (isa_irq >= ISA_IRQ_COUNT) panic("irq_register: %u is not an ISA IRQ", isa_irq);
    if (handler == NULL) panic("irq_register: IRQ %u: no handler", isa_irq);
    if (irq_handlers[isa_irq]) panic("irq_register: IRQ %u already has a handler", isa_irq);

    uint32_t gsi;
    enum acpi_polarity pol;
    enum acpi_trigger trig;
    if (!isa_route(isa_irq, &gsi, &pol, &trig))
        panic("irq_register: IRQ %u: its GSI is taken by another IRQ's override", isa_irq);
    const struct ioapic *io = ioapic_for_gsi(gsi);
    if (io == NULL) panic("irq_register: IRQ %u: no I/O APIC handles GSI %u", isa_irq, gsi);

    // Without interrupt remapping an I/O APIC can only name APIC IDs 0-255.
    uint32_t dest = info.lapic_id;
    if (dest > 0xff) panic("irq_register: APIC ID %u can't be an I/O APIC destination", dest);

    uint32_t low = (IRQ_VECTOR_BASE + isa_irq) | (pol == ACPI_ACTIVE_LOW ? REDIR_ACTIVE_LOW : 0) |
                   (trig == ACPI_LEVEL ? REDIR_LEVEL : 0); // fixed delivery, physical destination
    uint32_t pin = gsi - io->gsi_base;

    irq_handlers[isa_irq] = handler;
    idt_set_handler(IRQ_VECTOR_BASE + isa_irq, irq_dispatch);
    ioapic_write(io, IOAPIC_REDIR(pin), low | REDIR_MASKED);
    ioapic_write(io, IOAPIC_REDIR(pin) + 1, dest << 24);
    ioapic_write(io, IOAPIC_REDIR(pin), low); // unmask last
    return gsi;
}

void irq_unregister(uint8_t isa_irq) {
    if (isa_irq >= ISA_IRQ_COUNT || !irq_handlers[isa_irq]) panic("irq_unregister: IRQ %u has no handler", isa_irq);
    uint32_t gsi;
    enum acpi_polarity pol;
    enum acpi_trigger trig;
    isa_route(isa_irq, &gsi, &pol, &trig);
    const struct ioapic *io = ioapic_for_gsi(gsi);
    uint32_t pin = gsi - io->gsi_base;
    ioapic_write(io, IOAPIC_REDIR(pin), ioapic_read(io, IOAPIC_REDIR(pin)) | REDIR_MASKED);
    // Leave the IDT entry pointing at irq_dispatch, so an IRQ already on
    // its way still gets its EOI.
    irq_handlers[isa_irq] = NULL;
}

// ---------------------------------------------------------------------------
// Message-signalled interrupts
// ---------------------------------------------------------------------------

// The MSI address: 0xfee in bits 20-31, the destination APIC ID in bits
// 12-19, and redirection hint and destination mode 0 (this exact CPU, by
// physical ID). The data: the vector, with fixed delivery and edge trigger.
#define MSI_ADDRESS_BASE 0xfee00000u
#define MSI_DEST_SHIFT   12

static void msi_dispatch(struct interrupt_frame *f) {
    interrupt_handler_t h = msi_handlers[f->vector - MSI_VECTOR_BASE];
    if (h) h(f);
    lapic_eoi();
}

bool msi_vector_alloc(interrupt_handler_t handler, struct msi_message *msg) {
    if (handler == NULL) panic("msi_vector_alloc: no handler");
    uint32_t dest = info.lapic_id;
    if (dest > 0xff) return false;
    uint64_t flags = read_rflags();
    cli();
    raw_spin_lock(&msi_lock);
    for (unsigned v = MSI_VECTOR_BASE; v < MSI_VECTOR_END; v++) {
        if (msi_handlers[v - MSI_VECTOR_BASE]) continue;
        msi_handlers[v - MSI_VECTOR_BASE] = handler;
        idt_set_handler(v, msi_dispatch);
        raw_spin_unlock(&msi_lock);
        if (flags & RFLAGS_IF) sti();
        *msg = (struct msi_message){
            .vector = v, .address = MSI_ADDRESS_BASE | dest << MSI_DEST_SHIFT, .data = v};
        return true;
    }
    raw_spin_unlock(&msi_lock);
    if (flags & RFLAGS_IF) sti();
    return false;
}

void msi_vector_free(uint8_t vector) {
    if (vector < MSI_VECTOR_BASE || vector >= MSI_VECTOR_END || !msi_handlers[vector - MSI_VECTOR_BASE])
        panic("msi_vector_free: vector %u was not allocated", vector);
    // Like irq_unregister, leave the IDT entry, so a message already on its
    // way still gets its EOI.
    uint64_t flags = read_rflags();
    cli();
    raw_spin_lock(&msi_lock);
    msi_handlers[vector - MSI_VECTOR_BASE] = NULL;
    raw_spin_unlock(&msi_lock);
    if (flags & RFLAGS_IF) sti();
}

// ---------------------------------------------------------------------------

void apic_init(void) {
    if (acpi_get_info()->pcat_compat) pic_remap_and_mask();
    lapic_init();
    ioapic_init();
}

struct apic_info apic_get_info(void) { return info; }
