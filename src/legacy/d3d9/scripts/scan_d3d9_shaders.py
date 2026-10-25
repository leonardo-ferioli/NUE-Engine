#!/usr/bin/env python3
"""Census of Direct3D 9 shader bytecode (vs/ps 1.x-3.0) in files: input of the shader spike (docs/PLATFORM_API.md, 3.4).

Finds blobs that start with a shader version token (0xFFFE0xxx vertex, 0xFFFF0xxx pixel), parses the instruction stream structurally
(opcode, length field, comment blocks, END token) and keeps only blobs that parse completely. JDLZ-compressed bundles (the game's
container, magic 'JDLZ') are decompressed first. Read-only: prints a report and (optionally) writes the blobs of THIS machine's census to
a folder OUTSIDE the repository, because shader bytecode is game data.

    python tools/scan_d3d9_shaders.py <file-or-folder>... [--out <folder outside the repo>] [--json census.json]
"""
import argparse, collections, hashlib, json, os, re, struct, sys

VS_VERSIONS = {0x0100: "vs_1_0", 0x0101: "vs_1_1", 0x0200: "vs_2_0", 0x0201: "vs_2_x", 0x0300: "vs_3_0"}
PS_VERSIONS = {0x0100: "ps_1_0", 0x0101: "ps_1_1", 0x0102: "ps_1_2", 0x0103: "ps_1_3", 0x0104: "ps_1_4", 0x0200: "ps_2_0", 0x0201: "ps_2_x", 0x0300: "ps_3_0"}
# opcode -> name (D3DSIO_*)
OPCODES = {0: "nop", 1: "mov", 2: "add", 3: "sub", 4: "mad", 5: "mul", 6: "rcp", 7: "rsq", 8: "dp3", 9: "dp4", 10: "min", 11: "max", 12: "slt", 13: "sge",
           14: "exp", 15: "log", 16: "lit", 17: "dst", 18: "lrp", 19: "frc", 20: "m4x4", 21: "m4x3", 22: "m3x4", 23: "m3x3", 24: "m3x2", 25: "call", 26: "callnz",
           27: "loop", 28: "ret", 29: "endloop", 30: "label", 31: "dcl", 32: "pow", 33: "crs", 34: "sgn", 35: "abs", 36: "nrm", 37: "sincos", 38: "rep", 39: "endrep",
           40: "if", 41: "ifc", 42: "else", 43: "endif", 44: "break", 45: "breakc", 46: "mova", 47: "defb", 48: "defi", 64: "texcoord", 65: "texkill", 66: "tex",
           67: "texbem", 68: "texbeml", 69: "texreg2ar", 70: "texreg2gb", 71: "texm3x2pad", 72: "texm3x2tex", 73: "texm3x3pad", 74: "texm3x3tex", 76: "texm3x3spec",
           77: "texm3x3vspec", 78: "expp", 79: "logp", 80: "cnd", 81: "def", 82: "texreg2rgb", 83: "texdp3tex", 84: "texm3x2depth", 85: "texdp3", 86: "texm3x3",
           87: "texdepth", 88: "cmp", 89: "bem", 90: "dp2add", 91: "dsx", 92: "dsy", 93: "texldd", 94: "setp", 95: "texld", 96: "texldl", 97: "breakp",
           0xFFFD: "phase", 0xFFFE: "comment", 0xFFFF: "end"}
# Number of operand tokens after the opcode token, for the instructions whose length field is not stored (versions < 2.0). For >= 2.0 the
# length is in bits 24..27 of the opcode token.
FIXED_ARGS = {0: 0, 1: 2, 2: 3, 3: 3, 4: 4, 5: 3, 6: 2, 7: 2, 8: 3, 9: 3, 10: 3, 11: 3, 12: 3, 13: 3, 14: 2, 15: 2, 16: 2, 17: 3, 18: 4, 19: 2, 20: 3, 21: 3, 22: 3,
              23: 3, 24: 3, 28: 0, 33: 3, 34: 4, 35: 2, 36: 2, 37: 4, 31: 2, 64: 1, 65: 1, 66: 1, 67: 2, 68: 2, 69: 2, 70: 2, 71: 2, 72: 2, 73: 2, 74: 2, 76: 3, 77: 3,
              78: 2, 79: 2, 80: 4, 82: 2, 83: 2, 84: 2, 85: 2, 86: 2, 87: 1, 88: 4, 89: 3, 90: 4}


def parse_blob(data, at):
    """Parse a shader at byte offset `at`; return (length_in_bytes, version_name, opcode_counts, constants_defined) or None."""
    if at + 8 > len(data) or at % 4:
        return None
    version = struct.unpack_from("<I", data, at)[0]
    major_kind = version >> 16
    table = VS_VERSIONS if major_kind == 0xFFFE else PS_VERSIONS if major_kind == 0xFFFF else None
    if table is None or (version & 0xFFFF) not in table:
        return None
    name = table[version & 0xFFFF]
    sm2 = (version & 0xFFFF) >= 0x0200
    pos = at + 4
    counts = collections.Counter()
    instructions = 0
    while pos + 4 <= len(data):
        token = struct.unpack_from("<I", data, pos)[0]
        opcode = token & 0xFFFF
        if opcode == 0xFFFF:                       # END
            return pos + 4 - at, name, counts, instructions
        if opcode == 0xFFFE:                       # comment: size in dwords in bits 16..30
            size = (token >> 16) & 0x7FFF
            pos += 4 + 4 * size
            continue
        if opcode not in OPCODES:
            return None
        if sm2:
            length = (token >> 24) & 0xF
        elif opcode == 0xFFFD:                      # phase (ps_1_4)
            length = 0
        elif opcode == 81:                          # def: dest + 4 floats
            length = 5
        elif (version & 0xFFFF) == 0x0104 and major_kind == 0xFFFF and opcode in (64, 66):   # ps_1_4 texcrd / texld take a source
            length = 2
        elif opcode in FIXED_ARGS:
            length = FIXED_ARGS[opcode]
        else:
            return None
        if token & 0x80000000 and not sm2 and False:
            return None
        counts[OPCODES[opcode]] += 1
        instructions += 1
        pos += 4 + 4 * length
        if instructions > 4096:
            return None
    return None


VERSION_RE = re.compile(rb"(?:\x00\x01|\x01\x01|\x02\x01|\x03\x01|\x04\x01|\x00\x02|\x01\x02|\x00\x03)(?:\xfe\xff|\xff\xff)")


def jdlz(data):
    """Decompress a JDLZ stream (the OpenNFSTools reference that resource_tests.cpp checks the game's own routine against)."""
    if len(data) < 18 or data[:4] != b"JDLZ" or data[4] != 2:
        return None
    length, end = struct.unpack_from("<II", data, 8)
    if end > len(data) or length > 256 * 1024 * 1024:
        return None
    out = bytearray()
    cursor = 18
    flags = data[16] | 256
    flags2 = data[17] | 256
    try:
        while cursor < end and len(out) < length:
            if not (flags & 1):
                out.append(data[cursor]); cursor += 1
            else:
                first, second = data[cursor], data[cursor + 1]; cursor += 2
                if flags2 & 1:
                    count = ((first & 0xF0) << 4 | second) + 3; distance = (first & 15) + 1
                else:
                    count = (first & 31) + 3; distance = ((first & 0xE0) << 3 | second) + 17
                if distance > len(out) or len(out) + count > length:
                    return None
                for _ in range(count):
                    out.append(out[-distance])
                flags2 >>= 1
            if len(out) == length:
                break
            flags >>= 1
            if flags == 1:
                flags = data[cursor] | 256; cursor += 1
            if flags2 == 1:
                flags2 = data[cursor] | 256; cursor += 1
    except IndexError:
        return None
    return bytes(out) if len(out) == length else None


def scan_bytes(data, source, found):
    for m in VERSION_RE.finditer(data):
        at = m.start()
        if at % 4:
            continue
        parsed = parse_blob(data, at)
        if parsed:
            length, name, counts, instructions = parsed
            if instructions < 1:
                continue
            blob = data[at:at + length]
            found.setdefault(hashlib.sha256(blob).hexdigest(), {"version": name, "bytes": length, "instructions": instructions, "ops": counts, "sources": [], "blob": blob})["sources"].append((source, at))


def walk(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                for n in names:
                    yield os.path.join(root, n)
        else:
            yield p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--out")
    ap.add_argument("--json")
    ap.add_argument("--max-mb", type=int, default=300)
    args = ap.parse_args()
    found = {}
    files = scanned = 0
    for path in walk(args.paths):
        try:
            size = os.path.getsize(path)
            if size > args.max_mb * 1048576 or size < 16 or path.lower().endswith((".bmp", ".log", ".vp6", ".viv")):
                continue
            data = open(path, "rb").read()
        except OSError:
            continue
        files += 1
        scanned += len(data)
        scan_bytes(data, path, found)
        # JDLZ chunks anywhere in the file (bundles hold many).
        for m in re.finditer(rb"JDLZ", data):
            at = m.start()
            if at + 16 > len(data):
                continue
            end = struct.unpack_from("<I", data, at + 12)[0]
            inflated = jdlz(data[at:at + end]) if 18 <= end <= len(data) - at else None
            if inflated:
                scan_bytes(inflated, f"{path}!jdlz@{at:#x}", found)
                scanned += len(inflated)
    print(f"scanned {files} files, {scanned/1048576:.0f} MiB; {len(found)} distinct shader blobs")
    by_version = collections.Counter(v["version"] for v in found.values())
    for version, n in sorted(by_version.items()):
        print(f"  {version}: {n}")
    ops = collections.Counter()
    for v in found.values():
        ops.update(v["ops"])
    print("  instruction mix:", ", ".join(f"{k}={n}" for k, n in ops.most_common()))
    sizes = sorted(v["instructions"] for v in found.values())
    if sizes:
        print(f"  instructions per shader: min {sizes[0]}, median {sizes[len(sizes)//2]}, max {sizes[-1]}")
    for h, v in found.items():
        print(f"  {h[:12]} {v['version']} {v['instructions']:3d} instr {v['bytes']:4d} B  {os.path.basename(v['sources'][0][0])}+0x{v['sources'][0][1]:x} (x{len(v['sources'])})")
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        for h, v in found.items():
            open(os.path.join(args.out, f"{v['version']}_{h[:16]}.bin"), "wb").write(v["blob"])
    if args.json:
        json.dump({h: {k: (dict(val) if k == "ops" else val) for k, val in v.items() if k != "blob"} for h, v in found.items()}, open(args.json, "w"), indent=1, default=str)


if __name__ == "__main__":
    main()
