#!/usr/bin/env python3
"""scan_diff.py SCANS_DIR: the 32-bit floats in the PC build's guest memory scans (host-pc/input.cpp SCAN,
tools/scripts/mp2_scan_aim.txt) that followed the known turns between them: scan 0 to 1 the right stick right for 16
frames, 1 to 2 right for 10, 2 to 3 left for 20, 3 to 4 up for 6, 4 to 5 down for 6. Printed: the values that changed
in proportion to the yaw turns and not with the pitch ones, and the other way round (angles taken modulo 2 pi)."""
import math
import os
import struct
import sys
from array import array

YAW = [16, 10, -20, 0, 0]
PITCH = [0, 0, 0, 6, -6]


def load(path):
    """the scan as 4 KB pages: guest address -> bytes (regions differ from scan to scan as memory is committed)"""
    out = {}
    data = open(path, "rb").read()
    at = 0
    while at + 8 <= len(data):
        addr, n = struct.unpack_from("<II", data, at)
        at += 8
        for off in range(0, n, 4096):
            out[addr + off] = data[at + off:at + off + 4096]
        at += n
    return out


def wrap(d):
    if abs(d) > 3.0:
        d = (d + math.pi) % (2 * math.pi) - math.pi
    return d


def fits(ds, want):
    """ds follow want up to one scale (the per-frame rate), within 20% of the largest step"""
    k = sum(d * w for d, w in zip(ds, want)) / sum(w * w for w in want)
    if abs(k) < 1e-4:
        return None
    err = max(abs(d - k * w) for d, w in zip(ds, want))
    return k if err < 0.2 * abs(k) * max(abs(w) for w in want) else None


def main():
    d = sys.argv[1]
    scans = [load(os.path.join(d, "%03d.bin" % i)) for i in range(6)]
    found = {"yaw": [], "pitch": []}
    for addr, block in scans[0].items():
        blocks = [s.get(addr) for s in scans]
        if any(b is None for b in blocks) or blocks[0] == blocks[1] == blocks[2] == blocks[3] == blocks[4]:
            continue
        n = min(len(b) for b in blocks) // 4 * 4
        cols = [array("f", b[:n]) for b in blocks]
        a0, a1, a2, a3, a4, a5 = cols
        for i in range(n // 4):
            v0 = a0[i]
            if v0 == a1[i] and v0 == a3[i] and a3[i] == a4[i]:
                continue
            vs = [a0[i], a1[i], a2[i], a3[i], a4[i], a5[i]]
            if any(not math.isfinite(v) or abs(v) > 1e5 for v in vs):
                continue
            ds = [wrap(vs[j + 1] - vs[j]) for j in range(5)]
            for name, want in (("yaw", YAW), ("pitch", PITCH)):
                k = fits(ds, want)
                if k is not None:
                    found[name].append((addr + 4 * i, k, vs))
    for name in ("yaw", "pitch"):
        print("%s: %d candidates" % (name, len(found[name])))
        for addr, k, vs in sorted(found[name], key=lambda x: -abs(x[1]))[:40]:
            print("  %08X  per frame %+.5f  values %s" % (addr, k, " ".join("%.4f" % v for v in vs)))


if __name__ == "__main__":
    main()
