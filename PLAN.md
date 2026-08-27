# PLAN — current state and next steps (updated 2026-08-27)

This is the live plan. The user drives each step; nothing here is auto-approved.

## Standing constraints (non-negotiable)

- **Never reboot the camera** — the user reboots by hand.
- **Do NOT auto-start fshare2fifo or rRTSPServer.** No `start-rtsp.sh`
  runs, no watcher restarts, no manual chain launches unless the user
  asks. If the chain is already running, leave it alone (don't kill,
  don't respawn).
- **Do NOT auto-verify video** (no ffprobe/ffmpeg decode, no OSD-OCR)
  unless the user asks — it wastes credits. Dump, copy, hand over.
- Stock firmware dumps stay out of git; `backup/` and `analysis/` are
  gitignored.
- SD-folder deletions require explicit user confirmation.
- Conventional Commits.

## Primary objective (IN PROGRESS): prove fshare2fifo's emission is clean

1. The v2 lock-free reader (commit d34df8c, live-edge jump f2c786b) is
   deployed on the camera at `/tmp/sd/hack/bin/fshare2fifo`.
2. Found + fixed a NAL truncation bug (commit c77c753): the multi-NAL
   walk's end-scan (`while (e + 4 <= len ...)`) stopped every
   record-final NAL at len-3 — each slice lost its cabac tail, the
   decoder overread by 5–9 bytes, and the bottom macroblock row was
   concealed on every frame. The fix (`if (e + 4 > len) e = len;`) is
   built (ARMv6-clean) and deployed to the SD. The user's
   watcher-supervised chain (if still up) is already running the fixed
   binary — a respawn after the deploy picked it up.
3. **Next: fetch 60 s of the producer's emission for the user to view.**
   - Dump on the camera, detached so an ssh drop can't kill it:
     ```
     ssh ... root@10.1.2.19 'rm -f /tmp/cap60b.h264; (nohup timeout 60 /tmp/sd/hack/bin/fshare2fifo -o /tmp/cap60b.h264 > /tmp/dump60b.log 2>&1 &)'
     ```
     (`-o` writes the walk's exact emission; SIGTERM handler flushes
     cleanly. The dump reads the ring directly — it does NOT touch the
     fifo, the server, or the chain's producer; it just claims a second
     reader slot, which the protocol supports.)
   - Wait ~65 s, then check `ls -l /tmp/cap60b.h264` (~120 KB/s × 60 s ≈
     7 MB). A previous dump attempt may have left a partial
     `/tmp/cap60b.h264` or none — re-run the command if the file is
     missing or small. Do not disturb the running chain.
   - Copy down: pipe over ssh into `analysis/cap60.h264` (gitignored):
     ```
     ssh ... 'cat /tmp/cap60b.h264' > analysis/cap60.h264
     ```
   - Tell the user where it is. They play it with ffplay. No
     auto-verification.
4. Wait for the user's verdict. Do not proceed to the RTSP work until
   they are happy with the file.

## Secondary objective (BLOCKED on the primary): the RTSP server stall

Current understanding when we return to it:

- **Symptom**: any client (ffplay/ffmpeg) gets a ~400–600 KB burst
  (~2–4 s of stream: 4 chains + ~50 P slices), then nothing. The TCP
  session stays ESTABLISHED, Send-Q stays 0, the client just starves.
- **Producer side is exonerated**: ring flows (~56 records/s), our slot
  cursor tracks newest (caught up), the reader emits 15–20 NALs/s ≈
  150 KB/s, drops=0. Reproduced with a clean-bytes feeder (no producer
  at all) → the bytes/parser/sink chain on the SERVER is at fault.
- **Server log signature** (run with `rRTSPServer -d 4`, log goes to
  `/tmp/rtsp.log`): a few `src: read n=... fill=... awaiting=1` lines
  (reads capped at 150,000 = the live555 parser's BANK_SIZE), 6–8 s
  gaps between reads, then `src: retry fired, not awaiting` forever —
  the sink/framer/parser chain stops requesting the source.
- **Ruled out**: truncation (`OutPacketBuffer::maxSize` is already
  262144 in rRTSPServer.cpp:319; the client receives full 76 KB IDRs);
  blocked sends (Send-Q 0); pacing (playTimePerFrame=0 → fDuration=0);
  writer-gap EOF (the feeder test kept the fifo full with no gaps).
- **Open hypotheses**: (a) the parser wedges mid-NAL across the 150 KB
  bank boundary (the tee ended 26 KB into a 60 KB IDR); (b) something
  in the live555 H264or5VideoStreamParser state machine stalls on this
  era's NAL pattern (double synthesized SPS, 4-byte PPS, AUDs before
  slices). Not yet pinpointed.
- **Offline repro is ready** (built 2026-08-27, with debug symbols):
  `analysis/upstream-yi-hack-v5/src/rRTSPServer/live-host/rRTSPServer`
  — x86_64 native, vendored sources applied, built with NO_OPENSSL,
  **fifo path sed-patched to `/tmp/h264_fifo2`** (the original
  `/tmp/h264_high_fifo` is held by a stale root-owned process from an
  old session). Repro recipe: `mkfifo /tmp/h264_fifo2`, a python feeder
  looping `/tmp/feed.h264` (single open, 4 KB chunks — no writer gaps),
  run the server with `-d 4`, capture with ffmpeg. Next step there:
  strace/gdb the parse chain at the moment it stops requesting.

## Camera state caveats for the fresh session

- The user reports the camera **reboots after a while — something is
  blocking**. Suspected leftovers from the debugging session: an
  orphaned `(while true; do cat /tmp/feed.h264; done) > /tmp/h264_high_fifo`
  feeder loop, a manual `rRTSPServer -d 4` (pid 12048 at last check),
  and a detached `fshare2fifo -o` dump. Check `/proc/*/cmdline` before
  touching anything; clean up ONLY what the user approves. Do not run
  anything that can block (e.g. a raw `cat > fifo` without a reader).
- Kill-scan footgun: `grep -q start-rtsp /proc/*/cmdline` kills YOUR OWN
  remote shell (its command line contains the pattern). Use bracket
  patterns that don't self-match (e.g. `start[-]rtsp`, `feed[.]h264`)
  and never put the literal path in the same command line as the scan.
- Useful diagnostics already in place: `/tmp/rtsp.log` (server, -d 4),
  `/tmp/fshare2fifo.log` (producer), `F2F_TRACE`/`F2F_AGELOG` env vars
  on the producer, `F2F_TEE=<file>` on the server (writes every byte it
  serves).
