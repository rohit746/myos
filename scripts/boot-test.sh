#!/usr/bin/env bash
# Boot an ISO in QEMU without a display and check the kernel's serial output.
#
#   scripts/boot-test.sh <uefi|bios> <boot|power|crash|panic|fault|frame|nested> <iso> <logfile>
#
#   boot   pass when the kernel reports it's up, ACPI found all the CPUs QEMU
#          was given and all of them came online, a PIT IRQ came in through
#          the I/O APIC, the timer tick ran at the calibrated rate, the
#          scheduler preempted threads that never yield and spread them over
#          the CPUs, threads contending on locks got exact counts (and the
#          lock debug checks fired), a kernel page remapped while every CPU
#          read it was never read stale (TLB shootdown), PCI enumeration found devices
#          through ECAM and QEMU's edu and igb devices raised an MSI and an
#          MSI-X (igb sits behind a PCIe root port, so the scan has to go
#          through a bridge), the AHCI driver found a scratch disk on a
#          second AHCI controller and the NVMe driver one on QEMU's NVMe
#          controller, and the disk self-test read both and wrote to them
#          (checked again in the image files once QEMU is gone), the
#          partition tables of a copy of the ISO (MBR, as on a USB stick)
#          and of a GPT disk were read and the FAT12 EFI partition and a
#          FAT32 partition mounted on /mnt, the VFS passed its self-test
#          and the initramfs was unpacked into /, user programs passed their
#          self-test in ring 3, the PS/2 keyboard is ready, and /bin/init
#          was loaded from the initramfs, passed its libc self-test (files,
#          child processes, ...) and started the user-space shell, /bin/sh;
#          a screendump shows the green "[ ok ]" text the escape codes ask
#          for, and then, typing through QEMU's monitor, the shell works:
#          lines edited with Shift, Caps Lock, Num Lock, Backspace, Delete,
#          Left/Right, Home/End and the history (Up/Down) are echoed right,
#          every command prints what it should (kernel commands through the
#          shell; `ls` and `cat` show files from the initramfs; `user hello`
#          prints from ring 3, `lspci` finds the device behind the bridge,
#          `lspci -n` prints as Linux's does, `lspci -v` shows the MSI-X
#          the self-test turned on, `disk` lists the disks, `disk read`
#          prints their first sectors and `disk test` writes one on each and
#          puts it back, `disk` lists the partitions, `mount` the FAT
#          filesystems, `ls`, `stat`, `cat` and `crc32` read files and
#          directories on both (long names, any case, multi-cluster files and
#          directories), `>`, `>>`, `mkdir` and `rm` create, append to and
#          delete files there, and /bin/cat copies a file across many
#          clusters (the FAT32 image is checked with mtools and fsck.fat
#          once QEMU is gone), `user kread` gets killed for reading a kernel
#          address and the shell carries on; programs run as children of
#          the shell: `/bin/echo` and `init`, `echo ... > file` writes the
#          file, `crash` gets killed and the shell says so and carries on,
#          and files that aren't programs are refused), `clear` blanks the
#          screen and `reboot` resets the machine (QEMU exits: -no-reboot);
#          fail on any panic, exception or unhandled interrupt
#          With QEMU_USB=1, QEMU also gets an xHCI controller with a USB
#          keyboard on a root port and another behind a USB hub, which
#          takes every key QEMU's monitor types: the typing and the
#          commands above go through the hub. Both keyboards must be ready
#          at boot, `lsusb` must list them, and a keyboard plugged in once
#          the shell runs must be found, type a line, and be gone when it
#          is pulled out
#   power  pass when `reboot` restarts the machine (the kernel boots a second
#          time) and `poweroff` then turns it off through ACPI: QEMU exits on
#          its own, with status 0. QEMU runs without -no-reboot here, so a
#          reset or triple fault reboots instead of passing for a power-off
#   crash  pass when the crash screen for the deliberate page fault appears,
#          its backtrace naming the function that faulted (kmain+0x...)
#   panic  pass when the shell's `panic` command panics the kernel, with a
#          backtrace naming the command (cmd_panic+0x...)
#   fault  pass when the shell's `panic fault` brings up the crash screen
#   frame  pass when `panic frame` (a page fault with a frame pointer to an
#          unmapped address) brings up the crash screen, whose backtrace
#          stops at that frame pointer instead of faulting again
#   nested pass when `panic nested` (a page fault while printing a panic)
#          reports the fault inside the panic and halts, without a triple
#          fault
#
# UEFI mode needs the firmware path in $OVMF. Timeout: $BOOT_TIMEOUT (seconds).
# Needs mtools and fsck.fat (dosfstools) for the FAT32 disk.
# QEMU's CPU model: $QEMU_CPU (default max; max,-x2apic tests xAPIC mode).
# USB keyboards: $QEMU_USB=1 (boot only).

set -uo pipefail

mode=${1:?mode: uefi|bios}
expect=${2:?expect: boot|power|crash|panic|fault|frame|nested}
iso=${3:?iso path}
log=${4:?log path}
timeout_s=${BOOT_TIMEOUT:-90}

ok_marker="Kernel is up."
smp=4 # CPUs QEMU gets; the ACPI summary must list them all
cpus_marker="cpus:   $smp,"
smp_marker="SMP: $smp CPUs online"
tlb_marker="TLB shootdown self-test passed"
irq_marker="IRQ self-test passed"
timer_marker="timer self-test passed"
sched_marker="scheduler self-test passed"
sync_marker="sync self-test passed"
pci_marker="config space through ECAM"
msi_marker="edu: BAR mapped, MSI on vector 0x"
msix_marker="igb: MSI-X on vector 0x"
disk_marker="disk self-test passed ("
disk_markers=("sda: read sectors 1-300, wrote" "nvme0n1: read sectors 1-300, wrote") # both in that line
vfs_marker="VFS self-test passed"
part_marker="partitions: sdb MBR (4), sdc GPT (1)"
fat_marker="FAT12 on /mnt/sdb2, FAT32 on /mnt/sdc1"
initramfs_marker="initramfs: unpacked"
syscall_marker="system calls via syscall/sysret"
user_marker="user mode self-test passed"
init_marker="init: libc self-test passed"
sh_marker="init: started /bin/sh"
kbd_marker="PS/2 keyboard ready"
# QEMU's xHCI has USB 3 ports 1-4 and USB 2 ports 5-8: its USB port 1 (the
# hub) is xHCI port 5, port 2 is 6, port 3 is 7.
usb_marker="USB keyboards ready: QEMU USB Keyboard (usb0 port 5.1), QEMU USB Keyboard (usb0 port 6)"
# What `crc32` must print for files on the FAT disks: Limine's BOOTX64.EFI
# in the ISO's EFI partition (MBR partition 2), and big.bin on the FAT32
# disk (make_fat_disk).
crc32_of() { python3 -I -c 'import sys, zlib; print("%08x" % zlib.crc32(sys.stdin.buffer.read()))'; }
big_file() { python3 -I -c 'import sys; sys.stdout.buffer.write(bytes((i * 7 + i // 251) & 255 for i in range(100000)))'; }
efi_offset() { python3 -I -c 'import struct, sys; print(struct.unpack_from("<I", open(sys.argv[1], "rb").read(512), 462 + 8)[0] * 512)' "$1"; }
big_crc=$(big_file | crc32_of)
efi_crc=$( [[ -f "$iso" ]] && mtype -i "$iso@@$(efi_offset "$iso")" ::/EFI/BOOT/BOOTX64.EFI | crc32_of)

# Keys to type: QEMU sendkey names, or "=text" for the keys that type text.
# Each line runs `echo`, so what it prints is what the editing produced.
keys=("=echo " shift-h e l l p backspace o comma spc shift-w o r l d shift-1 ret
      "=echo " caps_lock m y shift-o s caps_lock kp_divide num_lock kp_4 kp_2 shift-kp_7 num_lock kp_7 shift-kp_1
      kp_enter
      "=cho rightx" home e end left delete shift-1 ret # echo right!
      "=echo lft" left left e right right "= ok" ret  # echo left ok
      up up up up up ret                               # the first line again (Up stops at the oldest)
      up up up up up up down down ret)                 # echo right! again
# Lines the shell must print, each "count:text".
typed=("2:Hello, World!" "1:MYoS/421" "2:right!" "1:left ok")
# Commands to run, and a line (extended regex) each must print ("": nothing
# to wait for; the shell runs the next command once this one is done).
commands=(help uptime mem cpu acpi ps threads lspci "lspci -n" "lspci -v" disk disk "disk read sda 0" "disk test sda 2000"
          "disk read nvme0n1 0" "disk test nvme0n1 2000" "user hello" "user kread" "user priv" "ls /" "ls /etc" "cat /etc/motd" "ls /dev" "stat /dev/null"
          "mkdir /etc" "cat /nope" "/bin/echo from ring 3, via libc" init "/etc/motd" "/bin" "/bin/nope" nope
          "echo into a file > /out" "stat /out" "cat /out"
          disk disk mount "ls /mnt/sdb2" "ls /mnt/sdb2/EFI/BOOT" "stat /mnt/sdb2/EFI"
          "crc32 /mnt/sdb2/efi/boot/bootx64.efi" "cat /mnt/sdc1/hello.txt" "ls /mnt/sdc1" "ls /mnt/sdc1/many"
          "crc32 /mnt/sdc1/big.bin" "echo written by myos > /mnt/sdc1/Created-by-myos.txt"
          "echo appended >> /mnt/sdc1/created-by-myos.txt" "cat /mnt/sdc1/created-by-myos.txt"
          "/bin/cat /mnt/sdc1/big.bin > /mnt/sdc1/copy.bin" "crc32 /mnt/sdc1/copy.bin" "mkdir /mnt/sdc1/New-dir"
          "echo inside > /mnt/sdc1/new-dir/inside.txt" "rm /mnt/sdc1/new-dir" "rm /mnt/sdc1/HELLO.TXT"
          "stat /mnt/sdc1/hello.txt" "rm /mnt/sdc1/many/file7.txt" "stat /mnt/sdc1/many" "rm /mnt/sdc1/LONGER~1.TXT"
          crash "echo still here")
outputs=("^  reboot +restart the machine"
         "^up [0-9]+:[0-9]{2}:[0-9]{2} \([0-9]+\.[0-9]{3} s, [0-9]+ ticks at 1000 Hz\)"
         "^physical: [0-9]+ MiB managed"
         "^cpus: +$smp in the MADT"
         "^ACPI .*: [0-9]+ tables via the"
         "^ +[0-9]+ +running +[0-9]+ +[0-9]+ +[0-9]+ +sh"
         "^threads: 4 threads ran and exited"
         "^01:00\.0 Ethernet controller \[0200\]: 8086:10c9 \(rev 01\)"
         "^00:1f\.2 0106: 8086:2922 \(rev 02\)"
         "Capabilities: \[70\] MSI-X: Enable\+ Count=10 Masked-"
         "^sda +1\.0 MiB +2048 sectors of 512 bytes +QEMU HARDDISK +\(AHCI [0-9a-f:.]+ port 0\)"
         "^nvme0n1 +1\.0 MiB +2048 sectors of 512 bytes +QEMU NVMe Ctrl +\(NVMe [0-9a-f:.]+ namespace 1\)"
         "^00000000  6d 79 6f 73 20 74 65 73  74 20 64 69 73 6b 3a 20  \|myos test disk: \|"
         "^sda: sector 2000 written, read back and restored"
         "^00000030  6e 76 6d 65 0a 00 00 00  00 00 00 00 00 00 00 00  \|nvme\.{12}\|" # only on the NVMe disk
         "^nvme0n1: sector 2000 written, read back and restored"
         "^process [0-9]+ \(hello\) exited with status 0"
         "^process [0-9]+ \(kread\) killed: page fault at 0xffffffff80000000 \(read, protection violation\)"
         "^process [0-9]+ \(priv\) killed: General protection fault \(exception 13"
         "^d +etc/"
         "^- +[0-9]+  motd"
         "^Welcome to myos!"
         "^c +null"
         "^/dev/null: character device, 0 bytes, inode [0-9]+ on devfs"
         "^mkdir: /etc: File exists"
         "^cat: /nope: No such file or directory"
         "^from ring 3, via libc"
         "^init: hello from init, pid [0-9]+"
         "^/etc/motd: Exec format error"
         "^/bin: Permission denied"
         "^/bin/nope: No such file or directory"
         "^nope: unknown command"
         ""
         "^/out: file, 12 bytes"
         "^into a file"
         "^sdb2 +2\.8 MiB +5760 sectors of 512 bytes +EFI system +\(MBR partition 2 of sdb\)"
         "^sdc1 +46\.9 MiB +96223 sectors of 512 bytes +myos test +\(GPT partition 1 of sdc\)"
         "^sdc1 on /mnt/sdc1 type fat32 \([0-9]+ clusters of 512 bytes, label MYOSTEST\)"
         "^d +EFI/"
         "^- +266240  BOOTRISCV64\.EFI"
         "^/mnt/sdb2/EFI: directory, 1 entries, inode [0-9]+ on fat12"
         "^$efi_crc 319488 /mnt/sdb2/efi/boot/bootx64\.efi"
         "^Hello from a FAT32 disk"
         "^- +12  A long file name\.txt"
         "^- +8  file39\.txt"
         "^$big_crc 100000 /mnt/sdc1/big\.bin"
         ""
         ""
         "^appended"
         ""
         "^$big_crc 100000 /mnt/sdc1/copy\.bin"
         ""
         ""
         "^rm: /mnt/sdc1/new-dir: Directory not empty"
         ""
         "^stat: /mnt/sdc1/hello\.txt: No such file or directory"
         ""
         "^/mnt/sdc1/many: directory, 39 entries, inode [0-9]+ on fat32"
         ""
         "^crash: killed by signal 11 \(segmentation fault\)"
         "^still here")
# With USB: commands starting with @ go to QEMU's monitor instead.
if [[ -n "${QEMU_USB:-}" ]]; then
    commands+=(lsusb lsusb
               "@device_add usb-kbd,id=hotkbd,bus=xhci.0,port=3" "echo typed on a keyboard plugged in just now"
               "@device_del hotkbd" "echo typed behind the hub again")
    outputs+=("^usb0 port 5: QEMU USB Hub \(0409:55aa, full speed\), hub, 8 ports"
              "^usb0 port 5\.1: QEMU USB Keyboard \(0627:0001, full speed\), keyboard"
              "usb0 port 7: QEMU USB Keyboard \(0627:0001, high speed\), keyboard"
              "^typed on a keyboard plugged in just now"
              "usb0 port 7: QEMU USB Keyboard disconnected"
              "^typed behind the hub again")
fi
reboot_marker="Rebooting..."
poweroff_marker="Powering off..."
crash_marker="CPU EXCEPTION 14: Page fault"
panic_marker="KERNEL PANIC *** panic command"
halted_marker="System halted."
# Backtrace lines (extended regexes): the function that faulted, the command
# that panicked, and where a walk gave up on a corrupt frame pointer.
crash_bt_re="^ +#0 0x[0-9a-f]+ kmain\+0x[0-9a-f]+"
panic_bt_re="^ +#0 0x[0-9a-f]+ cmd_panic\+0x[0-9a-f]+"
frame_bt_re="^ +stopped: frame pointer 0xffffff7f00000000 is not mapped"
nested_marker="CPU EXCEPTION 14: Page fault inside the crash screen"
fail_re="KERNEL PANIC|CPU EXCEPTION|unhandled interrupt|\\[warn\\] (IRQ|timer|PCI|AHCI|NVMe|disk|partitions|FAT|PS/2|xHCI|usb[0-9]|/bin/init)|init: (check failed|can.t start)"

[[ -f "$iso" ]] || { echo "boot-test: $iso not found"; exit 2; }
mkdir -p "$(dirname "$log")"
: > "$log"

monitor="$log.mon"
rm -f "$monitor" "$log.screen"

# Fresh 1 MiB scratch disks (the layout block.h describes): one on its own
# AHCI controller (the CD is on the chipset's), one on an NVMe controller,
# whose sector 0 says "nvme" after the marker, so `disk read` tells them apart.
disk="$log.disk"
nvme_disk="$log.nvme"
make_disk() {
    python3 -I - "$1" "$2" <<'PY'
import struct, sys
magic = b"myos test disk: the boot self-test writes to it\n" + sys.argv[2].encode()
with open(sys.argv[1], "wb") as f:
    f.write(magic.ljust(512, b"\0"))
    for s in range(1, 2048):
        f.write(struct.pack("<128I", *[s * 128 + i for i in range(128)]))
PY
}
make_disk "$disk" ""
make_disk "$nvme_disk" $'nvme\n'

# A 48 MiB disk with a GPT and one partition (sectors 2048-98270, "myos
# test"), formatted FAT32 with mtools (512-byte clusters, so files and
# directories take many), with a few files on it.
fat_disk="$log.fat"
fat_part=$((2048 * 512)) # where the partition starts, in bytes
fat_sectors=96223
make_fat_disk() {
    python3 -I - "$1" <<'PY' || return 1
import struct, sys, uuid, zlib
path, ss, n = sys.argv[1], 512, (48 << 20) // 512
first, last = 2048, n - 34
guid = lambda s: uuid.UUID(s).bytes_le
entry = (guid("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7") + guid("6d796f73-0000-4000-8000-000000000001") +
         struct.pack("<QQQ", first, last, 0) + "myos test".encode("utf-16-le").ljust(72, b"\0"))
entries = entry.ljust(128 * 128, b"\0")
def header(me, other, entries_at):
    h = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, me, other, 34, last,
                    guid("6d796f73-0000-4000-8000-000000000000"), entries_at, 128, 128, zlib.crc32(entries))
    return (h[:16] + struct.pack("<I", zlib.crc32(h)) + h[20:]).ljust(ss, b"\0")
mbr = bytearray(ss)
mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xee, 0xff, 0xff, 0xff, 1, n - 1)
mbr[510:512] = b"\x55\xaa"
with open(path, "wb") as f:
    f.truncate(n * ss)
    f.write(mbr + header(1, n - 1, 2) + entries)
    f.seek((n - 33) * ss)
    f.write(entries + header(n - 1, 1, n - 33))
PY
    local img="$1@@$fat_part" files="$1.files" i
    mformat -i "$img" -F -c 1 -T $fat_sectors -h 64 -s 32 -H 2048 -v MYOSTEST :: || return 1
    rm -rf "$files" && mkdir -p "$files/many" || return 1
    printf 'Hello from a FAT32 disk\n' > "$files/HELLO.TXT"
    printf 'a long name\n' > "$files/A long file name.txt"
    # Longer than a VFS name (63 bytes): shows, and is deleted, as LONGER~1.TXT.
    printf 'long\n' > "$files/Longer than the sixty-three bytes a name can have in the VFS.txt"
    big_file > "$files/big.bin"
    for (( i = 0; i < 40; i++ )); do printf 'file %d\n' $i > "$files/many/file$i.txt"; done
    mcopy -s -i "$img" "$files"/* ::/
}

# Check the FAT32 disk after QEMU has gone: what the shell wrote, appended,
# copied, made and deleted is there (or gone), and fsck.fat finds nothing
# wrong.
check_fat_disk() {
    local img="$1@@$fat_part" names
    [[ "$(mtype -i "$img" ::/Created-by-myos.txt)" == $'written by myos\nappended' ]] ||
        { echo "Created-by-myos.txt holds the wrong text"; return 1; }
    cmp -s <(mtype -i "$img" ::/copy.bin) "$1.files/big.bin" || { echo "copy.bin is not a copy of big.bin"; return 1; }
    [[ "$(mtype -i "$img" ::/New-dir/inside.txt)" == inside ]] || { echo "New-dir/inside.txt is wrong"; return 1; }
    names=$(mdir -b -i "$img" ::/ ::/many) || return 1
    if grep -qi 'hello\.txt\|file7\.txt\|Longer than' <<< "$names"; then echo "deleted files are still there"; return 1; fi
    grep -qx '::/Created-by-myos.txt' <<< "$names" || { echo "no Created-by-myos.txt, by that name"; return 1; }
    dd if="$1" of="$1.part" bs=512 skip=2048 count=$fat_sectors 2>/dev/null &&
        fsck.fat -n "$1.part" > "$1.fsck" 2>&1 || { echo "fsck.fat: $(cat "$1.fsck")"; return 1; }
}
make_fat_disk "$fat_disk" || { echo "boot-test: could not make the FAT32 disk (mtools installed?)"; exit 2; }

# Check a disk image after QEMU has gone: the self-test's sectors 1000-1007
# hold the inverted pattern, and their neighbours and sector 2000 (which
# `disk test` wrote and restored) the original one.
check_disk() {
    python3 -I - "$1" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
def ok(s, inverted):
    want = [(~(s * 128 + i) if inverted else s * 128 + i) & 0xffffffff for i in range(128)]
    return struct.unpack("<128I", d[s * 512:(s + 1) * 512]) == tuple(want)
bad = [s for s in range(999, 1009) if not ok(s, 1000 <= s < 1008)] + [s for s in (2000,) if not ok(s, False)]
sys.exit(f"sectors {bad} of {sys.argv[1]} are wrong" if bad else 0)
PY
}

args=(-M q35 -m 512M -cpu "${QEMU_CPU:-max}" -smp "$smp" -vga std -display none
      -serial "file:$log" -monitor "unix:$monitor,server=on,wait=off" -cdrom "$iso"
      -device edu -device pcie-root-port,id=rp1,chassis=1 -device igb,bus=rp1
      -device ahci,id=ahci1 -drive "if=none,id=disk,format=raw,file=$disk" -device ide-hd,drive=disk,bus=ahci1.0
      -drive "if=none,id=nvm,format=raw,file=$nvme_disk" -device nvme,serial=myos-test,drive=nvm
      -drive "if=none,id=usb,format=raw,file=$iso,snapshot=on" -device ide-hd,drive=usb,bus=ahci1.1
      -drive "if=none,id=fat,format=raw,file=$fat_disk" -device ide-hd,drive=fat,bus=ahci1.2)
[[ "$expect" == power ]] || args+=(-no-reboot)
if [[ -n "${QEMU_USB:-}" ]]; then
    args+=(-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0,port=2
           -device usb-hub,bus=xhci.0,port=1 -device usb-kbd,bus=xhci.0,port=1.1)
fi
if [[ "$mode" == uefi ]]; then
    [[ -n "${OVMF:-}" && -f "${OVMF}" ]] || { echo "boot-test: UEFI firmware not found (set OVMF)"; exit 2; }
    args+=(-drive "if=pflash,unit=0,format=raw,readonly=on,file=$OVMF")
fi

qemu-system-x86_64 "${args[@]}" 2>"$log.qemu" &
qemu=$!
trap 'kill $qemu 2>/dev/null; wait $qemu 2>/dev/null; rm -f "$monitor"' EXIT

# Press keys through the QEMU monitor; each one is held for 100 ms. An
# argument "=text" types the text (letters, digits, space and , . / - ! > ~);
# "@command" runs a monitor command.
send_keys() {
    python3 -I - "$monitor" "$@" <<'PY'
import socket, sys, time
names = {" ": "spc", ",": "comma", ".": "dot", "/": "slash", "-": "minus", "!": "shift-1", ">": "shift-dot",
         "~": "shift-grave_accent"}
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
for arg in sys.argv[2:]:
    if arg.startswith("@"):
        s.sendall(arg[1:].encode() + b"\n")
        time.sleep(0.2)
        continue
    if arg.startswith("="):
        keys = [names.get(c) or ("shift-" + c.lower() if c.isupper() else c) for c in arg[1:]]
    else:
        keys = [arg]
    for key in keys:
        s.sendall(b"sendkey " + key.encode() + b"\n")
        time.sleep(0.02)
time.sleep(0.5)
s.close()
PY
}

# Take a screendump and check that the console drew the boot log in color:
# mostly its background, with "[ ok ]" in green (ESC[32m). Raw escape codes,
# or no colors at all, would leave no green on screen. With "cleared": the
# screen must be almost all background, and the green boot log gone.
check_screen() {
    python3 -I - "$monitor" "$log.ppm" "${1:-}" <<'PY'
import socket, sys, time
mon, out, mode = sys.argv[1:]
try:
    import os
    os.remove(out)
except OSError:
    pass
s = socket.socket(socket.AF_UNIX)
s.connect(mon)
s.sendall(b"screendump " + out.encode() + b"\n")
for _ in range(100):  # QEMU writes a binary PPM: "P6\n<w> <h>\n255\n" + RGB
    time.sleep(0.1)
    try:
        data = open(out, "rb").read()
        magic, size, maxval, pixels = data.split(b"\n", 3)
        w, h = map(int, size.split())
        if len(pixels) == w * h * 3:
            break
    except (OSError, ValueError):
        pass
else:
    sys.exit("no screendump")
s.close()
px = [pixels[i:i + 3] for i in range(0, len(pixels), 3)]
bg, green = px.count(bytes([0x10, 0x12, 0x18])), px.count(bytes([0x5f, 0xd7, 0x5f]))
print(f"screen {w}x{h}: {bg} background pixels, {green} green")
if mode == "cleared":
    sys.exit(0 if bg > w * h * 95 // 100 and green == 0 else 1)
sys.exit(0 if bg > w * h // 2 and green > 500 else 1)
PY
}

# How many lines of the log are exactly $1 (the serial port turns \n into
# \r\n, and the shell's own line starts with its prompt).
count_lines() { grep -Fxc "$1"$'\r' "$log"; }

# Type `keys`, then wait until every count:text line in `typed` has been
# printed exactly that many times.
check_typing() {
    send_keys "${keys[@]}" || return 1
    local i want missing
    for (( i = 0; i < 40; i++ )); do
        missing=0
        for want in "${typed[@]}"; do
            [[ "$(count_lines "${want#*:}")" == "${want%%:*}" ]] || missing=1
        done
        (( missing == 0 )) && return 0
        sleep 0.5
    done
    return 1
}

# Run each command and wait until it prints its line, then `clear` and
# `reboot`. Sets `bad` to the command being checked.
check_commands() {
    local i j
    for (( i = 0; i < ${#commands[@]}; i++ )); do
        bad=${commands[i]}
        if [[ ${commands[i]} == @* ]]; then send_keys "${commands[i]}" || return 1
        else send_keys "=${commands[i]}" ret || return 1
        fi
        for (( j = 0; j < 20; j++ )); do
            grep -Eq "${outputs[i]}" "$log" && break
            sleep 0.5
        done
        (( j < 20 )) || return 1
    done
    bad=clear
    send_keys =clear ret || return 1
    check_screen cleared > "$log.screen" 2>&1 || return 1
    bad=reboot
    send_keys =reboot ret
    for (( j = 0; j < 20; j++ )); do
        if ! kill -0 $qemu 2>/dev/null; then
            grep -Fq "$reboot_marker" "$log"
            return
        fi
        sleep 0.5
    done
    return 1
}

# Wait up to 60 s for the kernel to have booted $1 times.
wait_for_boots() {
    local j
    for (( j = 0; j < 120; j++ )); do
        (( $(grep -Fc "$ok_marker" "$log") >= $1 )) && return 0
        sleep 0.5
    done
    return 1
}

# Wait up to 20 s for QEMU to exit on its own, with status 0.
wait_for_exit() {
    local j
    for (( j = 0; j < 40; j++ )); do
        if ! kill -0 $qemu 2>/dev/null; then
            wait $qemu
            return
        fi
        sleep 0.5
    done
    return 1
}

# Whether the disk self-test line names both scratch disks.
disk_test_passed() {
    local line m
    line=$(grep -F "$disk_marker" "$log") || return 1
    for m in "${disk_markers[@]}"; do grep -Fq "$m" <<< "$line" || return 1; done
}

# Wait up to 20 s for a fixed string in the log.
wait_for() {
    local j
    for (( j = 0; j < 40; j++ )); do grep -Fq "$1" "$log" && return 0; sleep 0.5; done
    return 1
}

result=""
start=$SECONDS
while (( SECONDS - start < timeout_s )); do
    if [[ "$expect" != crash ]]; then
        if grep -Eq "$fail_re" "$log"; then result=fail; break; fi
        if grep -Fq "$ok_marker" "$log"; then
            if [[ "$expect" != boot ]]; then result=pass
            elif ! grep -Fq "$cpus_marker" "$log"; then result=cpus
            elif ! grep -Fq "$smp_marker" "$log"; then result=smp
            elif ! grep -Fq "$irq_marker" "$log"; then result=irq
            elif ! grep -Fq "$timer_marker" "$log"; then result=timer
            elif ! grep -Fq "$sched_marker" "$log"; then result=sched
            elif ! grep -Fq "$sync_marker" "$log"; then result=sync
            elif ! grep -Fq "$tlb_marker" "$log"; then result=tlb
            elif ! grep -Fq "$pci_marker" "$log"; then result=pci
            elif ! grep -Fq "$msi_marker" "$log" || ! grep -Fq "$msix_marker" "$log"; then result=msi
            elif ! disk_test_passed; then result=disk
            elif ! grep -Fq "$part_marker" "$log"; then result=part
            elif ! grep -Fq "$vfs_marker" "$log"; then result=vfs
            elif ! grep -Fq "$fat_marker" "$log"; then result=fat
            elif ! grep -Fq "$initramfs_marker" "$log"; then result=initramfs
            elif ! grep -Fq "$syscall_marker" "$log"; then result=syscall
            elif ! grep -Fq "$user_marker" "$log"; then result=user
            elif ! grep -Fq "$kbd_marker" "$log"; then result=kbd
            elif [[ -n "${QEMU_USB:-}" ]] && ! grep -Fq "$usb_marker" "$log"; then result=usb
            elif ! wait_for "$init_marker" || ! wait_for "$sh_marker"; then result=init
            else result=pass
            fi
            break
        fi
    else
        if grep -Fq "$halted_marker" "$log"; then
            grep -Fq "$crash_marker" "$log" && grep -Eq "$crash_bt_re" "$log" && result=pass || result=fail
            break
        fi
    fi
    # -no-reboot makes QEMU exit on a triple fault (in every mode but power).
    if ! kill -0 $qemu 2>/dev/null; then result=exited; break; fi
    sleep 0.5
done
if [[ "$result" == pass ]]; then
    case "$expect" in
    boot)
        if ! check_screen > "$log.screen" 2>&1; then result=screen
        elif ! check_typing; then result=typing
        elif ! check_commands; then result=command
        elif ! { check_disk "$disk" && check_disk "$nvme_disk"; } > "$log.screen" 2>&1; then
            result=diskimage # QEMU is gone: `reboot` ended it
        elif ! check_fat_disk "$fat_disk" > "$log.screen" 2>&1; then
            result=fatimage
        fi ;;
    power)
        send_keys =reboot ret
        if ! wait_for_boots 2; then result=reboot
        else
            send_keys =poweroff ret # QEMU may be gone before this returns
            if ! wait_for_exit || ! grep -Fq "$poweroff_marker" "$log"; then result=poweroff; fi
        fi ;;
    panic)
        send_keys =panic ret
        if ! wait_for "$halted_marker" || ! grep -Fq "$panic_marker" "$log" || ! grep -Eq "$panic_bt_re" "$log"; then
            result=panic
        fi ;;
    fault)
        send_keys "=panic fault" ret
        if ! wait_for "$halted_marker" || ! grep -Fq "$crash_marker" "$log"; then result=fault; fi ;;
    frame)
        send_keys "=panic frame" ret
        if ! wait_for "$halted_marker" || ! grep -Fq "$crash_marker" "$log" || ! grep -Eq "$frame_bt_re" "$log"; then
            result=frame
        fi ;;
    nested)
        send_keys "=panic nested" ret
        if ! wait_for "$halted_marker" || ! grep -Fq "$nested_marker" "$log"; then result=nested; fi ;;
    esac
    # A triple fault ends QEMU (-no-reboot); a crash screen leaves it running.
    if [[ "$result" == pass && "$expect" =~ ^(panic|fault|frame|nested)$ ]] && ! kill -0 $qemu 2>/dev/null; then
        result=exited
    fi
fi
sleep 1 # let the last lines reach the log, and any stray interrupt arrive
if [[ "$result" == pass && ( "$expect" == boot || "$expect" == power ) ]] && grep -Eq "$fail_re" "$log"; then result=fail; fi

label="[$mode/$expect${QEMU_CPU:+ -cpu $QEMU_CPU}${QEMU_USB:+ +usb}]"
case "$result" in
pass)
    echo "PASS $label in $((SECONDS - start))s"
    exit 0 ;;
fail)    echo "FAIL $label unexpected kernel output:" ;;
cpus)    echo "FAIL $label ACPI did not report $smp CPUs (\"$cpus_marker\"):" ;;
smp)     echo "FAIL $label not every CPU came online (\"$smp_marker\"):" ;;
irq)     echo "FAIL $label no device IRQ through the I/O APIC (\"$irq_marker\"):" ;;
timer)   echo "FAIL $label the timer tick did not pass its self-test (\"$timer_marker\"):" ;;
sched)   echo "FAIL $label the scheduler did not pass its self-test (\"$sched_marker\"):" ;;
sync)    echo "FAIL $label the locks did not pass their self-test (\"$sync_marker\"):" ;;
tlb)     echo "FAIL $label the TLB shootdown self-test did not pass (\"$tlb_marker\"):" ;;
pci)     echo "FAIL $label PCI enumeration did not find devices through ECAM (\"$pci_marker\"):" ;;
msi)     echo "FAIL $label QEMU's edu and igb devices did not raise an MSI and an MSI-X (\"$msi_marker\", \"$msix_marker\"):" ;;
disk)    echo "FAIL $label the disk self-test did not pass on the AHCI and NVMe scratch disks (\"$disk_marker\" with \"${disk_markers[0]}\", \"${disk_markers[1]}\"):" ;;
diskimage) echo "FAIL $label the disk image does not hold what the kernel wrote ($(cat "$log.screen")):" ;;
vfs)     echo "FAIL $label the VFS did not pass its self-test (\"$vfs_marker\"):" ;;
part)    echo "FAIL $label the partition tables were not read (\"$part_marker\"):" ;;
fat)     echo "FAIL $label the FAT partitions were not mounted (\"$fat_marker\"):" ;;
fatimage) echo "FAIL $label the FAT32 disk image does not hold what the shell did ($(cat "$log.screen")):" ;;
initramfs) echo "FAIL $label the initramfs was not unpacked (\"$initramfs_marker\"):" ;;
syscall) echo "FAIL $label system calls were not set up (\"$syscall_marker\"):" ;;
user)    echo "FAIL $label user programs did not pass their self-test (\"$user_marker\"):" ;;
init)    echo "FAIL $label /bin/init did not pass its self-test and start the shell (\"$init_marker\", \"$sh_marker\"):" ;;
kbd)     echo "FAIL $label no PS/2 keyboard (\"$kbd_marker\"):" ;;
usb)     echo "FAIL $label the USB keyboards are not ready (\"$usb_marker\"):" ;;
screen)  echo "FAIL $label the screen does not show the colored boot log ($(cat "$log.screen")):" ;;
typing)  echo "FAIL $label the shell did not print these lines (count:text): ${typed[*]}" ;;
command) echo "FAIL $label the shell command \`$bad\` did not do what it should $(cat "$log.screen" 2>/dev/null):" ;;
reboot)  echo "FAIL $label \`reboot\` did not restart the machine (\"$ok_marker\" only once):" ;;
poweroff) echo "FAIL $label \`poweroff\` did not turn the machine off (QEMU still running, or exit status not 0):" ;;
panic)   echo "FAIL $label the panic command did not panic with a backtrace (\"$panic_marker\", /$panic_bt_re/, \"$halted_marker\"):" ;;
fault)   echo "FAIL $label \`panic fault\` did not bring up the crash screen (\"$crash_marker\"):" ;;
frame)   echo "FAIL $label \`panic frame\` did not bring up the crash screen with a backtrace stopped at the bad frame (\"$crash_marker\", /$frame_bt_re/):" ;;
nested)  echo "FAIL $label \`panic nested\` did not report the fault inside the panic and halt (\"$nested_marker\", \"$halted_marker\"):" ;;
exited)  echo "FAIL $label QEMU exited early (triple fault / reset?):" ;;
*)       echo "FAIL $label no result within ${timeout_s}s:" ;;
esac
# Strip the firmware's terminal escape codes so the log is readable.
sed -e 's/\x1b\[[0-9;=]*[a-zA-Z]//g' "$log" | sed 's/^/    /'
if [[ -s "$log.qemu" ]]; then echo "  QEMU stderr:"; sed 's/^/    /' "$log.qemu"; fi
exit 1
