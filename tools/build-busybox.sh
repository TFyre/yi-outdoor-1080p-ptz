#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Static ARMv6 build of busybox (full defconfig applet set) for the camera.
# Same toolchain + flags as build-armv6.sh. The device's stock busybox is
# 1.26.2 and lacks many applets (id/wc/head/uname/base64/mkfifo...).
#
# Usage: bash tools/build-busybox.sh   (run in WSL)
set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
VER=1.38.0
SRCDIR=$HOME/build-armv6/src
TC=$HOME/musl-tc/arm-linux-musleabi-cross
CROSS=$TC/bin/arm-linux-musleabi
FLAGS="-march=armv6 -mfloat-abi=soft -Os"

cd "$SRCDIR"
[ -f busybox-$VER.tar.bz2 ] || wget -q https://busybox.net/downloads/busybox-$VER.tar.bz2
[ -d busybox-$VER ] || tar xjf busybox-$VER.tar.bz2
cd busybox-$VER

make distclean >/dev/null 2>&1 || true
make defconfig >/dev/null
sed -i \
  -e 's|# CONFIG_STATIC is not set|CONFIG_STATIC=y|' \
  -e 's|CONFIG_CROSS_COMPILER_PREFIX=""|CONFIG_CROSS_COMPILER_PREFIX="arm-linux-musleabi-"|' \
  -e 's|CONFIG_EXTRA_CFLAGS=""|CONFIG_EXTRA_CFLAGS="-march=armv6 -mfloat-abi=soft -Os"|' \
  .config

export PATH="$TC/bin:$PATH"
make -j"$(nproc)" >/dev/null
"$CROSS-strip" busybox

# Same ARMv6/VFPv2 purity scan as build-armv6.sh
n=$(arm-linux-gnueabihf-objdump -d busybox \
    | grep -cE '\budiv\b|\bsdiv\b|\bmovw\b|\bmovt\b|\bubfx\b|\bsbfx\b|\bbfi\b|\bvmov\.f64|\bvld1\.64|\bvst1\.64|vldm[^,]*\{d1[6-9]|vldm[^,]*\{d2[0-9]|vldm[^,]*\{d3[0-1]|vstm[^,]*\{d1[6-9]|vstm[^,]*\{d2[0-9]|vstm[^,]*\{d3[0-1]' \
    || true)
if [ "$n" != "0" ]; then
    echo "FAIL: busybox still contains $n ARMv7-only instructions" >&2
    exit 1
fi
echo "OK: busybox is ARMv6-clean"
ls -la busybox

mkdir -p "$REPO/deploy/hack/bin"
cp busybox "$REPO/deploy/hack/bin/busybox"
