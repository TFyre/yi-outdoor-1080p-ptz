#!/bin/sh
# Run the producer in file mode for ~35 s, snapshot the ring at the end,
# restore the RTSP chain. The dump and the snapshot are era-coherent:
# the dump's tail is the snapshot's newest content.
# Run on the camera: ssh ... "sh -s" < tools/dump-walk.sh
PID=$(pidof rRTSPServer); [ -n "$PID" ] && kill $PID
PID=$(pidof fshare2fifo); [ -n "$PID" ] && kill $PID
sleep 1
[ -f /tmp/fshare2fifo.new ] && cp /tmp/fshare2fifo.new /tmp/sd/hack/bin/fshare2fifo
rm -f /tmp/dump_c.h264 /tmp/ring_e.bin
F2F_AGELOG=1 F2F_TRACE=1 nohup /tmp/sd/hack/bin/fshare2fifo -o /tmp/dump_c.h264 >>/tmp/fshare2fifo.log 2>&1 &
DUMP_PID=$!
sleep 35
kill $DUMP_PID 2>/dev/null
sleep 1
cp /dev/shm/fshare_frame_buf /tmp/ring_e.bin
ls -la /tmp/dump_c.h264 /tmp/ring_e.bin
# restore the chain (server first, then producer; see start-rtsp.sh)
rm -f /tmp/h264_high_fifo
mkfifo /tmp/h264_high_fifo
nohup /tmp/sd/hack/bin/rRTSPServer -a no >/tmp/rtsp.log 2>&1 &
sleep 2
n=0
while [ $n -lt 20 ]; do
  if [ -n "$(pidof fshare2fifo)" ]; then break; fi
  nohup /tmp/sd/hack/bin/fshare2fifo >>/tmp/fshare2fifo.log 2>&1 &
  n=$((n+1))
  sleep 3
done
sleep 3
echo "chain pids: $(pidof rRTSPServer fshare2fifo)"
