#!/usr/bin/env python3
"""Convert a fixed-width BDF font to a PSF2 font with a Unicode table.

    scripts/bdf2psf.py <font.bdf> <out.psf>

Keeps the printable Latin-1 glyphs (U+0020-U+007E, U+00A0-U+00FF) that the
BDF has. Used to make vendor/spleen/spleen-8x16.psf from Spleen's release;
see vendor/spleen/README.
"""
import struct
import sys

PSF2_MAGIC = 0x864AB572
PSF2_HAS_UNICODE_TABLE = 1


def wanted(cp):
    return 0x20 <= cp <= 0x7E or 0xA0 <= cp <= 0xFF


def read_bdf(path):
    width = height = None
    glyphs = {}
    cp = None
    rows = None
    with open(path, encoding="ascii") as f:
        for line in f:
            words = line.split()
            if not words:
                continue
            if words[0] == "FONTBOUNDINGBOX":
                width, height = int(words[1]), int(words[2])
            elif words[0] == "ENCODING":
                cp = int(words[1])
            elif words[0] == "BBX":
                if (int(words[1]), int(words[2])) != (width, height):
                    sys.exit(f"bdf2psf: glyph U+{cp:04X} is not {width}x{height}")
            elif words[0] == "BITMAP":
                rows = []
            elif words[0] == "ENDCHAR":
                if wanted(cp):
                    glyphs[cp] = rows
                rows = None
            elif rows is not None:
                rows.append(bytes.fromhex(words[0]))
    return width, height, glyphs


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    width, height, glyphs = read_bdf(sys.argv[1])
    bytes_per_row = (width + 7) // 8
    order = sorted(glyphs)

    bitmaps = bytearray()
    for cp in order:
        rows = glyphs[cp]
        if len(rows) != height or any(len(r) != bytes_per_row for r in rows):
            sys.exit(f"bdf2psf: glyph U+{cp:04X} has a bad bitmap")
        bitmaps += b"".join(rows)

    # One entry per glyph: its code point in UTF-8, then 0xFF.
    table = bytearray()
    for cp in order:
        table += chr(cp).encode("utf-8") + b"\xff"

    header = struct.pack("<8I", PSF2_MAGIC, 0, 32, PSF2_HAS_UNICODE_TABLE, len(order),
                         bytes_per_row * height, height, width)
    with open(sys.argv[2], "wb") as out:
        out.write(header + bitmaps + table)
    print(f"bdf2psf: {len(order)} glyphs, {width}x{height}", file=sys.stderr)


main()
