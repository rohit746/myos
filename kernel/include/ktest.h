#pragma once
// In-kernel unit tests, run by `make test-kernel`.
//
//   KTEST(crc32_check_value) {
//       KEXPECT_EQ(crc32(0, "123456789", 9), 0xcbf43926);
//   }
//
// Each KTEST goes into the .ktests linker section, so there is no list to
// keep: the kernel built with -DMYOS_KTEST runs every test there at the end
// of boot, prints the results to the serial port as TAP, and exits QEMU with
// the outcome (isa-debug-exit). Tests live in kernel/test/, which only that
// build compiles. A KTEST in any other file compiles to nothing without
// MYOS_KTEST, so a normal kernel holds no test code.
//
// A failed check prints its file:line. KASSERT also ends the test (it
// returns from the test's function, so use it in the KTEST's own body);
// KEXPECT_EQ lets the test carry on. Either way the run goes on to the next
// test.

#include <stdbool.h>
#include <stdint.h>

struct ktest_state {
    bool failed;
};

struct ktest {
    const char *name;
    void (*fn)(struct ktest_state *ktest_state_);
};

// Mark the running test failed and print "file:line: <message>" as a TAP
// diagnostic.
__attribute__((format(printf, 4, 5))) void ktest_fail(struct ktest_state *s, const char *file, int line,
                                                      const char *fmt, ...);

#ifdef MYOS_KTEST
#define KTEST(name)                                                                                    \
    static void ktest_fn_##name(struct ktest_state *ktest_state_);                                    \
    __attribute__((used, section(".ktests"), aligned(8))) static const struct ktest ktest_##name = { \
        #name, ktest_fn_##name};                                                                       \
    static void ktest_fn_##name(struct ktest_state *ktest_state_)
#else
// Never called, so never emitted.
#define KTEST(name) __attribute__((unused)) static inline void ktest_fn_##name(struct ktest_state *ktest_state_)
#endif

#define KASSERT(cond)                                                                \
    do {                                                                             \
        if (!(cond)) {                                                               \
            ktest_fail(ktest_state_, __FILE__, __LINE__, "KASSERT(%s) failed", #cond); \
            return;                                                                  \
        }                                                                            \
    } while (0)

// Compares integers or pointers, each evaluated once.
#define KEXPECT_EQ(a, b)                                                                                 \
    do {                                                                                                 \
        int64_t ktest_a_ = (int64_t)(a), ktest_b_ = (int64_t)(b);                                        \
        if (ktest_a_ != ktest_b_)                                                                        \
            ktest_fail(ktest_state_, __FILE__, __LINE__, "KEXPECT_EQ(%s, %s) failed: %ld (0x%lx) != %ld (0x%lx)", \
                       #a, #b, ktest_a_, (uint64_t)ktest_a_, ktest_b_, (uint64_t)ktest_b_);              \
    } while (0)

// Run every KTEST, print TAP to the serial port, then exit QEMU: status 1 if
// all passed, 3 if any failed (isa-debug-exit turns the value v written to
// its port into (v << 1) | 1). Halts if there is no such device.
__attribute__((noreturn)) void ktest_run_all(void);
