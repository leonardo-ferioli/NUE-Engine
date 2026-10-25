#!/usr/bin/env python3
"""Summary of a Direct3D 9 trace folder written by the recorder (trace.d3d9 + blobs.bin): what it contains and whether it is usable.

    python tools/d3d9_trace_info.py <trace folder>...
"""
import os, re, struct, sys, collections

HERE = os.path.dirname(os.path.abspath(__file__))
EXTRA = ["device_created", "get_back_buffer", "frame_marker", "reference_image", "unknown_call", "truncated", "window_begin", "window_end", "end", "checkpoint"]   # ops 128 and up


def method_names():
    text = open(os.path.join(HERE, "..", "nue_d3d9", "src", "null_gpu.cpp"), encoding="utf-8").read()
    block = text[text.index("static const char* const names[]"):]
    block = block[:block.index("};")]
    return re.findall(r'"([a-z0-9_]+)"', block)


def main():
    names = method_names() + ["?"] * (128 - len(method_names())) + EXTRA
    for folder in sys.argv[1:]:
        trace = os.path.join(folder, "trace.d3d9")
        blobs = os.path.join(folder, "blobs.bin")
        print(f"== {folder}")
        if not os.path.exists(trace):
            print("   no trace.d3d9"); continue
        data = open(trace, "rb").read()
        if data[:8] != b"NFD9TRC1":
            print("   not a trace"); continue
        pos, ops, size_by_op = 12, collections.Counter(), collections.Counter()
        windows, frames_in_window, in_window, truncated_tail = [], 0, False, False
        unknown_slots = collections.Counter()
        while pos + 5 <= len(data):
            op = data[pos]; size = struct.unpack_from("<I", data, pos + 1)[0]
            if pos + 5 + size > len(data):
                truncated_tail = True; break
            words = struct.unpack_from("<%dI" % (size // 4), data, pos + 5)
            name = names[op] if op < len(names) else f"op{op}"
            ops[name] += 1; size_by_op[name] += size
            if name == "window_begin": in_window = True; frames_in_window = 0; windows.append([words[0], words[1], None, 0, 0, 0])
            elif name == "window_end" and windows: windows[-1][2] = words[1]; in_window = False; windows[-1][4:6] = [words[2] if len(words) > 2 else 0, words[3] if len(words) > 3 else 0]
            elif name == "present" and in_window: windows[-1][3] += 1
            elif name == "unknown_call": unknown_slots[words[0]] += 1
            pos += 5 + size
        log = os.path.join(folder, "recorder.log")
        bmps = [f for f in os.listdir(folder) if f.lower().endswith(".bmp")]
        print(f"   trace.d3d9 {len(data)/1048576:.1f} MiB, blobs.bin {os.path.getsize(blobs)/1048576:.1f} MiB, recorder.log {'yes' if os.path.exists(log) else 'NO'}, reference images {len(bmps)}")
        print(f"   frames presented while recording the window(s): {sum(w[3] for w in windows)}; windows: {len(windows)} {[(w[0], 'frames', w[1], '..', w[2], 'draws', w[4], 'images', w[5]) for w in windows]}")
        print(f"   end of trace: {'closed normally (end record)' if ops['end'] else 'no end record (the game was closed abruptly; complete windows are still valid)'}"
              + ("; last record cut short" if truncated_tail else ""))
        resources = {k: ops[k] for k in ("create_texture", "create_cube_texture", "create_vertex_buffer", "create_index_buffer", "create_render_target", "create_depth_stencil",
                                         "create_vertex_declaration", "create_vertex_shader", "create_pixel_shader") if ops[k]}
        print(f"   resources created: {resources}")
        print(f"   uploads: lock_texture {ops['lock_texture']}, lock_buffer {ops['lock_buffer']}; destroyed: " +
              str(sum(ops[k] for k in ops if k.startswith("destroy_"))))
        print(f"   draws: draw_indexed {ops['draw_indexed']}, draw_up {ops['draw_up']}; set_* calls: {sum(ops[k] for k in ops if k.startswith('set_'))}; state snapshot records: set_render_state {ops['set_render_state']}")
        print(f"   unknown calls: {ops['unknown_call']} {dict(unknown_slots.most_common(8))}; truncated by size cap: {'yes' if ops['truncated'] else 'no'}")
        complete = [w for w in windows if w[2] is not None]
        incomplete = len(windows) - len(complete)
        draws_in_windows = sum(ops[k] for k in ("draw_indexed", "draw_up"))
        if complete and draws_in_windows:
            images = sum(w[5] for w in complete)
            print(f"   VERDICT: usable: {len(complete)} complete window(s), {images} reference image(s)"
                  + ("" if ops["end"] else " (the game ended without closing the trace, which does not matter: every complete window is intact)")
                  + (f"; {incomplete} window cut short at the end is ignored by the replay" if incomplete else ""))
        elif windows:
            print("   VERDICT: a window was opened but never completed: record again and wait for the low beep before closing the game")
        else:
            print("   VERDICT: NO recording window (the trigger was never accepted): only resource traffic")


if __name__ == "__main__":
    main()
