// Function names for addresses (backtrace.h).

#include <backtrace.h>
#include <kprintf.h>
#include <ktest.h>
#include <string.h>

extern char text_start[], text_end[];

// The start of a function, and an address inside it, give its name.
KTEST(symbolize_names_functions) {
    uint64_t offset = 99;
    const char *name = symbolize((uint64_t)strlen, &offset);
    KASSERT(name != NULL);
    KEXPECT_EQ(strcmp(name, "strlen"), 0);
    KEXPECT_EQ(offset, 0);
    name = symbolize((uint64_t)ksnprintf + 5, &offset);
    KASSERT(name != NULL);
    KEXPECT_EQ(strcmp(name, "ksnprintf"), 0);
    KEXPECT_EQ(offset, 5);
}

// Addresses outside the kernel's code have no name.
KTEST(symbolize_rejects_other_addresses) {
    static int data;
    uint64_t offset;
    KEXPECT_EQ(symbolize(0, &offset), NULL);
    KEXPECT_EQ(symbolize((uint64_t)&data, &offset), NULL);
    KEXPECT_EQ(symbolize((uint64_t)text_end, &offset), NULL);
    KASSERT(symbolize((uint64_t)text_start, &offset) != NULL);
}
