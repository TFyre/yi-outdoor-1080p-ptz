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

# Server FIRST: it opens the fifo (blocking until the producer's write
# open) and waits for data. Its startup drain keeps the tail from the
# last SPS+PPS+IDR chain, so clients join at a keyframe even if the
# producer has already written by the time the server starts.
rm -f /tmp/h264_high_fifo
mkfifo /tmp/h264_high_fifo
nohup $BIN/rRTSPServer -a no >/tmp/rtsp.log 2>&1 &

# Producer second: waits for an SPS+PPS+IDR chain in the ring, then writes
# it as the first bytes of the stream. The buffer file can exist before
# the app has written any frames; the producer exits if the chain never
# appears, so retry until it stays up (a few seconds at boot).
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

echo "rtsp chain up"

# Watch loop: the producer is a sitting duck for the OOM killer (35 MB
# RAM total; the app's encode bursts have SIGKILLed it - exit 137), and
# the server can die when the fifo briefly loses its writer mid-restart.
# Respawn either idempotently so the chain self-heals without a reboot.
# This script runs detached from boot.sh, so the loop lives for the
# whole uptime.
while :; do
  PID=$(pidof fshare2fifo)
  if [ -z "$PID" ]; then
    echo "producer gone - respawning"
    nohup $BIN/fshare2fifo >>/tmp/fshare2fifo.log 2>&1 &
    sleep 3
    continue
  fi
  PID=$(pidof rRTSPServer)
  if [ -z "$PID" ]; then
    echo "server gone - respawning"
    nohup $BIN/rRTSPServer -a no >/tmp/rtsp.log 2>&1 &
    sleep 2
    continue
  fi
  sleep 5
done
