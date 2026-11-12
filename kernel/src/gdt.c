// Global Descriptor Table and Task State Segment.
//
// In 64-bit mode segments are mostly vestigial, but the CPU still needs:
//   - code/data descriptors for ring 0 (kernel) and ring 3 (user programs)
//   - a TSS, which tells the CPU which stack to switch to on an interrupt
//     (rsp0 when coming from user mode, IST entries for critical faults)

#include <arch.h>
#include <cpu.h>
#include <stddef.h>
#include <stdint.h>

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// One GDT and TSS per CPU: the TSS holds that CPU's rsp0 and double-fault
// stack, and a TSS descriptor is marked busy once loaded, so no two CPUs can
// share one.
static uint64_t gdts[MAX_CPUS][7];
static struct tss tsses[MAX_CPUS];
struct cpu_local cpus[MAX_CPUS];

_Static_assert(offsetof(struct cpu_local, kernel_rsp) == CPU_LOCAL_KERNEL_RSP, "isr.S and syscall.S use it");
_Static_assert(offsetof(struct cpu_local, user_rsp) == CPU_LOCAL_USER_RSP, "syscall.S uses it");
_Static_assert(offsetof(struct cpu_local, current) == CPU_LOCAL_CURRENT, "thread_current() uses it");

// The BSP's; application processors' come from the heap (smp.c).
static uint8_t kernel_int_stack[16384] __attribute__((aligned(16)));
static uint8_t double_fault_stack[16384] __attribute__((aligned(16)));

void tss_set_rsp0(uint64_t rsp0) {
    struct cpu_local *c = this_cpu();
    tsses[c->index].rsp[0] = rsp0;
    c->kernel_rsp = rsp0;
}
uint64_t tss_get_rsp0(void) { return tsses[this_cpu()->index].rsp[0]; }

void cpu_early_init(void) {
    cpus[0].self = &cpus[0];
    wrmsr(MSR_GS_BASE, (uint64_t)&cpus[0]);
}

static void load(struct cpu_local *c, uint64_t df_stack_top) {
    uint64_t *gdt = gdts[c->index];
    struct tss *tss = &tsses[c->index];
    gdt[0] = 0;                  // null descriptor (required)
    gdt[1] = 0x00af9a000000ffff; // 0x08 kernel code: present, ring 0, executable, 64-bit
    gdt[2] = 0x00cf92000000ffff; // 0x10 kernel data: present, ring 0, writable
    gdt[3] = 0x00cff2000000ffff; // 0x18 user data:   present, ring 3, writable
    gdt[4] = 0x00affa000000ffff; // 0x20 user code:   present, ring 3, executable, 64-bit

    tss->ist[IST_DOUBLE_FAULT - 1] = df_stack_top;
    tss->iopb_offset = sizeof *tss; // no I/O permission bitmap

    // The TSS descriptor is 16 bytes and takes two GDT slots.
    uint64_t base = (uint64_t)tss;
    uint64_t limit = sizeof *tss - 1;
    gdt[5] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xf) << 48) | (((base >> 24) & 0xff) << 56);
    gdt[6] = base >> 32;

    struct gdtr gdtr = {.limit = sizeof gdts[0] - 1, .base = (uint64_t)gdt};

    __asm__ volatile(
        "lgdt %0\n\t"
        // Reload CS with a far return: push the new selector and target.
        "pushq %[kcode]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %[kdata], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "xorl %%eax, %%eax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %[tsssel], %%ax\n\t"
        "ltr %%ax\n\t"
        :
        : "m"(gdtr), [kcode] "i"(GDT_KERNEL_CODE), [kdata] "i"(GDT_KERNEL_DATA),
          [tsssel] "i"(GDT_TSS)
        : "rax", "memory");

    // Loading GS above zeroed its base; point it at this CPU's data (see
    // struct cpu_local). The kernel runs with this GS base from now on.
    c->self = c;
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

void gdt_init(void) {
    load(&cpus[0], (uint64_t)(double_fault_stack + sizeof double_fault_stack));
    tss_set_rsp0((uint64_t)(kernel_int_stack + sizeof kernel_int_stack));
}

void gdt_init_ap(struct cpu_local *c, uint64_t df_stack_top) { load(c, df_stack_top); }
