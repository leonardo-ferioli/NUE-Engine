#!/usr/bin/env python3
"""What a Direct3D 9 trace asks of a translator: draws by shader combination (fixed-function or programmable), formats, usages, lock flags,
render targets, clears, state usage. Reads trace.d3d9 (the recorder's format) only; no game data is printed.

    python tools/d3d9_trace_census.py <trace folder> [--max-windows N]
"""
import collections
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXTRA = {128: "device_created", 129: "get_back_buffer", 130: "frame_marker", 131: "reference_image", 132: "unknown_call", 133: "truncated",
         134: "window_begin", 135: "window_end", 136: "end", 137: "checkpoint"}


def method_names():
    text = open(os.path.join(HERE, "..", "nue_d3d9", "src", "null_gpu.cpp"), encoding="utf-8").read()
    block = text[text.index("static const char* const names[]"):]
    block = block[:block.index("};")]
    return re.findall(r'"([a-z0-9_]+)"', block)


def main():
    folder = sys.argv[1]
    max_windows = int(sys.argv[sys.argv.index("--max-windows") + 1]) if "--max-windows" in sys.argv else 99
    names = method_names()
    data = open(os.path.join(folder, "trace.d3d9"), "rb").read()
    pos = 12
    C = collections.Counter
    draws = C(); prims = C(); fvfs = C(); rs = C(); ss = C(); tss = C()
    tex_formats = C(); tex_usage = C(); vb_usage = C(); ib_usage = C(); lock_flags = C(); rt_sets = C(); clears = C()
    draw_fixed_stage_states = C()
    in_window = False; windows = 0; frames = 0
    vs = ps = decl = fvf = 0
    decls = {}
    tex_info = {}
    window_draws = C()
    draw_up_strides = C()
    while pos + 5 <= len(data):
        op = data[pos]; size = struct.unpack_from("<I", data, pos + 1)[0]
        w = struct.unpack_from("<%dI" % (size // 4), data, pos + 5)
        pos += 5 + size
        name = names[op] if op < len(names) else EXTRA.get(op, "op%d" % op)
        if name == "window_begin":
            in_window = True; windows += 1
            if windows > max_windows: break
            continue
        if name == "window_end":
            in_window = False; continue
        if name == "create_texture":
            tex_formats[w[5]] += 1; tex_usage[w[4]] += 1; tex_info[w[0]] = w[5]
        elif name == "create_cube_texture":
            tex_formats[("cube", w[4])] += 1
        elif name == "create_vertex_buffer":
            vb_usage[(w[2], w[4])] += 1
        elif name == "create_index_buffer":
            ib_usage[(w[2], w[3], w[4])] += 1
        elif name in ("lock_buffer", "lock_texture"):
            lock_flags[(name, w[3] if name == "lock_buffer" else w[3])] += 1
        if not in_window:
            continue
        if name == "set_vertex_shader": vs = w[0]
        elif name == "set_pixel_shader": ps = w[0]
        elif name == "set_vertex_declaration": decl = w[0]; fvf = 0
        elif name == "set_fvf": fvf = w[0]; decl = 0
        elif name == "set_render_state": rs[w[0]] += 1
        elif name == "set_sampler_state": ss[w[1]] += 1
        elif name == "set_texture_stage_state": tss[(w[1])] += 1
        elif name == "set_render_target": rt_sets[w[0]] += 1
        elif name == "clear": clears[(w[0], w[1])] += 1
        elif name == "present": frames += 1
        elif name in ("draw_indexed", "draw_up"):
            key = ("vs" if vs else "FF-vs", "ps" if ps else "FF-ps")
            draws[(name,) + key] += 1
            prims[(name, w[0])] += 1
            if not vs: fvfs[fvf if fvf else "decl"] += 1
            if name == "draw_up": draw_up_strides[w[2]] += 1
    print("windows", windows, "frames", frames)
    print("draws by (call, vertex stage, pixel stage):")
    for k, v in draws.most_common(): print("  ", k, v)
    print("primitive types (call, D3DPT):", dict(prims))
    print("FVFs for draws without a vertex shader:", dict(fvfs.most_common(10)))
    print("draw_up strides:", dict(draw_up_strides.most_common(10)))
    print("texture formats (D3DFMT):", dict(tex_formats.most_common()))
    print("texture usages:", dict(tex_usage.most_common()))
    print("vertex buffer (usage, pool):", dict(vb_usage.most_common()))
    print("index buffer (usage, fmt, pool):", dict(ib_usage.most_common()))
    print("lock flags:", dict(lock_flags.most_common()))
    print("render states used in windows (D3DRS):", sorted(rs))
    print("sampler state types:", sorted(ss))
    print("texture stage state types:", sorted(tss))
    print("set_render_target by index:", dict(rt_sets))
    print("clears (rect count, flags):", dict(clears.most_common()))


if __name__ == "__main__":
    main()
