#!/usr/bin/env python3
"""Inventory of how a Win32 executable uses DirectSound (read-only, prints Markdown, no game bytes: only addresses, slot numbers and counts).

    python src/legacy/dsound/tools/dsound_inventory.py path/to/game.exe [--window 0x400]

What it reports:
  1. audio-related imports (dsound.dll, winmm waveOut/mixer/midi, ole32 CoCreateInstance, dinput is not audio);
  2. which DirectSound / EAX interface identifiers (IIDs) exist in the image (3D buffer and listener, notify, IKsPropertySet, the EAX
     property sets, IDirectSound8) - an interface whose IID is absent cannot be obtained with QueryInterface;
  3. the call sites of DirectSoundCreate (an import by ordinal 1 or by name) and the global that receives the IDirectSound pointer;
  4. every code reference to that global and, in that neighbourhood, every virtual call `call [reg+disp]` with the slot number and the
     IDirectSound / IDirectSoundBuffer methods that slot could be. The neighbourhood is the module that owns the DirectSound objects.
Requires capstone (pip install capstone).
"""
import argparse
import struct
import sys
import uuid

try:
    import capstone
except ImportError:  # pragma: no cover
    sys.exit("capstone is required: pip install capstone")

IDS = {
    "IDirectSound": "279AFA83-4981-11CE-A521-0020AF0BE560", "IDirectSoundBuffer": "279AFA85-4981-11CE-A521-0020AF0BE560",
    "IDirectSound3DListener": "279AFA84-4981-11CE-A521-0020AF0BE560", "IDirectSound3DBuffer": "279AFA86-4981-11CE-A521-0020AF0BE560",
    "IDirectSoundNotify": "B0210783-89CD-11D0-AF08-00A0C925CD16", "IKsPropertySet": "31EFAC30-515C-11D0-A9AA-00AA0061BE93",
    "IDirectSound8": "C50A7E93-F395-4834-9EF6-7FA99DE82C07", "IDirectSoundBuffer8": "6825A449-7524-4D82-920F-50E36AB3AB1E",
    "EAX 1 listener (EAX_ReverbProperties)": "4B6A4FC0-D2CF-11D1-8E6A-00E04F44E8A8",
    "EAX 2 listener": "0306A6A8-B224-11D2-99E5-0000E8D8C722", "EAX 2 buffer": "0306A6A7-B224-11D2-99E5-0000E8D8C722",
    "EAX 3 listener": "A8FA6882-B476-11D3-BDB9-00C0F02DDF87", "EAX 3 buffer": "A8FA6881-B476-11D3-BDB9-00C0F02DDF87",
    "KSDATAFORMAT_SUBTYPE_PCM": "00000001-0000-0010-8000-00AA00389B71", "KSDATAFORMAT_SUBTYPE_IEEE_FLOAT": "00000003-0000-0010-8000-00AA00389B71",
}
DS_SLOTS = ["QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps", "DuplicateSoundBuffer", "SetCooperativeLevel", "Compact",
            "GetSpeakerConfig", "SetSpeakerConfig", "Initialize"]
BUF_SLOTS = ["QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition", "GetFormat", "GetVolume", "GetPan", "GetFrequency", "GetStatus",
             "Initialize", "Lock", "Play", "SetCurrentPosition", "SetFormat", "SetVolume", "SetPan", "SetFrequency", "Stop", "Unlock", "Restore"]
AUDIO_DLLS = ("dsound", "winmm", "ole32", "mmdevapi", "audioses", "xaudio", "x3daudio", "eax", "openal", "ksuser", "dsetup")
AUDIO_NAMES = ("waveout", "wavein", "mixer", "midi", "mci", "playsound", "sndplaysound", "cocreateinstance", "dsound", "directsound")


class Pe:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        n, opt = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        self.sections = []
        o = pe + 24 + opt
        for _ in range(n):
            name = d[o:o + 8].rstrip(b"\0").decode(errors="replace")
            vs, va, rs, ro = struct.unpack_from("<IIII", d, o + 8)
            chars = struct.unpack_from("<I", d, o + 36)[0]
            self.sections.append((name, self.base + va, vs, ro, rs, chars))
            o += 40
        self.import_rva = struct.unpack_from("<I", d, pe + 24 + 96 + 8)[0]

    def read(self, va, n):
        for _, sva, vs, ro, rs, _ in self.sections:
            if sva <= va < sva + max(vs, rs):
                off = ro + (va - sva)
                return self.data[off:off + n]
        return b""

    def cstr(self, va):
        out = bytearray()
        while True:
            b = self.read(va, 1)
            if not b or b == b"\0":
                return out.decode(errors="replace")
            out += b
            va += 1

    def imports(self):
        """(dll, name or '#ordinal', iat_va)"""
        result = []
        desc = self.base + self.import_rva
        while True:
            oft, _, _, name_rva, ft = struct.unpack("<IIIII", self.read(desc, 20))
            if not name_rva:
                break
            dll = self.cstr(self.base + name_rva)
            thunk, iat = self.base + (oft or ft), self.base + ft
            while True:
                (entry,) = struct.unpack("<I", self.read(thunk, 4))
                if not entry:
                    break
                name = "#%d" % (entry & 0xFFFF) if entry & 0x80000000 else self.cstr(self.base + (entry & 0x7FFFFFFF) + 2)
                result.append((dll, name, iat))
                thunk += 4
                iat += 4
            desc += 20
        return result

    def text(self):
        for name, va, vs, ro, rs, chars in self.sections:
            if chars & 0x20000000:   # IMAGE_SCN_MEM_EXECUTE
                return va, self.data[ro:ro + rs]
        raise SystemExit("no executable section")


def find_all(blob, needle):
    out, pos = [], 0
    while True:
        p = blob.find(needle, pos)
        if p < 0:
            return out
        out.append(p)
        pos = p + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--window", type=lambda s: int(s, 0), default=0x400, help="bytes around the first/last reference to the DirectSound global to scan for virtual calls")
    args = ap.parse_args()
    pe = Pe(args.exe)
    imports = pe.imports()
    print("# DirectSound inventory of `%s`\n" % args.exe.replace("\\", "/").split("/")[-1])

    print("## 1. Audio-related imports\n\n| DLL | function | IAT address |\n|---|---|---|")
    rows = [(d, n, a) for d, n, a in imports if d.lower().startswith(AUDIO_DLLS) or any(k in n.lower() for k in AUDIO_NAMES)]
    audio_rows = [r for r in rows if not r[0].lower().startswith("winmm") or any(k in r[1].lower() for k in ("wave", "mixer", "midi", "mci", "playsound"))]
    for d, n, a in audio_rows:
        print("| %s | %s | 0x%08x |" % (d, n, a))
    if not audio_rows:
        print("| (none) | | |")
    other_winmm = [n for d, n, a in rows if d.lower().startswith("winmm") and (d, n, a) not in audio_rows]
    if other_winmm:
        print("\n(winmm timing functions, not audio: %s)" % ", ".join(other_winmm))

    print("\n## 2. Interface identifiers present in the image\n\n| Identifier | Present |\n|---|---|")
    image = pe.data
    for name, text in IDS.items():
        print("| %s | %s |" % (name, "yes" if uuid.UUID(text).bytes_le in image else "**no**"))

    va, code = pe.text()
    creates = [(d, n, a) for d, n, a in imports if d.lower().startswith("dsound") and n in ("#1", "DirectSoundCreate", "#11", "DirectSoundCreate8")]
    print("\n## 3. DirectSoundCreate")
    if not creates:
        print("\nNo DirectSoundCreate import.")
        return
    for d, n, iat in creates:
        iat_bytes = struct.pack("<I", iat)
        thunks = [va + p for p in find_all(code, b"\xff\x25" + iat_bytes)]
        sites = []
        for t in thunks:
            rel = struct.pack("<i", 0)
            for p in find_all(code, b"\xe8"):
                (disp,) = struct.unpack_from("<i", code, p + 1) if p + 5 <= len(code) else (0,)
                if va + p + 5 + disp == t:
                    sites.append(va + p)
        sites += [va + p for p in find_all(code, b"\xff\x15" + iat_bytes)]
        print("\n`%s!%s`, IAT 0x%08x, import thunk(s) %s, %d call site(s): %s" % (d, n, iat, ", ".join("0x%x" % t for t in thunks) or "none", len(sites), ", ".join("0x%x" % s for s in sites)))
        globals_found = set()
        for s in sites:
            window = code[s - va - 24:s - va]
            pushes = [struct.unpack_from("<I", window, i + 1)[0] for i in range(len(window) - 4) if window[i] == 0x68]
            if len(pushes) >= 1:
                globals_found.add(pushes[-1] if len(pushes) == 1 else pushes[-2])   # arg 2 (ppDS) is the second push seen scanning backwards
        for g in sorted(globals_found):
            print("\nGlobal receiving the IDirectSound pointer: 0x%08x" % g)
            refs = [va + p for p in find_all(code, struct.pack("<I", g))]
            print("Code references to it: %d, from 0x%x to 0x%x" % (len(refs), min(refs), max(refs)))
            lo, hi = min(refs) - args.window, max(refs) + args.window
            md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
            md.detail = True
            counts = {}
            for ins in md.disasm(code[lo - va:hi - va], lo):
                if ins.mnemonic != "call" or not ins.operands:
                    continue
                op = ins.operands[0]
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base != 0 and op.mem.index == 0 and 0 <= op.mem.disp <= 0x60 and op.mem.disp % 4 == 0:
                    counts.setdefault(op.mem.disp // 4, []).append(ins.address)
            print("\n## 4. Virtual calls in 0x%x..0x%x (the module that owns the DirectSound objects)\n" % (lo, hi))
            print("| Slot | IDirectSound | IDirectSoundBuffer | Call sites |\n|---:|---|---|---|")
            for slot in sorted(counts):
                ds = DS_SLOTS[slot] if slot < len(DS_SLOTS) else "-"
                bf = BUF_SLOTS[slot] if slot < len(BUF_SLOTS) else "-"
                print("| %d | %s | %s | %s |" % (slot, ds, bf, ", ".join("0x%x" % a for a in counts[slot])))
            print("\nSlots 0-2 are IUnknown; which interface a call belongs to follows from the object it is made on (the three globals around the DirectSoundCreate result).")


if __name__ == "__main__":
    main()
