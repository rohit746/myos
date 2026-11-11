// Timekeeping: calibrate the TSC and local APIC timer, run the tick, and
// keep the monotonic clock.
//
// Calibration runs both counters for about CAL_MS of a reference clock with
// a known rate and divides. It does that CAL_RUNS times and keeps the median
// rate, so one run stretched by an SMI or a busy host can't skew it. The
// references, best first:
//
//   HPET           memory-mapped counter, its period (in femtoseconds) in its
//                  capability register; usually 14.318 MHz or faster
//   ACPI PM timer  3.579545 MHz, 24 or 32 bits, an I/O port or MMIO
//   PIT            1.193182 MHz; channel 2 counts down once and its output is
//                  read from port 0x61 (the speaker gate, with the speaker off)

#include <acpi.h>
#include <apic.h>
#include <arch.h>
#include <cpu.h>
#include <kprintf.h>
#include <sched.h>
#include <stddef.h>
#include <time.h>
#include <vmm.h>

#define CAL_MS        50
#define CAL_RUNS      3
#define CAL_MAX_READS 5000000 // give up on a reference counter that doesn't move

// HPET registers.
#define HPET_CAP            0x000
#define HPET_CONFIG         0x010
#define HPET_COUNTER        0x0f0
#define HPET_CAP_64BIT      (1ull << 13)
#define HPET_CONFIG_ENABLE  (1ull << 0)
#define HPET_MAX_PERIOD_FS  100000000ull // the spec's slowest: 100 ns
#define FS_PER_SEC          1000000000000000ull

#define PM_TIMER_HZ 3579545

#define PIT_HZ           1193182
#define PIT_CHANNEL2     0x42
#define PIT_COMMAND      0x43
#define PIT_CH2_ONESHOT  0xb0 // channel 2, low then high byte, mode 0 (count down once)
#define PORT_B           0x61
#define PORT_B_GATE2     (1u << 0)
#define PORT_B_SPEAKER   (1u << 1)
#define PORT_B_OUT2      (1u << 5)

#define CPUID_EXT_MAX       0x80000000
#define CPUID_APM           0x80000007
#define CPUID_APM_INVTSC    (1u << 8)

enum clock_source { CLOCK_NONE, CLOCK_TSC, CLOCK_HPET };

static enum clock_source source;
static uint64_t base;   // source's reading at time_init()
static uint64_t mult;   // ns = (reading - base) * mult >> 32
static volatile uint64_t ticks;
static bool have_date;
static int64_t boot_unix_time;
static struct time_info info;

static volatile uint64_t *hpet;
static uint64_t hpet_period_fs;
static bool hpet_64bit;

// ---------------------------------------------------------------------------
// Reference clocks
// ---------------------------------------------------------------------------

struct refclock {
    const char *name;
    uint64_t hz;
    uint64_t mask; // the counter's width
    uint64_t (*read)(void);
};

static uint64_t hpet_read(void) { return hpet[HPET_COUNTER / 8]; }

static bool hpet_init(void) {
    const struct acpi_info *acpi = acpi_get_info();
    if (!acpi->have_hpet) return false;
    hpet = vmm_map_mmio(acpi->hpet_address, 0x400);
    uint64_t cap = hpet[HPET_CAP / 8];
    hpet_period_fs = cap >> 32;
    if (hpet_period_fs == 0 || hpet_period_fs > HPET_MAX_PERIOD_FS) {
        kprintf("  time: HPET reports a period of %lu fs, ignoring it\n", hpet_period_fs);
        hpet = NULL;
        return false;
    }
    hpet_64bit = cap & HPET_CAP_64BIT;
    hpet[HPET_CONFIG / 8] |= HPET_CONFIG_ENABLE; // the main counter only runs when enabled
    return true;
}

static uint16_t pm_port;
static volatile uint32_t *pm_mmio;

static uint64_t pm_timer_read(void) { return pm_mmio ? *pm_mmio : inl(pm_port); }

static bool pm_timer_init(void) {
    const struct acpi_info *acpi = acpi_get_info();
    if (!acpi->pm_timer) return false;
    const struct acpi_gas *r = &acpi->pm_timer_reg;
    if (r->space == ACPI_SPACE_MEMORY) pm_mmio = vmm_map_mmio(r->address, 4);
    else if (r->address <= 0xffff) pm_port = r->address;
    else return false;
    return true;
}

// One calibration run against a counter: TSC cycles and local APIC timer
// counts over about CAL_MS of `ref`. False if the counter didn't move.
static bool measure_counter(const struct refclock *ref, uint64_t *ns, uint64_t *tsc, uint64_t *lapic) {
    uint64_t target = ref->hz * CAL_MS / 1000;
    lapic_timer_start(UINT32_MAX, false, false);
    uint64_t r0 = ref->read() & ref->mask;
    uint64_t t0 = rdtsc();
    uint32_t l0 = lapic_timer_count();
    uint64_t r1, elapsed;
    uint32_t reads = 0;
    do {
        r1 = ref->read() & ref->mask;
        elapsed = (r1 - r0) & ref->mask;
        if (++reads > CAL_MAX_READS) return false;
    } while (elapsed < target);
    uint64_t t1 = rdtsc();
    uint32_t l1 = lapic_timer_count();
    *ns = elapsed * NS_PER_SEC / ref->hz;
    *tsc = t1 - t0;
    *lapic = l0 - l1;
    return true;
}

// The same against the PIT, which can only say when a count has run out.
static bool measure_pit(uint64_t *ns, uint64_t *tsc, uint64_t *lapic) {
    uint16_t count = PIT_HZ * CAL_MS / 1000;
    uint8_t port_b = inb(PORT_B);
    outb(PORT_B, (port_b & ~PORT_B_SPEAKER) | PORT_B_GATE2);
    outb(PIT_COMMAND, PIT_CH2_ONESHOT); // OUT2 goes low
    outb(PIT_CHANNEL2, count & 0xff);
    outb(PIT_CHANNEL2, count >> 8);     // starts counting
    lapic_timer_start(UINT32_MAX, false, false);
    uint64_t t0 = rdtsc();
    uint32_t l0 = lapic_timer_count();
    bool done = false;
    for (uint32_t reads = 0; !done && reads < CAL_MAX_READS; reads++) done = inb(PORT_B) & PORT_B_OUT2;
    uint64_t t1 = rdtsc();
    uint32_t l1 = lapic_timer_count();
    outb(PORT_B, port_b);
    if (!done) return false;
    *ns = (uint64_t)count * NS_PER_SEC / PIT_HZ;
    *tsc = t1 - t0;
    *lapic = l0 - l1;
    return true;
}

static uint64_t median3(uint64_t a, uint64_t b, uint64_t c) {
    if (a > b) { uint64_t t = a; a = b; b = t; }
    if (b > c) b = c;
    return a > b ? a : b;
}

// Measure the TSC and local APIC timer rates against `ref` (NULL: the PIT).
static bool calibrate(const struct refclock *ref) {
    uint64_t tsc_hz[CAL_RUNS], lapic_hz[CAL_RUNS];
    for (int i = 0; i < CAL_RUNS; i++) {
        uint64_t ns, tsc, lapic;
        if (!(ref ? measure_counter(ref, &ns, &tsc, &lapic) : measure_pit(&ns, &tsc, &lapic))) return false;
        if (ns == 0 || lapic == 0) return false;
        tsc_hz[i] = tsc * NS_PER_SEC / ns;
        lapic_hz[i] = lapic * NS_PER_SEC / ns;
    }
    info.tsc_hz = median3(tsc_hz[0], tsc_hz[1], tsc_hz[2]);
    info.lapic_timer_hz = median3(lapic_hz[0], lapic_hz[1], lapic_hz[2]);
    info.calibrated_with = ref ? ref->name : "PIT";
    return true;
}

// ---------------------------------------------------------------------------
// The tick and the clock
// ---------------------------------------------------------------------------

static void tick_handler(struct interrupt_frame *f) {
    (void)f;
    ticks++;
    // Before sched_tick: it may switch to another thread, which then runs
    // for a while before this handler returns.
    lapic_eoi();
    sched_tick();
}

static uint64_t source_read(void) {
    switch (source) {
    case CLOCK_TSC: return rdtsc();
    case CLOCK_HPET: return hpet_read();
    default: return 0;
    }
}

static bool invariant_tsc(void) {
    uint32_t a, b, c, d;
    cpuid(CPUID_EXT_MAX, 0, &a, &b, &c, &d);
    if (a < CPUID_APM) return false;
    cpuid(CPUID_APM, 0, &a, &b, &c, &d);
    return d & CPUID_APM_INVTSC;
}

void time_init(bool date_known, int64_t unix_time) {
    bool irqs_on = read_rflags() & RFLAGS_IF;
    cli();

    bool have_hpet = hpet_init();
    struct refclock hpet_ref = {"HPET", have_hpet ? FS_PER_SEC / hpet_period_fs : 0,
                                hpet_64bit ? UINT64_MAX : UINT32_MAX, hpet_read};
    struct refclock pm_ref = {"ACPI PM timer", PM_TIMER_HZ,
                              acpi_get_info()->pm_timer_32bit ? UINT32_MAX : 0xffffff, pm_timer_read};
    bool hpet_works = have_hpet && calibrate(&hpet_ref);
    if (!hpet_works && !(pm_timer_init() && calibrate(&pm_ref)) && !calibrate(NULL))
        panic("time: no HPET, ACPI PM timer or PIT to calibrate the timers against");

    info.invariant_tsc = invariant_tsc();
    info.tick_count = (info.lapic_timer_hz + TIMER_HZ / 2) / TIMER_HZ;
    if (info.tick_count == 0) panic("time: the local APIC timer is too slow (%lu Hz)", info.lapic_timer_hz);

    // Pick the clock and work out its ns-per-count as a 32.32 fixed-point
    // multiplier: 1e9 << 32 still fits in 64 bits. A TSC that isn't
    // invariant may stop in deep sleep states or change rate with the CPU's
    // frequency, so the HPET comes first then (if its counter is 64 bits
    // wide; a 32-bit one wraps in minutes). Without either, such a TSC still
    // beats counting ticks, which are lost whenever interrupts are off for
    // longer than one, and the kernel neither changes the CPU's frequency nor
    // sleeps deeper than hlt.
    if (info.tsc_hz == 0) panic("time: the TSC did not move during calibration");
    if (!info.invariant_tsc && hpet_works && hpet_64bit) {
        source = CLOCK_HPET;
        info.clock_source = "HPET";
        mult = (hpet_period_fs << 32) / (FS_PER_SEC / NS_PER_SEC);
    } else {
        source = CLOCK_TSC;
        info.clock_source = "TSC";
        mult = (NS_PER_SEC << 32) / info.tsc_hz;
    }

    have_date = date_known;
    boot_unix_time = unix_time;

    idt_set_handler(LAPIC_TIMER_VECTOR, tick_handler);
    ticks = 0;
    base = source_read();
    lapic_timer_start(info.tick_count, true, true);

    if (irqs_on) sti();
}

uint64_t clock_monotonic_ns(void) {
    if (source == CLOCK_NONE) return 0;
    uint64_t delta = source_read() - base;
    return (uint64_t)(((unsigned __int128)delta * mult) >> 32);
}

uint64_t timer_ticks(void) { return ticks; }

void sleep_ms(uint64_t ms) {
    if (source == CLOCK_NONE) panic("sleep_ms: called before time_init()");
    uint64_t end = clock_monotonic_ns() + ms * NS_PER_MS;
    while (clock_monotonic_ns() < end) pause();
}

bool clock_realtime_ns(int64_t *ns) {
    if (!have_date) return false;
    *ns = boot_unix_time * (int64_t)NS_PER_SEC + (int64_t)clock_monotonic_ns();
    return true;
}

// Days since 1970-01-01 to a civil date: Howard Hinnant's civil_from_days.
void unix_to_date(int64_t seconds, struct date *d) {
    int64_t days = seconds / 86400, rem = seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    d->hour = rem / 3600;
    d->minute = rem / 60 % 60;
    d->second = rem % 60;

    days += 719468; // shift the epoch to 0000-03-01
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;                                  // [0, 146096]
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);              // [0, 365]
    int64_t mp = (5 * doy + 2) / 153;                                   // [0, 11], from March
    d->day = doy - (153 * mp + 2) / 5 + 1;
    d->month = mp < 10 ? mp + 3 : mp - 9;
    d->year = yoe + era * 400 + (d->month <= 2);
}

struct time_info time_get_info(void) { return info; }
