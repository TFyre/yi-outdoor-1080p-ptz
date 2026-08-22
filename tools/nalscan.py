#!/usr/bin/env python3
"""Scan an Annex-B H.264 file: NAL type histogram + count of NALs under a
minimum body size. Usage: python3 nalscan.py <file> [min_size]"""
import sys

path = sys.argv[1]
minsize = int(sys.argv[2]) if len(sys.argv) > 2 else 48

data = open(path, "rb").read()
i = 0
types = {}
small = 0
sizes = []
while True:
    j = data.find(b"\x00\x00\x00\x01", i)
    if j < 0:
        break
    k = data.find(b"\x00\x00\x00\x01", j + 4)
    n = (k if k >= 0 else len(data)) - j - 4
    t = data[j + 4] & 0x1F
    types[t] = types.get(t, 0) + 1
    sizes.append(n)
    if n < minsize:
        small += 1
    i = j + 4
print("size:", len(data))
print("types:", dict(sorted(types.items())))
print("NAL bodies < %d bytes: %d" % (minsize, small))
sizes.sort()
print("smallest 10:", sizes[:10])
