#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Swap in the new rRTSPServer, restart the chain with the tee armed.
# Run on the camera: ssh ... "sh -s" < tools/swap-rtsp.sh
# NOTE: no `set -e` - the producer-wait loop relies on if/break.
PID=$(pidof rRTSPServer); [ -n "$PID" ] && kill $PID
PID=$(pidof fshare2fifo); [ -n "$PID" ] && kill $PID
sleep 1
cp /tmp/rRTSPServer.new /tmp/sd/hack/bin/rRTSPServer
rm -f /tmp/h264_high_fifo
mkfifo /tmp/h264_high_fifo
rm -f /tmp/tee.bin
F2F_TEE=/tmp/tee.bin nohup /tmp/sd/hack/bin/rRTSPServer -a no >/tmp/rtsp.log 2>&1 &
sleep 2
n=0
while [ $n -lt 20 ]; do
  if [ -n "$(pidof fshare2fifo)" ]; then break; fi
  F2F_AGELOG=1 nohup /tmp/sd/hack/bin/fshare2fifo >>/tmp/fshare2fifo.log 2>&1 &
  n=$((n+1))
  sleep 3
done
sleep 4
echo "pids: $(pidof rRTSPServer fshare2fifo)"
echo "--- agelog:"
tail -2 /tmp/fshare2fifo.log
echo "--- rtsp.log:"
tail -3 /tmp/rtsp.log
