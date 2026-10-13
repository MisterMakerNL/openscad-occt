#!/usr/bin/env python3
"""Binary-STL bounding-box helper for differential verification.

  bbox.py tris  a.stl        -> triangle count (or -1 if not binary STL)
  bbox.py info  a.stl        -> "tris xmin ymin zmin xmax ymax zmax"
  bbox.py diff  a.stl b.stl  -> max abs difference of any bbox corner coord
  bbox.py vol   a.stl        -> signed volume (divergence theorem; closed mesh assumed)
"""
import struct
import sys


def read(path):
    d = open(path, "rb").read()
    if len(d) < 84:
        return 0, None, None
    n = struct.unpack("<I", d[80:84])[0]
    if len(d) < 84 + 50 * n:  # ASCII STL or truncated
        return -1, None, None
    o = 84
    mn = [9e9] * 3
    mx = [-9e9] * 3
    for _ in range(n):
        v = struct.unpack("<12f", d[o:o + 48])
        o += 50
        for k in range(3):
            for c in range(3):
                x = v[3 + k * 3 + c]
                mn[c] = min(mn[c], x)
                mx[c] = max(mx[c], x)
    return n, mn, mx


def volume(path):
    d = open(path, "rb").read()
    n = struct.unpack("<I", d[80:84])[0]
    o = 84
    vol = 0.0
    for _ in range(n):
        v = struct.unpack("<12f", d[o:o + 48])
        o += 50
        ax, ay, az = v[3], v[4], v[5]
        bx, by, bz = v[6], v[7], v[8]
        cx, cy, cz = v[9], v[10], v[11]
        vol += (ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx)) / 6.0
    return vol


def main(argv):
    cmd = argv[1]
    if cmd == "vol":
        print("%.6g" % volume(argv[2]))
        return
    if cmd == "tris":
        print(read(argv[2])[0])
    elif cmd == "info":
        n, mn, mx = read(argv[2])
        print(n, *(f"{x:.4f}" for x in (mn or []) + (mx or [])))
    elif cmd == "diff":
        na, amn, amx = read(argv[2])
        nb, bmn, bmx = read(argv[3])
        if not amn or not bmn:
            print("inf")
            return
        d = max(max(abs(amn[c] - bmn[c]), abs(amx[c] - bmx[c])) for c in range(3))
        print(f"{d:.4f}")
    else:
        sys.exit("unknown command: " + cmd)


if __name__ == "__main__":
    main(sys.argv)
