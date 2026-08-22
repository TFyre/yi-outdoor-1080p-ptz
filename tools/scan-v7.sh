#!/bin/bash
# Scan ARM binaries/objects for instructions that SIGILL on ARM1176:
# ARMv7 integer ops + VFPv3/NEON-only ops.
#
# VFPv2 (this core) is NOT flagged: it has full double precision and
# d0-d15 load/store multiples (verified on the unit with tools/probe-vfp —
# vldmia {d8-d15}, faddd, fldmias all execute fine, including as the very
# first VFP instruction of a process). Only d16-d31 multiples are VFPv3.
#
# Usage: scan-v7.sh <file...>
set -u
for f in "$@"; do
    c=$(arm-linux-gnueabihf-objdump -d "$f" 2>/dev/null \
        | grep -cE '\budiv\b|\bsdiv\b|\bmovw\b|\bmovt\b|\bubfx\b|\bsbfx\b|\bbfi\b|\bvmov\.f64|\bvld1\.64|\bvst1\.64|vldm[^,]*\{d1[6-9]|vldm[^,]*\{d2[0-9]|vldm[^,]*\{d3[0-1]|vstm[^,]*\{d1[6-9]|vstm[^,]*\{d2[0-9]|vstm[^,]*\{d3[0-1]' || true)
    echo "$c $f"
done
