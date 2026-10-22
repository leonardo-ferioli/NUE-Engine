#!/usr/bin/env python3
"""Prints the mean absolute difference (0..255) of two BMP images per cell of a grid, to see where they disagree.

    python tools/bmp_diff_grid.py <reference.bmp> <ours.bmp> [cols rows]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bmp_to_png import read_bmp  # noqa: E402


def main():
    a, b = sys.argv[1], sys.argv[2]
    cols, rows = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) >= 5 else (16, 9)
    w, h, ra = read_bmp(a)
    _, _, rb = read_bmp(b)
    cw, ch = w // cols, h // rows
    print("     " + "".join(f"{c * cw:5d}" for c in range(cols)))
    for r in range(rows):
        line = []
        for c in range(cols):
            total = 0
            for y in range(r * ch, (r + 1) * ch, 2):
                pa, pb = ra[y], rb[y]
                for x in range(c * cw * 3, (c + 1) * cw * 3, 6):
                    total += abs(pa[x] - pb[x]) + abs(pa[x + 1] - pb[x + 1]) + abs(pa[x + 2] - pb[x + 2])
            line.append(total / 3 / ((ch // 2) * (cw // 2)))
        print(f"{r * ch:4d} " + "".join(f"{v:5.0f}" for v in line))


if __name__ == "__main__":
    main()
