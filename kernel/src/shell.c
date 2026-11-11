// Kernel shell: the line editor, the history, the command table and the
// built-in commands.
//
// The line being edited never wraps: it is cut to what fits on one row after
// the prompt, so every change can be shown by redrawing that row ("\r",
// prompt, text, erase to the end, move the cursor back). The same bytes work
// on a terminal on the serial port.

#include <acpi.h>
#include <apic.h>
#include <console.h>
#include <cpu.h>
#include <errno.h>
#include <keyboard.h>
#include <kheap.h>
#include <kprintf.h>
#include <memory.h>
#include <pmm.h>
#include <proc.h>
#include <sched.h>
#include <shell.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <vfs.h>
#include <vmm.h>

#define PROMPT     ANSI_CYAN "myos>" ANSI_RESET " "
#define PROMPT_LEN 6 // what PROMPT shows, without its escape codes

#define LINE_MAX     200
#define HISTORY_SIZE 32
#define MAX_ARGS     16

// The linker puts every SHELL_COMMAND between these (linker.ld).
extern const struct shell_command shell_commands_start[], shell_commands_end[];

static char line[LINE_MAX + 1]; // what is being typed, NUL-terminated
static size_t len, pos;         // its length, and the cursor's place in it
static size_t limit;            // the longest line that fits on one row

// The last HISTORY_SIZE lines run, oldest first in the ring.
static char history[HISTORY_SIZE][LINE_MAX + 1];
static unsigned history_count; // lines added since boot
static unsigned browsing;      // 0: a new line; n: showing the n-th newest history line
static char draft[LINE_MAX + 1]; // the new line, kept while browsing the history

// ---------------------------------------------------------------------------
// Line editing
// ---------------------------------------------------------------------------

static void redraw(void) {
    kprintf("\r" PROMPT "%s\033[K", line);
    if (pos < len) kprintf("\033[%luD", len - pos);
}

static void set_line(const char *s) {
    len = strlen(s);
    if (len > limit) len = limit;
    memcpy(line, s, len);
    line[len] = '\0';
    pos = len;
}

static void new_line(void) {
    set_line("");
    browsing = 0;
    kprintf(PROMPT);
}

static void insert(char c) {
    if (len == limit) return;
    memmove(line + pos + 1, line + pos, len - pos + 1);
    line[pos++] = c;
    len++;
}

// Delete `n` characters starting at `at`.
static void remove_chars(size_t at, size_t n) {
    memmove(line + at, line + at + n, len - at - n + 1);
    len -= n;
}

// The n-th newest line in the history, from 1.
static const char *history_line(unsigned n) {
    return history[(history_count - n) % HISTORY_SIZE];
}

static void history_add(const char *s) {
    if (s[0] == '\0') return;
    if (history_count > 0 && strcmp(history_line(1), s) == 0) return;
    char *slot = history[history_count % HISTORY_SIZE];
    size_t n = strlen(s);
    memcpy(slot, s, n + 1);
    history_count++;
}

// Up (`older`) or Down through the history. Down from the newest line
// brings back the line that was being typed.
static void history_move(bool older) {
    unsigned kept = history_count < HISTORY_SIZE ? history_count : HISTORY_SIZE;
    if (older ? browsing == kept : browsing == 0) return;
    if (browsing == 0) memcpy(draft, line, len + 1);
    if (older) browsing++;
    else browsing--;
    set_line(browsing ? history_line(browsing) : draft);
}

// ---------------------------------------------------------------------------
// Running commands
// ---------------------------------------------------------------------------

static const struct shell_command *find_command(const char *name) {
    for (const struct shell_command *c = shell_commands_start; c < shell_commands_end; c++)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

// Split `s` into words in place. Spaces separate words, except inside
// double quotes, which are removed. Returns the number of words, or -1 if
// there are more than `max`.
static int split(char *s, char **argv, int max) {
    int argc = 0;
    for (;;) {
        while (*s == ' ') s++;
        if (*s == '\0') return argc;
        if (argc == max) return -1;
        argv[argc++] = s;
        char *out = s;
        bool quoted = false;
        for (; *s && (quoted || *s != ' '); s++) {
            if (*s == '"') quoted = !quoted;
            else *out++ = *s;
        }
        if (*s) s++; // past the space; `out` is behind it, so ending the word there is safe
        *out = '\0';
    }
}

static void run(char *s) {
    char *argv[MAX_ARGS + 1];
    int argc = split(s, argv, MAX_ARGS);
    if (argc == 0) return;
    if (argc < 0) {
        kprintf("too many words (at most %d)\n", MAX_ARGS);
        return;
    }
    argv[argc] = NULL;
    const struct shell_command *c = find_command(argv[0]);
    if (c) {
        c->run(argc, argv);
        return;
    }
    // A path runs that program; another word runs /bin/<word>, if it exists.
    bool is_path = false;
    for (const char *p = argv[0]; *p; p++)
        if (*p == '/') is_path = true;
    char path[VFS_PATH_MAX + 1];
    ksnprintf(path, sizeof path, is_path ? "%s" : "/bin/%s", argv[0]);
    struct vfs_stat st;
    if (!is_path && vfs_stat(path, &st) != 0) {
        kprintf("%s: unknown command; `help` lists them\n", argv[0]);
        return;
    }
    int status;
    int err = process_run(path, argv, &status);
    if (err) kprintf("%s: %s\n", argv[0], strerror(err));
    else if (status > 0) kprintf("%s: exited with status %d\n", argv[0], status);
}

static void enter(void) {
    // Show the whole line, with the cursor at its end, before moving on.
    pos = len;
    redraw();
    kprintf("\n");
    history_add(line);
    run(line); // may cut `line` into words
    new_line();
}

static void key(const struct key_event *ev) {
    if (!ev->pressed) return;
    switch (ev->key) {
    case KEY_LEFT: if (pos > 0) pos--; break;
    case KEY_RIGHT: if (pos < len) pos++; break;
    case KEY_HOME: pos = 0; break;
    case KEY_END: pos = len; break;
    case KEY_UP: history_move(true); break;
    case KEY_DOWN: history_move(false); break;
    case KEY_DELETE: if (pos < len) remove_chars(pos, 1); break;
    default:
        switch (ev->ch) {
        case 0: return; // a modifier, F-key, ...
        case '\n': enter(); return;
        case '\b':
            if (pos > 0) remove_chars(--pos, 1);
            break;
        case 'a' & 0x1f: pos = 0; break;   // Ctrl+A: start of line
        case 'e' & 0x1f: pos = len; break; // Ctrl+E: end of line
        case 'u' & 0x1f: // Ctrl+U: delete to the start of the line
            remove_chars(0, pos);
            pos = 0;
            break;
        case 'c' & 0x1f: // Ctrl+C: drop the line
            kprintf("^C\n");
            new_line();
            return;
        case 'l' & 0x1f: // Ctrl+L: clear the screen, keep the line
            kprintf("\033[2J\033[H");
            break;
        default:
            if (ev->ch >= ' ' && ev->ch <= '~') insert(ev->ch);
            else return;
        }
    }
    redraw();
}

void shell_run(void) {
    unsigned cols = console_get_info().cols; // 0 without a screen: the serial terminal wraps itself
    limit = LINE_MAX;
    if (cols > PROMPT_LEN + 16 && cols - PROMPT_LEN - 1 < limit) limit = cols - PROMPT_LEN - 1;

    new_line();
    for (;;) {
        struct key_event ev;
        while (keyboard_read(&ev)) key(&ev);
        thread_sleep(1); // other threads run meanwhile; a key waits a tick or two
    }
}

// ---------------------------------------------------------------------------
// Built-in commands
// ---------------------------------------------------------------------------

SHELL_COMMAND(help, "", "list the commands");
static void cmd_help(int argc, char **argv) {
    // In name order: pick the smallest name after the last one shown.
    const char *last = "";
    for (;;) {
        const struct shell_command *next = NULL;
        for (const struct shell_command *c = shell_commands_start; c < shell_commands_end; c++)
            if (strcmp(c->name, last) > 0 && (next == NULL || strcmp(c->name, next->name) < 0)) next = c;
        if (next == NULL) break;
        char name[40];
        ksnprintf(name, sizeof name, "%s %s", next->name, next->usage);
        kprintf("  %-16s %s\n", name, next->help);
        last = next->name;
    }
    kprintf("Programs: a path (/bin/echo hi) runs that file; a word that isn't a command runs /bin/<word>.\n");
    kprintf("Keys: Left/Right, Home/End, Backspace/Delete, Up/Down for the history, Ctrl+C drops the line,\n"
            "Ctrl+U deletes to its start, Ctrl+L clears the screen.\n");
}

SHELL_COMMAND(clear, "", "clear the screen");
static void cmd_clear(int argc, char **argv) {
    kprintf("\033[2J\033[H");
}

SHELL_COMMAND(echo, "[words...]", "print the words");
static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
    kprintf("\n");
}

static void print_date(int64_t unix_s) {
    struct date d;
    unix_to_date(unix_s, &d);
    kprintf("%04d-%02d-%02d %02d:%02d:%02d UTC", d.year, d.month, d.day, d.hour, d.minute, d.second);
}

SHELL_COMMAND(uptime, "", "time since boot, and the date");
static void cmd_uptime(int argc, char **argv) {
    uint64_t ns = clock_monotonic_ns();
    uint64_t s = ns / NS_PER_SEC;
    kprintf("up %lu:%02lu:%02lu (%lu.%03lu s, %lu ticks at %d Hz)\n", s / 3600, s / 60 % 60, s % 60, s,
            (uint64_t)(ns / NS_PER_MS % 1000), timer_ticks(), TIMER_HZ);
    int64_t now;
    if (clock_realtime_ns(&now)) {
        kprintf("date: ");
        print_date(now / (int64_t)NS_PER_SEC);
        kprintf("\n");
    } else {
        kprintf("date: unknown, the bootloader gave none\n");
    }
}

SHELL_COMMAND(mem, "", "physical memory and kernel heap usage");
static void cmd_mem(int argc, char **argv) {
    struct pmm_stats p = pmm_get_stats();
    uint64_t used = p.total_pages - p.free_pages;
    kprintf("physical: %lu MiB managed, %lu KiB used (%lu pages, %lu reserved), %lu MiB free (%lu pages)\n",
            p.total_pages * PAGE_SIZE >> 20, used * PAGE_SIZE >> 10, used, p.reserved_pages,
            p.free_pages * PAGE_SIZE >> 20, p.free_pages);
    struct kheap_stats h = kheap_get_stats();
    kprintf("heap:     %lu KiB mapped, %lu bytes in %lu allocations, %lu KiB free (free blocks: %lu, largest %lu KiB)\n",
            h.heap_bytes >> 10, h.used_bytes, h.allocations, h.free_bytes >> 10, h.free_blocks,
            h.largest_free >> 10);
    kprintf("          %lu allocations and %lu frees since boot\n", h.total_allocs, h.total_frees);
}

static void mhz(const char *label, uint64_t hz) {
    kprintf("%s%lu.%03lu MHz", label, hz / 1000000, hz / 1000 % 1000);
}

SHELL_COMMAND(cpu, "", "what the CPU is and what it can do");
static void cmd_cpu(int argc, char **argv) {
    struct cpu_ident id;
    cpu_identify(&id);
    kprintf("cpu:      %s (%s, family %u, model %u, stepping %u)\n", id.brand, id.vendor, id.family, id.model,
            id.stepping);

    const struct acpi_info *acpi = acpi_get_info();
    struct apic_info apic = apic_get_info();
    kprintf("cpus:     %lu in the MADT; this is local APIC %u (%s mode)\n", acpi->cpus_enabled, apic.lapic_id,
            apic.x2apic ? "x2APIC" : "xAPIC");

    struct time_info t = time_get_info();
    mhz("clocks:   TSC ", t.tsc_hz);
    kprintf("%s", t.invariant_tsc ? " (invariant)" : "");
    mhz(", local APIC timer ", t.lapic_timer_hz);
    kprintf("\n");

    // A few CPUID feature bits worth knowing about.
    static const struct {
        uint32_t leaf;
        uint8_t reg; // 1 ecx, 2 edx, 3 ebx
        uint8_t bit;
        const char *name;
    } features[] = {
        {1, 2, 0, "fpu"},     {1, 2, 4, "tsc"},       {1, 2, 9, "apic"},      {1, 2, 25, "sse"},
        {1, 2, 26, "sse2"},   {1, 1, 0, "sse3"},      {1, 1, 9, "ssse3"},     {1, 1, 19, "sse4.1"},
        {1, 1, 20, "sse4.2"}, {1, 1, 28, "avx"},      {7, 3, 5, "avx2"},      {7, 3, 16, "avx512f"},
        {1, 1, 25, "aes"},    {1, 1, 30, "rdrand"},   {7, 3, 18, "rdseed"},   {1, 1, 21, "x2apic"},
        {1, 1, 24, "tsc-deadline"}, {1, 1, 5, "vmx"}, {1, 1, 31, "hypervisor"}, {7, 3, 7, "smep"},
        {7, 3, 20, "smap"},   {0x80000001, 2, 20, "nx"}, {0x80000001, 2, 26, "1g-pages"},
        {0x80000001, 1, 2, "svm"},
    };
    uint32_t a, b, c, d, max_leaf, max_ext;
    cpuid(0, 0, &max_leaf, &b, &c, &d);
    cpuid(0x80000000, 0, &max_ext, &b, &c, &d);
    kprintf("features:");
    for (size_t i = 0; i < sizeof features / sizeof features[0]; i++) {
        uint32_t leaf = features[i].leaf;
        if (leaf < 0x80000000 ? leaf > max_leaf : leaf > max_ext) continue;
        cpuid(leaf, 0, &a, &b, &c, &d);
        uint32_t r = features[i].reg == 1 ? c : features[i].reg == 2 ? d : b;
        if (r & (1u << features[i].bit)) kprintf(" %s", features[i].name);
    }
    kprintf("\n");
}

SHELL_COMMAND(acpi, "", "the ACPI tables and what they say about the machine");
static void cmd_acpi(int argc, char **argv) {
    const struct acpi_info *acpi = acpi_get_info();
    kprintf("ACPI %s (OEM %s): %lu tables via the %s\n", acpi->revision >= 2 ? "2.0+" : "1.0", acpi->oem_id,
            acpi->table_count, acpi->xsdt ? "XSDT" : "RSDT");
    acpi_print_summary();
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

static const char *type_name(enum vnode_type t) {
    return t == VNODE_DIR ? "directory" : t == VNODE_CHARDEV ? "character device" : "file";
}

// One line of `ls`: "d          etc/", "-       42  motd", "c          null".
static void ls_line(const char *name, const struct vfs_stat *st) {
    char size[24] = "";
    if (st->type == VNODE_FILE) ksnprintf(size, sizeof size, "%lu", st->size);
    char kind = st->type == VNODE_DIR ? 'd' : st->type == VNODE_CHARDEV ? 'c' : '-';
    kprintf("%c %10s  %s%s\n", kind, size, name, st->type == VNODE_DIR ? "/" : "");
}

SHELL_COMMAND(ls, "[path]", "list a directory (/ by default)");
static void cmd_ls(int argc, char **argv) {
    if (argc > 2) {
        kprintf("usage: ls [path]\n");
        return;
    }
    const char *path = argc == 2 ? argv[1] : "/";
    struct vfs_stat st;
    int err = vfs_stat(path, &st);
    if (err == 0 && st.type != VNODE_DIR) {
        ls_line(path, &st);
        return;
    }
    int fd = err ? err : vfs_open(path, O_RDONLY);
    if (fd < 0) {
        kprintf("ls: %s: %s\n", path, strerror(fd));
        return;
    }
    struct vfs_dirent d;
    while ((err = vfs_readdir(fd, &d)) > 0) {
        char child[VFS_PATH_MAX + 1];
        ksnprintf(child, sizeof child, "%s/%s", path, d.name);
        if (vfs_stat(child, &st) == 0) ls_line(d.name, &st);
        else kprintf("? %10s  %s\n", "", d.name);
    }
    if (err < 0) kprintf("ls: %s: %s\n", path, strerror(err));
    vfs_close(fd);
}

// Whether Ctrl+C was pressed since the command started. Other keys typed
// meanwhile are dropped.
static bool interrupted(void) {
    struct key_event ev;
    bool hit = false;
    while (keyboard_read(&ev))
        if (ev.pressed && ev.ch == ('c' & 0x1f)) hit = true;
    return hit;
}

SHELL_COMMAND(cat, "<path...>", "print files (Ctrl+C stops)");
static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: cat <path...>\n");
        return;
    }
    bool stop = false;
    for (int i = 1; i < argc && !stop; i++) {
        int fd = vfs_open(argv[i], O_RDONLY);
        if (fd < 0) {
            kprintf("cat: %s: %s\n", argv[i], strerror(fd));
            continue;
        }
        char buf[512];
        int64_t n = 0;
        while (!stop && (n = vfs_read(fd, buf, sizeof buf)) > 0) {
            kwrite(buf, (size_t)n);
            if (interrupted()) {
                kprintf("^C\n");
                stop = true;
            }
        }
        if (n < 0) kprintf("cat: %s: %s\n", argv[i], strerror((int)n));
        vfs_close(fd);
    }
}

SHELL_COMMAND(mkdir, "<path...>", "make directories");
static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) kprintf("usage: mkdir <path...>\n");
    for (int i = 1; i < argc; i++) {
        int err = vfs_mkdir(argv[i]);
        if (err) kprintf("mkdir: %s: %s\n", argv[i], strerror(err));
    }
}

SHELL_COMMAND(stat, "<path...>", "what a path is: type, size, inode, filesystem");
static void cmd_stat(int argc, char **argv) {
    if (argc < 2) kprintf("usage: stat <path...>\n");
    for (int i = 1; i < argc; i++) {
        struct vfs_stat st;
        int err = vfs_stat(argv[i], &st);
        if (err) {
            kprintf("stat: %s: %s\n", argv[i], strerror(err));
            continue;
        }
        kprintf("%s: %s, %lu %s, inode %lu on %s\n", argv[i], type_name(st.type), st.size,
                st.type == VNODE_DIR ? "entries" : "bytes", st.ino, st.fs);
    }
}

// Give a reset method time to work before trying the next one.
#define RESET_WAIT_MS 100

SHELL_COMMAND(reboot, "", "restart the machine");
static void cmd_reboot(int argc, char **argv) {
    kprintf("Rebooting...\n");
    cli();
    const struct acpi_info *acpi = acpi_get_info();

    // 1. The FADT's reset register, the way the firmware says to do it.
    if (acpi->reset_supported) {
        const struct acpi_gas *r = &acpi->reset_reg;
        if (r->space == ACPI_SPACE_IO) outb((uint16_t)r->address, acpi->reset_value);
        else if (r->space == ACPI_SPACE_MEMORY) *(volatile uint8_t *)vmm_map_mmio(r->address, 1) = acpi->reset_value;
        sleep_ms(RESET_WAIT_MS);
    }

    // 2. Have the PS/2 controller pulse the CPU's reset line.
    if (acpi->has_8042) {
        for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++) pause(); // input buffer full
        outb(0x64, 0xfe);
        sleep_ms(RESET_WAIT_MS);
    }

    // 3. The chipset's Reset Control Register: a full (hard) reset.
    outb(0xcf9, 0x02);
    outb(0xcf9, 0x06);
    sleep_ms(RESET_WAIT_MS);

    // 4. A triple fault: with an empty IDT, the breakpoint can't be delivered.
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } empty = {0, 0};
    __asm__ volatile("lidt %0; int3" : : "m"(empty) : "memory");
    kprintf("reboot: nothing worked; halting\n");
    halt_forever();
}

SHELL_COMMAND(poweroff, "", "turn the machine off");
static void cmd_poweroff(int argc, char **argv) {
    if (!acpi_get_info()->can_power_off) {
        kprintf("poweroff: the ACPI tables don't say how (`acpi` shows why); turn it off by hand\n");
        return;
    }
    kprintf("Powering off...\n");
    cli();
    acpi_power_off();
    sti();
    kprintf("poweroff: the machine is still on; turn it off by hand\n");
}

SHELL_COMMAND(panic, "[fault]", "crash on purpose: a kernel panic, or a page fault for the crash screen");
static void cmd_panic(int argc, char **argv) {
    if (argc == 1) panic("panic command");
    if (argc == 2 && strcmp(argv[1], "fault") == 0) {
        kprintf("writing to an unmapped address...\n");
        *(volatile uint64_t *)0xdeadbeef000 = 42;
        kprintf("panic: the write did not fault\n");
        return;
    }
    kprintf("usage: panic [fault]\n");
}
