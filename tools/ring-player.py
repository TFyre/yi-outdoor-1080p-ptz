#!/usr/bin/env python3
"""Replay a ring snapshot into /dev/shm/fshare_frame_buf as a live feed.

The x86 fshare2fifo harness walks the ring as if the camera's encoder
were writing it: the player advances a write head through the snapshot
at RATE bytes/s, wrapping at the buffer end forever. Run it in the
background, then start the harness producer (and ffmpeg on the fifo).

Usage: python3 tools/ring-player.py <snapshot> [rate_bps] [out_file]
"""
import os, sys, time

snap = sys.argv[1]
rate = int(sys.argv[2]) if len(sys.argv) > 2 else 80000
out = sys.argv[3] if len(sys.argv) > 3 else '/dev/shm/fshare_frame_buf'

data = open(snap, 'rb').read()
N = len(data)

with open(out, 'wb') as f:
    f.truncate(N)

fd = os.open(out, os.O_RDWR)
pos = 0
t0 = time.time()
written = 0
chunk = 32 * 1024
print("ring-player: %s (%d bytes) into %s at %d B/s" % (snap, N, out, rate),
      flush=True)
while True:
    time.sleep(0.02)
    target = int((time.time() - t0) * rate)
    while written < target:
        n = min(chunk, target - written)
        if pos + n > N:
            n = N - pos
        os.pwrite(fd, data[pos:pos + n], pos)
        pos = (pos + n) % N
        written += n
