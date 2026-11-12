# myos

A hobby operating system for x86-64 PCs, written from scratch in C and a little
assembly. It boots on real hardware through the [Limine](https://github.com/Limine-Bootloader/Limine)
bootloader, from UEFI or legacy BIOS.

## Build and run

Requirements (macOS, Homebrew): `clang` (Xcode), `brew install lld qemu xorriso`.
On Linux, install `clang lld qemu-system-x86 ovmf xorriso`.

```sh
make            # build/myos.iso
make user       # just the user programs (user/), into build/user
make run        # boot in QEMU with UEFI; serial output goes to this terminal
make run-bios   # boot in QEMU with legacy BIOS
make debug      # QEMU waits for gdb: gdb build/kernel.elf -ex 'target remote :1234'
make test       # headless boot test under UEFI and BIOS, driving the shell, then reboot and poweroff (what CI runs)
make test-crash # headless check that the crash screen and `panic` work
make crash-test # boot a kernel that page-faults on purpose, in a window
make compdb     # compile_commands.json for clangd/VS Code
```

## CI and releases

GitHub Actions ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) runs on
every push to `main` and every pull request. It builds with `-Werror`, boots the
ISO in QEMU under UEFI and BIOS, checks the crash screen, and uploads the ISO,
kernel and boot logs as a build artifact.

To publish a release, push a version tag:

```sh
git tag v0.1.0 && git push origin v0.1.0
```

CI then creates a GitHub Release with `myos-v0.1.0.iso`, the kernel ELF (for
`llvm-symbolizer`) and `SHA256SUMS`. The boot screen shows the version it was
built from (`git describe`).

## Booting a real PC

1. `make usb DEV=/dev/diskN` (or download an ISO from Releases and write it
   with `scripts/write-usb.sh /dev/diskN myos-vX.Y.Z.iso`). Run `make usb` with no `DEV` first to list
   external disks; on macOS, `diskutil list` shows them too. **This erases
   the USB stick.**
2. In the target PC's firmware settings, **disable Secure Boot**. Limine
   isn't signed with Microsoft's keys.
3. Power on and open the one-time boot menu (usually F12, F11, F8 or Esc).
   Pick the USB drive (the "UEFI:" entry if both are listed).
4. Limine's menu appears for 3 seconds, then the kernel prints its boot log,
   starts `/bin/init`, which starts the shell, `/bin/sh`, and you get a
   `myos>` prompt. `help` lists the shell's commands. `lspci -n` prints
   the PCI devices in the same format as Linux's `lspci -n`, so the two can
   be compared line by line. `disk` lists the SATA and NVMe disks, `disk read sda 0`
   prints a sector, and `disk test sda <sector>` writes one, reads it back
   and puts back what was there (try it on a scratch disk).

Most PCs have no serial port, so on real hardware everything shows on the
screen. If the machine crashes you get a red crash screen with the
exception, registers and a backtrace. Photograph it, then turn the
addresses into source lines with:

```sh
llvm-symbolizer --obj=build/kernel.elf 0xffffffff80001234
```

## Layout

```
kernel/linker.ld        memory layout (kernel at 0xffffffff80000000)
kernel/src/entry.S      _start: first instruction the kernel runs
kernel/src/main.c       kmain + the requests that ask Limine for info
kernel/src/gdt.c        segment descriptors + TSS (stacks for interrupts)
kernel/src/idt.c        interrupt table, exception handling, crash screen
kernel/src/isr.S        256 interrupt entry stubs
kernel/src/pmm.c        physical page allocator (bitmap)
kernel/src/vmm.c        the kernel's own page tables, vmm_map/vmm_unmap
kernel/src/acpi.c       ACPI tables: CPUs, I/O APICs, IRQ overrides, FADT, HPET, MCFG; \_S5 power off
kernel/src/apic.c       local APIC (x2APIC when available), I/O APIC, irq_register, MSI vectors; 8259 PICs masked
kernel/src/pci.c        PCI/PCIe: ECAM (or ports 0xcf8/0xcfc), bus scan through bridges, BARs, capabilities, MSI/MSI-X, driver table, lspci
kernel/src/pci_test.c   PCI self-test: QEMU's edu and igb devices raise an MSI and an MSI-X
kernel/src/ahci.c       AHCI (SATA) driver: BIOS/OS handoff, HBA reset, COMRESET, IDENTIFY, READ/WRITE DMA EXT, MSI completion
kernel/src/nvme.c       NVMe driver: controller reset and enable, admin and I/O queues, IDENTIFY, READ/WRITE with PRP lists, MSI-X completion
kernel/src/block.c      block devices: block_read/block_write for every disk driver, the disk self-test, `disk`
kernel/src/time.c       timer calibration (HPET / PM timer / PIT), 1000 Hz tick, monotonic clock, sleep_ms
kernel/src/ps2.c        i8042 PS/2 controller, keyboard on IRQ 1, scancode set 1, LEDs
kernel/src/keyboard.c   key codes, modifiers, US keymap, key event queue
kernel/src/shell.c      kernel shell: the command table (mem, ps, reboot, ...) /bin/sh runs, and a fallback shell
kernel/src/sched.c      kernel threads, round-robin preemptive scheduler, sleep, idle thread
kernel/src/switch.S     switch_context: swaps kernel stacks between threads
kernel/src/sync.c       spinlocks, wait queues, mutexes, semaphores, condition variables, lock debug checks
kernel/src/vfs.c        VFS: vnodes with an operations table, path lookup, mounts, refcounted open files, open/read/write/readdir/stat/mkdir
kernel/src/tmpfs.c      tmpfs: files and directories in RAM (the root filesystem)
kernel/src/devfs.c      devfs on /dev: console (keys in, raw), null, zero
kernel/src/proc.c       user processes: own address spaces, ring 3, argv/envp/auxv, fd tables, spawn/waitpid with zombies and orphans, killed on a fault
kernel/src/elf.c        ELF64 loader for static executables: PT_LOAD segments with their permissions, .bss zeroed
kernel/src/syscall.c    syscall/sysret setup, SMEP/SMAP, the system calls (read, write, open, close, dup, dup2, mmap, spawn, wait4, exit, ...)
kernel/src/syscall.S    syscall entry (swapgs, kernel stack) and the first iretq to ring 3
kernel/src/user_programs.S  built-in ring-3 test programs (`user hello`, `user kread`, ...)
kernel/src/initramfs.c  unpacks the initramfs (a USTAR archive Limine loads as a module) into /
kernel/src/cpu.c        CPU identification (CPUID)
kernel/src/console.c    framebuffer text console: 8x16 font, ANSI escape codes, RAM shadow buffer
kernel/src/font.S      embeds the console font (a PSF2 file)
kernel/src/serial.c     COM1 serial output (QEMU / debugging)
kernel/src/kprintf.c    printf, snprintf, panic
kernel/src/klog.c       kernel log: a 64 KiB ring of everything kprintf printed
user/                   user programs, built as static ELF executables and put in the initramfs's /bin
user/crt0.S             _start: hands the stack the kernel set up to __libc_start
user/linker.ld          user memory layout (programs at 0x400000)
user/libc/              tiny libc: syscall wrappers (files, spawn, waitpid), printf, string functions, malloc over mmap, getenv, getauxval
user/bin/init.c         /bin/init, the first program the kernel runs: checks the libc, files and processes, then runs /bin/sh
user/bin/sh.c           /bin/sh, the shell: line editor, history, runs programs as children and waits, `> file`
user/bin/echo.c         /bin/echo
user/bin/cat.c          /bin/cat
user/bin/crash.c        /bin/crash: writes to address 0, to show a crash only kills the program
initramfs/              files packed into the initramfs; they show up under / at boot
scripts/mkinitramfs.py  packs initramfs/ and the user programs into build/initramfs.tar (USTAR)
scripts/boot-test.sh    headless QEMU boot test used by `make test` and CI (also types keys and checks a screendump)
scripts/bdf2psf.py      turns a BDF font into the PSF2 file the console uses
scripts/write-usb.sh    safely write the ISO to a USB stick
vendor/limine           Limine 11.4.1 bootloader binaries (BSD-2-Clause)
vendor/limine-protocol  limine.h boot protocol header (0BSD)
vendor/spleen           Spleen 8x16 console font, Latin-1 (BSD-2-Clause)
```

## Roadmap

- [x] **Boot**: Limine, UEFI + BIOS, framebuffer text console, serial, kprintf
- [x] **CPU setup**: GDT, TSS, IDT, crash screen with backtrace
- [ ] **Physical memory**: page-frame allocator built from the memory map
- [ ] **Virtual memory**: own page tables, kernel heap (`kmalloc`)
- [ ] **Interrupts from hardware**: ACPI tables (MADT), Local APIC + I/O APIC, timer
- [ ] **Keyboard**: PS/2 first, a simple shell
- [ ] **Multitasking**: kernel threads, scheduler, context switch
- [ ] **User mode**: ring 3, syscalls, ELF loader, first user program
- [ ] **Storage**: PCI enumeration, AHCI/NVMe driver, a filesystem
- [ ] **USB**: xHCI + HID keyboard (most modern laptops have no PS/2)
- [ ] **SMP**: start the other CPU cores
