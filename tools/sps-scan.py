#!/usr/bin/env python3
"""Scan a fshare buffer sample for SPS NALs and parse their dimensions.
Reveals whether the ring interleaves multiple streams (resolutions).
Usage: python3 sps_scan.py <file> [<file>...]
"""
import sys


def parse_sps(rbsp):
    if len(rbsp) < 8:
        return None
    # strip emulation prevention bytes (00 00 03 -> 00 00)
    out = bytearray()
    z = 0
    for b in rbsp:
        if z >= 2 and b == 3:
            z = 0
            continue
        out.append(b)
        z = z + 1 if b == 0 else 0
    data = bytes(out)
    pos = 24  # skip profile_idc, constraints, level_idc (3 bytes)

    def read_bit():
        nonlocal pos
        if pos >= len(data) * 8:
            raise ValueError("eof")
        b = (data[pos >> 3] >> (7 - (pos & 7))) & 1
        pos += 1
        return b

    def read_ue():
        zeros = 0
        while read_bit() == 0:
            zeros += 1
            if zeros > 24:
                raise ValueError("ue overflow")
        val = 0
        for _ in range(zeros):
            val = (val << 1) | read_bit()
        return (1 << zeros) - 1 + val

    try:
        profile = data[0]
        read_ue()  # seq_parameter_set_id
        if profile in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
            chroma = read_ue()
            if chroma == 3:
                read_bit()
            read_ue()  # bit_depth_luma_minus8
            read_ue()  # bit_depth_chroma_minus8
            read_bit()  # qpprime_y_zero_transform_bypass
            if read_bit():  # seq_scaling_matrix_present
                for i in range(8 if chroma != 3 else 12):
                    if read_bit():
                        n = 16 if i < 6 else 64
                        last = 8
                        nxt = 8
                        for _ in range(n):
                            if nxt != 0:
                                nxt = (last + read_ue()) % 256
                            last = nxt if nxt != 0 else last
        read_ue()  # log2_max_frame_num_minus4
        poc_type = read_ue()
        if poc_type == 0:
            read_ue()
        elif poc_type == 1:
            read_bit()
            read_ue()
            read_ue()
            for _ in range(read_ue()):
                read_ue()
        read_ue()  # max_num_ref_frames
        read_bit()  # gaps_in_frame_num_value_allowed
        w = (read_ue() + 1) * 16
        h = (read_ue() + 1) * 16
        frame_mbs_only = read_bit()
        if not frame_mbs_only:
            read_bit()
            h *= 2
        return (w, h)
    except (ValueError, IndexError):
        return None


def scan(path):
    data = open(path, "rb").read()
    i = 0
    hits = []
    while True:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        if j + 3 < len(data) and (data[j + 3] & 0x1F) == 7:
            k = data.find(b"\x00\x00\x01", j + 4)
            end = k if k >= 0 else min(j + 200, len(data))
            hits.append((j, parse_sps(data[j + 4:end])))
        i = j + 4
    return hits


def main():
    for path in sys.argv[1:]:
        if path == "-v":
            continue
        hits = scan(path)
        from collections import Counter
        dims = Counter(d for _, d in hits)
        print("%s: %d SPS, dims: %s" % (path, len(hits), dict(dims)))
        if "-v" in sys.argv:
            for j, d in hits[:24]:
                print("  %08x %s" % (j, d))


if __name__ == "__main__":
    main()
