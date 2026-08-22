#!/bin/bash
# Native (x86_64) build of rRTSPServer for debugging under gdb in WSL.
set -e

SRC=/mnt/c/myprogs/yi-outdoor-1080p-ptz/analysis/upstream-yi-hack-v5/src/rRTSPServer
rm -rf "$SRC/live-host"
cp -r "$SRC/live" "$SRC/live-host"
cd "$SRC/live-host"

./genMakefiles linux-cross >/dev/null 2>&1
rm -f Makefile && cp "$SRC/Makefile.rRTSPServer" Makefile
sed -i 's|$(LIBRARY_LINK)$@|$(LIBRARY_LINK) $@|' \
    liveMedia/Makefile groupsock/Makefile \
    UsageEnvironment/Makefile BasicUsageEnvironment/Makefile
make clean >/dev/null 2>&1 || true   # drop stale cross-compiled objects
rm -f src/*.o                       # the clean target misses the wrapper objs
make -j4 CC=gcc CXX=g++ 2>&1 | tail -3
ls -la rRTSPServer
