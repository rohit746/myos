#!/usr/bin/env bash
# Boot a kernel built with -DMYOS_KTEST (`make test-kernel`) headless in
# QEMU and pass or fail on its unit tests.
#
#   scripts/ktest.sh <iso> <logfile>
#
# The kernel runs every KTEST at the end of boot, prints TAP to the serial
# port (the log), and exits QEMU through the isa-debug-exit device: QEMU's
# status is 1 when every test passed and 3 when any failed. Anything else (a
# panic, a CPU exception, a reset, the timeout) fails too. The TAP output is
# printed either way, so a failing test shows up by name.
#
# With $OVMF set to the UEFI firmware, QEMU boots through UEFI; otherwise
# through legacy BIOS. Timeout: $KTEST_TIMEOUT (seconds).

set -uo pipefail

iso=${1:?iso path}
log=${2:?log path}
timeout_s=${KTEST_TIMEOUT:-90}

[[ -f "$iso" ]] || { echo "ktest: $iso not found"; exit 2; }
mkdir -p "$(dirname "$log")"
: > "$log"

args=(-M q35 -m 512M -cpu max -smp 4 -vga std -display none -no-reboot
      -serial "file:$log" -cdrom "$iso"
      -device isa-debug-exit,iobase=0xf4,iosize=0x04)
mode=bios
if [[ -n "${OVMF:-}" ]]; then
    [[ -f "$OVMF" ]] || { echo "ktest: UEFI firmware $OVMF not found"; exit 2; }
    args+=(-drive "if=pflash,unit=0,format=raw,readonly=on,file=$OVMF")
    mode=uefi
fi

qemu-system-x86_64 "${args[@]}" 2>"$log.qemu" &
qemu=$!
trap 'kill $qemu 2>/dev/null; wait $qemu 2>/dev/null' EXIT

# Wait for QEMU to exit, giving up on a panic or the timeout.
status=
crashed=
for (( i = 0; i < timeout_s * 10; i++ )); do
    if ! kill -0 $qemu 2>/dev/null; then
        wait $qemu
        status=$?
        break
    fi
    if grep -qE "KERNEL PANIC|CPU EXCEPTION" "$log"; then
        crashed=1
        sleep 1 # let the crash screen finish printing
        break
    fi
    sleep 0.1
done

# The TAP output, from "TAP version" on.
sed -n '/^TAP version/,$p' "$log" | tr -d '\r'

if [[ "$status" == 1 ]] && grep -q "^# [0-9]* passed, 0 failed" "$log"; then
    echo "ktest ($mode): PASS"
    exit 0
fi
if [[ "$status" == 3 ]]; then
    echo "ktest ($mode): FAIL:"
    grep -a "^not ok " "$log" | tr -d '\r' | sed 's/^/  /'
    exit 1
fi
if [[ -n "$crashed" ]]; then
    echo "ktest ($mode): FAIL: the kernel crashed; the end of $log:"
elif [[ -n "$status" ]]; then
    echo "ktest ($mode): FAIL: QEMU exited with status $status before the tests finished; the end of $log:"
else
    echo "ktest ($mode): FAIL: no result after ${timeout_s} s; the end of $log:"
fi
tail -n 40 "$log" | tr -d '\r'
[[ -s "$log.qemu" ]] && cat "$log.qemu"
exit 1
