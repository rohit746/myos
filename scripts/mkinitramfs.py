#!/usr/bin/env python3
"""Pack a directory into a USTAR archive for the initramfs.

    scripts/mkinitramfs.py <dir> <out.tar>

Plain USTAR (no pax or GNU extensions), entries sorted by path, owned by
root, so the same tree gives the same archive on macOS and Linux.
"""
import os
import sys
import tarfile

src, out = sys.argv[1], sys.argv[2]

def reset(info):
    info.uid = info.gid = 0
    info.uname = info.gname = "root"
    info.mtime = 0
    return info

tmp = out + ".tmp"
with tarfile.open(tmp, "w", format=tarfile.USTAR_FORMAT) as tar:
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames.sort()
        rel = os.path.relpath(dirpath, src)
        for name in sorted(dirnames) + sorted(filenames):
            if name == ".DS_Store":
                continue
            path = os.path.join(dirpath, name)
            arcname = os.path.normpath(os.path.join(rel, name))
            tar.add(path, arcname=arcname, recursive=False, filter=reset)
os.replace(tmp, out)
