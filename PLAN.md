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

## Secondary objective (primary settled 2026-08-27): the RTSP server stall

**Server changes vs stock yi-hack-v5 (inventoried 2026-08-27 — the
"zoom out" question):** the diff surface is 3 files in `tools/vendor/`,
and none of it was fifo-health compensation:

- `rRTSPServer.cpp`: ZERO changes (upstream already ships
  maxSize=262144; the plan's truncation line cited that upstream line).
- `H264VideoFifoServerMediaSubsession`: ONE change — playTimePerFrame
  50000→0. A live555 pacing misconfiguration (50 ms PER PACKET → the
  12 s stalls / frozen frame). Real server bug; keep.
- `RTPInterface.cpp`: slow-client drop (a paused player wedged the
  single-threaded event loop). Client-behavior defense; keep.
- `ByteStreamFifoSource.cpp`: the real rework — 1 MB fifo
  (F_SETPIPE_SZ: chains are 300-500 KB and must fit whole — driven by
  chain SIZE, not producer health), drain→keep-last-chain (join
  decode + live-edge, no backlog replay), heartbeat poll (the server
  must never stop reading the fifo — the unresolved stall lives in
  this area), 4-byte SC handling (now producer-side too; harmless
  belt-and-braces), F2F_TEE forensics tee (env-gated; keep for the
  stall work). One fifo-compensation experiment (session-gap skip,
  f81439f) was already reverted.
- Everything else (ADTS framer, DummySink, the 20 fps / PRES_TIME_CLOCK
  live555 patch, NO_OPENSSL, the %lld SDP sed) is the upstream baseline
  or platform build fixes — untouched by us.

Verdict: do NOT start from scratch. With the producer provably clean,
the stall hunt can trust the input completely and focus on the
server's parse/sink chain — the only suspect left, matching the
hypotheses below.


**RESOLVED (2026-08-27, commit 560f6c3) — the stall was TWO server bugs,
found with strace+gdb on the offline repro:**

- **Bug 1 — the sink paced every NAL.** `MultiFramedRTPSink::
  afterGettingFrame1` advances `fNextSendTime` by the frame's duration,
  with a 66667 us PER-NAL fallback when the duration is 0. The
  subsession's `playTimePerFrame=0` never reached the sink — the
  FRAMER recomputes `fDurationInMicroseconds` from its own fFrameRate
  — so the earlier "pacing ruled out" was based on zeroing the wrong
  knob. Measured: duration=0 at the sink → 15 NALs/s cap against a
  ~19.5 fps source → the fifo fills, the producer's whole-or-drop
  kicks in, clients see a burst then a frozen stream. The 6–8 s read
  gaps were the sink trickling through a 150 KB parser bank. Fixed:
  `MultiFramedRTPSink.cpp` vendored with the advance removed — a live
  fifo source IS the clock; every packet goes out immediately.
- **Bug 2 — the RTPInterface drop killed healthy clients.** The
  vendored drop-on-any-partial-send fired on the first EAGAIN of an
  unpaced burst (the drain-kept chain alone is ~90 KB back-to-back):
  one 1076-byte packet partially sent → the client's whole RTP stream
  was removed → "burst then nothing" with the session still
  ESTABLISHED and Send-Q 0 — exactly the camera signature (gdb caught
  it: `sendDataOverTCP: send buffer full (324 of 1352 bytes sent)`).
  Fixed: bounded poll-writable retry (500 ms budget) to complete the
  packet; only a genuinely stalled client (budget expiry) gets
  dropped — preserving the 6f7b205 intent (never wedge the event
  loop) without killing healthy streams.
- Repro verification: 45 s capture, **9.08 MB at full source rate,
  0 drops** (was: 1.4 MB then the drop). The refreshed repro (1 MB
  fifo + drain-keep-chain + heartbeat) never showed the old "reads
  stop forever" pattern — the pacing+trickle was its whole stall.
- **VERIFIED LIVE (2026-08-27)**: the fixed binary is deployed
  (md5-checked) and the user started the chain by hand — the client
  streams continuously. Both objectives are closed. The chain is UP on
  the camera right now; auto-start at boot is still opt-in via the
  auto-rtsp flag.
- **Offline repro refreshed (2026-08-27)**: the tree's
  `src/ByteStreamFifoSource.cpp` had been byte-identical to upstream —
  missing the heartbeat poll, 1 MB F_SETPIPE_SZ growth, drain-keep-
  chain, and tee. `build-rtspserver-native.sh` now applies
  `tools/vendor/` first (same as the armv6 script), sed-patches the
  fifo path to `/tmp/h264_fifo2` (native-only; the camera's path is
  held by a stale root-owned WSL process), builds with `-O0 -g`, and
  no longer uses `-j` (the link raced libliveMedia.a). Rebuilt and
  verified: vendor copy matches, F2F_TEE compiled in. Repro recipe:
  `mkfifo /tmp/h264_fifo2`, a python feeder looping `/tmp/feed.h264`
  (single open, 4 KB chunks — no writer gaps), server with `-d 4`,
  capture with ffmpeg. Next step: strace/gdb the parse chain at the
  moment it stops requesting.

## Audio (future work, status 2026-08-27)

The live stream is VIDEO-ONLY: the server logs "Audio fifo does not
exist, disabling audio" at startup and serves a video-only SDP. The
upstream server's audio path is ready and gated on a fifo:
`ADTSAudioFifoSource` reading **`/tmp/aac_audio_fifo`** (ADTS-framed
AAC), added as a substream of the same session. Nothing writes that
fifo — our producer filters the ring for hi-res video only.

The audio data IS in the ring: ~198 `0x0100` records per lap (raw AAC
payloads, no ADTS headers, tail `00 00 ff f9`). The path to audio:

1. A second fshare2fifo mode filtering `0x0100`, extracting the AAC
   payloads, wrapping each in an ADTS header (needs the AAC sample
   rate/profile/channel config — the ring's audio chain records would
   tell us), and writing `/tmp/aac_audio_fifo`. The server's audio
   substream then lights up automatically.
2. Verify the client muxes the interleaved audio track cleanly.

Unknowns: AAC sample rate/channel config, whether the audio records
carry their own codec-config chain (the era90 snapshot's 0x0100
records have no chain-parts analyzed yet).

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
