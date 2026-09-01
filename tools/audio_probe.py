#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Probe 0x0100 (audio) records in ring snapshots with the validated
fshare framing (tools/verify_fshare_map.py): what do the AAC payloads
look like, how fast do they arrive relative to video, and what does the
'00 00 ff f9' tail really mean?"""
import struct
import sys
import collections

DATA_OFF = 300
DATA_SIZE = 0x1B4000
HDRLEN = 26


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def main(path):
    b = open(path, "rb").read()
    assert len(b) == DATA_OFF + DATA_SIZE, f"bad size {len(b)}"
    tail = u32(b, 0x10)
    valid = u32(b, 0x04)

    def rd(o, n):
        o %= DATA_SIZE
        if o + n <= DATA_SIZE:
            return b[DATA_OFF + o:DATA_OFF + o + n]
        first = DATA_SIZE - o
        return b[DATA_OFF + o:DATA_OFF + DATA_SIZE] + b[DATA_OFF:DATA_OFF + n - first]

    recs = []
    off, left = tail, valid
    while left > HDRLEN:
        h = rd(off, HDRLEN)
        ln, seq, magic = u32(h, 0), u32(h, 4), u32(h, 8)
        ts, ty, chain = u32(h, 16), u16(h, 20), u16(h, 22)
        if ln <= 0 or ln > DATA_SIZE:
            break
        recs.append((off, ln, seq, ts, ty, chain))
        off = (off + HDRLEN + ln) % DATA_SIZE
        left -= HDRLEN + ln
    print(f"{path}: {len(recs)} records walked, left={left}")

    audio = [r for r in recs if r[4] == 0x0100]
    video = [r for r in recs if r[4] == 0x0400]
    print(f"audio={len(audio)} video={len(video)} "
          f"ratio={len(audio)/max(1,len(video)):.3f}")

    # length histogram of audio payloads
    lens = collections.Counter()
    for r in audio:
        lens[r[1]] += 1
    print("audio payload len histogram (top 20):",
          sorted(lens.items(), key=lambda kv: -kv[1])[:20])

    # ts deltas: video first (calibration), then audio
    for name, rs in (("video", video), ("audio", audio)):
        deltas = [rs[i + 1][3] - rs[i][3] for i in range(len(rs) - 1)]
        deltas = [d for d in deltas if 0 <= d < 2**31]
        if deltas:
            dd = collections.Counter(deltas)
            print(f"{name} ts delta (top 8):",
                  sorted(dd.items(), key=lambda kv: -kv[1])[:8],
                  f"n={len(deltas)}")

    # audio payloads: first and last bytes
    print("\nfirst 8 audio payloads: len / first12 / last8")
    for r in audio[:8]:
        p = rd(r[0] + HDRLEN, r[1])
        print(f"  len={r[1]:4d} seq={r[2]} ts={r[3]} "
              f"head={p[:12].hex(' ')} tail={p[-8:].hex(' ')}")

    # where does the 'ff f9' tail sit? scan one payload
    p = rd(audio[0][0] + HDRLEN, audio[0][1])
    for pat in (b"\x00\x00\xff\xf9", b"\xff\xf9", b"\xff\xf1", b"\xff\xf0"):
        idxs = [i for i in range(len(p)) if p.startswith(pat, i)]
        print(f"  pattern {pat.hex()}: {len(idxs)} hits, first at {idxs[:5]}")
        for i in idxs[:3]:
            print(f"    @{i}: ...{p[max(0,i-6):i+10].hex(' ')}...")

    # first 3 bits of each payload = id_syn_ele (0=SCE,1=CPE,7=END)
    cnt = collections.Counter()
    for r in audio:
        p = rd(r[0] + HDRLEN, r[1])
        if p:
            cnt[p[0] >> 5] += 1
    print("first-3-bits (id_syn_ele) census:",
          {f"0x{k:x}": v for k, v in sorted(cnt.items())})

    # does any payload contain >1 AAC frame? check for 0x21/0x2b-ish sync-ish
    # (raw AAC has no sync; instead check SCE tag: bits 3..7 of payload
    #  after 3-bit id = element_instance_tag, usually 0)
    tags = collections.Counter()
    for r in audio:
        p = rd(r[0] + HDRLEN, r[1])
        if p:
            tags[(p[0] & 0x1F) >> 1] += 1  # not meaningful, just noise census
    print("byte0 low bits census:", sorted(tags.items()))


if __name__ == "__main__":
    for f in sys.argv[1:]:
        main(f)
