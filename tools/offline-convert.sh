#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Offline ring conversion (run INSIDE WSL): replay a ring snapshot as a
# live feed, walk it with the x86 build of fshare2fifo, capture the fifo
# with ffmpeg into a playable h264 file.
#
# Usage: wsl bash tools/offline-convert.sh <ring-snapshot> <out.h264> [seconds] [rate_bps]
#   seconds   - the media-time capture bound (default 60)
#   rate_bps  - the simulated writer's speed (default 80000; use
#               800000 to stress-test the hyperactive-era behavior)
set -e
SNAP=$1
OUT=$2
SECS=${3:-60}
RATE=${4:-80000}
cd "$(dirname "$0")/.."

gcc -O0 -g -o /tmp/f2f_x86 src/fshare2fifo/fshare2fifo.c -lpthread
rm -f /tmp/h264_fifo
mkfifo /tmp/h264_fifo

python3 tools/ring-player.py "$SNAP" "$RATE" /dev/shm/fshare_frame_buf \
    >/tmp/player.log 2>&1 &
PLAYER=$!
sleep 1
/tmp/f2f_x86 -f /tmp/h264_fifo >/tmp/f2f_x86.log 2>&1 &
WALK=$!
sleep 4

ffmpeg -loglevel error -f h264 -i /tmp/h264_fifo -t "$SECS" -c copy "$OUT" \
    || true
kill $WALK $PLAYER 2>/dev/null || true

ls -la "$OUT"
echo "walk events:" $(grep -cE 're-join|maxlag' /tmp/f2f_x86.log)
