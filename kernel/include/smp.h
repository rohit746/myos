#pragma once
// Symmetric multiprocessing: starting the other CPUs, the raw spinlocks
// the lowest layers share between them, TLB shootdowns, and stopping every
// CPU on a panic.
//
// The bootloader (Limine's MP request) wakes the application processors
// (APs) and parks them; smp_init() releases each into ap_main(), which
// loads the kernel's page tables, its own GDT and TSS, the IDT, the
// system-call MSRs, enables its local APIC, starts its own timer tick and
// becomes that CPU's idle thread. From then on it takes threads from the
// scheduler's run queue like the bootstrap processor (BSP) does. Device
// interrupts (I/O APIC, MSI) all go to the BSP.
//
// TLB shootdowns: a CPU caches page translations, and changing the page
// tables in memory doesn't drop them. Whoever unmaps a page flushes its own
// TLB and, with tlb_shootdown(), every other CPU that may have the
// translation: all of them for a kernel mapping, the ones that have the
// space loaded for a user one. Those get an IPI and flush. The sender waits
// until each has: only then may the page be reused.
//
// A CPU that spins with interrupts off (on a spinlock, say) can't take
// that IPI, and if the lock's holder is the one waiting for it, neither
// would ever move. So every spin loop calls smp_poll(), which does any
// flush waiting for this CPU: waiting for a lock is never in the way of a
// shootdown.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct limine_mp_response;

// Start the CPUs in `mp` (NULL: the bootloader gave no MP response) and wait
// for them to come online. Needs the scheduler, the heap, the APIC, the
// timer and syscall_init(). Writes a line for the boot log.
void smp_init(struct limine_mp_response *mp, char *line, size_t size);

// CPUs online (running threads), and which (bit i: cpus[i]).
unsigned smp_cpu_count(void);
uint64_t smp_online_mask(void);

// ---------------------------------------------------------------------------
// Raw spinlocks: no debug checks, no preemption count. For the layers below
// the debug-checked spinlocks of sync.h (the scheduler, the lock checker,
// the console). The caller turns interrupts or preemption off, so it stays
// on this CPU while it spins.
// ---------------------------------------------------------------------------

struct raw_spinlock {
    volatile uint32_t locked;
};

void raw_spin_lock(struct raw_spinlock *l);

static inline void raw_spin_unlock(struct raw_spinlock *l) {
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

// Do the TLB flush another CPU is waiting for on this one, if any. Every
// loop that spins waiting for another CPU calls it (see above). Call with
// interrupts or preemption off.
void smp_poll(void);

// ---------------------------------------------------------------------------
// TLB shootdowns
// ---------------------------------------------------------------------------

// Drop `pages` pages from `virt` on from this CPU's TLB.
void tlb_flush_local(uint64_t virt, uint64_t pages);

// Drop them from every other CPU's TLB that may hold them: all CPUs if
// `pml4` is the kernel's (kernel mappings are in every space), else those
// with `pml4` loaded. Returns once they all have. Call after the page-table
// entries were changed; the caller flushes its own TLB.
void tlb_shootdown(uint64_t pml4, uint64_t virt, uint64_t pages);

// ---------------------------------------------------------------------------
// Panics
// ---------------------------------------------------------------------------

// First thing on the way to a panic or the crash screen, interrupts off.
// The first CPU here stops every other one (an NMI) and lets the console be
// used without its lock; another CPU that panics meanwhile halts here.
void smp_panic_begin(void);

// Another CPU panicked: this one is to stop. Doesn't use %gs (an NMI can
// land before the entry code's swapgs).
bool smp_stopping(void);
__attribute__((noreturn)) void smp_stop_this_cpu(void);

// Boot self-test: a kernel page is remapped while threads on every CPU read
// it, and none may read the old page afterwards. Writes a line for the boot
// log. Panics on failure.
void smp_self_test(char *line, size_t size);
