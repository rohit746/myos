#!/bin/bash
# Write the myos ISO to a USB stick so a real PC can boot it.
#
#   scripts/write-usb.sh /dev/diskN build/myos.iso     (macOS)
#   scripts/write-usb.sh /dev/sdX   build/myos.iso     (Linux)
#
# This ERASES the whole target device. The script refuses anything that
# doesn't look like an external/removable disk, and asks you to type the
# device name back before writing.

set -euo pipefail

dev="${1:-}"
iso="${2:-build/myos.iso}"

list_disks() {
    if [[ "$(uname)" == Darwin ]]; then
        diskutil list external physical
    else
        lsblk -d -o NAME,SIZE,TRAN,RM,MODEL | awk 'NR==1 || $3=="usb" || $4=="1"'
    fi
}

if [[ -z "$dev" ]]; then
    echo "usage: make usb DEV=/dev/diskN"
    echo
    echo "External disks I can see:"
    list_disks
    exit 1
fi

[[ -f "$iso" ]] || { echo "error: $iso not found (run make first)"; exit 1; }
[[ -e "$dev" ]] || { echo "error: $dev does not exist"; echo; list_disks; exit 1; }

if [[ "$(uname)" == Darwin ]]; then
    [[ "$dev" =~ ^/dev/disk[0-9]+$ ]] || { echo "error: pass a whole disk like /dev/disk4, not a partition"; exit 1; }
    info="$(diskutil info "$dev")"
    if ! grep -Eq 'Device Location: +External|Removable Media: +Removable|Protocol: +USB' <<<"$info" \
       || grep -Eq 'Internal: +Yes' <<<"$info"; then
        echo "error: $dev does not look like an external USB disk. Refusing."
        exit 1
    fi
    echo "$info" | grep -E 'Device / Media Name|Disk Size|Protocol|Device Location' || true
    raw="/dev/r${dev#/dev/}" # raw device: much faster writes
else
    name="$(basename "$dev")"
    [[ "$(cat "/sys/block/$name/removable" 2>/dev/null)" == 1 ]] ||
        [[ "$(lsblk -dno TRAN "$dev" 2>/dev/null)" == usb ]] ||
        { echo "error: $dev does not look like a removable/USB disk. Refusing."; exit 1; }
    lsblk "$dev"
    raw="$dev"
fi

echo
echo "This will ERASE EVERYTHING on $dev and write $iso to it."
read -r -p "Type the device name ($dev) to continue: " answer
[[ "$answer" == "$dev" ]] || { echo "Aborted."; exit 1; }

if [[ "$(uname)" == Darwin ]]; then
    diskutil unmountDisk "$dev"
    sudo dd if="$iso" of="$raw" bs=4m
    sync
    diskutil eject "$dev"
else
    sudo umount "$dev"?* 2>/dev/null || true
    sudo dd if="$iso" of="$raw" bs=4M conv=fsync status=progress
    sync
fi

echo
echo "Done. Plug the stick into the target PC, open its boot menu"
echo "(usually F12, F11, F8 or Esc at power-on) and pick the USB drive."
