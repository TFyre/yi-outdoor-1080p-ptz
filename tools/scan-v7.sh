#!/bin/bash
# Scan ARM binaries/objects for instructions that SIGILL on ARM1176 (VFPv2):
# ARMv7 integer ops + VFP double-precision ops.
# Usage: scan-v7.sh <file...>
set -u
for f in "$@"; do
    c=$(arm-linux-gnueabihf-objdump -d "$f" 2>/dev/null \
        | grep -cE '\budiv\b|\bsdiv\b|\bmovw\b|\bmovt\b|\bubfx\b|\bsbfx\b|\bbfi\b|vldr[[:space:]]+d[0-9]|vmov\.f64|vld1\.64|vst1\.64' || true)
    echo "$c $f"
done
