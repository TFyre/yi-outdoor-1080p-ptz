# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

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
  this order: **rRTSPServer first** (its startup drain discards buffered
  fifo content, so it must open the fifo before the producer writes), then
  **fshare2fifo** (retried until it survives). Idempotent. Limitation: if
  rmm restarts mid-uptime and recreates its buffer, the producer's mmap
  goes stale — re-run the script or reboot.
- `hack/bin/` — binaries, all static armv6 (gitignored, rebuilt with
  `tools/build-*.sh` in WSL): busybox 1.38.0, curl 8.21.0 (http-only, no
  TLS), dropbearmulti 2026.94 (built from source), fshare2fifo, rRTSPServer.
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
chain, and grows the fifo to 256 KB via F_SETPIPE_SZ on its own read end
(the producer's write-end call fails with EEXIST once other handles are
open).

**The walk (commit 9382405, replaces all earlier pacing)**: the producer
does NOT chase the frame counter (a ~45/s heartbeat, not a position) and
does NOT use a fixed byte gap (bitrate-blind: 192 KB ≈ 27 s of content at
an ultra-static ~7 KB/s scene — the observed 13 s latency). It emits a
NAL only when its END lies ≥ NAL length + 128 B before the writer's
position signal — provably complete, ~one frame old at ANY bitrate. The
writer's position comes from a 250 ms ring-diff sampler (shadow copy +
memcmp; the top of the newest changed run IS the write head in every
ring mode); the c0-checkpoint forward scan and the header slots
(0x04/0x08/0x0C/0x10, jitter-clamped) are the seed/fallback. A join (or
a MAX_LAG escape after a client stall) emits the newest chain then jumps
to just behind the head — the mid-GOP backlog is dropped, because the
fifo drains at the writer's own rate and a backlog can never be closed
by sprinting it (measured: sprint → lag equilibrium). There is no
time-based lap re-sync — it fired on sprint blocks and looped forever
(sprint → block ≥1.1 s → re-sync → sprint). Per-second diagnostics:
`F2F_AGELOG=1` logs pos/head/dist/emission/block stats.

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
base: `analysis/s*.bin` snapshots, `tools/ringdiff.py`. Note the ring
format has MODES: transient boot-time record streams (26/34-byte headers
with `6a 8a 33 f?` magics) and the steady-state raw Annex-B mode above.
The modes ALSO change mid-uptime: verified live that the c0 entries
vanish while the encoder keeps running, and the header slots freeze when
the writer reaches the buffer end — the ring-diff sampler in the
producer tracks the writer through all of it.

## Backup

Full stock backup taken (all MTD partitions + file-level tars of `/home` and `/backup`), md5-verified on both ends:
- On the SD card: `/tmp/sd/backup/mtd/`
- Local copy (gitignored via `backup/`): `backup/mtd/`

Keep stock-firmware dumps out of git.

## Upstream and prior work

- The camera is **unsupported** by upstream [alienatedsec/yi-hack-v5](https://github.com/alienatedsec/yi-hack-v5) (a Hi3518ev200 project, GPL-3.0; its "Yi Outdoor" support is the older h30 model, base fw 3.0.0.0D). Porting the hack to this Fullhan unit is the core challenge.
- [Issue #457](https://github.com/alienatedsec/yi-hack-v5/issues/457) (closed, not planned) documents prior recon of this exact unit. Baseline tried: release 0.4.1 (`yi_outdoor_0.4.1.tgz`), payload at `/tmp/sd/yi-hack-v5/` (binaries in `bin/`, busybox in `busybox/`). The SD also holds later experiments (mediamtx armv6, ffmpeg, h201c/r10m/cloud-dome tarballs).
