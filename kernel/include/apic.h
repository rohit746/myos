#pragma once
// Interrupt controllers: the local APIC (one per CPU) and the I/O APICs that
// route device IRQs to it. The legacy 8259 PICs are remapped and masked.
//
// Vectors:
//   0x00-0x1f  CPU exceptions
//   0x20-0x2f  the 8259 PICs, remapped out of the way and fully masked
//   0x30-0x3f  ISA IRQs 0-15, through the I/O APIC (IRQ_VECTOR_BASE + irq)
//   0x40-0xef  message-signalled interrupts (PCI MSI and MSI-X), handed out
//              by msi_vector_alloc()
//   0xf0       local APIC timer
//   0xff       local APIC spurious interrupt

#include <arch.h>
#include <stdbool.h>
#include <stdint.h>

#define PIC_VECTOR_BASE      0x20
#define IRQ_VECTOR_BASE      0x30
#define MSI_VECTOR_BASE      0x40
#define MSI_VECTOR_END       0xf0 // one past the last
#define LAPIC_TIMER_VECTOR   0xf0
#define APIC_SPURIOUS_VECTOR 0xff

#define ISA_IRQ_COUNT 16

// Remap and mask the 8259 PICs, enable this CPU's local APIC (in x2APIC mode
// when the CPU has it), and mask every I/O APIC pin. Interrupts stay off;
// the caller does sti() when it is ready. Needs ACPI and the VMM.
void apic_init(void);

// Tell the local APIC the current interrupt has been handled. irq_register
// handlers don't need this: it is sent for them.
void lapic_eoi(void);

// This CPU's local APIC ID (the x2APIC ID in x2APIC mode).
uint32_t lapic_id(void);

// The local APIC timer counts down at its input clock divided by
// LAPIC_TIMER_DIVIDER. Its rate is not architectural: time.c measures it.
#define LAPIC_TIMER_DIVIDER 16

// Load the timer with `count`. A periodic timer reloads `count` each time it
// reaches 0; a one-shot one stops there. With `interrupt` it raises
// LAPIC_TIMER_VECTOR at 0, otherwise it only counts.
void lapic_timer_start(uint32_t count, bool periodic, bool interrupt);

// What the timer has left to count down (0 when a one-shot one is done).
uint32_t lapic_timer_count(void);

// Route ISA IRQ `isa_irq` (0-15) to `handler` on this CPU and unmask it.
// The MADT's interrupt source overrides decide the GSI, polarity and trigger
// mode. The local APIC gets its EOI after `handler` returns. Returns the GSI
// the IRQ was wired to. Panics if the IRQ is taken or has no I/O APIC pin.
uint32_t irq_register(uint8_t isa_irq, interrupt_handler_t handler);

// Mask ISA IRQ `isa_irq` again and drop its handler.
void irq_unregister(uint8_t isa_irq);

// Message-signalled interrupts: a device raises one by writing `data` to
// `address`, which the local APIC turns into an interrupt on `vector`.
// msi_vector_alloc() takes a free vector in 0x40-0xef for `handler` (the
// local APIC gets its EOI after `handler` returns) and fills in the message
// that raises it on this CPU, edge-triggered, fixed delivery. It returns
// false when every vector is taken, or when this CPU's APIC ID is above 255,
// which an MSI can't name without interrupt remapping.
struct msi_message {
    uint8_t vector;
    uint64_t address;
    uint32_t data;
};

bool msi_vector_alloc(interrupt_handler_t handler, struct msi_message *msg);

// Give the vector back. The device must not raise it any more.
void msi_vector_free(uint8_t vector);

struct apic_info {
    bool x2apic;            // the local APIC is in x2APIC mode (MSRs, not MMIO)
    uint32_t lapic_id;
    uint8_t lapic_version;
    uint8_t lvt_count;
    uint64_t lapic_phys;    // xAPIC register base (unused in x2APIC mode)
    uint32_t ioapic_pins;   // redirection entries over all I/O APICs
    uint64_t spurious;      // spurious interrupts seen so far
};

struct apic_info apic_get_info(void);
