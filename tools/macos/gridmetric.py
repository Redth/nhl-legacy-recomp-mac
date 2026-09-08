#!/usr/bin/env python3
"""Measure the screen-locked 4px grid artifact in a captured frame.

Use THIS metric for any A/B on that artifact, not mean luma difference. It is a
periodicity amplitude, so it does not care where the camera is - it reproduces
to two decimal places across independent runs (2.34 / 2.34 on two baselines),
whereas mean|luma diff| between runs is swamped by simulation divergence (53 by
frame 6600 against a 2.6 same-config floor). Several inconclusive A/B attempts
were caused by using divergence-sensitive metrics.

  tools/macos/gridmetric.py frame_5300.png [x0 y0 x1 y1]

Reports mean luma, the 4-pixel periodicity peak-to-peak, and its phase. Compare
amplitudes only between frames of similar mean luma: configs that render the
scene differently (e.g. REX_FSI_SAMPLE_RATE, which comes out washed out at luma
63 vs 8.6) are not comparable no matter what the number says.
"""
import sys, math, zlib, struct

def read_png(path):
    d = open(path, 'rb').read()
    pos, idat, w, h, ct = 8, b'', None, None, None
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos+4])[0]
        typ = d[pos+4:pos+8]
        if typ == b'IHDR':
            w, h, _, ct = struct.unpack('>IIBB', d[pos+8:pos+18])
        elif typ == b'IDAT':
            idat += d[pos+8:pos+8+ln]
        pos += 12 + ln
    ch = {0: 1, 2: 3, 4: 2, 6: 4}[ct]
    raw = zlib.decompress(idat)
    out = bytearray(w * h * ch)
    stride, prev = w * ch, bytearray(w * ch)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        for i in range(stride):
            a = line[i-ch] if i >= ch else 0
            b = prev[i]
            c = prev[i-ch] if i >= ch else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pp = a + b - c
                pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out[y*stride:(y+1)*stride] = line
        prev = line
    return w, h, ch, out

def grid(path, x0, y0, x1, y1):
    w, h, ch, px = read_png(path)
    L = lambda i: (px[i]*299 + px[i+1]*587 + px[i+2]*114) / 1000.0
    cols = [0.0] * (x1 - x0)
    for y in range(y0, y1):
        for x in range(x0, x1):
            cols[x-x0] += L((y*w + x) * ch)
    cols = [c / (y1 - y0) for c in cols]
    m = sum(cols) / len(cols)
    d = [c - m for c in cols]
    re = sum(d[i] * math.cos(2*math.pi*i/4) for i in range(len(d)))
    im = sum(d[i] * math.sin(2*math.pi*i/4) for i in range(len(d)))
    return m, 2*math.hypot(re, im)/len(d), math.degrees(math.atan2(im, re)) % 360

if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    box = [int(v) for v in sys.argv[2:6]] if len(sys.argv) >= 6 else [380, 380, 900, 560]
    m, amp, ph = grid(sys.argv[1], *box)
    print(f'{sys.argv[1]}: mean_luma={m:.1f}  grid_4px_p2p={amp:.2f}  phase={ph:.1f}')
