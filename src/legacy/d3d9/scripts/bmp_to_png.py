#!/usr/bin/env python3
"""Converts 24/32-bit BMP files (the recorder's reference images) to PNG, optionally downscaled, with no third-party packages.

    python tools/bmp_to_png.py <in.bmp> <out.png> [--scale N]     (N: keep every Nth pixel in both directions)
"""
import struct
import sys
import zlib


def read_bmp(path):
    data = open(path, "rb").read()
    off = struct.unpack_from("<I", data, 10)[0]
    _, w, h, _, bpp, comp = struct.unpack_from("<IiiHHI", data, 14)
    if comp not in (0, 3) or bpp not in (24, 32):
        raise SystemExit(f"{path}: unsupported BMP (bpp {bpp}, compression {comp})")
    top_down = h < 0
    h = abs(h)
    row = (w * bpp // 8 + 3) & ~3
    rows = []
    for y in range(h):
        src = off + (y if top_down else h - 1 - y) * row
        line = data[src:src + w * bpp // 8]
        step = bpp // 8
        rgb = bytearray(w * 3)
        rgb[0::3] = line[2::step]
        rgb[1::3] = line[1::step]
        rgb[2::3] = line[0::step]
        rows.append(bytes(rgb))
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\0" + r for r in rows)

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    args = sys.argv[1:]
    scale = 1
    if "--scale" in args:
        i = args.index("--scale")
        scale = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        raise SystemExit(__doc__)
    w, h, rows = read_bmp(args[0])
    if scale > 1:
        rows = [bytes(b for x in range(0, w, scale) for b in r[x * 3:x * 3 + 3]) for r in rows[::scale]]
        w, h = (w + scale - 1) // scale, (h + scale - 1) // scale
    write_png(args[1], w, h, rows)


if __name__ == "__main__":
    main()
