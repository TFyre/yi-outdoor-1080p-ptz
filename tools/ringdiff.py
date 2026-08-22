#!/usr/bin/env python3
"""Diff two fshare ring snapshots: the changed byte ranges are exactly
what the app wrote between the two captures. Prints each changed region
(offset, size, leading bytes) and scans for start codes inside it.
Usage: python3 ringdiff.py <snap1> <snap2>
"""
import sys

a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
assert len(a) == len(b)

# changed regions
regions = []
start = None
for i in range(len(a)):
    if a[i] != b[i]:
        if start is None:
            start = i
    else:
        if start is not None:
            regions.append((start, i - start))
            start = None
if start is not None:
    regions.append((start, len(a) - start))

total = sum(n for _, n in regions)
print("changed bytes: %d in %d regions" % (total, len(regions)))
for off, n in regions:
    head = b[off:off + 16]
    # scan the region for 00 00 01 start codes
    scs = []
    for j in range(off, min(off + n - 3, len(b))):
        if b[j:j + 3] == b"\x00\x00\x01":
            scs.append(j - off)
    print("  %08x + %6d  head: %s  codes@%s" %
          (off, n, " ".join("%02x" % c for c in head),
           ",".join("+%d" % s for s in scs[:8])))
