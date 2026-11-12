# myos — build system
#
#   make            build build/myos.iso (boots on UEFI and legacy BIOS)
#   make user       build the user programs (user/) into build/user
#   make run        boot the ISO in QEMU with UEFI firmware
#   make run-bios   boot the ISO in QEMU with legacy BIOS
#   make debug      like `run`, but wait for gdb on :1234, and freeze instead of
#                   resetting or exiting (a triple fault stays there to inspect)
#   make usb DEV=/dev/diskN   write the ISO to a USB stick (asks first)
#   make test       boot the ISO headless under UEFI and BIOS (and once with an
#                   xAPIC-only CPU), check the output and drive the shell
#                   (under BIOS, through USB keyboards);
#                   then check that `reboot` and `poweroff` work
#   make test-crash headless check that the crash screen works (a kernel
#                   that faults on purpose, and the shell's panic command)
#   make crash-test  boot a kernel that page-faults on purpose (with a window)
#   make compdb      write compile_commands.json for your editor
#   make clean

CC      := clang
LD      := ld.lld
TARGET  := x86_64-unknown-none-elf

BUILD   := build
KERNEL  := $(BUILD)/kernel.elf
ISO     := $(BUILD)/myos.iso
INITRAMFS := $(BUILD)/initramfs.tar
LIMINE  := vendor/limine

# Shown on the boot screen: the git tag (v0.2.0), or a commit hash between tags.
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

# UEFI firmware for QEMU (Homebrew on Apple Silicon / Intel, then common Linux paths).
OVMF := $(firstword $(wildcard \
	/opt/homebrew/share/qemu/edk2-x86_64-code.fd \
	/usr/local/share/qemu/edk2-x86_64-code.fd \
	/usr/share/qemu/edk2-x86_64-code.fd \
	/usr/share/OVMF/OVMF_CODE_4M.fd \
	/usr/share/OVMF/OVMF_CODE.fd \
	/usr/share/ovmf/OVMF.fd))

CLANG_INC := $(shell $(CC) -print-resource-dir)/include

CFLAGS := \
	--target=$(TARGET) -std=gnu11 -O2 -g -pipe \
	-Wall -Wextra -Wno-unused-parameter \
	-ffreestanding -fno-builtin -fno-stack-protector -fno-stack-check \
	-fno-PIC -fno-lto -fno-omit-frame-pointer \
	-march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
	-mno-red-zone -mcmodel=kernel \
	-nostdinc -isystem $(CLANG_INC) \
	-Ikernel/include -I$(BUILD)/gen -Ivendor/limine-protocol/include -Ivendor \
	-MMD -MP $(EXTRA_CFLAGS)

ASFLAGS := --target=$(TARGET) -g -Ikernel/include -Ivendor -MMD -MP

LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	--no-dynamic-linker -T kernel/linker.ld

SRCS := $(shell find kernel/src -name '*.c' -o -name '*.S')
OBJS := $(SRCS:%=$(BUILD)/obj/%.o)

# Screen size for `run`, `run-bios` and `debug`: QEMU's display advertises it
# as the preferred mode (EDID), and the firmware and Limine pick it. On macOS,
# QEMU's window shows one guest pixel per physical pixel, so on a Retina
# display 1280x800 comes out half size; 2560x1600 makes a 1280x800-point
# window, and the console draws its text at scale 2. Override with
# `make run RES=1280x800` (for example on a non-Retina screen).
ifeq ($(shell uname -s),Darwin)
RES ?= 2560x1600
else
RES ?= 1280x800
endif
RES_X = $(word 1,$(subst x, ,$(RES)))
RES_Y = $(word 2,$(subst x, ,$(RES)))

QEMU_COMMON = qemu-system-x86_64 -M q35 -m 512M -cpu max -smp 4 \
	-vga none -device VGA,xres=$(RES_X),yres=$(RES_Y),vgamem_mb=32 \
	-serial stdio

.PHONY: all kernel user run run-bios debug usb test test-crash crash-test compdb clean FORCE

all: $(ISO)
kernel: $(KERNEL)

# Regenerated every build, but only rewritten (forcing a recompile of the
# files that include it) when the version string actually changes.
$(BUILD)/gen/version.h: FORCE
	@mkdir -p $(dir $@)
	@echo '#define MYOS_VERSION "$(VERSION)"' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(OBJS): | $(BUILD)/gen/version.h

$(BUILD)/obj/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

# .incbin'd files aren't in the -MMD dependency lists.
$(BUILD)/obj/kernel/src/font.S.o: vendor/spleen/spleen-8x16.psf

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# User programs (user/): each user/bin/<name>.c is linked with crt0 and the
# libc into a static executable, build/user/<name>.elf (with debug info, for
# llvm-symbolizer), and stripped into the initramfs as /bin/<name>. No SSE:
# the kernel doesn't save SSE registers between processes yet.
USER_BUILD := $(BUILD)/user

USER_CFLAGS := \
	--target=$(TARGET) -std=gnu11 -O2 -g -pipe \
	-Wall -Wextra -Wno-unused-parameter \
	-ffreestanding -fno-builtin -fno-stack-protector -fno-stack-check \
	-fno-pic -fno-pie -fno-lto -fno-omit-frame-pointer \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-ffunction-sections -fdata-sections \
	-march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
	-nostdinc -isystem user/include -isystem $(CLANG_INC) \
	-MMD -MP $(EXTRA_CFLAGS)

USER_LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	--no-dynamic-linker --gc-sections -T user/linker.ld

USER_LIBC_SRCS := user/crt0.S $(wildcard user/libc/*.c user/libc/*.S)
USER_LIBC_OBJS := $(USER_LIBC_SRCS:%=$(USER_BUILD)/obj/%.o)
USER_PROGS := $(patsubst user/bin/%.c,%,$(wildcard user/bin/*.c))
USER_BINS := $(USER_PROGS:%=$(USER_BUILD)/root/bin/%)
USER_OBJS := $(USER_LIBC_OBJS) $(USER_PROGS:%=$(USER_BUILD)/obj/user/bin/%.c.o)

$(USER_BUILD)/obj/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/obj/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) --target=$(TARGET) -g -MMD -MP -c $< -o $@

$(USER_BUILD)/%.elf: $(USER_BUILD)/obj/user/bin/%.c.o $(USER_LIBC_OBJS) user/linker.ld
	$(LD) $(USER_LDFLAGS) $(USER_LIBC_OBJS) $< -o $@

$(USER_BUILD)/root/bin/%: $(USER_BUILD)/obj/user/bin/%.c.o $(USER_LIBC_OBJS) user/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) --strip-all $(USER_LIBC_OBJS) $< -o $@

user: $(USER_BINS) $(USER_PROGS:%=$(USER_BUILD)/%.elf)

# The initramfs: everything under initramfs/, and the user programs in
# /bin, as a USTAR archive that Limine loads as a module and the kernel
# unpacks into / (see limine.conf).
INITRAMFS_FILES := $(shell find initramfs -type f -o -type d)

$(INITRAMFS): $(INITRAMFS_FILES) $(USER_BINS) $(USER_PROGS:%=$(USER_BUILD)/%.elf) scripts/mkinitramfs.py
	@mkdir -p $(dir $@)
	python3 -I scripts/mkinitramfs.py $@ initramfs $(USER_BUILD)/root

# Host tool that patches the ISO so it also boots on legacy BIOS.
$(LIMINE)/limine: $(LIMINE)/limine.c
	$(MAKE) -C $(LIMINE) CC=cc

$(ISO): $(KERNEL) $(INITRAMFS) limine.conf $(LIMINE)/limine
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/kernel.elf
	cp $(INITRAMFS) $(BUILD)/iso_root/boot/initramfs.tar
	cp limine.conf $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(LIMINE)/BOOTIA32.EFI $(BUILD)/iso_root/EFI/BOOT/
	xorriso -as mkisofs -quiet -R -r -J \
	   -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
	   -hfsplus -apm-block-size 2048 \
	   --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	   --protective-msdos-label $(BUILD)/iso_root -o $@
	$(LIMINE)/limine bios-install $@ >/dev/null 2>&1
	@echo "==> $@ ready"

run: $(ISO)
	@test -n "$(OVMF)" || { echo "UEFI firmware (OVMF/edk2) not found"; exit 1; }
	$(QEMU_COMMON) -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF) -cdrom $(ISO)

run-bios: $(ISO)
	$(QEMU_COMMON) -cdrom $(ISO)

debug: $(ISO)
	$(QEMU_COMMON) -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF) -cdrom $(ISO) -s -S \
	   -no-reboot -no-shutdown

usb: $(ISO)
	@scripts/write-usb.sh "$(DEV)" "$(ISO)"

test: $(ISO)
	OVMF="$(OVMF)" scripts/boot-test.sh uefi boot $(ISO) $(BUILD)/test/uefi.log
	QEMU_USB=1 scripts/boot-test.sh bios boot $(ISO) $(BUILD)/test/bios.log
	QEMU_CPU=max,-x2apic OVMF="$(OVMF)" scripts/boot-test.sh uefi boot $(ISO) $(BUILD)/test/uefi-xapic.log
	OVMF="$(OVMF)" scripts/boot-test.sh uefi power $(ISO) $(BUILD)/test/power-uefi.log
	scripts/boot-test.sh bios power $(ISO) $(BUILD)/test/power-bios.log

# A kernel that deliberately page-faults, built separately in build/crash.
CRASH_MAKE := $(MAKE) BUILD=$(BUILD)/crash EXTRA_CFLAGS="$(EXTRA_CFLAGS) -DMYOS_CRASH_TEST"

test-crash: $(ISO)
	$(CRASH_MAKE) $(BUILD)/crash/myos.iso
	OVMF="$(OVMF)" scripts/boot-test.sh uefi crash $(BUILD)/crash/myos.iso $(BUILD)/test/crash.log
	OVMF="$(OVMF)" scripts/boot-test.sh uefi panic $(ISO) $(BUILD)/test/panic.log
	OVMF="$(OVMF)" scripts/boot-test.sh uefi fault $(ISO) $(BUILD)/test/fault.log

crash-test:
	$(CRASH_MAKE) run

# compile_commands.json so editors (clangd) see the real x86-64 flags.
compdb:
	@python3 -c 'import json,os,sys; d=os.getcwd(); k,u=sys.argv[1].split(), sys.argv[2].split(); print(json.dumps([{"directory":d,"file":s,"arguments":["$(CC)"]+(u if s.startswith("user/") else k)+["-c",s]} for s in sys.argv[3:]],indent=1))' \
	  "$(CFLAGS)" "$(USER_CFLAGS)" $(filter %.c,$(SRCS) $(USER_LIBC_SRCS)) $(wildcard user/bin/*.c) > compile_commands.json
	@echo "==> compile_commands.json written"

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(USER_OBJS:.o=.d)
