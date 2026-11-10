/* Kernel memory layout. Limine maps us at the top 2 GiB of the address space. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

/* Separate segments so Limine can map each with the right permissions
 * (FLAGS: 4 = read, 2 = write, 1 = execute). */
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

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
