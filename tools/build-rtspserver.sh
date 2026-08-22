#!/bin/bash
# Build the yi-hack-v5 rRTSPServer (LIVE555 fifo-based RTSP server) for the
# FH8626V100 camera: static musl, ARMv6 + VFPv2 (runs on ARM1176).
#
# Runs inside WSL. Prereqs:
#   - ~/musl-tc/arm-linux-musleabihf-cross  (musl.cc toolchain, see analysis/)
#   - live555 2020.01.19 tree at
#     analysis/upstream-yi-hack-v5/src/rRTSPServer/live
#     (from Ubuntu archive liblivemedia_2020.01.19.orig.tar.gz)
set -e

REPO=/mnt/c/myprogs/yi-outdoor-1080p-ptz
SRC=$REPO/analysis/upstream-yi-hack-v5/src/rRTSPServer
TC=$HOME/musl-tc/arm-linux-musleabihf-cross
LIVE=$SRC/live

# --- assemble the source tree ---
mkdir -p "$LIVE/src"
cp "$SRC/include/"*.hh "$LIVE/liveMedia/include/"
cp "$SRC/src/"*.cpp "$LIVE/src/"
# ADTSAudioStreamDiscreteFramer does not exist in live555 2020.01.19; it is
# vendored from a newer live555 (added upstream ~2020.04).
cp "$REPO/tools/vendor/ADTSAudioStreamDiscreteFramer.hh" "$LIVE/liveMedia/include/"
cp "$REPO/tools/vendor/ADTSAudioStreamDiscreteFramer.cpp" "$LIVE/liveMedia/"
sed -i 's/fFrameRate = 30.0/fFrameRate = 20.0/' "$LIVE/liveMedia/H264or5VideoStreamFramer.cpp"

# --- cross config (from upstream rRTSPServer.patch) ---
cat > "$LIVE/config.linux-cross" <<'EOF'
COMPILE_OPTS =		$(INCLUDES) -I. -O1 -ffunction-sections -fdata-sections -DSOCKLEN_T=socklen_t -D_LARGEFILE_SOURCE=1 -D_FILE_OFFSET_BITS=64 -DNO_OPENSSL=1 -DRTP_PAYLOAD_MAX_SIZE=1352 -DNEWLOCALE_NOT_USED
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

export PATH="$TC/bin:/usr/bin:/bin"
export AR=arm-linux-musleabihf-ar
export RANLIB=arm-linux-musleabihf-ranlib
cd "$LIVE"

# generate subdir Makefiles, then swap the top Makefile for the yi-hack one
./genMakefiles linux-cross
# the config's LIBRARY_LINK has no trailing space; the generated rules need one
sed -i 's|\$(LIBRARY_LINK)\$@|$(LIBRARY_LINK) $@|' \
    liveMedia/Makefile groupsock/Makefile \
    UsageEnvironment/Makefile BasicUsageEnvironment/Makefile
rm -f Makefile && cp "$SRC/Makefile.rRTSPServer" Makefile

# NOTE: no -j here. With parallel make, rRTSPServer's prerequisite check races
# the livemedia target and fails with "No rule to make target liveMedia/…".
make \
  CC=arm-linux-musleabihf-gcc \
  CXX=arm-linux-musleabihf-g++ \
  CFLAGS="-march=armv6 -mfpu=vfp -mfloat-abi=hard" \
  CXXFLAGS="-march=armv6 -mfpu=vfp -mfloat-abi=hard" \
  LDFLAGS="-static"

"$TC/bin/arm-linux-musleabihf-strip" rRTSPServer
ls -la rRTSPServer
