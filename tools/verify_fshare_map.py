#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate the fshare header/entry map derived from the tserver disassembly
against real /dev/shm/fshare_frame_buf snapshots.

Map under test (all offsets from the mapping base):
  0x00 u32 reader_count | 0x04 u32 valid_bytes | 0x08 ? | 0x0C ?
  0x10 u32 tail_off     | 0x14 u32 now_ts      | 0x18 u32 newest_seq | 0x1C..0x12B slot[17]{16B}
  0x12C data ring, size 0x1B4000
Entry: 26-byte header + payload; +0 u32 len, +4 u32 seq, +16 u32 ts, +20 u16 type.
"""
import struct
import sys
import glob
import collections

DATA_OFF = 300
DATA_SIZE = 0x1B4000
HDRLEN = 26
NSLOT = 17


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def walk(path):
    b = open(path, "rb").read()
    print("=" * 72)
    print(f"{path}  ({len(b)} bytes, expected {DATA_OFF + DATA_SIZE})")
    if len(b) != DATA_OFF + DATA_SIZE:
        print("  !! size mismatch")

    hdr = {o: u32(b, o) for o in range(0, 0x1C, 4)}
    print("  header 0x00..0x1B:", "  ".join(f"{o:#04x}={v}" for o, v in hdr.items()))

    print("  slots (base 0x1C, stride 16):")
    for k in range(NSLOT):
        s = 0x1C + k * 16
        cnt, wait, cur = u32(b, s), u32(b, s + 4), u32(b, s + 8)
        filt, pad = u16(b, s + 12), u16(b, s + 14)
        if cnt or wait or cur or filt or pad:
            print(f"    slot[{k:2d}] @0x{s:03x}: count={cnt} waiting={wait} "
                  f"cursor={cur} filter=0x{filt:04x} pad=0x{pad:04x}")

    # walk the entry chain from the tail
    off = hdr[0x10]
    budget = hdr[0x04]
    print(f"  walk from tail_off={off}, valid_bytes={budget}, "
          f"newest_seq={hdr[0x18]}, now_ts={hdr[0x14]}")
    if not (0 <= off < DATA_SIZE) or budget <= 0 or budget > DATA_SIZE:
        print("  !! tail/valid out of range; skipping walk")
        return

    def rd(o, n):
        o %= DATA_SIZE
        if o + n <= DATA_SIZE:
            return b[DATA_OFF + o:DATA_OFF + o + n]
        first = DATA_SIZE - o
        return b[DATA_OFF + o:DATA_OFF + DATA_SIZE] + b[DATA_OFF:DATA_OFF + n - first]

    types = collections.Counter()
    n = 0
    prev_seq = prev_ts = None
    seq_ok = ts_ok = True
    first_seq = last_seq = last_ts = None
    remaining = budget
    while remaining > 0 and n < 200000:
        h = rd(off, HDRLEN)
        ln, seq = u32(h, 0), u32(h, 4)
        ts, ty = u32(h, 16), u16(h, 20)
        if ln < 0 or ln > DATA_SIZE:
            print(f"  !! entry {n} @{off}: absurd len={ln}")
            break
        types[ty] += 1
        if first_seq is None:
            first_seq = seq
        if prev_seq is not None and seq != prev_seq + 1:
            if seq_ok:
                print(f"  !! seq break at entry {n} @{off}: {prev_seq} -> {seq}")
            seq_ok = False
        if prev_ts is not None and ts < prev_ts:
            if ts_ok:
                print(f"  !! ts went backward at entry {n}: {prev_ts} -> {ts}")
            ts_ok = False
        prev_seq, prev_ts = seq, ts
        last_seq, last_ts = seq, ts
        off = (off + HDRLEN + ln) % DATA_SIZE
        remaining -= HDRLEN + ln
        n += 1

    print(f"  entries={n}  remaining_budget={remaining}  "
          f"seq_contiguous={seq_ok}  ts_monotonic={ts_ok}")
    print(f"  seq {first_seq}..{last_seq} (hdr 0x18={hdr[0x18]}, "
          f"match={last_seq == hdr[0x18]})")
    print(f"  last_ts={last_ts} (hdr 0x14={hdr[0x14]}, "
          f"delta={None if last_ts is None else hdr[0x14] - last_ts})")
    print("  types:", "  ".join(f"0x{t:04x}x{c}" for t, c in
                                sorted(types.items(), key=lambda kv: -kv[1])))


if __name__ == "__main__":
    args = sys.argv[1:] or sorted(glob.glob("ring_*.bin"))[-4:]
    for p in args:
        walk(p)
