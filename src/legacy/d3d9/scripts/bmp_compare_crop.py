#!/usr/bin/env python3
"""Side-by-side crop of two BMP images (reference | ours | amplified difference) as one PNG, no third-party packages.

    python tools/bmp_compare_crop.py <reference.bmp> <ours.bmp> <out.png> x y w h [--zoom N]
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bmp_to_png import read_bmp, write_png  # noqa: E402


def main():
    args = sys.argv[1:]
    zoom = 1
    if "--zoom" in args:
        i = args.index("--zoom")
        zoom = int(args[i + 1])
        del args[i:i + 2]
    ref_path, our_path, out, x, y, w, h = args[0], args[1], args[2], *map(int, args[3:7])
    rw, rh, ref = read_bmp(ref_path)
    ow, oh, ours = read_bmp(our_path)
    rows = []
    for yy in range(y, min(y + h, rh)):
        a = ref[yy][x * 3:(x + w) * 3]
        b = ours[yy][x * 3:(x + w) * 3]
        d = bytes(min(255, abs(p - q) * 4) for p, q in zip(a, b))
        row = a + b + d
        if zoom > 1:
            row = bytes(c for px in range(len(row) // 3) for _ in range(zoom) for c in row[px * 3:px * 3 + 3])
        rows.extend([row] * zoom)
    write_png(out, (w * 3) * zoom, len(rows), rows)


if __name__ == "__main__":
    main()
