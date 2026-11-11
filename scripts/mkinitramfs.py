#!/usr/bin/env python3
"""Pack directories into a USTAR archive for the initramfs.

    scripts/mkinitramfs.py <out.tar> <dir>...

Each directory's contents go at the root of the archive; when two hold the
same path, the later one wins (a directory that is in both appears once).
Plain USTAR (no pax or GNU extensions), entries sorted by path, owned by
root, so the same trees give the same archive on macOS and Linux.
"""
import os
import sys
import tarfile

out, srcs = sys.argv[1], sys.argv[2:]

def reset(info):
    info.uid = info.gid = 0
    info.uname = info.gname = "root"
    info.mtime = 0
    return info

entries = {}  # path in the archive -> path on disk
for src in srcs:
    for dirpath, dirnames, filenames in os.walk(src):
        rel = os.path.relpath(dirpath, src)
        for name in dirnames + filenames:
            if name == ".DS_Store":
                continue
            arcname = os.path.normpath(os.path.join(rel, name))
            if arcname in entries and os.path.isdir(entries[arcname]) and os.path.isdir(os.path.join(dirpath, name)):
                continue
            entries[arcname] = os.path.join(dirpath, name)

tmp = out + ".tmp"
with tarfile.open(tmp, "w", format=tarfile.USTAR_FORMAT) as tar:
    # Sorted by path components, so a directory comes before what is in it.
    for arcname in sorted(entries, key=lambda a: a.split(os.sep)):
        tar.add(entries[arcname], arcname=arcname, recursive=False, filter=reset)
os.replace(tmp, out)
