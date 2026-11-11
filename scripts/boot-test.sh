#!/usr/bin/env bash
# Boot an ISO in QEMU without a display and check the kernel's serial output.
#
#   scripts/boot-test.sh <uefi|bios> <boot|power|crash|panic|fault> <iso> <logfile>
#
#   boot   pass when the kernel reports it's up, ACPI found all the CPUs QEMU
#          was given, a PIT IRQ came in through the I/O APIC, the timer
#          tick ran at the calibrated rate, the scheduler preempted threads
#          that never yield, threads contending on locks got exact counts
#          (and the lock debug checks fired), the VFS passed its self-test
#          and the initramfs was unpacked into /, user programs passed their
#          self-test in ring 3, and the PS/2 keyboard is ready,
#          a screendump shows the green "[ ok ]" text the escape codes ask
#          for, and then, typing through QEMU's monitor, the shell works:
#          lines edited with Shift, Caps Lock, Num Lock, Backspace, Delete,
#          Left/Right, Home/End and the history (Up/Down) are echoed right,
#          every command prints what it should (`ls` and `cat` show files
#          from the initramfs; `user hello` prints from ring 3, `user kread`
#          gets killed for reading a kernel address and the shell carries
#          on), `clear` blanks the screen
#          and `reboot` resets the machine (QEMU exits: -no-reboot); fail on
#          any panic, exception or unhandled interrupt
#   power  pass when `reboot` restarts the machine (the kernel boots a second
#          time) and `poweroff` then turns it off through ACPI: QEMU exits on
#          its own, with status 0. QEMU runs without -no-reboot here, so a
#          reset or triple fault reboots instead of passing for a power-off
#   crash  pass when the crash screen for the deliberate page fault appears
#   panic  pass when the shell's `panic` command panics the kernel
#   fault  pass when the shell's `panic fault` brings up the crash screen
#
# UEFI mode needs the firmware path in $OVMF. Timeout: $BOOT_TIMEOUT (seconds).
# QEMU's CPU model: $QEMU_CPU (default max; max,-x2apic tests xAPIC mode).

set -uo pipefail

mode=${1:?mode: uefi|bios}
expect=${2:?expect: boot|power|crash|panic|fault}
iso=${3:?iso path}
log=${4:?log path}
timeout_s=${BOOT_TIMEOUT:-90}

ok_marker="Kernel is up."
smp=4 # CPUs QEMU gets; the ACPI summary must list them all
cpus_marker="cpus:   $smp,"
irq_marker="IRQ self-test passed"
timer_marker="timer self-test passed"
sched_marker="scheduler self-test passed"
sync_marker="sync self-test passed"
vfs_marker="VFS self-test passed"
initramfs_marker="initramfs: unpacked"
syscall_marker="system calls via syscall/sysret"
user_marker="user mode self-test passed"
kbd_marker="PS/2 keyboard ready"
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
# Commands to run, and a line (extended regex) each must print.
commands=(help uptime mem cpu acpi ps threads "user hello" "user kread" "user priv" "ls /" "ls /etc" "cat /etc/motd" "ls /dev" "stat /dev/null"
          "mkdir /etc" "cat /nope" nope)
outputs=("^  reboot +restart the machine"
         "^up [0-9]+:[0-9]{2}:[0-9]{2} \([0-9]+\.[0-9]{3} s, [0-9]+ ticks at 1000 Hz\)"
         "^physical: [0-9]+ MiB managed"
         "^cpus: +$smp in the MADT"
         "^ACPI .*: [0-9]+ tables via the"
         "^ +[0-9]+ +running +[0-9]+ +[0-9]+ +main"
         "^threads: 4 threads ran and exited"
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
         "^nope: unknown command")
reboot_marker="Rebooting..."
poweroff_marker="Powering off..."
crash_marker="CPU EXCEPTION 14: Page fault"
panic_marker="KERNEL PANIC *** panic command"
halted_marker="System halted."
fail_re="KERNEL PANIC|CPU EXCEPTION|unhandled interrupt|\\[warn\\] (IRQ|timer|PS/2)"

[[ -f "$iso" ]] || { echo "boot-test: $iso not found"; exit 2; }
mkdir -p "$(dirname "$log")"
: > "$log"

monitor="$log.mon"
rm -f "$monitor" "$log.screen"
args=(-M q35 -m 512M -cpu "${QEMU_CPU:-max}" -smp "$smp" -vga std -display none
      -serial "file:$log" -monitor "unix:$monitor,server=on,wait=off" -cdrom "$iso")
[[ "$expect" == power ]] || args+=(-no-reboot)
if [[ "$mode" == uefi ]]; then
    [[ -n "${OVMF:-}" && -f "${OVMF}" ]] || { echo "boot-test: UEFI firmware not found (set OVMF)"; exit 2; }
    args+=(-drive "if=pflash,unit=0,format=raw,readonly=on,file=$OVMF")
fi

qemu-system-x86_64 "${args[@]}" 2>"$log.qemu" &
qemu=$!
trap 'kill $qemu 2>/dev/null; wait $qemu 2>/dev/null; rm -f "$monitor"' EXIT

# Press keys through the QEMU monitor; each one is held for 100 ms. An
# argument "=text" types the text (letters, digits, space and , . / - !).
send_keys() {
    python3 -I - "$monitor" "$@" <<'PY'
import socket, sys, time
names = {" ": "spc", ",": "comma", ".": "dot", "/": "slash", "-": "minus", "!": "shift-1"}
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
for arg in sys.argv[2:]:
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
        send_keys "=${commands[i]}" ret || return 1
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
            elif ! grep -Fq "$irq_marker" "$log"; then result=irq
            elif ! grep -Fq "$timer_marker" "$log"; then result=timer
            elif ! grep -Fq "$sched_marker" "$log"; then result=sched
            elif ! grep -Fq "$sync_marker" "$log"; then result=sync
            elif ! grep -Fq "$vfs_marker" "$log"; then result=vfs
            elif ! grep -Fq "$initramfs_marker" "$log"; then result=initramfs
            elif ! grep -Fq "$syscall_marker" "$log"; then result=syscall
            elif ! grep -Fq "$user_marker" "$log"; then result=user
            elif ! grep -Fq "$kbd_marker" "$log"; then result=kbd
            else result=pass
            fi
            break
        fi
    else
        if grep -Fq "$halted_marker" "$log"; then
            grep -Fq "$crash_marker" "$log" && result=pass || result=fail
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
        wait_for "$panic_marker" || result=panic ;;
    fault)
        send_keys "=panic fault" ret
        if ! wait_for "$halted_marker" || ! grep -Fq "$crash_marker" "$log"; then result=fault; fi ;;
    esac
fi
sleep 1 # let the last lines reach the log, and any stray interrupt arrive
if [[ "$result" == pass && ( "$expect" == boot || "$expect" == power ) ]] && grep -Eq "$fail_re" "$log"; then result=fail; fi

label="[$mode/$expect${QEMU_CPU:+ -cpu $QEMU_CPU}]"
case "$result" in
pass)
    echo "PASS $label in $((SECONDS - start))s"
    exit 0 ;;
fail)    echo "FAIL $label unexpected kernel output:" ;;
cpus)    echo "FAIL $label ACPI did not report $smp CPUs (\"$cpus_marker\"):" ;;
irq)     echo "FAIL $label no device IRQ through the I/O APIC (\"$irq_marker\"):" ;;
timer)   echo "FAIL $label the timer tick did not pass its self-test (\"$timer_marker\"):" ;;
sched)   echo "FAIL $label the scheduler did not pass its self-test (\"$sched_marker\"):" ;;
sync)    echo "FAIL $label the locks did not pass their self-test (\"$sync_marker\"):" ;;
vfs)     echo "FAIL $label the VFS did not pass its self-test (\"$vfs_marker\"):" ;;
initramfs) echo "FAIL $label the initramfs was not unpacked (\"$initramfs_marker\"):" ;;
syscall) echo "FAIL $label system calls were not set up (\"$syscall_marker\"):" ;;
user)    echo "FAIL $label user programs did not pass their self-test (\"$user_marker\"):" ;;
kbd)     echo "FAIL $label no PS/2 keyboard (\"$kbd_marker\"):" ;;
screen)  echo "FAIL $label the screen does not show the colored boot log ($(cat "$log.screen")):" ;;
typing)  echo "FAIL $label the shell did not print these lines (count:text): ${typed[*]}" ;;
command) echo "FAIL $label the shell command \`$bad\` did not do what it should $(cat "$log.screen" 2>/dev/null):" ;;
reboot)  echo "FAIL $label \`reboot\` did not restart the machine (\"$ok_marker\" only once):" ;;
poweroff) echo "FAIL $label \`poweroff\` did not turn the machine off (QEMU still running, or exit status not 0):" ;;
panic)   echo "FAIL $label the panic command did not panic (\"$panic_marker\"):" ;;
fault)   echo "FAIL $label \`panic fault\` did not bring up the crash screen (\"$crash_marker\"):" ;;
exited)  echo "FAIL $label QEMU exited early (triple fault / reset?):" ;;
*)       echo "FAIL $label no result within ${timeout_s}s:" ;;
esac
# Strip the firmware's terminal escape codes so the log is readable.
sed -e 's/\x1b\[[0-9;=]*[a-zA-Z]//g' "$log" | sed 's/^/    /'
if [[ -s "$log.qemu" ]]; then echo "  QEMU stderr:"; sed 's/^/    /' "$log.qemu"; fi
exit 1
