# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**Active work plan: read `PLAN.md` first** (updated 2026-08-27). Standing rules there: never reboot the camera; do NOT auto-start fshare2fifo/rRTSPServer; do NOT auto-verify video; the user drives each step.

## Project

Firmware modification project for the **YI Outdoor 1080p PTZ** camera (yitechnology.com/outdoor-ptz): a yi-hack-v5-style mod — RTSP, SSH, web UI, PTZ control — that runs from a FAT32 microSD card and leaves the stock flash untouched.

The repository is tooling + docs so far (see `tools/`); there is no build system or test suite yet.

## Commits

Use Conventional Commits: `type(scope): description` (e.g. `feat(rtsp): …`, `fix: …`, `docs: …`, `chore: …`).

## Device facts (verified on the unit)

- SoC: Fullhan **FH8626V100** — single ARMv6 core, ~35 MB RAM (MemTotal: 35904 kB). Not the Allwinner `r40ga` and not the Hi3518ev200 `h30ga` outdoor models; do not mix them up.
- CPU: **ARM1176** (part 0xb76) with **VFPv2** — full single AND double precision, d0–d15 (verified on the unit: `tools/probe-vfp.c` runs `flds/fadds/fldd/faddd/vldmia {d8-d15}` fine, even as a process's first VFP instruction; `movw` SIGILLs as expected). What SIGILLs: ARMv7 integer ops (`udiv/sdiv/movw/movt/ubfx/sbfx/bfi`), VFPv3-only forms (`vmov.f64` immediate, d16–d31 multiples, NEON `vld1.64`). `tools/scan-v7.sh` flags exactly those.
- Board suffix `b221fp`; stock firmware `5.0.00.00_202204281015` (`/home/homever`, `/home/app/.appver`).
- SPI NOR flash, 64k erase size, per the kernel cmdline (`console=ttyS0,115200` — UART is the unbrick path):

| MTD | Size | Name | Notes |
|-----|------|------|-------|
| mtd0 | 64k | bootstrap | |
| mtd1 | 64k | uboot-env | |
| mtd2 | 256k | uboot | |
| mtd3 | 2304k | kernel | includes the rootfs initramfs — rootfs is NOT a separate partition (17.2M ramdisk, rw but lost on reboot) |
| mtd4 | 3840k | home | squashfs, ro at `/home` — **100% full, never write to /home** |
| mtd5 | 1536k | backup | jffs2, rw at `/backup` |
| mtd6 | 64k | mfc | |
| mtd7 | 64k | conf | |

- `/tmp` is a 32M tmpfs; SD card `/dev/mmcblk0` mounts at `/tmp/sd` (vfat — no unix permissions: anything that chmods/renames files on the SD fails with EPERM).
- Stock busybox is 1.26.2 and lacks many applets (`id`, `wc`, `head`, `uname`, `base64` all missing). Dynamically linked binaries from the yi-hack-v5 release fail on this unit (`rRTSPServer`: missing `libstdc++.so.6`; `curl`: missing `libssl.so.1.1`); self-contained binaries (busybox 1.36.1, dropbearmulti 2018.76) run fine.

## Camera access

- **Telnet (bootstrap)**: `debug.sh` on the SD card root runs at boot. Connect: `telnet 10.1.2.19 9999` (no password). Non-interactive: `bash tools/camcmd.sh "command"`.
- **SSH (preferred)**: dropbear on 2222, started by boot.sh at boot. The host key persists on the SD at `/tmp/sd/hack/etc/dropbear/ecdsa.key` (stable across reboots, so host-key pinning works). dropbearkey cannot write to vfat directly (its chmod fails), so boot.sh generates in tmpfs and copies with `cat` when the key is missing. The listener writes `/tmp/dropbear.pid`; boot.sh skips starting if that pid is alive (idempotent re-runs).
  Client side (this machine):
  ```
  ssh -i ~/.ssh/yi_cam_rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o StrictHostKeyChecking=accept-new -p 2222 root@10.1.2.19
  scp -O -P 2222 -i ~/.ssh/yi_cam_rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o StrictHostKeyChecking=accept-new root@10.1.2.19:<file> <dest>
  ```
  `-O` forces legacy SCP protocol — dropbear has no sftp subsystem, so there is no sftp-server. Client key: `~/.ssh/yi_cam_rsa` (pubkey installed as `/root/.ssh/authorized_keys` on the camera).

## SD hack package (`/tmp/sd/hack/`)

The whole mod lives on the SD card; stock flash stays untouched. `debug.sh`
on the SD root is a one-liner that execs `hack/boot.sh` at every boot.

- `hack/boot.sh` — orchestrator (idempotent, fast): installs the static
  busybox (full applets) into `/bin` (ramdisk, redone each boot), starts
  telnetd 9999 + dropbear 2222 (pidfile-guarded), creates the
  dropbearmulti applet symlinks `/bin/scp` and `/bin/dbclient` (client-side
  `scp -O` execs a remote `scp -t`, which must resolve to something), then
  spawns `start-rtsp.sh` detached (the stock boot may wait for debug.sh to
  return).
- `hack/start-rtsp.sh` — waits for `/dev/shm/fshare_frame_buf` (the app
  creates it once the camera pipeline is up), then starts the chain in
  this order: **f2f_audio first** (ADTSAudioFifoSource::createNew sits in
  a syncword scan until the audio fifo has a writer, and with no writer
  EVER the server start hangs — video included; if the producer never
  comes up the script unlinks the fifo so the server's stat() check
  disables audio cleanly), then **rRTSPServer** (no `-a no` — audio is on
  and gated by the fifo's existence; its startup drain discards buffered
  fifo content), then **fshare2fifo** (retried until it survives).
  Idempotent. Then a watch loop runs for the whole uptime: it respawns
  either producer (sitting ducks for the OOM killer) or the server (dies
  when a fifo briefly loses its writer) whenever one disappears; a failed
  audio respawn unlinks the audio fifo so the next server start degrades
  to video-only instead of hanging. Limitation: if rmm restarts
  mid-uptime and recreates its buffer, the producers' mmaps go stale —
  re-run the script or reboot.
- `hack/bin/` — binaries, all static armv6 (gitignored, rebuilt with
  `tools/build-*.sh` in WSL): busybox 1.38.0, curl 8.21.0 (http-only, no
  TLS), dropbearmulti 2026.94 (built from source), fshare2fifo, f2f_audio
  (the same binary under a name whose comm fits pidof; argv[0] dispatch
  turns on audio mode), rRTSPServer.
- `hack/root/.ssh/authorized_keys` — pubkeys boot.sh installs to
  `/root/.ssh/` each boot (the ramdisk resets).
- `hack/etc/dropbear/ecdsa.key` — host key (gitignored; boot.sh regenerates
  onto the SD if missing).
- Repo layout: `deploy/hack/` mirrors the SD folder. Deploying: tar it up,
  scp to `/tmp`, untar in tmpfs, `cp -r` onto the SD (vfat: no chmod, no
  symlinks — tar extraction onto vfat works but modes are ignored; scripts
  are invoked as `sh <script>`).

Mediamtx and sftp-server were deliberately not included: LIVE555 already
serves RTSP (mediamtx would add ~10 MB to a 35 MB-RAM device), and dropbear
has no sftp subsystem (`scp -O` covers transfers). Revisit if HLS/WebRTC is
needed.

## RTSP streaming

The live H.264 stream is exported by the stock app via the shared buffer
`/dev/shm/fshare_frame_buf` (ring; frame counter at offset 0x18; NAL-chained
frames). The working chain: `fshare2fifo` (reads the ring, writes
`/tmp/h264_high_fifo`) → `rRTSPServer` (LIVE555, port 554) →
`rtsp://10.1.2.19/ch0_0.h264`.

Binaries are static musl soft-float ARMv6 builds (ARM1176-compatible) produced
by `tools/build-armv6.sh` in WSL. Prebuilt binaries from every other
toolchain SIGILL here: ARMv7 instructions (musl.cc armhf libs), VFPv3-D16
hard-float (Ubuntu armhf gcc), or MIPS (RTS3903N package). Also: musl's
64-bit time_t breaks live555's `%ld` SDP printf — the build script
sed-fixes it; the fifo EAGAIN fix lives in `tools/vendor/`.

The chain is reboot-persistent: `hack/start-rtsp.sh` brings it up at every
boot (spawned by `hack/boot.sh`). Restart it manually with
`sh /tmp/sd/hack/start-rtsp.sh`.

**Join quality**: fshare2fifo waits for a complete SPS→PPS→IDR chain
before emitting, converts the ring's 3-byte NAL start codes to **4-byte**
(LIVE555's framer discards input until it sees `00 00 00 01` and would
otherwise sync on accidental alignments), and the fifo unlock thread must
never `read()` from the fifo (an early version ate the first 1024 bytes =
the stream head). The server side (vendored
`tools/vendor/ByteStreamFifoSource.{hh,cpp}`) drains the fifo at open
keeping the tail from the last complete chain, waits up to ~3 s for a
chain, and grows the fifo to 1 MB via F_SETPIPE_SZ on its own read end
(the producer's pipe probe does the actual sizing before any writer
opens; both set the same size).

**The walk (commit 9382405, replaces all earlier pacing)**: the producer
does NOT chase the frame counter (a ~45/s heartbeat, not a position) and
does NOT use a fixed byte gap (bitrate-blind: 192 KB ≈ 27 s of content at
an ultra-static ~7 KB/s scene — the observed 13 s latency). It emits a
NAL only when its END lies ≥ NAL length + 128 B before the writer's
position signal — provably complete, ~one frame old at ANY bitrate. The
writer's position comes from a 250 ms ring-diff sampler (a 256 KB moving
shadow window, tear-checked — see below; the top of the newest changed
run IS the write head in every ring mode); the c0-checkpoint forward
scan and the header slots (0x04/0x08/0x0C/0x10, jitter-clamped) are the
seed/fallback. A join (or a MAX_LAG escape after a client stall) emits
the newest chain then jumps to just behind the head — the mid-GOP
backlog is dropped, because the fifo drains at the writer's own rate
and a backlog can never be closed by sprinting it (measured: sprint →
lag equilibrium). There is no time-based lap re-sync — it fired on
sprint blocks and looped forever (sprint → block ≥1.1 s → re-sync →
sprint). Per-second diagnostics:
`F2F_AGELOG=1` logs pos/head/dist/emission/block stats.

**The fshare protocol (fully reverse-engineered 2026-08-25/26, commits
57e4e2d/b65159d — the authoritative spec is `analysis/fshare-protocol.md`)**: the
ring is a sequence-numbered RECORD LOG, not a byte stream. The mapping is a
300-byte header + a 0x1B4000-byte ring: 0x00 active-reader count, 0x04 valid
bytes, 0x0C head = (tail + valid) mod ring (derived), 0x10 tail, 0x14 newest
record ts, 0x18 newest seq, then `reader_slot[17]` (stride 16: +0 pending,
+4 waiting, +8 cursor, +12 filter). Records: 26-byte header (len@+0, seq@+4,
magic@+8 — the two-byte shift that made the old +6 scans see "zero records"
and invent the raw-Annex-B era — ts@+16, type@+20, chain-part@+22) + payload.
Types: 0x0100 audio, 0x0400 hi-res frame, 0x0800 low-res, chain parts
0x0422/0x0401/0x0404 (hi) and 0x08xx (lo). The 0x0401 record carries the
WHOLE GOP head (SPS+PPS+~20 zeros+IDR) in one payload; SPS/PPS NALs need
stop-bit trims (the app glues metadata after the rbsp_stop). The writer
publishes seq/valid/head BEFORE copying the record bytes, one record at a
time; it posts a slot's notify only when that slot's reader is parked.

**fshare2fifo is v2 (commit d34df8c)**: a registered reader (slot 10, filter
0x0400), polling `pending`, walking records by seq, emitting only the hi-res
stream. **LOCK-FREE — NEVER take the app's semaphores**: hand-rolled futex
interop against the uClibc-layout sem words wedged the camera's write lock
twice (the app's wake-then-inc post order vs ours corrupts the {0,1}
invariant), leaving the lock held by nobody and stalling rmm's whole
pipeline (H264_CB stuck in `down`; a reboot is the recovery). The lock-free
walk is correct by the doc's own analysis: records with seq < hdr[0x18] are
complete by construction; magic/stride validation stops the walk on anything
torn; slot writes race benignly (the writer never reads cursors). The whole
v1 machinery (diff sampler, shadows, tear checks, head estimation, MAX_LAG,
re-joins, start-code ring scanning) is deleted — ~950 lines vs the walk's
~3000.

**Ring format (reverse-engineered from live captures, commit 1677010)**:
the ring carries TWO interleaved H.264 streams — the target 1920×1088
(SPS level 0x29, P-slices `41 9a 00 …`, IDRs `65 88 80 …`) and a second
stream (SPS level 0x16; its P-slices share the `9a 00` shape and differ
only in the pic_order_cnt byte: 0x91 vs 0x90) — plus `00 00 01 c0` table
entries (30-byte headers with the app frame counter) and raw low-res
frames without start codes. Mixing both streams corrupts every P frame.
The producer therefore: two-pass joins (learn the largest stream's dims,
then pick its nearest complete SPS→PPS→IDR chain), gates IDR/PPS on the
chain state, learns the target stream's pic_order_cnt byte from the first
slice after the chain and emits only matching slices, tracks the write
head via the counter-validated c0 entries, and detects ring laps by the
frame-counter delta across blocking fifo writes (the ring laps every ~4 s
at ~440 KB/s; positional re-gates churn against that). Measured live:
~1 partial-frame decode error per 10–13 s (source-side), stable 15 fps
1080p — was full-frame concealment on nearly every P frame. Forensics
base: `analysis/s*.bin` snapshots, `tools/ringdiff.py`.

**The record-framing (reverse-engineered from live snapshots 2026-08-24,
commits f02fa63/c8437c4 — the producer's PRIMARY mode)**: the ring
switches mid-uptime between the raw Annex-B mode above and a
RECORD-framed mode whose every frame is a 24-byte header + a 4-byte-SC
NAL (the magic's low byte is a writer sequence, not a class — classify
by the TYPE):

- header: `[0:2] u16 | [2:6] counter u32 (+1 per record, strictly
  monotonic) | [6:10] magic u32 (mask 0xffff0000 == 0x6a8c0000) |
  [10:14] u32 | [14:18] ts u32 | [18:20] type u16 | [20:22] sub u16`
- types: **0x0400 hi-res frame / 0x0800 low-res frame / 0x0100 audio
  (raw AAC, no SC, tail `00 00 ff f9`)** — the Allwinner flags — and
  the chain parts **0x0422 SPS#1 (SC at +30, after an 8-byte prefix) /
  0x0401 SPS#2 / 0x0404 PPS (SC at +24)** (0x08xx = the low-res chain)
- the IDR is NOT record-framed: after the SPS#2 record's NAL it follows
  RAW (PPS + ~20 zeros + IDR) until the next record — chains every
  ~78 records
- the record walk (drain_record_round) emits only 0x0400 + the 0x04xx
  chain parts with EXACT boundaries (successor record = the slice end);
  the successor's existence IS the completeness proof, and the
  successor-less newest record IS the write head (walk-driven — no c0
  slots, no diff sampler, no MAX_LAG false fires). The raw Annex-B/c0
  mode remains the fallback when records go stale (5 s); the NAL walk
  there trims the next record's 25-byte header from slice ends.
- **The record counter is the only reliable clock**: the 0x18 frame
  counter races (~45–300/s) and wraps BACKWARD mid-era; every
  freshness/ordering check must use the record counter + wall clock.
  The app RESETS the counter era mid-uptime (126K→10K and smaller
  drops); rec_head_adopt re-anchors the walk on a >10k drop.
- **The fifo must be 1 MB** (chains are 300–500 KB at IR-noise
  bitrates; a 256 KB fifo wedged the chain writes in EAGAIN forever).
  The producer's pipe probe must keep its read fd open until the write
  end opens — otherwise the pipe inode dies with the probe's close and
  the capacity reverts to 64 KB. `copy_nal` must handle a start-code
  view at buf_size-1 (the chunked copy; the old two-memcpy version
  computed a negative chunk length = one corrupt slice per lap).
- Offline repro: copy a ring snapshot to `/dev/shm/fshare_frame_buf`
  in WSL and run the x86 build (`gcc -O0 -g` of the same source) —
  `-o out.h264` dumps the walk's exact emission for decode/diff.

Note the ring format has MODES: transient boot-time record streams
(26/34-byte headers with `6a 8a 33 f?` magics — an older record era)
and the steady-state raw Annex-B mode above. The modes ALSO change
mid-uptime: verified live that the c0 entries vanish while the encoder
keeps running, and the header slots freeze when the writer reaches the
buffer end — the ring-diff sampler in the producer tracks the writer
through all of it (legacy; the record walk no longer needs it).

**The delivery layer is exonerated (2026-08-25)**: with F2F_TEE set,
the vendored ByteStreamFifoSource writes every byte it serves to a
file — decoded locally, the server's exact input showed the same error
signatures as the client (identical MB/bytestream lines), so the fifo
reads, framer, RTP, and TCP add zero corruption. An era-matched
file-mode dump of the producer's own walk decoded 481 frames with 0
errors — when the ring is clean, the chain is clean; the residual
decode errors are the app's own stressed-era output (the hyperactive
encoders), not the chain.

**Tear protection (commits 49e04b7/3f44cf9)**: the sampler's shadow
copies are double-copy tear-checked (bounded retries — the unbounded
version livelocked when the writer stayed inside the copy window), and
emit_nal re-reads its copy and drops a torn NAL as a concealable gap.
Before this, a torn shadow refresh seeded a false head advance and the
walk emitted one ~21 KB pre/post-lap byte mix per ~2 s. The shadow is
a 256 KB moving window, not a full-ring copy: the 1.79 MB shadow made
the producer the OOM killer's victim (exit 137, silent, during the
app's bursts — dmesg on this unit doesn't log OOM kills).

**Server-side delivery fixes (commits 8a444e8/6f7b205)**: the fifo
subsession passed playTimePerFrame=50000 us, which made the RTP sink
pace EACH PACKET by 50 ms — 12 s stalls between reads, ffplay's frozen
single frame (zeroed). And a client that stops reading (paused player)
filled the TCP window; the blocking retry send wedged the
single-threaded event loop — one slow client froze the whole camera
(measured: ~56 KB stuck in the tx_queue, the fifo filling, the
producer blocking). The vendored RTPInterface now drops such a client's
RTP stream instead of blocking (RTSP_DROP_LOG=1 logs it); verified with
a raw-socket client that streams then stops reading.

**Audio (2026-08-27)**: the ring's 0x0100 records carry the audio, and
every payload is already ONE COMPLETE MPEG-2 ADTS frame — `ff f9` sync,
profile AAC-LC, **16 kHz, mono**, protection absent, and the ADTS
frame_length equals the record length exactly (171/170 B; 21.4 kbps,
15.6 frames/s, 64 ms/frame). No 0x01xx codec-config records exist; the
ADTS header is the whole config. So `fshare2fifo -a` (or the deployed
copy `f2f_audio`, argv[0]-dispatched) just passes 0x0100 payloads
through whole to `/tmp/aac_audio_fifo` (filter 0x0100, no chain gate,
cursor seeded 0 with the live-edge jump, purger trim 16 KB). The server
needs no changes: ADTSAudioFifoSource parses the ADTS header for the
SDP config ("1408"), strips it per frame, and MPEG4GenericRTPSink
payloads; the vendored MultiFramedRTPSink pacing removal also un-paces
the audio sink while the source's 64 ms presentation-time increments
keep the RTP timestamps right. `rtsp://10.1.2.19/ch0_2.h264` is the
audio-only stream; ch0_0/ch0_1 interleave it. Verified offline: the
walk's exact ADTS output decodes 0 errors, and a native server run fed
from the fifo logged "profile 1 … 16000 … channel_configuration 1" with
a clean ffmpeg RTSP capture. Offline repro for audio: `analysis/audio_probe.py`
(snapshot record stats) and a WSL x86 `fshare2fifo -a -n -o out.aac` on
a snapshot copied to `/dev/shm/fshare_frame_buf` (the -n budget must be
≤ the records available — on a static snapshot video -n hangs waiting
for records that never come).

**Known remaining issue**: in the hyperactive eras (~600 KB-1 MB/s
bursts; the ring laps every ~3 s) the NAL-mode walk still churns on
join/jump cycles (~1 re-join per 6 s) and the source's own output
carries decode errors in those eras. The 2026-08-25 fixes (the diff
sampler anchored to the 0x0C reservation frontier with re-seed + the
jitter clamp, and the 4-byte start-code scan) eliminated the measured
backward OSD jumps (3 per 60 s -> 0 in two 90 s runs) and raised the
emit ceiling to ~500 KB/s, but the churn and the low readable-frame
fraction remain. The record-mode walk handles the hyperactive era
cleanly (drops 0) but requires records; the NAL-era equivalent is
still the open hard case.

## Backup

Full stock backup taken (all MTD partitions + file-level tars of `/home` and `/backup`), md5-verified on both ends:
- On the SD card: `/tmp/sd/backup/mtd/`
- Local copy (gitignored via `backup/`): `backup/mtd/`

Keep stock-firmware dumps out of git.

## Upstream and prior work

- The camera is **unsupported** by upstream [alienatedsec/yi-hack-v5](https://github.com/alienatedsec/yi-hack-v5) (a Hi3518ev200 project, GPL-3.0; its "Yi Outdoor" support is the older h30 model, base fw 3.0.0.0D). Porting the hack to this Fullhan unit is the core challenge.
- [Issue #457](https://github.com/alienatedsec/yi-hack-v5/issues/457) (closed, not planned) documents prior recon of this exact unit. Baseline tried: release 0.4.1 (`yi_outdoor_0.4.1.tgz`), payload at `/tmp/sd/yi-hack-v5/` (binaries in `bin/`, busybox in `busybox/`). The SD also holds later experiments (mediamtx armv6, ffmpeg, h201c/r10m/cloud-dome tarballs).
