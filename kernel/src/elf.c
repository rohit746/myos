// Load static ELF64 executables into an address space. See elf.h.

#include <elf.h>
#include <errno.h>
#include <memory.h>
#include <pmm.h>
#include <sched.h>
#include <stdbool.h>
#include <string.h>
#include <vmm.h>

// [start, start + n) lies inside [0, limit), without overflowing.
static bool fits(uint64_t start, uint64_t n, uint64_t limit) { return start <= limit && n <= limit - start; }

static bool header_ok(const Elf64_Ehdr *h, size_t size) {
    const unsigned char *id = h->e_ident;
    if (id[0] != 0x7f || id[1] != 'E' || id[2] != 'L' || id[3] != 'F') return false;
    if (id[4] != ELFCLASS64 || id[5] != ELFDATA2LSB || id[6] != EV_CURRENT) return false;
    // ET_DYN (PIE) would need relocating: only fixed-address executables.
    if (h->e_type != ET_EXEC || h->e_machine != EM_X86_64 || h->e_version != EV_CURRENT) return false;
    if (h->e_phentsize != sizeof(Elf64_Phdr) || h->e_phnum == 0 || h->e_phnum > ELF_MAX_PHDRS) return false;
    return fits(h->e_phoff, (uint64_t)h->e_phnum * sizeof(Elf64_Phdr), size);
}

// A PT_LOAD segment this kernel can map.
static bool segment_ok(const Elf64_Phdr *ph, size_t size) {
    if (ph->p_filesz > ph->p_memsz || !fits(ph->p_offset, ph->p_filesz, size)) return false;
    if (ph->p_vaddr < ELF_LOAD_MIN || !fits(ph->p_vaddr, ph->p_memsz, ELF_LOAD_END)) return false;
    return true;
}

// Program header `i`, copied out (the file may not be aligned for it).
static Elf64_Phdr phdr(const uint8_t *file, const Elf64_Ehdr *h, unsigned i) {
    Elf64_Phdr ph;
    memcpy(&ph, file + h->e_phoff + i * sizeof ph, sizeof ph);
    return ph;
}

// Map the pages under one segment, copying in its bytes from the file.
static int map_segment(struct vmm_space *space, const uint8_t *image, const Elf64_Phdr *ph) {
    uint32_t flags = VMM_USER | (ph->p_flags & PF_W ? VMM_WRITE : 0) | (ph->p_flags & PF_X ? VMM_EXEC : 0);
    uint64_t file_end = ph->p_vaddr + ph->p_filesz; // past the last byte that comes from the file
    uint64_t end = ALIGN_UP(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
    for (uint64_t page = ALIGN_DOWN(ph->p_vaddr, PAGE_SIZE); page < end; page += PAGE_SIZE) {
        uint64_t phys;
        if (vmm_virt_to_phys(space, page, &phys)) return -ENOEXEC; // another segment has this page
        preempt_disable(); // the page allocator isn't thread-safe
        phys = pmm_alloc_page();
        if (phys) {
            uint8_t *dst = phys_to_virt(phys);
            memset(dst, 0, PAGE_SIZE);
            // The part of [p_vaddr, file_end) on this page.
            uint64_t from = page > ph->p_vaddr ? page : ph->p_vaddr;
            uint64_t to = page + PAGE_SIZE < file_end ? page + PAGE_SIZE : file_end;
            if (from < to) memcpy(dst + (from - page), image + ph->p_offset + (from - ph->p_vaddr), to - from);
            vmm_map(space, page, phys, flags);
        }
        preempt_enable();
        if (phys == 0) return -ENOMEM;
    }
    return 0;
}

int elf_load(struct vmm_space *space, const void *image, size_t size, struct elf_info *out) {
    const uint8_t *file = image;
    Elf64_Ehdr h;
    if (size < sizeof h) return -ENOEXEC;
    memcpy(&h, file, sizeof h);
    if (!header_ok(&h, size)) return -ENOEXEC;

    // Check everything before mapping anything.
    bool loads = false, entry_ok = false;
    out->entry = h.e_entry;
    out->phdr = 0;
    out->phnum = h.e_phnum;
    for (unsigned i = 0; i < h.e_phnum; i++) {
        Elf64_Phdr ph = phdr(file, &h, i);
        if (ph.p_type == PT_INTERP || ph.p_type == PT_DYNAMIC) return -ENOEXEC; // needs a dynamic linker
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        if (!segment_ok(&ph, size)) return -ENOEXEC;
        loads = true;
        if ((ph.p_flags & PF_X) && h.e_entry >= ph.p_vaddr && h.e_entry - ph.p_vaddr < ph.p_memsz) entry_ok = true;
        // The program headers are in memory if this segment's file bytes
        // hold them (the usual layout puts them at the start of the first).
        uint64_t phdrs_size = (uint64_t)h.e_phnum * sizeof ph;
        if (h.e_phoff >= ph.p_offset && fits(h.e_phoff - ph.p_offset, phdrs_size, ph.p_filesz))
            out->phdr = ph.p_vaddr + (h.e_phoff - ph.p_offset);
    }
    if (!loads || !entry_ok) return -ENOEXEC;

    for (unsigned i = 0; i < h.e_phnum; i++) {
        Elf64_Phdr ph = phdr(file, &h, i);
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        int err = map_segment(space, file, &ph);
        if (err) return err;
    }
    return 0;
}
