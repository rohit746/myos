# myos — build system
#
#   make            build build/myos.iso (boots on UEFI and legacy BIOS)
#   make run        boot the ISO in QEMU with UEFI firmware
#   make run-bios   boot the ISO in QEMU with legacy BIOS
#   make debug      like `run`, but wait for gdb on :1234
#   make usb DEV=/dev/diskN   write the ISO to a USB stick (asks first)
#   make test       boot the ISO headless under UEFI and BIOS, check the output
#   make test-crash headless check that the crash screen works
#   make crash-test  boot a kernel that page-faults on purpose (with a window)
#   make compdb      write compile_commands.json for your editor
#   make clean

CC      := clang
LD      := ld.lld
TARGET  := x86_64-unknown-none-elf

BUILD   := build
KERNEL  := $(BUILD)/kernel.elf
ISO     := $(BUILD)/myos.iso
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

ASFLAGS := --target=$(TARGET) -g -Ikernel/include -MMD -MP

LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	--no-dynamic-linker -T kernel/linker.ld

SRCS := $(shell find kernel/src -name '*.c' -o -name '*.S')
OBJS := $(SRCS:%=$(BUILD)/obj/%.o)

QEMU_COMMON := qemu-system-x86_64 -M q35 -m 512M -cpu max -smp 4 \
	-serial stdio -no-reboot -no-shutdown

.PHONY: all kernel run run-bios debug usb test test-crash crash-test compdb clean FORCE

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

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# Host tool that patches the ISO so it also boots on legacy BIOS.
$(LIMINE)/limine: $(LIMINE)/limine.c
	$(MAKE) -C $(LIMINE) CC=cc

$(ISO): $(KERNEL) limine.conf $(LIMINE)/limine
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/kernel.elf
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
	$(QEMU_COMMON) -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF) -cdrom $(ISO) -s -S

usb: $(ISO)
	@scripts/write-usb.sh "$(DEV)" "$(ISO)"

test: $(ISO)
	OVMF="$(OVMF)" scripts/boot-test.sh uefi boot $(ISO) $(BUILD)/test/uefi.log
	scripts/boot-test.sh bios boot $(ISO) $(BUILD)/test/bios.log

# A kernel that deliberately page-faults, built separately in build/crash.
CRASH_MAKE := $(MAKE) BUILD=$(BUILD)/crash EXTRA_CFLAGS="$(EXTRA_CFLAGS) -DMYOS_CRASH_TEST"

test-crash:
	$(CRASH_MAKE) $(BUILD)/crash/myos.iso
	OVMF="$(OVMF)" scripts/boot-test.sh uefi crash $(BUILD)/crash/myos.iso $(BUILD)/test/crash.log

crash-test:
	$(CRASH_MAKE) run

# compile_commands.json so editors (clangd) see the real x86-64 flags.
compdb:
	@python3 -c 'import json,os,sys; d=os.getcwd(); f=sys.argv[1]; print(json.dumps([{"directory":d,"file":s,"arguments":["$(CC)"]+f.split()+["-c",s]} for s in sys.argv[2:]],indent=1))' \
	  "$(CFLAGS)" $(filter %.c,$(SRCS)) > compile_commands.json
	@echo "==> compile_commands.json written"

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
