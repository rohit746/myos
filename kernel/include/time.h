#pragma once
// Timekeeping: a periodic tick, a monotonic clock, busy-wait sleeps and the
// wall-clock date.
//
// The TSC and the local APIC timer run at rates nothing tells us reliably,
// so time_init() measures them against a clock with a known rate: the HPET,
// else the ACPI PM timer, else the PIT. The local APIC timer then drives a
// TIMER_HZ tick, and clock_monotonic_ns() reads the TSC when it is
// invariant (constant rate, keeps counting when the CPU sleeps), else the
// HPET, else the TSC anyway.

#include <stdbool.h>
#include <stdint.h>

#define TIMER_HZ 1000

#define NS_PER_SEC 1000000000ull
#define NS_PER_MS  1000000ull

// Calibrate the TSC and local APIC timer, then start the tick. `have_date`
// says whether `boot_unix_time` (seconds since 1970, UTC, from the
// bootloader) is known. Needs ACPI and the APIC. The tick starts counting
// once interrupts are on.
void time_init(bool have_date, int64_t boot_unix_time);

// Nanoseconds since time_init(). Never goes backwards. 0 before time_init().
uint64_t clock_monotonic_ns(void);

// Ticks since time_init(): TIMER_HZ a second, while interrupts are on.
uint64_t timer_ticks(void);

// Wait at least `ms` milliseconds, spinning. Works with interrupts off.
void sleep_ms(uint64_t ms);

// Wall-clock time, in nanoseconds since 1970 (UTC, if the firmware's clock
// is in UTC). False if the bootloader gave no date.
bool clock_realtime_ns(int64_t *ns);

struct date {
    int year, month, day; // month and day from 1
    int hour, minute, second;
};

// Split seconds since 1970 into a calendar date (proleptic Gregorian, UTC).
void unix_to_date(int64_t seconds, struct date *d);

struct time_info {
    const char *calibrated_with; // "HPET", "ACPI PM timer" or "PIT"
    const char *clock_source;    // what clock_monotonic_ns() reads: "TSC" or "HPET"
    uint64_t tsc_hz;
    bool invariant_tsc;
    uint64_t lapic_timer_hz;     // after the LAPIC_TIMER_DIVIDER
    uint32_t tick_count;         // local APIC timer count per tick
};

struct time_info time_get_info(void);
