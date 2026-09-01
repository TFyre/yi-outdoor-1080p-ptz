#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay a ring snapshot into /dev/shm/fshare_frame_buf as a live feed.

The x86 fshare2fifo harness walks the ring as if the camera's encoder
were writing it: the player advances a write head through the snapshot
at RATE bytes/s. In the default loop mode the player wraps at the
buffer end forever (the walk keeps streaming, but the burned-in OSD
clock restarts every lap - the offline output visibly loops). With
--once the snapshot is fed exactly once and the player exits; the walk
then emits the recorded content a single time (the faithful
"record -> convert" mode).

Usage: python3 tools/ring-player.py <snapshot> [rate_bps] [out_file] [--once]
"""
import os, sys, time

snap = sys.argv[1]
rate = int(sys.argv[2]) if len(sys.argv) > 2 else 80000
out = sys.argv[3] if len(sys.argv) > 3 else '/dev/shm/fshare_frame_buf'
once = '--once' in sys.argv

data = open(snap, 'rb').read()
N = len(data)

with open(out, 'wb') as f:
    f.truncate(N)

fd = os.open(out, os.O_RDWR)
pos = 0
t0 = time.time()
written = 0
chunk = 32 * 1024
print("ring-player: %s (%d bytes) into %s at %d B/s%s"
      % (snap, N, out, rate, " (single pass)" if once else ""), flush=True)
while True:
    time.sleep(0.02)
    target = int((time.time() - t0) * rate)
    if once:
        target = min(target, N)
    while written < target:
        n = min(chunk, target - written)
        if pos + n > N:
            n = N - pos
        os.pwrite(fd, data[pos:pos + n], pos)
        pos = (pos + n) % N
        written += n
    if once and written >= N:
        print("ring-player: single pass complete", flush=True)
        time.sleep(5)   # let the walk drain the tail before exiting
        break
