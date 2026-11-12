// Starting the application processors, TLB shootdowns and panic stops. See
// smp.h.

#include <apic.h>
#include <arch.h>
#include <cpu.h>
#include <kheap.h>
#include <kprintf.h>
#include <limine.h>
#include <memory.h>
#include <pmm.h>
#include <sched.h>
#include <smp.h>
#include <string.h>
#include <syscall.h>
#include <time.h>
#include <vmm.h>

#define AP_STACK_SIZE   16384 // each AP's double-fault stack
#define AP_START_MS     2000  // how long all of them get to come online
#define FLUSH_ALL_PAGES 64    // a range this long: reload CR3 instead of invlpg each page

static volatile unsigned online_count = 1;
static volatile uint64_t online_mask = 1; // the BSP, cpus[0]
static uint64_t df_stack_tops[MAX_CPUS];

unsigned smp_cpu_count(void) { return online_count; }
uint64_t smp_online_mask(void) { return online_mask; }

// ---------------------------------------------------------------------------
// Raw spinlocks and TLB shootdowns
// ---------------------------------------------------------------------------

// One shootdown at a time: the request below, and the CPUs that still have
// to do it (bit i: cpus[i]). The sender holds shootdown_lock until every
// bit is clear, so the request doesn't change under a CPU doing it.
static struct raw_spinlock shootdown_lock;
static volatile uint64_t tlb_pending;
static volatile uint64_t req_virt, req_pages;
static volatile uint64_t shootdowns, ipis_sent; // for the self-test's line

void raw_spin_lock(struct raw_spinlock *l) {
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) {
            pause();
            smp_poll();
        }
    }
}

void tlb_flush_local(uint64_t virt, uint64_t pages) {
    if (pages >= FLUSH_ALL_PAGES) {
        write_cr3(read_cr3()); // the kernel uses no global pages: this drops everything
        return;
    }
    for (uint64_t i = 0; i < pages; i++) invlpg(virt + i * PAGE_SIZE);
}

void smp_poll(void) {
    if (__atomic_load_n(&tlb_pending, __ATOMIC_ACQUIRE) == 0) return;
    // Interrupts off: the IPI handler mustn't do this request (and let the
    // sender start the next one) between this flush and its acknowledgement.
    uint64_t flags = read_rflags();
    cli();
    uint64_t bit = 1ull << this_cpu()->index;
    if (__atomic_load_n(&tlb_pending, __ATOMIC_ACQUIRE) & bit) {
        tlb_flush_local(req_virt, req_pages);
        __atomic_fetch_and(&tlb_pending, ~bit, __ATOMIC_RELEASE);
    }
    if (flags & RFLAGS_IF) sti();
}

static void tlb_ipi(struct interrupt_frame *f) {
    (void)f;
    smp_poll();
    lapic_eoi();
}

void tlb_shootdown(uint64_t pml4, uint64_t virt, uint64_t pages) {
    if (online_count < 2) return;
    uint64_t flags = read_rflags();
    cli();
    raw_spin_lock(&shootdown_lock);
    // The page-table change must be visible before cpus[].cr3 is read: a
    // CPU that loads `pml4` after this read walks the new tables anyway.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    unsigned me = this_cpu()->index;
    bool kernel = pml4 == kernel_space.pml4;
    uint64_t targets = 0;
    for (unsigned i = 0; i < MAX_CPUS; i++) {
        if (i == me || !(online_mask & (1ull << i))) continue;
        if (kernel || cpus[i].cr3 == pml4) targets |= 1ull << i;
    }
    if (targets) {
        req_virt = virt;
        req_pages = pages;
        __atomic_store_n(&tlb_pending, targets, __ATOMIC_RELEASE);
        for (unsigned i = 0; i < MAX_CPUS; i++) {
            if (!(targets & (1ull << i))) continue;
            lapic_send_ipi(cpus[i].lapic_id, IPI_TLB_VECTOR);
            ipis_sent++;
        }
        while (__atomic_load_n(&tlb_pending, __ATOMIC_ACQUIRE)) pause();
        shootdowns++;
    }
    raw_spin_unlock(&shootdown_lock);
    if (flags & RFLAGS_IF) sti();
}

// ---------------------------------------------------------------------------
// Panics
// ---------------------------------------------------------------------------

static volatile bool stopping;
static volatile uint32_t panic_apic_id = UINT32_MAX;
static volatile unsigned stopped;

bool smp_stopping(void) { return stopping; }

// This CPU's APIC ID from CPUID: works before the local APIC is set up, and
// without %gs.
static uint32_t cpuid_apic_id(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 0xb) {
        cpuid(0xb, 0, &a, &b, &c, &d);
        if (b) return d; // the x2APIC ID
    }
    cpuid(1, 0, &a, &b, &c, &d);
    return b >> 24;
}

void smp_panic_begin(void) {
    uint32_t me = cpuid_apic_id(), first = UINT32_MAX;
    if (!__atomic_compare_exchange_n(&panic_apic_id, &first, me, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (first == me) return; // a panic while printing this CPU's panic: carry on
        halt_forever();          // another CPU is panicking: let it speak
    }
    stopping = true;
    kprintf_break_lock();
    unsigned others = 0;
    for (unsigned i = 0; online_count > 1 && i < MAX_CPUS; i++) {
        if (!(online_mask & (1ull << i)) || cpus[i].lapic_id == me) continue;
        lapic_send_nmi(cpus[i].lapic_id);
        others++;
    }
    // Give them a moment to stop, so nothing else reaches the screen.
    for (uint64_t spins = 0; stopped < others && spins < 100000000; spins++) pause();
}

// The NMI from smp_panic_begin (idt.c sends it here).
__attribute__((noreturn)) void smp_stop_this_cpu(void) {
    __atomic_add_fetch(&stopped, 1, __ATOMIC_RELEASE);
    halt_forever();
}

// ---------------------------------------------------------------------------
// Starting the application processors
// ---------------------------------------------------------------------------

// Where an AP starts, on the bootloader's stack and page tables, with
// interrupts off. `extra_argument` is its index in cpus[].
__attribute__((noreturn)) static void ap_main(struct limine_mp_info *info) {
    struct cpu_local *c = &cpus[info->extra_argument];
    vmm_init_ap();                           // NX, PAT, write protect, the kernel's page tables
    gdt_init_ap(c, df_stack_tops[c->index]); // its own GDT and TSS; %gs reaches cpus[index]
    idt_load();
    syscall_init_ap();                       // syscall MSRs, SMEP, SMAP
    apic_init_ap(c->acpi_uid);
    time_init_ap();                          // this CPU's tick
    c->cr3 = kernel_space.pml4;
    c->online = true;
    __atomic_or_fetch(&online_mask, 1ull << c->index, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&online_count, 1, __ATOMIC_SEQ_CST);
    // Shootdowns reach this CPU from now on; drop whatever one sent before
    // could have left behind.
    write_cr3(read_cr3());
    sched_ap_enter();                        // becomes cpus[index].idle; never returns
}

void smp_init(struct limine_mp_response *mp, char *line, size_t size) {
    struct cpu_local *bsp = &cpus[0];
    bsp->lapic_id = lapic_id();
    bsp->online = true;
    bsp->cr3 = kernel_space.pml4;
    idt_set_handler(IPI_TLB_VECTOR, tlb_ipi);

    if (mp == NULL) {
        ksnprintf(line, size, "SMP: the bootloader started no other CPUs; 1 CPU online");
        return;
    }

    unsigned started = 0, skipped = 0, failed = 0;
    for (uint64_t i = 0; i < mp->cpu_count; i++) {
        struct limine_mp_info *info = mp->cpus[i];
        if (info->lapic_id == mp->bsp_lapic_id) {
            bsp->acpi_uid = info->processor_id;
            continue;
        }
        unsigned n = 1 + started;
        if (n >= MAX_CPUS) {
            skipped++;
            continue;
        }
        struct cpu_local *c = &cpus[n];
        uint8_t *df_stack = kmalloc(AP_STACK_SIZE);
        struct thread *idle = sched_create_idle(n);
        if (df_stack == NULL || idle == NULL) {
            kfree(df_stack);
            failed++;
            continue;
        }
        c->index = n;
        c->lapic_id = info->lapic_id;
        c->acpi_uid = info->processor_id;
        c->idle = idle;
        df_stack_tops[n] = ((uint64_t)df_stack + AP_STACK_SIZE) & ~15ull;
        info->extra_argument = n;
        // Everything the AP reads is written above; this store releases it.
        __atomic_store_n(&info->goto_address, ap_main, __ATOMIC_SEQ_CST);
        started++;
    }

    uint64_t t0 = clock_monotonic_ns();
    while (online_count < 1 + started && clock_monotonic_ns() - t0 < AP_START_MS * NS_PER_MS) thread_sleep(1);

    int len = ksnprintf(line, size, "SMP: %u CPUs online (local APICs", online_count);
    for (unsigned i = 0; i < MAX_CPUS && len < (int)size; i++)
        if (online_mask & (1ull << i)) len += ksnprintf(line + len, size - len, " %u", cpus[i].lapic_id);
    if (len < (int)size) len += ksnprintf(line + len, size - len, ")");
    if (online_count < 1 + started && len < (int)size)
        len += ksnprintf(line + len, size - len, "; %u did not start", 1 + started - online_count);
    if (skipped && len < (int)size) len += ksnprintf(line + len, size - len, "; %u over the limit of %d", skipped, MAX_CPUS);
    if (failed && len < (int)size) ksnprintf(line + len, size - len, "; %u without memory", failed);
}

// ---------------------------------------------------------------------------
// Boot self-test: a TLB shootdown, seen from every CPU
// ---------------------------------------------------------------------------

#define TEST_VIRT       (VMM_MMIO_BASE + VMM_MMIO_SIZE) // nothing else maps there
#define TEST_OLD        0x01d01d01d01d01d0ull
#define TEST_NEW        0x0e30e30e30e30e30ull
#define TEST_TIMEOUT_MS 5000

enum { PHASE_OLD, PHASE_REMAP, PHASE_NEW, PHASE_DONE };

static volatile int phase;
static volatile unsigned inside, readers_done;
static volatile uint64_t cpus_old, cpus_new; // CPUs that read the old page, the new one
static volatile uint64_t reads, stale, garbage;

// Keep reading the test page. While the page is remapped (PHASE_REMAP) no
// reader may be in the middle of a read: `inside` counts the ones that are.
static void reader(void *arg) {
    for (;;) {
        if (__atomic_load_n(&phase, __ATOMIC_SEQ_CST) == PHASE_REMAP) {
            pause();
            continue;
        }
        __atomic_add_fetch(&inside, 1, __ATOMIC_SEQ_CST);
        int ph = __atomic_load_n(&phase, __ATOMIC_SEQ_CST);
        if (ph == PHASE_OLD || ph == PHASE_NEW) {
            preempt_disable(); // so the CPU it reads on is the one noted
            uint64_t v = *(volatile uint64_t *)TEST_VIRT;
            uint64_t bit = 1ull << this_cpu()->index;
            preempt_enable();
            if (ph == PHASE_OLD) {
                if (v != TEST_OLD) __atomic_add_fetch(&garbage, 1, __ATOMIC_RELAXED);
                else if (!(cpus_old & bit)) __atomic_or_fetch(&cpus_old, bit, __ATOMIC_RELAXED);
            } else if (v == TEST_OLD) {
                __atomic_add_fetch(&stale, 1, __ATOMIC_RELAXED); // a translation the shootdown missed
            } else if (v != TEST_NEW) {
                __atomic_add_fetch(&garbage, 1, __ATOMIC_RELAXED);
            } else if (!(cpus_new & bit)) {
                __atomic_or_fetch(&cpus_new, bit, __ATOMIC_RELAXED);
            }
            __atomic_add_fetch(&reads, 1, __ATOMIC_RELAXED);
        }
        __atomic_sub_fetch(&inside, 1, __ATOMIC_SEQ_CST);
        if (ph == PHASE_DONE) break;
    }
    __atomic_add_fetch(&readers_done, 1, __ATOMIC_RELAXED);
}

static void wait_until(volatile uint64_t *mask, uint64_t want, const char *what) {
    uint64_t t0 = clock_monotonic_ns();
    while ((*mask & want) != want) {
        if (clock_monotonic_ns() - t0 > TEST_TIMEOUT_MS * NS_PER_MS)
            panic("SMP self-test: after %d ms only CPUs %lx of %lx %s (%lu stale reads of the old page)",
                  TEST_TIMEOUT_MS, *mask, want, what, stale);
        thread_sleep(1);
    }
}

void smp_self_test(char *line, size_t size) {
    uint64_t phys;
    if (vmm_virt_to_phys(&kernel_space, TEST_VIRT, &phys)) panic("SMP self-test: %p is mapped", (void *)TEST_VIRT);
    uint64_t old_page = pmm_alloc_page(), new_page = pmm_alloc_page();
    if (old_page == 0 || new_page == 0) panic("SMP self-test: no memory");
    *(uint64_t *)phys_to_virt(old_page) = TEST_OLD;
    *(uint64_t *)phys_to_virt(new_page) = TEST_NEW;
    vmm_map(&kernel_space, TEST_VIRT, old_page, VMM_WRITE);

    phase = PHASE_OLD;
    inside = readers_done = 0;
    cpus_old = cpus_new = reads = stale = garbage = 0;
    uint64_t ipis0 = ipis_sent;
    unsigned n = 2 * online_count; // more than there are CPUs, so they all keep reading
    for (unsigned i = 0; i < n; i++)
        if (thread_create("tlb reader", reader, NULL) == NULL) panic("SMP self-test: no memory");

    // Every CPU has read the old page, and has it in its TLB...
    wait_until(&cpus_old, online_mask, "read the old page");
    __atomic_store_n(&phase, PHASE_REMAP, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&inside, __ATOMIC_SEQ_CST)) thread_yield();
    // ...and after the remap, every CPU must read the new one.
    vmm_unmap(&kernel_space, TEST_VIRT);
    vmm_map(&kernel_space, TEST_VIRT, new_page, VMM_WRITE);
    __atomic_store_n(&phase, PHASE_NEW, __ATOMIC_SEQ_CST);
    wait_until(&cpus_new, online_mask, "read the new page");

    __atomic_store_n(&phase, PHASE_DONE, __ATOMIC_SEQ_CST);
    uint64_t t0 = clock_monotonic_ns();
    while (readers_done < n) {
        if (clock_monotonic_ns() - t0 > TEST_TIMEOUT_MS * NS_PER_MS) panic("SMP self-test: readers did not finish");
        thread_sleep(1);
    }
    vmm_unmap(&kernel_space, TEST_VIRT);
    pmm_free_page(old_page);
    pmm_free_page(new_page);

    if (stale || garbage)
        panic("SMP self-test: after the remap, %lu of %lu reads saw the old page (%lu saw neither)", stale, reads,
              garbage);
    ksnprintf(line, size,
              "TLB shootdown self-test passed (a kernel page remapped under %u threads reading it on %u CPUs: "
              "%lu reads, none stale; %lu IPIs)",
              n, online_count, reads, ipis_sent - ipis0);
}
