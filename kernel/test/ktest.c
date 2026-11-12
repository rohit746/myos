// The KTEST runner (ktest.h). Only built by `make test-kernel`.

#include <cpu.h>
#include <kprintf.h>
#include <ktest.h>
#include <stdarg.h>
#include <stddef.h>

// QEMU's isa-debug-exit device (scripts/ktest.sh gives QEMU one).
#define DEBUG_EXIT_PORT 0xf4

// Every KTEST, placed between these by the linker script.
extern const struct ktest ktests_start[], ktests_end[];

void ktest_fail(struct ktest_state *s, const char *file, int line, const char *fmt, ...) {
    s->failed = true;
    kprintf("#   %s:%d: ", file, line);
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");
}

void ktest_run_all(void) {
    size_t count = (size_t)(ktests_end - ktests_start), failed = 0;
    kprintf("\nTAP version 13\n1..%zu\n", count);
    for (size_t i = 0; i < count; i++) {
        const struct ktest *t = &ktests_start[i];
        struct ktest_state s = {.failed = false};
        t->fn(&s);
        if (s.failed) failed++;
        kprintf("%s %zu - %s\n", s.failed ? "not ok" : "ok", i + 1, t->name);
    }
    kprintf("# %zu passed, %zu failed\n", count - failed, failed);
    // QEMU exits soon after (not always before the next instruction). On a
    // machine without the device, this just halts.
    outb(DEBUG_EXIT_PORT, failed ? 1 : 0);
    halt_forever();
}
