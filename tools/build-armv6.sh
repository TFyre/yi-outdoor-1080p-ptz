#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
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

REPO=$(cd "$(dirname "$0")/.." && pwd)
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
    # The audio producer is the same binary under a name whose comm
    # fits pidof (<= 15 chars); argv[0] dispatch turns on audio mode.
    cp fshare2fifo f2f_audio
    ls -la fshare2fifo f2f_audio
}

build_ringcapture() {
    cd "$REPO/src/ring-capture"
    ${CROSS}-gcc $FLAGS -Wall -o ring-capture ring-capture.c -lpthread
    verify_armv6 ring-capture
    ls -la ring-capture
}

build_slotprobe() {
    cd "$REPO/src/ring-capture"
    ${CROSS}-gcc $FLAGS -Wall -o slotprobe slotprobe.c -lpthread
    verify_armv6 slotprobe
    ls -la slotprobe
}

build_endprobe() {
    cd "$REPO/src/ring-capture"
    ${CROSS}-gcc $FLAGS -Wall -o endprobe endprobe.c -lpthread
    verify_armv6 endprobe
    ls -la endprobe
}

build_scanprobe() {
    cd "$REPO/src/ring-capture"
    ${CROSS}-gcc $FLAGS -Wall -o scanprobe scanprobe.c -lpthread
    verify_armv6 scanprobe
    ls -la scanprobe
}

build_captimeline() {
    cd "$REPO/src/ring-capture"
    ${CROSS}-gcc $FLAGS -Wall -o cap-timeline cap-timeline.c -lpthread
    verify_armv6 cap-timeline
    ls -la cap-timeline
}

build_rtspserver() {
    SRC=$REPO/analysis/upstream-yi-hack-v5/src/rRTSPServer
    LIVE=$SRC/live

    mkdir -p "$LIVE/src"
    cp "$SRC/include/"*.hh "$LIVE/liveMedia/include/"
    cp "$SRC/src/"*.cpp "$LIVE/src/"
    # Vendored fixes on top of the upstream tree (the tree is gitignored;
    # everything we change must live here or a fresh clone loses it):
    cp "$REPO/tools/vendor/H264VideoFifoServerMediaSubsession.hh" "$LIVE/liveMedia/include/"
    cp "$REPO/tools/vendor/H264VideoFifoServerMediaSubsession.cpp" "$LIVE/src/"
    cp "$REPO/tools/vendor/ADTSAudioStreamDiscreteFramer.hh" "$LIVE/liveMedia/include/"
    cp "$REPO/tools/vendor/ADTSAudioStreamDiscreteFramer.cpp" "$LIVE/liveMedia/"
    cp "$REPO/tools/vendor/ByteStreamFifoSource.hh" "$LIVE/liveMedia/include/"
    cp "$REPO/tools/vendor/ByteStreamFifoSource.cpp" "$LIVE/src/"
    cp "$REPO/tools/vendor/RTPInterface.cpp" "$LIVE/liveMedia/"
    cp "$REPO/tools/vendor/MultiFramedRTPSink.cpp" "$LIVE/liveMedia/"
    cp "$REPO/tools/vendor/H264or5VideoStreamFramer.cpp" "$LIVE/liveMedia/"
    sed -i 's/fFrameRate = 30.0/fFrameRate = 20.0/' "$LIVE/liveMedia/H264or5VideoStreamFramer.cpp"
    # musl 32-bit ARM: time_t is 64-bit; the upstream %ld printf reads only
    # 32 bits, misaligning every subsequent vararg and crashing sprintf.
    sed -i 's/o=- %ld%06ld %d IN IP4/o=- %lld%06lld %d IN IP4/' "$LIVE/liveMedia/ServerMediaSession.cpp"

    cat > "$LIVE/config.linux-cross" <<'EOF'
COMPILE_OPTS =		$(INCLUDES) -I. -O1 -ffunction-sections -fdata-sections -DSOCKLEN_T=socklen_t -D_LARGEFILE_SOURCE=1 -D_FILE_OFFSET_BITS=64 -DNO_OPENSSL=1 -DPRES_TIME_CLOCK=1 -DRTP_PAYLOAD_MAX_SIZE=1352 -DNEWLOCALE_NOT_USED -DALLOW_RTSP_SERVER_PORT_REUSE=1
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
    # strip copies-then-renames; on DrvFs a freshly linked binary is often
    # briefly locked (Windows Defender) - retry rather than fail the build.
    ok=0
    for i in 1 2 3 4 5; do
        if ${CROSS}-strip rRTSPServer; then ok=1; break; fi
        sleep 1
    done
    [ "$ok" = "1" ] || { echo "strip failed after 5 retries" >&2; exit 1; }
    verify_armv6 rRTSPServer
    ls -la rRTSPServer
}

build_ptz() {
    cd "$REPO/src/ptz"
    ${CROSS}-gcc $FLAGS -Wall -o ptz ptz.c -lrt
    verify_armv6 ptz
    # the same binary doubles as the web CGI (argv[0] "ptz.cgi"):
    # a shell CGI costs ~0.6 s per command on this CPU, C is ~0.05 s
    mkdir -p "$REPO/deploy/hack/www/cgi-bin"
    cp ptz "$REPO/deploy/hack/www/cgi-bin/ptz.cgi"
    ls -la ptz "$REPO/deploy/hack/www/cgi-bin/ptz.cgi"
}

build_onvif() {
    cd "$REPO/src/onvif"
    ${CROSS}-gcc $FLAGS -Wall -o onvif onvif.c -lrt
    verify_armv6 onvif
    ls -la onvif
}

build_cpldio() {
    cd "$REPO/src/cpldio"
    ${CROSS}-gcc $FLAGS -Wall -o cpldio cpldio.c
    verify_armv6 cpldio
    ls -la cpldio
}

case "$TARGET" in
    fshare2fifo) build_fshare2fifo ;;
    rtspserver)  build_rtspserver ;;
    ptz)         build_ptz ;;
    onvif)       build_onvif ;;
    cpldio)      build_cpldio ;;
    ringcapture) build_ringcapture ;;
    slotprobe)   build_slotprobe ;;
    endprobe)    build_endprobe ;;
    scanprobe)   build_scanprobe ;;
    captimeline) build_captimeline ;;
    all)         build_fshare2fifo; build_ptz; build_onvif; build_cpldio; build_ringcapture; build_slotprobe; build_endprobe; build_scanprobe; build_captimeline; build_rtspserver ;;
    *) echo "usage: $0 <fshare2fifo|rtspserver|ptz|onvif|cpldio|ringcapture|all>"; exit 1 ;;
esac
