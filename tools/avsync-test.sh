#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# A/V sync repro (run INSIDE WSL): feed both fifos at their real-time
# rates, capture ch0_0 (video+audio), and compare the two streams'
# pts spans. Both streams share the muxer's 90 kHz clock, so the span
# ratio is the sync verdict:
#   1.000 = in sync; 0.975 = the old bug (video clock counted at 20 fps
#   while the source runs ~19.5 fps -> video lags ~1.5 s per minute).
#
# Usage: wsl bash tools/avsync-test.sh <server-binary> <out.ts> [seconds] [video-rate]
#   video-rate: B/s, 0 = unlimited (default: the file's real-time rate)
set -e
SRV=$1
OUT=$2
SECS=${3:-45}
cd "$(dirname "$0")/.."

VFEED=/tmp/feed.h264
[ -f "$VFEED" ] || cp analysis/cap60.h264 "$VFEED"
AFEED=/tmp/audio_test.aac
[ -f "$AFEED" ] || { echo "audio feed missing (recreate with the -n capture)"; exit 1; }

rm -f /tmp/h264_fifo2 /tmp/aac_audio_fifo /tmp/avsync_srv.log
mkfifo /tmp/h264_fifo2 /tmp/aac_audio_fifo

# real-time feed rates: cap60.h264 = 60 s of 19.5 fps video; the audio
# capture = 16.35 s of 15.6 fps ADTS. NOTE: pacing the video feed below
# the server's drain rate starves the fifo and trips a known latent
# crash in the DESCRIBE dummy-sink path (use the unlimited rate with the
# fixed server; its wall-clock timestamps make any rate valid).
VSZ=$(stat -c%s "$VFEED"); VRATE=${4:-$((VSZ / 60))}
ASZ=$(stat -c%s "$AFEED"); ARATE=$((ASZ / 16))
echo "video feed rate=$VRATE B/s  audio feed rate=$ARATE B/s"

python3 analysis/feed-loop.py /tmp/h264_fifo2 "$VFEED" $VRATE >/tmp/avsync_vfeed.log 2>&1 &
VF=$!
python3 analysis/feed-loop.py /tmp/aac_audio_fifo "$AFEED" $ARATE >/tmp/avsync_afeed.log 2>&1 &
AF=$!
"$SRV" -p 8554 -d 0 >/tmp/avsync_srv.log 2>&1 &
SP=$!
sleep 2
ffmpeg -v error -i rtsp://127.0.0.1:8554/ch0_0.h264 -t $SECS -c copy "$OUT" 2>&1 | head -3 || true
kill $SP $VF $AF 2>/dev/null || true

python3 - "$OUT" <<'PYEOF'
import sys, subprocess
out = sys.argv[1]
def pts_times(sel):
    r = subprocess.run(["ffprobe","-v","error","-select_streams",sel,
                        "-show_entries","packet=pts_time","-of","csv=p=0",
                        out], capture_output=True, text=True)
    return [float(x) for x in r.stdout.split() if x]
v = pts_times("v:0"); a = pts_times("a:0")
if not v or not a:
    print("MISSING STREAMS: video=%d audio=%d" % (len(v), len(a))); sys.exit(1)
vspan = v[-1]-v[0]; aspan = a[-1]-a[0]
print("video: %d packets, pts span %.3f s" % (len(v), vspan))
print("audio: %d packets, pts span %.3f s" % (len(a), aspan))
print("span ratio (video/audio) = %.4f" % (vspan/aspan))
print("sync verdict: %.1f s of video lag per 60 s of stream"
      % ((aspan-vspan) * 60/aspan))
PYEOF
