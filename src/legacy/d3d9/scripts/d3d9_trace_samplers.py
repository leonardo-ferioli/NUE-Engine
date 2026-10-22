#!/usr/bin/env python3
"""Histogram of the values the game gives to sampler states and texture stage transform flags inside the recording windows of a trace.

    python tools/d3d9_trace_samplers.py <trace folder> [--max-windows N]
"""
import collections
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def method_names():
    text = open(os.path.join(HERE, "..", "nue_d3d9", "src", "null_gpu.cpp"), encoding="utf-8").read()
    block = text[text.index("static const char* const names[]"):]
    block = block[:block.index("};")]
    return re.findall(r'"([a-z0-9_]+)"', block)


def main():
    folder = sys.argv[1]
    max_windows = int(sys.argv[sys.argv.index("--max-windows") + 1]) if "--max-windows" in sys.argv else 1
    names = method_names()
    data = open(os.path.join(folder, "trace.d3d9"), "rb").read()
    pos, windows, in_window = 12, 0, False
    hist = collections.defaultdict(collections.Counter)
    tss = collections.defaultdict(collections.Counter)
    while pos + 5 <= len(data):
        op = data[pos]
        size = struct.unpack_from("<I", data, pos + 1)[0]
        w = struct.unpack_from("<%dI" % (size // 4), data, pos + 5)
        pos += 5 + size
        if op == 134:
            windows += 1
            in_window = True
            if windows > max_windows:
                break
            continue
        if op == 135:
            in_window = False
        if not in_window or op >= len(names):
            continue
        if names[op] == "set_sampler_state":
            hist[w[1]][w[2]] += 1
        elif names[op] == "set_texture_stage_state" and w[1] in (24, 11):
            tss[w[1]][w[2]] += 1
    sampler_names = {1: "ADDRESSU", 2: "ADDRESSV", 3: "ADDRESSW", 4: "BORDERCOLOR", 5: "MAGFILTER", 6: "MINFILTER", 7: "MIPFILTER", 8: "MIPMAPLODBIAS", 9: "MAXMIPLEVEL", 10: "MAXANISOTROPY", 11: "SRGBTEXTURE"}
    for t in sorted(hist):
        print(sampler_names.get(t, t), dict(hist[t].most_common(8)))
    print("TEXTURETRANSFORMFLAGS", dict(tss[24].most_common(8)))
    print("TEXCOORDINDEX", dict(tss[11].most_common(8)))


if __name__ == "__main__":
    main()
