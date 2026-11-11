#pragma once
// ELF64 executables: the file format user programs come in.
//
// elf_load() takes a static executable (ET_EXEC, x86-64, no interpreter)
// that is already in kernel memory and maps each PT_LOAD segment into an
// address space: fresh zeroed pages, user-accessible, writable only with
// PF_W and executable only with PF_X, with the segment's p_filesz bytes
// copied in and the rest of p_memsz (.bss) left zero.
//
// Every segment must sit in [ELF_LOAD_MIN, ELF_LOAD_END), below where mmap
// and the stack go (proc.h), and no two segments may share a page. Anything
// it can't load fails with -ENOEXEC before a page is mapped: a bad header,
// a dynamic or position-independent executable, a segment outside the file
// or out of range, an entry point outside the executable segments.
//
// See https://wiki.osdev.org/ELF and the System V ABI's x86-64 supplement.

#include <stddef.h>
#include <stdint.h>

struct vmm_space;

#define ELF_LOAD_MIN ((uint64_t)0x10000)        // the first 64 KiB stay unmapped, so NULL+n faults
#define ELF_LOAD_END ((uint64_t)0x100000000000) // USER_MMAP_BASE
#define ELF_MAX_PHDRS 64

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EV_CURRENT  1
#define ET_EXEC     2
#define ET_DYN      3
#define EM_X86_64   62

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_PHDR    6

#define PF_X 1
#define PF_W 2
#define PF_R 4

// Auxiliary vector entries (the System V ABI's values), on a new process's
// stack after envp (proc.h).
#define AT_NULL   0
#define AT_PHDR   3 // the program headers in memory
#define AT_PHENT  4 // the size of one
#define AT_PHNUM  5 // how many
#define AT_PAGESZ 6
#define AT_ENTRY  9 // the program's entry point

// What the new process's auxiliary vector needs to know (proc.c).
struct elf_info {
    uint64_t entry;
    uint64_t phdr;  // where the program headers are in the process's memory, or 0 if no segment holds them
    uint64_t phnum;
};

// Check `image` (`size` bytes) and map its segments into `space`. 0, or
// -ENOEXEC if it isn't an executable this kernel can run, or -ENOMEM. On an
// error, pages already mapped stay mapped: they go with the space.
int elf_load(struct vmm_space *space, const void *image, size_t size, struct elf_info *out);
