#!/usr/bin/env python3
"""Generates tests/audio/adpcm_vectors.h: reference outputs of the ADPCM decoders for fixed pseudo-random inputs.

The decoders here are a line-by-line transcription of the reference decoders in vgmstream (ISC licence,
https://github.com/vgmstream/vgmstream, src/coding/{psx,ngc_dsp,ea_xa,ea_xas,ima}_decoder.c, commit d9875b1) written with Python
integers/floats, i.e. independent of the C++ implementation in src/audio/codecs/adpcm.cpp. The C++ test compares the outputs by hash.
Run:  python3 tests/audio/gen_adpcm_vectors.py > tests/audio/adpcm_vectors.h
"""
import random
import struct
import sys

def clamp16(v):
    return -32768 if v < -32768 else (32767 if v > 32767 else v)

def fnv(samples):
    h = 2166136261
    for s in samples:
        for b in struct.pack('<h', s):
            h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h

def s4(n):
    return n - 16 if n >= 8 else n

# ---------------------------------------------------------------- IMA
STEP = [7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767]
IDX = [-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8]

def ima_nib(code, hist, idx):
    step = STEP[idx]
    delta = step >> 3
    if code & 1: delta += step >> 2
    if code & 2: delta += step >> 1
    if code & 4: delta += step
    if code & 8: delta = -delta
    hist = clamp16(hist + delta)
    idx = max(0, min(88, idx + IDX[code]))
    return hist, idx

def ima_wav_block(block, ch):
    spb = 1 + (len(block) - 4 * ch) // ch * 2
    out = [[] for _ in range(ch)]
    st = []
    for c in range(ch):
        h, i = struct.unpack_from('<hb', block, 4 * c)
        i = max(0, min(88, i))
        st.append([h, i])
        out[c].append(h)
    p = 4 * ch
    for g in range((len(block) - 4 * ch) // (4 * ch)):
        for c in range(ch):
            for b in range(4):
                v = block[p]; p += 1
                for code in (v & 15, v >> 4):
                    st[c][0], st[c][1] = ima_nib(code, st[c][0], st[c][1])
                    out[c].append(st[c][0])
    return [out[c][i] for i in range(spb) for c in range(ch)]

def xbox_block(block, ch):
    full = ima_wav_block(block, ch)
    return full[:64 * ch]

# ---------------------------------------------------------------- MS ADPCM
MS_COEF = [(256,0),(512,-256),(0,0),(192,64),(240,0),(460,-208),(392,-232)]
MS_ADAPT = [230,230,230,230,307,409,512,614,768,614,512,409,307,230,230,230]

def ms_block(block, ch):
    p = 0
    k = []
    for c in range(ch):
        k.append(MS_COEF[block[p]]); p += 1
    delta = []; s1 = []; s2 = []
    for c in range(ch): delta.append(struct.unpack_from('<h', block, p)[0]); p += 2
    for c in range(ch): s1.append(struct.unpack_from('<h', block, p)[0]); p += 2
    for c in range(ch): s2.append(struct.unpack_from('<h', block, p)[0]); p += 2
    out = []
    for c in range(ch): out.append(s2[c])
    for c in range(ch): out.append(s1[c])
    nibbles = (len(block) - 7 * ch) * 2
    for n in range(nibbles):
        c = n % ch
        v = block[p + n // 2]
        code = (v & 15) if (n & 1) else (v >> 4)
        sc = code - 16 if code >= 8 else code
        pred = ((s1[c] * k[c][0] + s2[c] * k[c][1]) >> 8) + sc * delta[c]
        smp = clamp16(pred)
        out.append(smp)
        s2[c] = s1[c]; s1[c] = smp
        delta[c] = min(max(16, (MS_ADAPT[code] * delta[c]) >> 8), 0x7FFFFFFF // 768)
    return out

# ---------------------------------------------------------------- PSX (float coefficients, as the reference)
PSX_F = [(0.0,0.0),(0.9375,0.0),(1.796875,-0.8125),(1.53125,-0.859375),(1.90625,-0.9375)]

def psx_frame(frame, h1, h2, ignore_flags=False):
    coef = (frame[0] >> 4) & 15
    shift = frame[0] & 15
    flag = frame[1]
    if coef > 4: coef = 0
    if shift > 12: shift = 9
    if ignore_flags: flag = 0
    shift = 20 - shift
    out = []
    for i in range(28):
        s = 0
        if flag < 7:
            n = frame[2 + i // 2]
            n = (n >> 4) if (i & 1) else (n & 15)
            s = (s4(n) << shift)
            s = s + int((PSX_F[coef][0] * h1 + PSX_F[coef][1] * h2) * 256.0)
            s >>= 8
        c = clamp16(s)
        out.append(c)
        h2 = h1; h1 = clamp16(s)       # hardware keeps the clamped value
    return out, h1, h2

# ---------------------------------------------------------------- DSP
def dsp_frame(frame, coefs, h1, h2):
    scale = 1 << (frame[0] & 15)
    ci = min(7, (frame[0] >> 4) & 15)
    c1, c2 = coefs[ci * 2], coefs[ci * 2 + 1]
    out = []
    for i in range(14):
        n = frame[1 + i // 2]
        n = (n & 15) if (i & 1) else (n >> 4)
        s = s4(n)
        s = (s * scale) << 11
        s = (s + 1024 + c1 * h1 + c2 * h2) >> 11
        s = clamp16(s)
        out.append(s)
        h2 = h1; h1 = s
    return out, h1, h2

# ---------------------------------------------------------------- EA XAS v1
XA_COEF = [(0.0,0.0),(0.9375,0.0),(1.796875,-0.8125),(1.53125,-0.859375)] + [(0.0,0.0)] * 12

def xas_block(block):
    out = []
    for g in range(4):
        hdr = struct.unpack_from('<I', block, g * 4)[0]
        c1, c2 = XA_COEF[hdr & 15]
        h2 = struct.unpack('<h', struct.pack('<H', hdr & 0xFFF0))[0]
        h1 = struct.unpack('<h', struct.pack('<H', (hdr >> 16) & 0xFFF0))[0]
        shift = (hdr >> 16) & 15
        out += [h2, h1]
        for row in range(15):
            byte = block[16 + row * 4 + g]
            for half in range(2):
                n = (byte >> 4) if half == 0 else (byte & 15)
                s = struct.unpack('<h', struct.pack('<H', (n << 12) & 0xFFFF))[0] >> shift
                v = s + h1 * c1 + h2 * c2
                s = clamp16(int(v))     # int() truncates toward zero like the C float->int conversion
                out.append(s)
                h2 = h1; h1 = s
    return out

# ---------------------------------------------------------------- EA-XA
EA_T = [0,240,460,392, 0,0,-208,-220, 0,1,3,4, 7,8,10,11, 0,-1,-3,-4]

def ea_expand(nib, shift, c1, c2, h1, h2, rnd):
    v = (nib << 28) & 0xFFFFFFFF
    if v & 0x80000000: v -= 1 << 32
    v >>= shift
    return clamp16((v + c1 * h1 + c2 * h2 + rnd) >> 8)

def ea_xa_frame(frame, h1, h2, v1):
    ci = frame[0] >> 4
    c1, c2 = EA_T[ci], EA_T[ci + 4]
    shift = (frame[0] & 15) + 8
    out = []
    for i in range(28):
        b = frame[1 + i // 2]
        n = (b & 15) if (i & 1) else (b >> 4)
        s = ea_expand(n, shift, c1, c2, h1, h2, 128 if v1 else 0)
        out.append(s); h2 = h1; h1 = s
    return out, h1, h2

def ea_xa_stereo(frame, st):
    outs = [[], []]
    for ch in range(2):
        hn = ch == 0
        ci = (frame[0] >> 4) if hn else (frame[0] & 15)
        c1, c2 = EA_T[ci], EA_T[ci + 4]
        shift = ((frame[1] >> 4) if hn else (frame[1] & 15)) + 8
        h1, h2 = st[ch]
        for i in range(28):
            b = frame[2 + i]
            n = (b >> 4) if hn else (b & 15)
            s = ea_expand(n, shift, c1, c2, h1, h2, 128)
            outs[ch].append(s); h2 = h1; h1 = s
        st[ch] = (h1, h2)
    return outs

def c_bytes(name, data):
    lines = []
    for i in range(0, len(data), 16):
        lines.append('    ' + ', '.join('0x%02X' % b for b in data[i:i + 16]) + ',')
    return 'static const uint8_t %s[%d] = {\n%s\n};\n' % (name, len(data), '\n'.join(lines))

def main():
    rnd = random.Random(0x4E5545)           # "NUE"
    o = []
    o.append('// GENERATED by tests/audio/gen_adpcm_vectors.py - do not edit. Reference: vgmstream decoders (ISC), see that script.\n#pragma once\n#include <cstdint>\n')

    # IMA WAV: stereo 2 blocks of 64 bytes (block with random headers), mono block of 36 bytes, 4 channels
    def ima_case(name, ch, bsize, nblocks):
        data = bytearray()
        exp = []
        for _ in range(nblocks):
            blk = bytearray(rnd.randrange(256) for _ in range(bsize))
            for c in range(ch):
                blk[4 * c + 2] = rnd.randrange(0, 89)
                blk[4 * c + 3] = 0
            data += blk
            exp += ima_wav_block(bytes(blk), ch)
        o.append(c_bytes(name + '_data', bytes(data)))
        o.append('static const uint32_t %s_hash = 0x%08Xu; static const uint32_t %s_frames = %d;\n' % (name, fnv(exp), name, len(exp) // ch))
    ima_case('ima_mono', 1, 36, 3)
    ima_case('ima_stereo', 2, 72, 3)
    ima_case('ima_quad', 4, 80, 2)

    def xbox_case(name, ch, nblocks):
        data = bytearray(); exp = []
        for _ in range(nblocks):
            blk = bytearray(rnd.randrange(256) for _ in range(36 * ch))
            for c in range(ch):
                blk[4 * c + 2] = rnd.randrange(0, 89); blk[4 * c + 3] = 0
            data += blk
            exp += xbox_block(bytes(blk), ch)
        o.append(c_bytes(name + '_data', bytes(data)))
        o.append('static const uint32_t %s_hash = 0x%08Xu; static const uint32_t %s_frames = %d;\n' % (name, fnv(exp), name, len(exp) // ch))
    xbox_case('xbox_mono', 1, 3)
    xbox_case('xbox_stereo', 2, 3)

    def ms_case(name, ch, bsize, nblocks):
        data = bytearray(); exp = []
        for _ in range(nblocks):
            blk = bytearray(rnd.randrange(256) for _ in range(bsize))
            for c in range(ch):
                blk[c] = rnd.randrange(0, 7)
                struct.pack_into('<h', blk, ch + 2 * c, rnd.randrange(16, 3000))          # delta
                struct.pack_into('<h', blk, 3 * ch + 2 * c, rnd.randrange(-20000, 20000))    # s1
                struct.pack_into('<h', blk, 5 * ch + 2 * c, rnd.randrange(-20000, 20000))    # s2
            data += blk
            exp += ms_block(bytes(blk), ch)
        o.append(c_bytes(name + '_data', bytes(data)))
        o.append('static const uint32_t %s_hash = 0x%08Xu; static const uint32_t %s_frames = %d;\n' % (name, fnv(exp), name, len(exp) // ch))
    ms_case('ms_mono', 1, 64, 3)
    ms_case('ms_stereo', 2, 128, 3)

    # PSX: 40 frames; random headers within valid + a few weird ones (bad filter, big shift, flag 7)
    frames = bytearray(); exp = []; h1 = h2 = 0
    for i in range(40):
        f = bytearray(rnd.randrange(256) for _ in range(16))
        if i % 9 == 0: f[0] = (rnd.randrange(0, 5) << 4) | rnd.randrange(0, 13)
        if i == 11: f[0] = 0xF0 | 14          # bad filter + bad shift
        f[1] = 0 if i not in (20, 30) else 7  # flag 7 -> silence
        if i not in (11,) and i % 9 != 0: f[0] = (rnd.randrange(0, 5) << 4) | rnd.randrange(0, 13)
        frames += f
        s, h1, h2 = psx_frame(bytes(f), h1, h2)
        exp += s
    o.append(c_bytes('psx_data', bytes(frames)))
    o.append('static const uint32_t psx_hash = 0x%08Xu; static const uint32_t psx_frames = %d;\n' % (fnv(exp), len(exp)))

    # DSP: 16 coefficients, 40 frames
    coefs = [rnd.randrange(-2048, 2048) for _ in range(16)]
    coefs[0], coefs[1] = 0, 0
    frames = bytearray(); exp = []; h1 = h2 = 0
    for i in range(40):
        f = bytearray(rnd.randrange(256) for _ in range(8))
        f[0] = (rnd.randrange(0, 8) << 4) | rnd.randrange(0, 9)
        frames += f
        s, h1, h2 = dsp_frame(bytes(f), coefs, h1, h2)
        exp += s
    o.append(c_bytes('dsp_data', bytes(frames)))
    o.append('static const int16_t dsp_coefs[16] = {%s};\n' % ', '.join(str(c) for c in coefs))
    o.append('static const uint32_t dsp_hash = 0x%08Xu; static const uint32_t dsp_frames = %d;\n' % (fnv(exp), len(exp)))

    # XAS: stereo, 3 blocks (2 channels * 0x4c each)
    data = bytearray(); exp = []
    for _ in range(3):
        chans = []
        for c in range(2):
            blk = bytearray(rnd.randrange(256) for _ in range(0x4c))
            for g in range(4):
                hdr = struct.unpack_from('<I', blk, g * 4)[0]
                hdr = (hdr & ~0xF) | rnd.randrange(0, 4)          # valid filters (index > 3 means "no filter", also covered by a few blocks below)
                struct.pack_into('<I', blk, g * 4, hdr)
            chans.append(bytes(blk))
        data += b''.join(chans)
        a = xas_block(chans[0]); b = xas_block(chans[1])
        for i in range(128):
            exp += [a[i], b[i]]
    o.append(c_bytes('xas_data', bytes(data)))
    o.append('static const uint32_t xas_hash = 0x%08Xu; static const uint32_t xas_frames = %d;\n' % (fnv(exp), len(exp) // 2))

    # EA-XA v1 / v2 mono, 30 frames
    for name, v1 in (('eaxa1', True), ('eaxa2', False)):
        frames = bytearray(); exp = []; h1 = h2 = 0
        for i in range(30):
            f = bytearray(rnd.randrange(256) for _ in range(15))
            f[0] = (rnd.randrange(0, 5) << 4) | rnd.randrange(0, 12)
            frames += f
            s, h1, h2 = ea_xa_frame(bytes(f), h1, h2, v1)
            exp += s
        o.append(c_bytes(name + '_data', bytes(frames)))
        o.append('static const uint32_t %s_hash = 0x%08Xu; static const uint32_t %s_frames = %d;\n' % (name, fnv(exp), name, len(exp)))
    # EA-XA stereo v1, 20 frames
    frames = bytearray(); exp = []; st = [(0, 0), (0, 0)]
    for i in range(20):
        f = bytearray(rnd.randrange(256) for _ in range(30))
        f[0] = (rnd.randrange(0, 5) << 4) | rnd.randrange(0, 5)
        f[1] = (rnd.randrange(0, 12) << 4) | rnd.randrange(0, 12)
        frames += f
        l, r = ea_xa_stereo(bytes(f), st)
        for j in range(28): exp += [l[j], r[j]]
    o.append(c_bytes('eaxa_st_data', bytes(frames)))
    o.append('static const uint32_t eaxa_st_hash = 0x%08Xu; static const uint32_t eaxa_st_frames = %d;\n' % (fnv(exp), len(exp) // 2))
    sys.stdout.write('\n'.join(o))

main()
