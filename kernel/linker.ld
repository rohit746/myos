/* Kernel memory layout. Limine maps us at the top 2 GiB of the address space. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

/* Separate segments so Limine can map each with the right permissions
 * (FLAGS: 4 = read, 2 = write, 1 = execute). vmm.c maps the same ranges
 * again in the kernel's own page tables, using the *_start/_end symbols. */
PHDRS
{
    limine_requests PT_LOAD FLAGS(6);
    text            PT_LOAD FLAGS(5);
    rodata          PT_LOAD FLAGS(4);
    data            PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0xffffffff80000000;
    kernel_start = .;

    requests_start = .;
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests
    requests_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    text_start = .;
    .text : {
        *(.text .text.*)
    } :text
    text_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)

        /* The shell's command table: every SHELL_COMMAND (shell.h). */
        . = ALIGN(8);
        shell_commands_start = .;
        KEEP(*(.shell_commands))
        shell_commands_end = .;

        /* The PCI driver table: every PCI_DRIVER (pci.h). */
        . = ALIGN(8);
        pci_drivers_start = .;
        KEEP(*(.pci_drivers))
        pci_drivers_end = .;

        /* The kernel's unit tests: every KTEST (ktest.h). Empty unless the
         * kernel is built with -DMYOS_KTEST. */
        . = ALIGN(8);
        ktests_start = .;
        KEEP(*(.ktests))
        ktests_end = .;
    } :rodata
    rodata_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    data_start = .;
    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    data_end = .;

    kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
