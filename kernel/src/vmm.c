// Virtual memory manager: builds the kernel's own page tables and switches to
// them, so the kernel no longer depends on the ones Limine left behind.
//
// x86-64 4-level paging: a virtual address picks one entry in each of four
// 512-entry tables (PML4 -> PDPT -> PD -> PT), 9 bits per level, and the low
// 12 bits are the offset in a 4 KiB page. A PDPT or PD entry with PS set maps
// a whole 1 GiB or 2 MiB page instead of pointing to the next table.
//
// Table pages come from the PMM and are reached through the HHDM, which is
// mapped at the same offset in Limine's tables and in ours.

#include <arch.h>
#include <cpu.h>
#include <kprintf.h>
#include <limine.h>
#include <memory.h>
#include <pmm.h>
#include <string.h>
#include <vmm.h>

// Page-table entry bits.
#define PTE_PRESENT   (1ull << 0)
#define PTE_WRITE     (1ull << 1)
#define PTE_USER      (1ull << 2)
#define PTE_PWT       (1ull << 3)
#define PTE_PCD       (1ull << 4)
#define PTE_HUGE      (1ull << 7)  // PS in a PDPT/PD entry
#define PTE_PAT_4K    (1ull << 7)  // the PAT bit sits here in a 4 KiB entry...
#define PTE_PAT_LARGE (1ull << 12) // ...and here in a 2 MiB / 1 GiB entry
#define PTE_NX        (1ull << 63)
#define PTE_ADDR      0x000ffffffffff000ull

#define SIZE_2M (2ull << 20)
#define SIZE_1G (1ull << 30)

#define MSR_EFER  0xc0000080
#define EFER_NXE  (1ull << 11)
#define MSR_PAT   0x277
#define CR0_WP    (1ull << 16)

// Memory types in the PAT, chosen to match what Limine sets up (PAT0..5) so
// nothing already mapped changes type when we write the MSR:
//   0 WB  1 WT  2 UC-  3 UC  4 WP  5 WC  6 UC-  7 UC
// A page picks an entry with its PAT, PCD and PWT bits (index = PAT:PCD:PWT).
#define PAT_UC  0x00
#define PAT_WC  0x01
#define PAT_WT  0x04
#define PAT_WP  0x05
#define PAT_WB  0x06
#define PAT_UCM 0x07
#define PAT_LAYOUT                                                                              \
    ((uint64_t)PAT_WB | (uint64_t)PAT_WT << 8 | (uint64_t)PAT_UCM << 16 | (uint64_t)PAT_UC << 24 | \
     (uint64_t)PAT_WP << 32 | (uint64_t)PAT_WC << 40 | (uint64_t)PAT_UCM << 48 | (uint64_t)PAT_UC << 56)

struct vmm_space kernel_space;

static uint64_t nx_bit; // PTE_NX when the CPU has NX, else 0 (the bit is reserved)
static bool have_1g_pages;
static uint64_t kernel_phys_base, kernel_virt_base;
static struct vmm_stats stats;

// Section bounds from kernel/linker.ld.
extern char requests_start[], requests_end[];
extern char text_start[], text_end[];
extern char rodata_start[], rodata_end[];
extern char data_start[], data_end[];
extern char kernel_end[];

static inline unsigned pt_index(uint64_t virt, int level) {
    return (virt >> (PAGE_SHIFT + 9 * (level - 1))) & 511;
}

static inline uint64_t level_size(int level) { return 1ull << (PAGE_SHIFT + 9 * (level - 1)); }

static uint64_t alloc_table(void) {
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) panic("vmm: out of memory for page tables");
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    stats.table_pages++;
    return phys;
}

// The entry that maps `virt` at `level` (1 = PT, 2 = PD, 3 = PDPT), creating
// the tables above it when `create` is set. NULL if one is missing otherwise.
static uint64_t *walk(struct vmm_space *s, uint64_t virt, int level, bool create, bool user) {
    uint64_t *table = phys_to_virt(s->pml4);
    for (int l = 4; l > level; l--) {
        uint64_t *e = &table[pt_index(virt, l)];
        if (!(*e & PTE_PRESENT)) {
            if (!create) return NULL;
            // Intermediate entries allow everything; the leaf decides.
            *e = alloc_table() | PTE_PRESENT | PTE_WRITE;
        }
        if (*e & PTE_HUGE)
            panic("vmm: %p is inside a %s page", (void *)virt, l == 3 ? "1 GiB" : "2 MiB");
        if (user) *e |= PTE_USER;
        table = phys_to_virt(*e & PTE_ADDR);
    }
    return &table[pt_index(virt, level)];
}

static uint64_t leaf_bits(uint32_t flags, int level) {
    uint64_t b = PTE_PRESENT;
    if (flags & VMM_WRITE) b |= PTE_WRITE;
    if (flags & VMM_USER) b |= PTE_USER;
    if (!(flags & VMM_EXEC)) b |= nx_bit;
    if (flags & VMM_WC) b |= (level > 1 ? PTE_PAT_LARGE : PTE_PAT_4K) | PTE_PWT; // PAT5
    else if (flags & VMM_UC) b |= PTE_PCD | PTE_PWT;                             // PAT3
    if (level > 1) b |= PTE_HUGE;
    return b;
}

static void set_leaf(struct vmm_space *s, uint64_t virt, uint64_t phys, int level, uint32_t flags) {
    uint64_t *e = walk(s, virt, level, true, flags & VMM_USER);
    if (*e & PTE_PRESENT) panic("vmm: %p is already mapped", (void *)virt);
    *e = phys | leaf_bits(flags, level);
    if (level == 3) stats.pages_1g++;
    else if (level == 2) stats.pages_2m++;
    else stats.pages_4k++;
}

static bool is_active(struct vmm_space *s) { return (read_cr3() & PTE_ADDR) == s->pml4; }

// Map [virt, virt + size) to [phys, ...) with the largest pages that fit.
static void map_range(struct vmm_space *s, uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags) {
    while (size > 0) {
        int level = 1;
        if (have_1g_pages && ((virt | phys) & (SIZE_1G - 1)) == 0 && size >= SIZE_1G) level = 3;
        else if (((virt | phys) & (SIZE_2M - 1)) == 0 && size >= SIZE_2M) level = 2;
        set_leaf(s, virt, phys, level, flags);
        virt += level_size(level);
        phys += level_size(level);
        size -= level_size(level);
    }
}

void vmm_map(struct vmm_space *s, uint64_t virt, uint64_t phys, uint32_t flags) {
    if ((virt | phys) & (PAGE_SIZE - 1))
        panic("vmm_map: %p -> %p is not page-aligned", (void *)virt, (void *)phys);
    // Not-present entries are never cached in the TLB, so no flush is needed.
    set_leaf(s, virt, phys, 1, flags);
}

void vmm_unmap(struct vmm_space *s, uint64_t virt) {
    if (virt & (PAGE_SIZE - 1)) panic("vmm_unmap: %p is not page-aligned", (void *)virt);
    uint64_t *e = walk(s, virt, 1, false, false);
    if (e == NULL || !(*e & PTE_PRESENT)) panic("vmm_unmap: %p is not mapped", (void *)virt);
    *e = 0;
    stats.pages_4k--;
    if (is_active(s)) invlpg(virt);
}

static uint64_t mmio_next = VMM_MMIO_BASE;

volatile void *vmm_map_mmio(uint64_t phys, uint64_t size) {
    uint64_t start = ALIGN_DOWN(phys, PAGE_SIZE);
    uint64_t end = ALIGN_UP(phys + size, PAGE_SIZE);
    if (size == 0 || end <= start || end - start > VMM_MMIO_BASE + VMM_MMIO_SIZE - mmio_next)
        panic("vmm_map_mmio: no room to map %p (%lu bytes)", (void *)phys, size);
    uint64_t virt = mmio_next;
    for (uint64_t p = start; p < end; p += PAGE_SIZE)
        vmm_map(&kernel_space, virt + (p - start), p, VMM_WRITE | VMM_UC);
    mmio_next += end - start;
    return (volatile void *)(virt + (phys - start));
}

bool vmm_virt_to_phys(struct vmm_space *s, uint64_t virt, uint64_t *phys) {
    uint64_t *table = phys_to_virt(s->pml4);
    for (int level = 4; level >= 1; level--) {
        uint64_t e = table[pt_index(virt, level)];
        if (!(e & PTE_PRESENT)) return false;
        if (level == 1 || (level <= 3 && (e & PTE_HUGE))) {
            uint64_t size = level_size(level);
            *phys = (e & PTE_ADDR & ~(size - 1)) + (virt & (size - 1));
            return true;
        }
        table = phys_to_virt(e & PTE_ADDR);
    }
    return false;
}

struct vmm_stats vmm_get_stats(void) { return stats; }

// ---------------------------------------------------------------------------
// Building the kernel's tables.
// ---------------------------------------------------------------------------

static void map_kernel_section(const char *start, const char *end, uint32_t flags) {
    uint64_t v = ALIGN_DOWN((uint64_t)start, PAGE_SIZE);
    uint64_t e = ALIGN_UP((uint64_t)end, PAGE_SIZE);
    map_range(&kernel_space, v, v - kernel_virt_base + kernel_phys_base, e - v, flags);
}

// Which memory-map regions go in the HHDM: the ones Limine maps there for
// base revision 4 and later, so nothing reachable before the switch is lost.
static bool hhdm_flags(uint64_t type, uint32_t *flags) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE:
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: // boot stack, Limine's responses
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
    case LIMINE_MEMMAP_ACPI_NVS:
    case LIMINE_MEMMAP_RESERVED_MAPPED:
        *flags = VMM_WRITE;
        return true;
    case LIMINE_MEMMAP_FRAMEBUFFER:
        *flags = VMM_WRITE | VMM_WC;
        return true;
    default:
        return false;
    }
}

static void map_hhdm(struct limine_memmap_response *mm) {
    // Regions are sorted and don't overlap. Join neighbours with the same
    // flags so that more of the map fits in large pages.
    uint64_t run_start = 0, run_end = 0;
    uint32_t run_flags = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *r = mm->entries[i];
        uint32_t flags;
        if (!hhdm_flags(r->type, &flags)) continue;
        // Whole pages, like Limine (a partial page at either end is included).
        uint64_t start = ALIGN_DOWN(r->base, PAGE_SIZE);
        uint64_t end = ALIGN_UP(r->base + r->length, PAGE_SIZE);
        if (start < run_end) start = run_end; // shares a page with the previous region
        if (start >= end) continue;
        if (start == run_end && flags == run_flags) {
            run_end = end;
            continue;
        }
        if (run_end > run_start)
            map_range(&kernel_space, hhdm_offset + run_start, run_start, run_end - run_start, run_flags);
        run_start = start;
        run_end = end;
        run_flags = flags;
    }
    if (run_end > run_start)
        map_range(&kernel_space, hhdm_offset + run_start, run_start, run_end - run_start, run_flags);
    stats.hhdm_end = run_end;
}

static void program_pat(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1u << 16))) panic("vmm: CPU has no PAT");
    if (rdmsr(MSR_PAT) == PAT_LAYOUT) return;
    // Write back dirty lines before any memory type changes. The CR3 load
    // in vmm_init() then flushes the TLB.
    __asm__ volatile("wbinvd" ::: "memory");
    wrmsr(MSR_PAT, PAT_LAYOUT);
}

void vmm_init(struct limine_memmap_response *mm, uint64_t kernel_phys, uint64_t kernel_virt) {
    kernel_phys_base = kernel_phys;
    kernel_virt_base = kernel_virt;

    uint32_t a, b, c, d;
    cpuid(0x80000001, 0, &a, &b, &c, &d);
    have_1g_pages = d & (1u << 26);
    if (d & (1u << 20)) {
        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
        nx_bit = PTE_NX;
        stats.nx = true;
    }
    program_pat();

    kernel_space.pml4 = alloc_table();

    // Limine writes its responses into the requests section; we only read them.
    map_kernel_section(requests_start, requests_end, VMM_WRITE);
    map_kernel_section(text_start, text_end, VMM_EXEC);
    map_kernel_section(rodata_start, rodata_end, 0);
    map_kernel_section(data_start, data_end, VMM_WRITE);
    map_hhdm(mm);

    // Limine turns WP on; make sure it stays on so the kernel itself faults
    // when it writes to a read-only page.
    write_cr0(read_cr0() | CR0_WP);
    write_cr3(kernel_space.pml4);
}

// ---------------------------------------------------------------------------
// Boot self-test (moves into a KTEST once the test framework exists).
// ---------------------------------------------------------------------------

// Page-fault error code bits.
#define PF_PRESENT (1u << 0) // protection violation (else: page not present)
#define PF_WRITE   (1u << 1)
#define PF_FETCH   (1u << 4)

// While a probe runs, a page fault records what happened and resumes at
// `probe_resume` instead of crashing.
static volatile uint64_t probe_resume, probe_error, probe_addr;
static volatile bool probe_faulted;

static void probe_page_fault(struct interrupt_frame *f) {
    if (probe_resume == 0)
        panic("vmm self-test: unexpected page fault at %p (rip %p, error 0x%lx)", (void *)read_cr2(),
              (void *)f->rip, f->error_code);
    probe_faulted = true;
    probe_error = f->error_code;
    probe_addr = read_cr2();
    f->rip = probe_resume;
    probe_resume = 0;
}

// Read a byte and write the same value back. True if it page-faulted.
static bool probe_write(volatile uint8_t *p) {
    probe_faulted = false;
    __asm__ volatile("leaq 1f(%%rip), %%rax\n\t"
                     "movq %%rax, %[resume]\n\t"
                     "movb (%[p]), %%cl\n\t"
                     "movb %%cl, (%[p])\n"
                     "1:"
                     : [resume] "=m"(probe_resume)
                     : [p] "r"(p)
                     : "rax", "rcx", "memory");
    probe_resume = 0;
    return probe_faulted;
}

// Jump to `code`. True if fetching it page-faulted. The code must be
// `jmp *%rax` so that it comes straight back if it does run.
static bool probe_exec(const volatile uint8_t *code) {
    probe_faulted = false;
    __asm__ volatile("leaq 1f(%%rip), %%rax\n\t"
                     "movq %%rax, %[resume]\n\t"
                     "jmp *%[code]\n"
                     "1:"
                     : [resume] "=m"(probe_resume)
                     : [code] "r"(code)
                     : "rax", "memory");
    probe_resume = 0;
    return probe_faulted;
}

static void expect_fault(const char *what, bool faulted, const volatile void *addr, uint64_t error) {
    if (!faulted) panic("vmm self-test: %s did not page-fault", what);
    if (probe_addr != (uint64_t)addr)
        panic("vmm self-test: %s faulted at %p, expected %p", what, (void *)probe_addr, addr);
    if (probe_error != error)
        panic("vmm self-test: %s faulted with error 0x%lx, expected 0x%lx", what, probe_error, error);
}

// `jmp *%rax`, placed in .data so it is not executable.
__attribute__((used, section(".data.vmm_probe"))) static uint8_t data_code[] = {0xff, 0xe0};
__attribute__((used, section(".rodata.vmm_probe"))) static const uint8_t rodata_byte[] = {0x5a};
static volatile uint8_t data_byte = 1;

static void expect_phys(const char *what, uint64_t virt, uint64_t want) {
    uint64_t got;
    if (!vmm_virt_to_phys(&kernel_space, virt, &got))
        panic("vmm self-test: %s (%p) is not mapped", what, (void *)virt);
    if (got != want)
        panic("vmm self-test: %s (%p) maps to %p, expected %p", what, (void *)virt, (void *)got,
              (void *)want);
}

void vmm_self_test(void) {
    if (!is_active(&kernel_space)) panic("vmm self-test: CR3 is not the kernel's page tables");
    if (!(read_cr0() & CR0_WP)) panic("vmm self-test: CR0.WP is off");
    if (rdmsr(MSR_PAT) != PAT_LAYOUT) panic("vmm self-test: PAT is 0x%lx", rdmsr(MSR_PAT));

    // Translation: the kernel image and the HHDM (large pages, odd offset).
    expect_phys(".text", (uint64_t)text_start, kernel_phys_base + (uint64_t)(text_start - kernel_virt_base));
    expect_phys(".data", (uint64_t)&data_byte, kernel_phys_base + ((uint64_t)&data_byte - kernel_virt_base));
    uint64_t page = pmm_alloc_page();
    if (page == 0) panic("vmm self-test: out of memory");
    expect_phys("HHDM", (uint64_t)phys_to_virt(page) + 0x123, page + 0x123);

    idt_set_handler(14, probe_page_fault);

    // Map a page just past the kernel image, use it, unmap it.
    uint64_t virt = ALIGN_UP((uint64_t)kernel_end, PAGE_SIZE) + PAGE_SIZE;
    uint64_t phys;
    if (vmm_virt_to_phys(&kernel_space, virt, &phys)) panic("vmm self-test: %p already mapped", (void *)virt);
    vmm_map(&kernel_space, virt, page, VMM_WRITE);
    expect_phys("new mapping", virt + 8, page + 8);
    volatile uint64_t *v = (volatile uint64_t *)virt;
    volatile uint64_t *h = phys_to_virt(page);
    v[0] = 0x1122334455667788;
    h[1] = 0x8877665544332211;
    if (h[0] != 0x1122334455667788 || v[1] != 0x8877665544332211)
        panic("vmm self-test: new mapping and HHDM disagree");
    if (probe_write((volatile uint8_t *)virt)) panic("vmm self-test: writable mapping faulted");
    vmm_unmap(&kernel_space, virt);
    if (vmm_virt_to_phys(&kernel_space, virt, &phys)) panic("vmm self-test: page still mapped after unmap");
    expect_fault("reading an unmapped page", probe_write((volatile uint8_t *)virt), (void *)virt, 0);
    pmm_free_page(page);

    // Permissions on the kernel image.
    if (probe_write(&data_byte)) panic("vmm self-test: writing .data faulted");
    volatile uint8_t *code = (volatile uint8_t *)(uintptr_t)vmm_self_test;
    expect_fault("writing .text", probe_write(code), code, PF_PRESENT | PF_WRITE);
    volatile uint8_t *ro = (volatile uint8_t *)rodata_byte;
    expect_fault("writing .rodata", probe_write(ro), ro, PF_PRESENT | PF_WRITE);
    if (nx_bit)
        expect_fault("executing .data", probe_exec(data_code), data_code, PF_PRESENT | PF_FETCH);

    idt_set_handler(14, NULL);
}
