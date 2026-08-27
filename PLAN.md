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
3. **DONE (2026-08-27): 60 s dump captured on a clean platform.** First
   the auto-start was disabled (section below), then the camera was
   rebooted on the user's order so no debug leftovers could pollute the
   capture. Post-boot verified clean: no chain processes, no feeder, no
   fifo; ring flowing (seq +43/s). The dump ran detached:
     ```
     ssh ... root@10.1.2.19 'rm -f /tmp/cap60b.h264; (nohup timeout 60 /tmp/sd/hack/bin/fshare2fifo -o /tmp/cap60b.h264 </dev/null > /tmp/dump60b.log 2>&1 &)'
     ```
     (`</dev/null` makes ssh return immediately — without it the
     nohup'd child keeps the session channel open until its 60 s
     timeout. `-o` writes the walk's exact emission; the SIGTERM
     handler flushed cleanly: log "slot 10 claimed (filter 0x0400,
     cursor 29058) / slot 10 released". The dump reads the ring
     directly — it does NOT touch the fifo, the server, or the chain's
     producer; it just claims a second reader slot, which the protocol
     supports.)
   - Copied down to `analysis/cap60.h264` (gitignored): **8,924,017
     bytes, md5 8e270ef54c62a2d08b8f6b3aad0ffcf5** (camera+local match,
     ~148 KB/s). The user plays it with ffplay.
4. Settled (2026-08-27): the user's 30 s movement capture
   (`cap60tfyre.h264`, 599 NALs) and the 60 s static capture
   (`analysis/cap60.h264`, 1162 NALs) both show the source at
   ~19.5 fps — the record-era hi-res stream really runs ~19-20 fps,
   NOT the documented 15 fps. Nothing was missing: the "fast OSD" was
   the playback filter forcing a higher FRAME_RATE than the source
   (30 s of camera time in 20 s of playback = 1.5×). For 1:1 viewing,
   play with `-framerate 20` or `setpts=N/20/TB`.
   While verifying, a REAL bug surfaced: mid-uptime the app's writer
   moved the record magic prefix from 0x6a8x to 0x6a90 (era drift;
   format unchanged — offline parse of `analysis/ring-era90.bin`
   shows 26-byte headers, exact stride arithmetic, consecutive seq).
   `magic_ok()` accepted only 0x6a80..0x6a8f, so the walk rejected
   every record of the new era: cursor pinned, 0 emission for as long
   as the era lasted (AGELOG: nals=0 while seq marched +41/s). Fixed:
   `magic_ok` now accepts any 0x6aXX family — the torn-record
   protection is the len/type/seq/next-magic cascade that follows the
   gate, so era drift can never stall the walk again. Rebuilt
   ARMv6-clean, deployed (md5-checked), verified live: emission
   resumed at 16-20 nals/s, drops=0, cursor tracks seq within 2.
   Primary objective verdict: emission is clean AND complete
   (drops=0, full source rate, quality great per the user). Awaiting
   the user's OK to move to the RTSP stall work.

## Auto-start of the RTSP chain is now OPT-IN (2026-08-27)

`hack/boot.sh` gates the `start-rtsp.sh` spawn behind a flag file:
create `/tmp/sd/hack/auto-rtsp` (any content) to bring the chain up at
boot; without it boot leaves the chain down, and it is started by hand
with `sh /tmp/sd/hack/start-rtsp.sh`. Deployed to the SD (md5-checked
match with `deploy/hack/boot.sh`). The chain is currently DOWN.

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
- Ctrl+C on the dump is SAFE: the deployed fshare2fifo handles
  SIGINT/SIGTERM (on_term → stop_flag → release_slot; seen as "slot 10
  released" in dump logs). The camera reboot the user saw right after a
  Ctrl+C was the known spontaneous-reboot flakiness (stressed movement
  era), not the dump — v2 touches no app semaphores/futexes. nohup
  itself only ignores SIGHUP (session close); if the dump also ran with
  `&`, Ctrl+C simply never reached it.
- Windows ssh can wedge probing the local SSH agent: every camera
  command hangs with no output while ping and dropbear stay healthy
  (`ssh -vv` stops right after "Next authentication method: publickey").
  Add `-o IdentityAgent=none` to the ssh command — the key is given
  with `-i` anyway. (Hit 2026-08-27; cost a lot of dead time.)
- Kill-scan footgun: `grep -q start-rtsp /proc/*/cmdline` kills YOUR OWN
  remote shell (its command line contains the pattern). Use bracket
  patterns that don't self-match (e.g. `start[-]rtsp`, `feed[.]h264`)
  and never put the literal path in the same command line as the scan.
- Useful diagnostics already in place: `/tmp/rtsp.log` (server, -d 4),
  `/tmp/fshare2fifo.log` (producer), `F2F_TRACE`/`F2F_AGELOG` env vars
  on the producer, `F2F_TEE=<file>` on the server (writes every byte it
  serves).
