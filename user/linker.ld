/* User programs: static executables at 0x400000. Three segments, each
 * starting on its own page so the kernel can map each with its own
 * permissions (FLAGS: 4 = read, 2 = write, 1 = execute). The first one also
 * holds the ELF header and program headers, so the program can find them
 * (AT_PHDR in the auxiliary vector). */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FILEHDR PHDRS FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x400000 + SIZEOF_HEADERS;
    .text : { *(.text .text.*) } :text

    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) } :rodata

    . = ALIGN(4096);
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data

    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
}
