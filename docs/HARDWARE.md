# Hardware test matrix

Which physical machines myos has been booted on, and what happened. QEMU is
far more forgiving than real firmware, so a change that passes `make test`
can still hang or crash on a real PC. This table is how such regressions get
noticed.

To add a machine, open a
[hardware report](https://github.com/rohit746/myos/issues/new?template=hardware-report.yml)
or add a row here in a pull request. After each milestone, try the new
version on at least one older BIOS/CSM PC and one modern UEFI laptop, and
add a row for each, even if nothing changed.

## Real machines

| Machine | CPU | Firmware | Boot mode | myos version | Result | Notes |
|---------|-----|----------|-----------|--------------|--------|-------|
| | | | | | | |

No real machine has been recorded yet.

How to fill in the columns:

- **Machine**: make and model, e.g. "Lenovo ThinkPad T480" or "Custom desktop, ASUS PRIME B450M-A".
- **CPU**: the `cpu:` line of the boot log.
- **Firmware**: `UEFI` (no legacy option), `UEFI + CSM` (has a legacy/CSM
  option), or `BIOS` (legacy only).
- **Boot mode**: the end of the boot log's `booted by Limine ... via` line:
  `UEFI (64-bit)` or `legacy BIOS`.
- **myos version**: the first line of the boot log (`myos v0.1.0` or a commit
  like `myos 969ccb2`).
- **Result**: one of
  - **shell**: the `myos>` prompt came up and the keyboard types into it
  - **partial**: the shell came up but something is wrong (say what in Notes)
  - **crash**: the red crash screen (link a photo)
  - **hang**: the screen stopped changing (note the last line printed)
  - **no boot**: Limine or the kernel never appeared
- **Notes**: keyboard (PS/2, USB, built-in), disks found, anything odd, and a
  link to the issue or photo.

## Emulator baseline

What CI checks on every pull request (`make test`, `-M q35 -m 512M -cpu max -smp 4`).
A real machine that fails where these pass points at something QEMU doesn't
emulate: the firmware, a device, or timing.

| Machine | CPU | Firmware | Boot mode | myos version | Result | Notes |
|---------|-----|----------|-----------|--------------|--------|-------|
| QEMU 11.1 q35 | QEMU TCG (`-cpu max`), 4 CPUs | OVMF (UEFI) | UEFI (64-bit) | 969ccb2 | shell | `make test`: shell, reboot and poweroff pass |
| QEMU 11.1 q35 | QEMU TCG (`-cpu max`), 4 CPUs | SeaBIOS | legacy BIOS | 969ccb2 | shell | `make test`: shell, reboot and poweroff pass |

## Common gotchas

- **Secure Boot must be off.** Limine isn't signed with Microsoft's keys, so
  with Secure Boot on, the firmware refuses the USB stick, often with no
  message or a "security violation" one. Some laptops only let you turn it
  off after setting a firmware (supervisor) password.
- **Pick the USB stick in the one-time boot menu** (usually F12, F11, F8 or
  Esc right after power-on) instead of changing the permanent boot order. If
  the stick isn't listed, check that USB boot is enabled in the firmware
  settings; some machines also hide USB devices from the menu while "Fast
  Boot" is on.
- **CSM vs UEFI entries.** On a machine with CSM (legacy support) enabled the
  boot menu often lists the stick twice, e.g. "UEFI: SanDisk" and "SanDisk".
  The "UEFI:" entry boots through UEFI; the other boots through the legacy
  BIOS path. Both should work: test the one you mean and write it down
  (the boot log's `via` line says which one ran). UEFI-only machines (most
  laptops since about 2020) have no legacy entry at all.
- **Keyboard.** PS/2 and USB keyboards both work. On a laptop the built-in
  keyboard is usually PS/2-compatible; if it doesn't type, try a USB
  keyboard and say so in the report.
- **No serial port.** Most PCs have none, so the screen is the only output.
  Photograph the screen, especially the crash screen: its backtrace names
  the functions and can be turned into source lines with `llvm-symbolizer`
  (see the [README](../README.md#booting-a-real-pc)).
