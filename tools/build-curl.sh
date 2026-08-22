#!/bin/bash
# Static ARMv6 build of curl for the camera — HTTP (+RTSP) only, no TLS.
# TLS would need mbedtls/openssl static builds; ONVIF SOAP is plain HTTP,
# so this is enough for Phase 3. Same toolchain + flags as build-armv6.sh.
#
# Usage: bash tools/build-curl.sh   (run in WSL)
set -e

REPO=/mnt/c/myprogs/yi-outdoor-1080p-ptz
VER=8.4.0
SRCDIR=$HOME/build-armv6/src
TC=$HOME/musl-tc/arm-linux-musleabi-cross
CROSS=$TC/bin/arm-linux-musleabi

cd "$SRCDIR"
[ -f curl-$VER.tar.gz ] || wget -q https://curl.se/download/curl-$VER.tar.gz
[ -d curl-$VER ] || tar xzf curl-$VER.tar.gz
cd curl-$VER

export PATH="$TC/bin:$PATH"
./configure \
  --host=arm-linux-musleabi \
  CC="$CROSS-gcc" AR="$CROSS-ar" RANLIB="$CROSS-ranlib" \
  CFLAGS="-march=armv6 -mfloat-abi=soft -Os" LDFLAGS="-static" \
  --disable-shared --enable-static \
  --without-ssl --without-libpsl --without-libidn2 --without-brotli \
  --without-zstd --without-zlib --without-libssh2 --without-nghttp2 \
  --without-ngtcp2 --without-nghttp3 --without-librtmp \
  --disable-ldap --disable-ldaps --disable-manual --disable-docs \
  --disable-dict --disable-gopher --disable-mqtt \
  --disable-ftp --disable-tftp --disable-telnet \
  --disable-pop3 --disable-imap --disable-smtp \
  >/dev/null

make -j"$(nproc)" -C lib >/dev/null
make -C src curl_LDFLAGS="-static" >/dev/null
"$CROSS-strip" src/curl

# Same ARMv6/VFPv2 purity scan as build-armv6.sh
n=$(arm-linux-gnueabihf-objdump -d src/curl \
    | grep -cE '\budiv\b|\bsdiv\b|\bmovw\b|\bmovt\b|\bubfx\b|\bsbfx\b|\bbfi\b|\bvmov\.f64|\bvld1\.64|\bvst1\.64|vldm[^,]*\{d1[6-9]|vldm[^,]*\{d2[0-9]|vldm[^,]*\{d3[0-1]|vstm[^,]*\{d1[6-9]|vstm[^,]*\{d2[0-9]|vstm[^,]*\{d3[0-1]' \
    || true)
if [ "$n" != "0" ]; then
    echo "FAIL: curl still contains $n ARMv7-only instructions" >&2
    exit 1
fi
echo "OK: curl is ARMv6-clean"
ls -la src/curl

mkdir -p "$REPO/deploy/hack/bin"
cp src/curl "$REPO/deploy/hack/bin/curl"
