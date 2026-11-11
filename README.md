# myos

A hobby operating system for x86-64 PCs, written from scratch in C and a little
assembly. It boots on real hardware through the [Limine](https://github.com/Limine-Bootloader/Limine)
bootloader, from UEFI or legacy BIOS.

## Build and run

Requirements (macOS, Homebrew): `clang` (Xcode), `brew install lld qemu xorriso`.
On Linux, install `clang lld qemu-system-x86 ovmf xorriso`.

```sh
make            # build/myos.iso
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
4. Limine's menu appears for 3 seconds, then the kernel prints its boot log
   and a `myos>` prompt. `help` lists the shell's commands.

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
kernel/src/apic.c       local APIC (x2APIC when available), I/O APIC, irq_register; 8259 PICs masked
kernel/src/time.c       timer calibration (HPET / PM timer / PIT), 1000 Hz tick, monotonic clock, sleep_ms
kernel/src/ps2.c        i8042 PS/2 controller, keyboard on IRQ 1, scancode set 1, LEDs
kernel/src/keyboard.c   key codes, modifiers, US keymap, key event queue
kernel/src/shell.c      kernel shell: line editor, history, command table, built-in commands
kernel/src/sched.c      kernel threads, round-robin preemptive scheduler, sleep, idle thread
kernel/src/switch.S     switch_context: swaps kernel stacks between threads
kernel/src/sync.c       spinlocks, wait queues, mutexes, semaphores, condition variables, lock debug checks
kernel/src/cpu.c        CPU identification (CPUID)
kernel/src/console.c    framebuffer text console: 8x16 font, ANSI escape codes, RAM shadow buffer
kernel/src/font.S      embeds the console font (a PSF2 file)
kernel/src/serial.c     COM1 serial output (QEMU / debugging)
kernel/src/kprintf.c    printf, snprintf, panic
kernel/src/klog.c       kernel log: a 64 KiB ring of everything kprintf printed
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
