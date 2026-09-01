#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Native (x86_64) build of rRTSPServer for debugging under gdb in WSL.
set -e

REPO=/mnt/c/myprogs/yi-outdoor-1080p-ptz
SRC=$REPO/analysis/upstream-yi-hack-v5/src/rRTSPServer
LIVE=$SRC/live

# Re-apply the vendored fixes into the tree first - the same step the
# armv6 build does. The tree's src/ files can lag tools/vendor (the
# repro has run a stale upstream ByteStreamFifoSource before: no
# heartbeat poll, 64 KB fifo, blind drain - divergent from the deployed
# server), so this keeps the repro honest about what the camera runs.
cp "$REPO/tools/vendor/H264VideoFifoServerMediaSubsession.hh" "$LIVE/liveMedia/include/"
cp "$REPO/tools/vendor/H264VideoFifoServerMediaSubsession.cpp" "$LIVE/src/"
cp "$REPO/tools/vendor/ADTSAudioStreamDiscreteFramer.hh" "$LIVE/liveMedia/include/"
cp "$REPO/tools/vendor/ADTSAudioStreamDiscreteFramer.cpp" "$LIVE/liveMedia/"
cp "$REPO/tools/vendor/ByteStreamFifoSource.hh" "$LIVE/liveMedia/include/"
cp "$REPO/tools/vendor/ByteStreamFifoSource.cpp" "$LIVE/src/"
cp "$REPO/tools/vendor/RTPInterface.cpp" "$LIVE/liveMedia/"
cp "$REPO/tools/vendor/MultiFramedRTPSink.cpp" "$LIVE/liveMedia/"
cp "$REPO/tools/vendor/H264or5VideoStreamFramer.cpp" "$LIVE/liveMedia/"

rm -rf "$SRC/live-host"
cp -r "$SRC/live" "$SRC/live-host"
cd "$SRC/live-host"

# The camera's fifo path is held by a stale root-owned process in WSL;
# the repro feeds /tmp/h264_fifo2 instead. Native-only change.
sed -i 's|/tmp/h264_high_fifo|/tmp/h264_fifo2|' src/rRTSPServer.cpp

# Wall-clock presentation times (A/V sync) - the armv6 build bakes the
# same define into config.linux-cross, but this repro may run first (or
# against a config generated before the define existed); add it only if
# missing so the sed stays idempotent.
grep -q PRES_TIME_CLOCK config.linux-cross 2>/dev/null || \
    sed -i 's/-DNO_OPENSSL=1/-DNO_OPENSSL=1 -DPRES_TIME_CLOCK=1/' config.linux-cross 2>/dev/null || true

./genMakefiles linux-cross >/dev/null 2>&1
rm -f Makefile && cp "$SRC/Makefile.rRTSPServer" Makefile
sed -i 's|$(LIBRARY_LINK)$@|$(LIBRARY_LINK) $@|' \
    liveMedia/Makefile groupsock/Makefile \
    UsageEnvironment/Makefile BasicUsageEnvironment/Makefile
make clean >/dev/null 2>&1 || true   # drop stale cross-compiled objects
rm -f src/*.o                       # the clean target misses the wrapper objs
# NOTE: no -j; rRTSPServer's prereq check races the livemedia target
# (same as the armv6 build) - a -j4 link starts before libliveMedia.a
# is finished and fails with undefined symbols.
make CC=gcc CXX=g++ CFLAGS="-O0 -g" CXXFLAGS="-O0 -g" 2>&1 | tail -3
ls -la rRTSPServer
