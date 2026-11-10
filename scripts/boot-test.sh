#!/usr/bin/env bash
# Boot an ISO in QEMU without a display and check the kernel's serial output.
#
#   scripts/boot-test.sh <uefi|bios> <boot|crash> <iso> <logfile>
#
#   boot   pass when the kernel reports it's up, ACPI found all the CPUs QEMU
#          was given, a PIT IRQ came in through the I/O APIC, the timer
#          tick ran at the calibrated rate and the PS/2 keyboard is ready,
#          and then keys typed through QEMU's monitor are echoed back right
#          (Shift, Caps Lock, Num Lock, Backspace, 0xE0 keys); fail on any
#          panic, exception or unhandled interrupt, also shortly after boot
#          while the kernel idles with interrupts on
#   crash  pass when the crash screen for the deliberate page fault appears
#
# UEFI mode needs the firmware path in $OVMF. Timeout: $BOOT_TIMEOUT (seconds).
# QEMU's CPU model: $QEMU_CPU (default max; max,-x2apic tests xAPIC mode).

set -uo pipefail

mode=${1:?mode: uefi|bios}
expect=${2:?expect: boot|crash}
iso=${3:?iso path}
log=${4:?log path}
timeout_s=${BOOT_TIMEOUT:-90}

ok_marker="Kernel is up."
smp=4 # CPUs QEMU gets; the ACPI summary must list them all
cpus_marker="cpus:   $smp,"
irq_marker="IRQ self-test passed"
timer_marker="timer self-test passed"
kbd_marker="PS/2 keyboard ready"
# Keys to type (QEMU sendkey names) and the lines the kernel must echo.
keys=(shift-h e l l p backspace o comma spc shift-w o r l d shift-1 ret
      caps_lock m y shift-o s caps_lock kp_divide num_lock kp_4 kp_2 num_lock kp_7 kp_enter)
typed=("Hello, World!" "MYoS/42")
crash_marker="CPU EXCEPTION 14: Page fault"
halted_marker="System halted."
fail_re="KERNEL PANIC|CPU EXCEPTION|unhandled interrupt|\\[warn\\] (IRQ|timer|PS/2)"

[[ -f "$iso" ]] || { echo "boot-test: $iso not found"; exit 2; }
mkdir -p "$(dirname "$log")"
: > "$log"

monitor="$log.mon"
rm -f "$monitor"
args=(-M q35 -m 512M -cpu "${QEMU_CPU:-max}" -smp "$smp" -vga std -display none -no-reboot
      -serial "file:$log" -monitor "unix:$monitor,server=on,wait=off" -cdrom "$iso")
if [[ "$mode" == uefi ]]; then
    [[ -n "${OVMF:-}" && -f "${OVMF}" ]] || { echo "boot-test: UEFI firmware not found (set OVMF)"; exit 2; }
    args+=(-drive "if=pflash,unit=0,format=raw,readonly=on,file=$OVMF")
fi

qemu-system-x86_64 "${args[@]}" 2>"$log.qemu" &
qemu=$!
trap 'kill $qemu 2>/dev/null; wait $qemu 2>/dev/null; rm -f "$monitor"' EXIT

# Press keys through the QEMU monitor; each one is held for 100 ms.
send_keys() {
    python3 -I - "$monitor" "$@" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
for key in sys.argv[2:]:
    s.sendall(b"sendkey " + key.encode() + b"\n")
    time.sleep(0.02)
time.sleep(0.5)
s.close()
EOF
}

# The kernel redraws the line being typed on, and ends it with \r\n when
# Enter is pressed (the serial port turns \n into \r\n).
typed_line() { grep -Eq "> $1"$'\r$' "$log"; }

result=""
start=$SECONDS
while (( SECONDS - start < timeout_s )); do
    if [[ "$expect" == boot ]]; then
        if grep -Eq "$fail_re" "$log"; then result=fail; break; fi
        if grep -Fq "$ok_marker" "$log"; then
            if ! grep -Fq "$cpus_marker" "$log"; then result=cpus
            elif ! grep -Fq "$irq_marker" "$log"; then result=irq
            elif ! grep -Fq "$timer_marker" "$log"; then result=timer
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
    # -no-reboot makes QEMU exit on a triple fault.
    if ! kill -0 $qemu 2>/dev/null; then result=exited; break; fi
    sleep 0.5
done
if [[ "$result" == pass && "$expect" == boot ]]; then
    result=typing
    if send_keys "${keys[@]}"; then
        for (( i = 0; i < 40; i++ )); do
            missing=0
            for line in "${typed[@]}"; do typed_line "$line" || missing=1; done
            if (( missing == 0 )); then result=pass; break; fi
            sleep 0.5
        done
    fi
fi
sleep 1 # let the last lines reach the log, and any stray interrupt arrive
if [[ "$result" == pass && "$expect" == boot ]] && grep -Eq "$fail_re" "$log"; then result=fail; fi

label="[$mode/$expect${QEMU_CPU:+ -cpu $QEMU_CPU}]"
case "$result" in
pass)
    echo "PASS $label in $((SECONDS - start))s"
    exit 0 ;;
fail)    echo "FAIL $label unexpected kernel output:" ;;
cpus)    echo "FAIL $label ACPI did not report $smp CPUs (\"$cpus_marker\"):" ;;
irq)     echo "FAIL $label no device IRQ through the I/O APIC (\"$irq_marker\"):" ;;
timer)   echo "FAIL $label the timer tick did not pass its self-test (\"$timer_marker\"):" ;;
kbd)     echo "FAIL $label no PS/2 keyboard (\"$kbd_marker\"):" ;;
typing)  echo "FAIL $label typed keys were not echoed as these lines: ${typed[*]}" ;;
exited)  echo "FAIL $label QEMU exited early (triple fault / reset?):" ;;
*)       echo "FAIL $label no result within ${timeout_s}s:" ;;
esac
# Strip the firmware's terminal escape codes so the log is readable.
sed -e 's/\x1b\[[0-9;=]*[a-zA-Z]//g' "$log" | sed 's/^/    /'
if [[ -s "$log.qemu" ]]; then echo "  QEMU stderr:"; sed 's/^/    /' "$log.qemu"; fi
exit 1
