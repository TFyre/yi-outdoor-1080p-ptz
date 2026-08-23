#!/bin/bash
# ARMv6-clean STATIC builds of the camera binaries (run on ARM1176 + VFPv2).
#
# Toolchain: musl.cc arm-linux-musleabi (SOFT-float, ARMv5TE baseline) at
#   $HOME/musl-tc/arm-linux-musleabi-cross
# The hard-float musl.cc armhf toolchain was abandoned: its prebuilt
# libgcc/libstdc++ contain ARMv7 instructions (movw/udiv) that SIGILL here,
# and Ubuntu's armhf gcc silently upgrades hard-float to VFPv3-D16.
# Soft-float + static = fully self-contained, ARM1176-safe by construction.
#
# Usage: build-armv6.sh <fshare2fifo|rtspserver|all>
set -e

REPO=/mnt/c/myprogs/yi-outdoor-1080p-ptz
TC=$HOME/musl-tc/arm-linux-musleabi-cross
CROSS=$TC/bin/arm-linux-musleabi
FLAGS="-march=armv6 -mfloat-abi=soft -static -Os"
TARGET=${1:-all}

# ARMv7 integer ops + VFPv3/NEON-only ops. Note: VFPv2 (this core) has full
# double precision and d0-d15 multiples — probe-vfp proved vldmia {d8-d15}
# and faddd legal on the unit, so only d16-d31 multiples are flagged.
ARM7_PAT='\budiv\b|\bsdiv\b|\bmovw\b|\bmovt\b|\bubfx\b|\bsbfx\b|\bbfi\b|\bvmov\.f64|\bvld1\.64|\bvst1\.64|vldm[^,]*\{d1[6-9]|vldm[^,]*\{d2[0-9]|vldm[^,]*\{d3[0-1]|vstm[^,]*\{d1[6-9]|vstm[^,]*\{d2[0-9]|vstm[^,]*\{d3[0-1]'

verify_armv6() {
    local n
    n=$(arm-linux-gnueabihf-objdump -d "$1" \
        | grep -cE "$ARM7_PAT" \
        || true)
    if [ "$n" != "0" ]; then
        echo "FAIL: $1 still contains $n ARMv7-only instructions" >&2
        exit 1
    fi
    echo "OK: $1 is ARMv6-clean"
}

build_fshare2fifo() {
    cd "$REPO/src/fshare2fifo"
    ${CROSS}-gcc $FLAGS -Wall -o fshare2fifo fshare2fifo.c -lpthread
    verify_armv6 fshare2fifo
    ls -la fshare2fifo
}

build_rtspserver() {
    SRC=$REPO/analysis/upstream-yi-hack-v5/src/rRTSPServer
    LIVE=$SRC/live

    mkdir -p "$LIVE/src"
    cp "$SRC/include/"*.hh "$LIVE/liveMedia/include/"
    cp "$SRC/src/"*.cpp "$LIVE/src/"
    cp "$REPO/tools/vendor/ADTSAudioStreamDiscreteFramer.hh" "$LIVE/liveMedia/include/"
    cp "$REPO/tools/vendor/ADTSAudioStreamDiscreteFramer.cpp" "$LIVE/liveMedia/"
    cp "$REPO/tools/vendor/ByteStreamFifoSource.hh" "$LIVE/liveMedia/include/"
    cp "$REPO/tools/vendor/ByteStreamFifoSource.cpp" "$LIVE/src/"
    # Patched subsession: adds the PacedFilter (15 fps delivery pacing)
    cp "$REPO/tools/vendor/H264VideoFifoServerMediaSubsession.cpp" "$LIVE/src/"
    sed -i 's/fFrameRate = 30.0/fFrameRate = 20.0/' "$LIVE/liveMedia/H264or5VideoStreamFramer.cpp"
    # musl 32-bit ARM: time_t is 64-bit; the upstream %ld printf reads only
    # 32 bits, misaligning every subsequent vararg and crashing sprintf.
    sed -i 's/o=- %ld%06ld %d IN IP4/o=- %lld%06lld %d IN IP4/' "$LIVE/liveMedia/ServerMediaSession.cpp"

    cat > "$LIVE/config.linux-cross" <<'EOF'
COMPILE_OPTS =		$(INCLUDES) -I. -O1 -ffunction-sections -fdata-sections -DSOCKLEN_T=socklen_t -D_LARGEFILE_SOURCE=1 -D_FILE_OFFSET_BITS=64 -DNO_OPENSSL=1 -DRTP_PAYLOAD_MAX_SIZE=1352 -DNEWLOCALE_NOT_USED -DALLOW_RTSP_SERVER_PORT_REUSE=1
C =			c
C_COMPILER =		$(CC)
C_FLAGS =		$(COMPILE_OPTS) $(CPPFLAGS) $(CFLAGS)
CPP =			cpp
CPLUSPLUS_COMPILER =	$(CXX)
CPLUSPLUS_FLAGS =	$(COMPILE_OPTS) -Wall -DBSD=1 $(CPPFLAGS) $(CXXFLAGS)
OBJ =			o
LINK =			$(CXX) -o
LINK_OPTS =		-Wl,--gc-sections -L. $(LDFLAGS)
CONSOLE_LINK_OPTS =	$(LINK_OPTS)
LIBRARY_LINK =		$(AR) cr
LIBRARY_LINK_OPTS =
LIB_SUFFIX =		a
LIBS_FOR_CONSOLE_APPLICATION =
LIBS_FOR_GUI_APPLICATION =
EXE =
EOF

    cd "$LIVE"
    ./genMakefiles linux-cross
    sed -i 's|$(LIBRARY_LINK)$@|$(LIBRARY_LINK) $@|' \
        liveMedia/Makefile groupsock/Makefile \
        UsageEnvironment/Makefile BasicUsageEnvironment/Makefile
    rm -f Makefile && cp "$SRC/Makefile.rRTSPServer" Makefile
    make clean >/dev/null 2>&1 || true   # drop stale objects from other toolchains
    rm -f src/*.o

    # NOTE: no -j; rRTSPServer's prereq check races the livemedia target.
    export PATH="$TC/bin:$PATH"
    export AR=${CROSS}-ar RANLIB=${CROSS}-ranlib
    make \
        CC=${CROSS}-gcc CXX=${CROSS}-g++ \
        CFLAGS="$FLAGS" CXXFLAGS="$FLAGS" LDFLAGS="$FLAGS"
    ${CROSS}-strip rRTSPServer
    verify_armv6 rRTSPServer
    ls -la rRTSPServer
}

case "$TARGET" in
    fshare2fifo) build_fshare2fifo ;;
    rtspserver)  build_rtspserver ;;
    all)         build_fshare2fifo; build_rtspserver ;;
    *) echo "usage: $0 <fshare2fifo|rtspserver|all>"; exit 1 ;;
esac
