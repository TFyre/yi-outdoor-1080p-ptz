# PLAN — current state and next steps (updated 2026-09-01)

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

## Auto-start of the RTSP chain is OPT-IN — currently ENABLED (2026-08-27)

`hack/boot.sh` gates the `start-rtsp.sh` spawn behind a flag file:
create `/tmp/sd/hack/auto-rtsp` (any content) to bring the chain up at
boot; without it boot leaves the chain down, and it is started by hand
with `sh /tmp/sd/hack/start-rtsp.sh`. Deployed to the SD (md5-checked
match with `deploy/hack/boot.sh`). The user enabled the flag after the
live verification: the chain now comes up automatically at every boot
and the watcher keeps it up.

## Next item (DONE 2026-08-27): survive ring mode flips

Investigation first: all 52 ring snapshots in `analysis/` (s*.bin,
rs*.bin, ring_*.bin, era90, Aug 23-27) are RECORD-framed — magic
density 7,200-9,500 per ring, start-code and c0 counts at noise level.
Raw Annex-B mode has never been captured on this firmware; the pre-v2
raw-mode notes describe the pre-record era. A full raw NAL-walk
fallback would therefore be speculative.

What the magic-era flip (17af484) DID prove is the generic failure
mode: ring flowing, walk silent, no error. That is now guarded by the
stall watchdog (9549191): no record consumed for 15 s while HDR_VALID
moves -> re-anchor the cursor at the newest seq; four consecutive
re-anchors with no records -> exit and let start-rtsp.sh's watcher
respawn with a fresh claim. A static ring never triggers it. Built
ARMv6-clean, deployed (md5-checked), verified live (AGELOG dump:
cursor tracking, 16 nals/s, drops=0).

Note: the running chain keeps the old producer inode until the next
respawn/reboot; the watchdog binary activates on the next natural
restart (auto-rtsp is enabled, so the next boot brings it up).

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

## Scratch cleanup (2026-08-27)

The truncation fix (c77c753) landed 2026-08-27 10:00 — every recording
from Aug 26 and earlier predates it and carries the cabac-tail bug (and
earlier-era quirks). Cleaned out of `analysis/` and WSL `/tmp`: the 52
pre-era90 ring snapshots (the mode conclusion above was recorded
first), all pre-Aug-27 media captures (.h264/.ts), the old OSD-test
artifacts, and the old helper scripts. Kept: `cap60.h264` and
`ring-era90.bin` (today's clean captures), the build trees/toolchains,
stock-firmware material, docs, and the parallel audio session's files.
Impact: none of today's conclusions depend on the deleted files — the
stall mechanism (pacing + drop) is content-independent and was
re-verified with the post-fix feed2 + the live camera; the snapshots
were app-side ring ground truth, not walk output.

## Audio (IMPLEMENTED + verified offline 2026-08-27; camera run is user-driven)

The live stream was VIDEO-ONLY: the server logs "Audio fifo does not
exist, disabling audio" at startup and serves a video-only SDP. The
upstream server's audio path is ready and gated on a fifo:
`ADTSAudioFifoSource` reading **`/tmp/aac_audio_fifo`** (ADTS-framed
AAC), added as a substream of the same session. Nothing wrote that
fifo — the producer filtered the ring for hi-res video only.

**The unknowns resolved from the ring snapshots** (`tools/audio_probe.py`,
ring_n.bin + ring_now2.bin, 1203 audio records): every 0x0100 record's
payload is already ONE COMPLETE ADTS frame — the app writes MPEG-2 ADTS
directly, so the producer needs no re-framing, and there is no separate
codec-config chain (no 0x01xx records exist):

- `ff f9` syncword + MPEG-2 ID, protection absent; `60 40` = profile 1
  (AAC-LC), sampling_frequency_index 8 = **16 kHz**, channel_configuration
  **1 = mono**; the ADTS frame_length field equals the record length
  exactly (171/170 B); the payload ends in the encoder's zero padding
  (the old "tail `00 00 ff f9`" note was the 2-byte-shifted framing).
- Audio ts delta is a rock-steady ~65 units vs video's ~80 — at
  1024 samples/frame that pins the ts unit at ~1 ms, the audio rate at
  15.6 frames/s (64 ms/frame), and the record-era video of that
  snapshot at ~12.5 fps. 21.4 kbps total; a 256-frame capture decodes
  with 0 errors.

**Implementation (fshare2fifo audio mode, `src/fshare2fifo/fshare2fifo.c`):**
`-a` (or argv[0] = `f2f_audio`, the deployed copy's name — comm stays
short enough for pidof) switches the filter to 0x0100 and passes each
payload through whole to `/tmp/aac_audio_fifo` (whole-or-drop, purger
trim 16 KB ≈ 6 s of audio). No chain gate: the claim seeds cursor 0 and
the live-edge jump lands it live. The server needs NO changes — the
deployed binary already carries the audio path; it parses the ADTS
header for the SDP config ("1408" = AAC-LC 16 kHz mono), strips ADTS
per frame, and the vendored MultiFramedRTPSink pacing removal (560f6c3)
also un-paces the audio sink (the source's 64 ms/frame presentation
times keep the RTP timestamps right).

**start-rtsp.sh is now audio-first:** ADTSAudioFifoSource::createNew
sits in a syncword scan until the fifo has a writer, and with no writer
EVER the server start hangs forever — video included. Order: f2f_audio
up (or the fifo removed → the server's stat() check disables audio) →
rRTSPServer (no `-a no` anymore) → fshare2fifo. The watchdog respawns
f2f_audio too. Server restart while the producer is down unlinks the
fifo so the next start degrades to video-only instead of hanging.

**Verified offline end-to-end (WSL):** snapshot → x86 producer `-n -o`
→ 256-frame ADTS file (ffprobe: AAC-LC 16 kHz mono, 16.35 s, decode
0 errors); and the fifo path → native rRTSPServer `-r none` → the
source logged "Read first frame: profile 1, … 16000, channel_configuration 1",
"Config string: 1408" → an ffmpeg RTSP capture of ch0_2.h264 decodes
clean. NOT yet done (the user drives it): deploy `f2f_audio` + the new
start-rtsp.sh to the SD, restart the chain, and listen to
`rtsp://10.1.2.19/ch0_0.h264` (video+audio interleaved).

## Next workstream (RECON DONE 2026-08-27, implementation pending): PTZ control + web UI

The last core goals (RTSP ✓, SSH ✓, web UI ✗, PTZ ✗). Overnight recon
findings — binaries pulled to `analysis/app/` (gitignored):

- **No stock web UI exists** (listening ports: 554/2222/21/23/9999
  only). busybox httpd 1.38 is on the camera with CGI — the UI server
  is free.
- **The app's IPC is named POSIX mqueues**: `/ipc_dispatch`,
  `/ipc_dispatch_worker`, `/ipc_cloud`, `/ipc_p2p`, `/ipc_rmm`,
  `/ipc_rcd`, `/ipc_rtmp`, `/ipc_mdns` (dispatch strings; messages
  ≤512 B, depth 16, mode 0666 O_RDWR|O_CREAT|O_NONBLOCK).
- **The PTZ brain is `dispatch`** (120 KB, /home/app/dispatch, pid
  holds fh_pwm): cloud PTZ arrives via P2P (p2p_tnp) → cloudAPI → the
  `/ipc_dispatch` mqueue → `p2p_ptz_direction_ctrl` reads the cloud
  message's PTZ fields at offsets 561 (func char), 562, 582, normalizes
  the direction (mount-inversion swaps 1↔2 and 3↔4), and forwards a
  24-byte command on a second mqueue. The motor hardware path is a
  **UART to the CPLD motor board**: `uart_ptz_open` / `uart_com_init` /
  `uart_com_send_cmd` on `/dev/ttyS1`, plus `/dev/cpld_periph` ioctls
  (0x7004/0x700a/0x7025 seen). `get_ptz_position` + "save device ptz
  position(%d,%d)" → position persists in the mtd productinfo.
- **The command envelope matches upstream yi-hack-v5's ipc_cmd
  protocol** (same app family): MOVE = `{u32 1, u32 8, u16 0x4006,
  u16 0x4006, u32 24, u32 dir, u32 0}` with dir 1=up 2=down 3=left
  4=right; STOP = `{1, 8, 0x4007, 0x0001, 0}`; presets 0x4000/1/2,
  cruise 0x4003/4/5, jump 0x4009 (x/y at offsets 16/20), LED 0x76/77.
  Upstream sends these to the app's mqueue via their `ipc_cmd` tool.
- **Snapshot**: the app writes `/tmp/panorama_capture/%d.jpg` (a
  DISPATCH_SET_PANORAMA_CAPTURE_STATE command exists) — likely
  triggerable via the same mqueue; confirm the command id from
  dispatch's 0x40xx table.
- Factory helpers (read_dev/write_dev/read_gpio/write_gpio) are
  productinfo provisioning tools, not motor tools.

**Implementation plan:**
1. (With the user) strace `dispatch` during a phone-driven pan: confirm
   the exact target mqueue + message bytes end-to-end.
2. Write `ptz` — a ~100-line static tool: `mq_open` + `mq_send` of the
   MOVE/STOP envelopes (+ presets/jump). NO motor movement until the
   user is present.
3. Web UI: busybox httpd on 8080 from the SD + a CGI (upstream's
   ptz.sh pattern: `ptz -m <dir>; sleep 0.3; ptz -m stop`), PTZ pad +
   snapshot + RTSP link.
4. Deploy (SD additions only), verify with the user watching, wire
   into boot.sh if wanted.

**VERIFIED LIVE (2026-08-28) — no strace was needed:** the first fire
test on `/ipc_dispatch` with the upstream envelope (type 8) drove the
motor in ALL FOUR DIRECTIONS — the user fired up/down/left/right with
1 s holds + stops, all worked natively through dispatch's own motor
path. The `ptz` tool is deployed to `/tmp/sd/hack/bin/ptz`, the web UI
is live at **http://10.1.2.19:8080/** (busybox httpd, wired into
boot.sh idempotently, deployed md5-checked). Remaining polish: the
snapshot CGI — rmm has a venc JPEG capture path ("Capture JPEG file
to %s", /tmp/oss.jpg) whose trigger command id is not yet pinned; the
UI degrades gracefully without it.

## Workstream: ONVIF Profile S (DONE 2026-08-29 — video + 4-dir PTZ verified in HA)

Hand-rolled SOAP/XML-over-HTTP daemon (`src/onvif/onvif.c`, ~3 KB,
ARMv6-clean, port 8082, wired into boot.sh): device service
(GetDeviceInformation/Capabilities/Scopes/SystemDateAndTime/GetServices/
GetNetworkInterfaces — real wlan0 MAC), media (GetProfiles/GetStreamUri),
PTZ (ContinuousMove/Stop/SetPreset/GotoPreset/GetPresets/GetStatus/
GetNodes/GetNode/GetConfigurations/GetServiceCapabilities).
`rtsp://10.1.2.19/ch0_0.h264` serves in Home Assistant (zeep client).

**The HA bring-up found three real server bugs (all fixed + committed):**

- `e436d9d` — GetProfiles element order: VideoEncoderConfiguration must
  follow AudioSourceConfiguration (zeep validates schema order; HA
  reported "ContinuousMove not supported").
- `6ff6e3b` — velocity tie-break prefers tilt on equal magnitudes (this
  camera's vertical axis is inverted: 1=down 2=up).
- `4e8e50f` — the left/right-arrows-do-nothing debugging round:
  1. **loop-read** — zeep's requests arrive in multiple TCP segments;
     a single read() cut the SOAP body mid-attribute (`PanTilt x="...`)
     which parsed as zero velocity. `read_request()` now loops until
     Content-Length is satisfied; SO_RCVTIMEO 5 s per client socket.
  2. **xml_attr static-buffer clobber** — `xml_attr` returns a pointer
     into its own `static char val[64]`; `x` was strtof'd after `y`'s
     extraction overwrote it, so BOTH axes read y's value — up/down
     looked diagonal, left/right looked zero. x is now converted before
     y is extracted.
  3. Raw PTZ request bodies are logged (RAW BEGIN/END markers in
     /tmp/onvif.log) — kept for future debugging.

**Diagonals: NOT supported by the app.** Confirmed in dispatch/rmm
disassembly: the MOVE envelope carries ONE scalar dir (1-4); dispatch
swaps pairs for the mount-flip settings and forwards the raw value,
rmm forwards it unchanged (the envelope's last u32 is always 0 — a
dead field). No 5-8 diagonal values exist anywhere in the path. The
stock app's own pad is 4-way too. A diagonal request is realized as
the dominant axis (ONVIF-legal: the device decides).

**Known gaps (backlog, user picks):**
1. WS-Discovery UDP responder — HA/ODM auto-discovery instead of
   manual add.
2. GetStatus real position values — dispatch has `get_ptz_position` +
   "save device ptz position(%d,%d)"; the read-back command is not yet
   RE'd (currently a static 0/0 response).
3. Snapshot: rmm venc JPEG trigger command id (see PTZ section) — would
   also enable GetSnapshotUri.

## Workstream: flashlight / white LED (DONE 2026-09-01 — auto/on/off via the app's own command)

**Ground truth captured from the app itself** (the experiment): the
phone app's lamp setting arrives as p2p `IOTYPE_USER_IPCAM_SET_WHITE_LED_MODE`
→ dispatch logs `set DISPATCH_SET_WHITE_LED_MODE, N` and runs
`save_config → write_mtd_conf` (persisted to flash). The app's values:
**1 = on, 0 = off, 2 = auto**. The envelope is `{1, 8, 0x009c, 0x0001,
4, N}` (cmd 0x9c = DISPATCH_SET_WHITE_LED_MODE): it stores the mode in
the day/night judge's config field (cfg+0x19c0) AND persists it; mode 1
also fires `ioctl(/dev/cpld_periph, 0x7021)` immediately.

**The dead end that preceded it**: the FACTORY variant (cmd 0xa2,
`FACTORY_SET_WHITE_LED_MODE`) fires the same ioctls (0x7021/0x7022)
but stores NOTHING — rmm's judge (`is_day_night_judge`, logs
"white_led_light is light set day_mode" every second while managing)
re-applies its stored state over the forced ioctl, which was the whole
"intermittent / works for a while then flips back" behavior. The
momentary toggles cmd 0x76/0x77 (ioctl 0x701c/0x701b) are the app's
live-view flashlight button — kept in the tool as `lighton`/`lightoff`.

**ioctl map on /dev/cpld_periph** (fd slot +40 of dispatch's global
struct, verified via /proc/PID/mem): 0x701c = on, 0x701b = off,
0x7021 = on, 0x7022 = off, 0x7028 = off (the power-off sequence fires
0x701b+0x7022+0x7028). **0x7019 HANGS the kernel path and the camera
watchdog-reboots — never fire it** (the 0x7018/0x7019 pair is some
wait/off sequence the app only runs in its own state).

**Observability win**: the app writes rotating logs (~100 KB each) to
`/tmp/sd/log/log1..5.txt` — every p2p command, dispatch handler, and
the judge's decisions with timestamps. This is how the app's exact
envelopes were captured; grep for "WHITE_LED" / "white_led".

**Delivery**: `ptz light on|off|auto` (and numeric `lightmode N`,
toggles `lighton`/`lightoff`), CGI `light=on|off|auto` on the web pad
(three buttons). Because the command is the app's own, the state is
stable AND the YI app shows the change after its next config sync.

## Workstream: block the cloud (RECON + IMPLEMENTED 2026-09-01 — activation is user-driven)

Recon done, mechanism implemented and deployed INERT; nothing is
active until the user creates the flag and reboots.

**Recon findings (all verified live 2026-09-01):**

- `cloud` (59 KB, /home/app) is the resident cloud client; it execs
  ONE-SHOT `/home/app/cloudAPI -c <cmd> -url ...` processes per webapi
  call (log: `cloud.c(webapi_do_event_upload-3218)
  cmd=/home/app/cloudAPI -c 411 -url ...`). Observed cmd ids: 411
  (event upload), 304/306 (event update), 136/138/141/142, 422.
- `watch_process` (9.5 KB) checks every 10 s and re-runs each watched
  process's cmd from `/home/app/wp_cmd` (ro squashfs): dispatch→restart,
  cloud→restart, rmm→REBOOT, p2p_tnp→restart, mp4record→restart,
  oss→restart. It links `/dev/pid_list` (pid_list.ko misc device) —
  cloud/p2p_tnp/watch_process all hold it open — and logs "X crashed!"
  then runs the wp_cmd cmd. Killing cloud is PROVEN safe: the logs show
  cloud+p2p_tnp "crashed!" several times historically (8/31 14:39,
  9/1 8:08, 9/1 8:50) with only a restart, never a reboot, and no app
  degradation. `/home/app/localbin/reboot_by_watchdog` exists (the
  rmm-death path).
- **No iptables in the kernel** (`/proc/net/ip_tables_names` missing) —
  mechanism 3 dead. Kernel is **4.9.129**; file bind mounts work
  (tested live with an identical wp_cmd copy, self-reversing).
- The whole observed cloud surface is 4 hostnames:
  `plt-api-de.xiaoyi.com` (API), `plt-api.xiaoyi.com` (fallback),
  `log.eu.xiaoyi.com` (log uploads), `motiondetection-eu-1d.oss-eu-
  central-1.aliyuncs.com` (event uploads). `/tmp/mmap.info` (tmpfs,
  shared config) holds the API + log URLs.
- Clock: `/dev/rtc` exists but hwclock reads 2059 (no battery), so the
  app syncs time in software (rmm has `settimeofday` +
  `send_set_time_msg`; cloud/p2p push it). Router 10.1.2.254 serves no
  NTP. Blocking cloud therefore loses the clock → replaced by busybox
  ntpd (the deployed /bin/busybox1.36.1 is busybox 1.38.0 full applets,
  incl. ntpd/rdate).
- cloud/cloudAPI/p2p_tnp are UPX-packed → the hardcoded-IP check is
  still open (see the UPX backlog item below). Observed behavior is all
  DNS-based (log lines carry hostnames), so hosts sinking should cover;
  the acceptance test confirmed it.

**Mechanism (implemented + deployed 2026-09-01, INERT until flagged):**

`deploy/hack/boot.sh` section 7, gated on `/tmp/sd/hack/block-cloud`:
1. `killall watch_process`, then kill `cloud cloudAPI p2p_tnp`.
2. Bind-mount `$HACK/etc/wp_cmd.block` (the wp_cmd without the
   cloud/p2p_tnp entries; dispatch/rmm/mp4record/oss kept, **rmm→reboot
   preserved**) over `/home/app/wp_cmd`, then restart watch_process on
   the shadow — the respawn is neutralized, the self-recovery is not.
3. Append the 4 hostnames → 127.0.0.1 to the ramdisk /etc/hosts
   (belt-and-braces: covers cloudAPI processes anyone else execs,
   e.g. log_server's log uploads).
4. Start `ntpd -p pool.ntp.org -p time.cloudflare.com` (clock
   replacement; retries on its own until wlan is up).
All idempotent (mounts/hosts/pidof guards); un-flag + reboot = stock.

Deployed to the SD (md5-checked: boot.sh d17b4076, wp_cmd.block
0393299c). Flag file NOT created.

**ACTIVATED + VERIFIED LIVE (2026-09-01):** the user created the flag
and rebooted (~12:54). All checks pass: of the blocked set only
watch_process runs (no cloud/p2p_tnp/cloudAPI — zero cloudAPI execs
this boot); /home/app/wp_cmd shows the shadow via the vfat bind mount;
marker + hosts sink present; clock synced by ntpd; netstat shows only
LAN clients (2× RTSP 554, ssh, telnet). The only xiaoyi refs in the
post-boot logs are two `[./dispatch]` config echoes at boot+15 s
(epoch-time, pre-sink; no traffic). Found + fixed a boot.sh
idempotency bug: busybox applets (ntpd/httpd) carry comm
`busybox1.36.1`, so `pidof` never matches them — both guards now use
`ps | grep -q "[n]tpd"`-style patterns (fixed boot.sh eefd99a8
deployed; a live section re-run left exactly one ntpd/httpd/
watch_process). A second guard fix followed (adca5b42): the mount
guard must match the /proc/mounts TARGET (` /home/app/wp_cmd `), not
the source path — the old pattern never matched and would stack a
duplicate bind on every re-run. The 43-min follow-up check (13:38)
was all green. **Hand-checks by the user (2026-09-01, all pass):**
RTSP playback ✓, PTZ ✓, flashlight ✓, web pad ✓, YI app shows the
camera offline ✓ (by design). Final battery (13:16, 21 min up): 0
cloud/p2p/cloudAPI processes, exactly one watch_process + one ntpd,
chain up, no external connections, and a functional sink proof —
`wget http://plt-api-de.xiaoyi.com/` from the camera resolves to
127.0.0.1 and refuses. The only post-boot cloud refs remain the two
dispatch config echoes. Un-flag + reboot restores stock. The ffplay
latency recipes live in CLAUDE.md's RTSP section.

## Backlog item: UPX unpack + hardcoded-IP sweep (needs download approval)

cloud/cloudAPI/p2p_tnp are UPX-packed, so their hostnames and IP
literals were never swept directly (everything observed came from the
app logs + /tmp/mmap.info). To close the residual: download upx
v4.2.4 (static Linux build from the github.com/upx/upx releases — the
permission classifier blocks external executable downloads without
explicit user sign-off; the user can grant it in settings or approve
when asked), unpack copies in `analysis/app/`, re-run
`analysis/strings-sweep.sh` on them, and check for hardcoded IPs that
would bypass the /etc/hosts sink. Low priority: the kill-based block
makes respawns impossible, and 43 min of live observation shows zero
traffic.

## Repo reorganization for open-sourcing (EXECUTED 2026-09-01; publication user-gated)

Decisions taken: **GPL-3.0-or-later**; keep the real author identity on
commits (already used on other projects); **no history squash** — the
113-commit RE narrative is published as-is, with a surgical filter-repo
pass (below) instead of a rewrite.

**Done:**
1. `LICENSE` (canonical GPL-3.0 text) + SPDX headers on all `src/`,
   `tools/`, and `deploy/hack/` sources (`tools/vendor/` stays LGPL-3.0+
   with live555's own headers; README notes the mix).
2. `README.md` — what/why, device facts, features, build + install,
   a **Related projects** section (yi-hack-v5 + issue #457, the three
   Allwinner ports, MStar port, live555) that absorbed the unique links
   from `docs/plan.md`, license section, brick disclaimer.
3. `deploy/hack/root/.ssh/authorized_keys` → `authorized_keys.example`
   (real key gitignored); `boot.sh` installs the key only if the file
   exists (a fresh clone just boots with ssh auth off).
4. Moves out of `analysis/`: `fshare-protocol.md` → `docs/`,
   `audio_probe.py` / `verify_fshare_map.py` / `slotwatch.c` → `tools/`;
   all internal references updated; the analysis/ gitignore exceptions
   removed. `docs/plan.md` (early-phase roadmap, superseded) deleted.
5. `src/onvif/onvif.c`: the hardcoded 10.1.2.19 is gone — `mk_ip()`
   reads wlan0 via `SIOCGIFADDR`, refreshed per request (wlan0 may not
   be up when the daemon starts, and DHCP can move it). Built
   ARMv6-clean. **Deploying the new onvif to the SD is user-driven.**
6. `.gitignore`: `deploy/hack/root/.ssh/authorized_keys`, root
   `/*.h264` and `/*.aac` strays.

**Pre-publication audit (done 2026-09-01):** the whole history (113
commits, all blobs) contains no firmware dumps, ring snapshots,
captures, logs, credentials, SSIDs, or private keys — the file
inventory is today's tree plus 3 deleted innocuous scripts. The only
personal items ever in git: the pubkey (handled above) and the author
line (intentional).

**Remaining (user-gated):**
1. filter-repo pass, once, before the first push: `--replace-text` the
   pubkey line → placeholder (removes it from every historical blob)
   and inject `LICENSE` into the ROOT commit's tree (all descendants
   inherit it, so every snapshot in the published history is
   license-clean).
2. Create the GitHub repo (empty, no auto-init) and push once.
   No force-push is ever needed after that.

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
