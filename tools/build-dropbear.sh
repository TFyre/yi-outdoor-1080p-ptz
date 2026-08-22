#!/bin/bash
# Static ARMv6 build of dropbear (dropbearmulti: dropbear/dropbearkey/scp/
# dbclient) for the camera. Replaces the 2018.76 binary inherited from the
# yi-hack release — that one predates CVE-2019-6111, CVE-2025-14282 and
# the 2026 forced-command fixes, and we run it as root.
# Same toolchain + flags as build-armv6.sh.
#
# Usage: bash tools/build-dropbear.sh   (run in WSL)
set -e

REPO=/mnt/c/myprogs/yi-outdoor-1080p-ptz
VER=2026.94
SRCDIR=$HOME/build-armv6/src
TC=$HOME/musl-tc/arm-linux-musleabi-cross
CROSS=$TC/bin/arm-linux-musleabi
FLAGS="-march=armv6 -mfloat-abi=soft -Os"

cd "$SRCDIR"
[ -f dropbear-$VER.tar.gz ] || \
    wget -q -O dropbear-$VER.tar.gz \
    https://github.com/mkj/dropbear/archive/refs/tags/DROPBEAR_$VER.tar.gz
[ -d dropbear-DROPBEAR_$VER ] || tar xzf dropbear-$VER.tar.gz
cd dropbear-DROPBEAR_$VER

export PATH="$TC/bin:$PATH"
./configure \
  --host=arm-linux-musleabi \
  CC="$CROSS-gcc" AR="$CROSS-ar" RANLIB="$CROSS-ranlib" \
  CFLAGS="$FLAGS" LDFLAGS="-static" \
  --disable-zlib --disable-pam --disable-lastlog --disable-utmp \
  --disable-utmpx --disable-wtmp --disable-wtmpx --disable-loginfunc \
  --disable-pututline --disable-pututxline --disable-harden \
  >/dev/null

# MULTI=1 produces the multicall dropbearmulti, same interface as the
# yi-hack binary (dropbearmulti dropbear -p 2222 ...).
make -j"$(nproc)" PROGRAMS="dropbear dbclient dropbearkey scp" MULTI=1 >/dev/null
"$CROSS-strip" dropbearmulti

# Same ARMv6/VFPv2 purity scan as build-armv6.sh
n=$(arm-linux-gnueabihf-objdump -d dropbearmulti \
    | grep -cE '\budiv\b|\bsdiv\b|\bmovw\b|\bmovt\b|\bubfx\b|\bsbfx\b|\bbfi\b|\bvmov\.f64|\bvld1\.64|\bvst1\.64|vldm[^,]*\{d1[6-9]|vldm[^,]*\{d2[0-9]|vldm[^,]*\{d3[0-1]|vstm[^,]*\{d1[6-9]|vstm[^,]*\{d2[0-9]|vstm[^,]*\{d3[0-1]' \
    || true)
if [ "$n" != "0" ]; then
    echo "FAIL: dropbearmulti still contains $n ARMv7-only instructions" >&2
    exit 1
fi
echo "OK: dropbearmulti is ARMv6-clean"
ls -la dropbearmulti

mkdir -p "$REPO/deploy/hack/bin"
cp dropbearmulti "$REPO/deploy/hack/bin/dropbearmulti"
