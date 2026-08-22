#!/bin/sh
# /tmp/sd/hack/start-rtsp.sh — bring up the live H.264 RTSP chain.
#
# Runs detached from boot.sh: the stock app (rmm) creates
# /dev/shm/fshare_frame_buf only once the camera pipeline is up, and that
# can take a while after boot. Idempotent: safe to re-run by hand.
#
# Known limitation: if rmm restarts mid-uptime and recreates its buffer,
# the mmap in fshare2fifo keeps pointing at the old file and the chain
# stalls; re-run this script (or reboot) to recover.

HACK=/tmp/sd/hack
BIN=$HACK/bin

# Wait for the app's shared frame buffer (up to ~2 min)
i=0
while [ ! -e /dev/shm/fshare_frame_buf ]; do
  i=$((i+1))
  [ $i -ge 120 ] && { echo "fshare_frame_buf never appeared"; exit 1; }
  sleep 1
done

# Stop anything already running (idempotent re-run), server first so the
# producer is not torn down under a live client.
PID=$(pidof rRTSPServer)
[ -n "$PID" ] && kill $PID
PID=$(pidof fshare2fifo)
[ -n "$PID" ] && kill $PID
sleep 1
rm -f /tmp/h264_high_fifo

# Producer: fshare ring -> fifo (gates on the ring's frame counter).
# The buffer file can exist before the app has written any frames; the
# producer exits if its initial NAL scan finds nothing, so retry until it
# stays up (usually succeeds within a few seconds of boot).
n=0
while [ $n -lt 20 ]; do
  PID=$(pidof fshare2fifo)
  [ -n "$PID" ] && break
  nohup $BIN/fshare2fifo >>/tmp/fshare2fifo.log 2>&1 &
  n=$((n+1))
  sleep 3
done
PID=$(pidof fshare2fifo)
if [ -z "$PID" ]; then
  echo "producer never came up (buffer still empty?)"
  exit 1
fi

# Server: LIVE555 on 554, video only (audio fifo does not exist)
sleep 2
nohup $BIN/rRTSPServer -a no >/tmp/rtsp.log 2>&1 &

echo "rtsp chain up"
exit 0
