"""Decode an Address Library .bin (SE format 1, AE format 2) into {id: offset}."""
import struct, sys

def load(path):
    d = open(path, "rb").read()
    p = 0
    def rd(fmt):
        nonlocal p
        v = struct.unpack_from(fmt, d, p); p += struct.calcsize(fmt); return v[0]
    fmt = rd("<i"); assert fmt in (1, 2), fmt
    ver = [rd("<i") for _ in range(4)]
    n = rd("<i"); p += n
    ptr = rd("<i"); count = rd("<i")
    out = {}; pvid = 0; poff = 0
    for _ in range(count):
        t = rd("<B"); lo = t & 0xF; hi = t >> 4
        if lo == 0: q1 = rd("<Q")
        elif lo == 1: q1 = pvid + 1
        elif lo == 2: q1 = pvid + rd("<B")
        elif lo == 3: q1 = pvid - rd("<B")
        elif lo == 4: q1 = pvid + rd("<H")
        elif lo == 5: q1 = pvid - rd("<H")
        elif lo == 6: q1 = rd("<H")
        elif lo == 7: q1 = rd("<I")
        tp = poff // ptr if hi & 8 else poff
        h = hi & 7
        if h == 0: q2 = rd("<Q")
        elif h == 1: q2 = tp + 1
        elif h == 2: q2 = tp + rd("<B")
        elif h == 3: q2 = tp - rd("<B")
        elif h == 4: q2 = tp + rd("<H")
        elif h == 5: q2 = tp - rd("<H")
        elif h == 6: q2 = rd("<H")
        elif h == 7: q2 = rd("<I")
        if hi & 8: q2 *= ptr
        out[q1] = q2; poff = q2; pvid = q1
    return ver, out

if __name__ == "__main__":
    ver, m = load(sys.argv[1])
    with open(sys.argv[2], "w") as f:
        f.write("id,offset\n")
        for k in sorted(m): f.write(f"{k},{m[k]:X}\n")
    print(ver, len(m))
