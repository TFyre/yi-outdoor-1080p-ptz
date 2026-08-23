# Project plan: RTSP/ONVIF for YI Outdoor 1080p PTZ (b221fp / FH8626V100)

**Goal:** use this camera in Home Assistant via RTSP/ONVIF, without destroying the
stock firmware. See CLAUDE.md for device facts and access methods.

## Constraints (verified on the unit)

- Kernel 4.9.129, **uclibc toolchain** — no glibc on the device. Every shipped
  binary must be static or uclibc-linked (explains all "can't load library"
  failures seen so far).
- ~35 MB RAM, single ARMv6 core, 2.3 MB kernel partition (kernel + initramfs).
- Media pipeline is kernel-module based (`media_process`/`isp`/`enc`/`jpeg`/`bgm`)
  behind `/dev/media_process` and `/dev/isp`. No V4L2.
- `/home` = squashfs (ro). **`/backup` = jffs2 (rw, persistent)** with `init.sh`,
  `ko/`, `sensor_lib/`, `tools/`.
- SD-card `debug.sh` runs at boot (current injection point).

## Strategy

The blocker is not packaging or the filesystem: nothing currently taps the
encoded H.264 stream. The stock kernel + app already encode continuously
(motion detection depends on it). Kernel replacement would discard the only
working part (app + Fullhan drivers) and needs the NDA'd Fullhan SDK — last
resort, not the next step.

## Options (ranked by effort → payoff)

1. **Find an existing RTSP path** in stock/available binaries: the
   `Yi-RTS3903N-RTSPServerV03.tar.gz` on the SD, `tserver`, `p2p_tnp`, `rmm`
   string scans, `/backup/init.sh` hooks.
2. **Tap the stream the app already makes**: redirect `mp4record`/cloud path
   to a fifo/tmpfs and serve via mediamtx (static armv6, already on the SD).
3. **Initramfs userspace patch** inside mtd3 (keep kernel + drivers + app,
   change userspace only). Requires UART recovery setup first.
4. **Kernel replacement** — realistically no: Fullhan SDK is NDA-bound, SoC is
   not in mainline; without the SDK the camera loses ISP tuning, WiFi, and the
   app.

## Roadmap

- **Phase 0 — read-only recon (DONE):** findings below.
  - The `Yi-RTS3903N-RTSPServerV03` package is built for the **MIPS**
    Realtek RTS3903N platform — its binaries can never run here. Its design
    (grabber → fifo → LIVE555 RTSP) is the blueprint.
  - All stock app binaries are ARM + uclibc; the yi-hack-v5 release
    rRTSPServer is ARM glibc (VFPv3-only → SIGILL) or musl-armhf (ARMv7 →
    SIGILL). Nothing prebuilt runs on this ARMv6+VFPv2 CPU; we must build.
  - **The stock app exports the live H.264 stream via
    `/dev/shm/fshare_frame_buf`** (1.7MB ring, frame counter at 0x18, NAL
    chaining, named semaphores). This is the producer-side answer.
  - mtd3 = raw ARM Image with an initramfs in an unidentified compression;
    extraction deferred (live rootfs is readable directly). `update.sh` not
    yet analyzed.
- **Phase 1 — clean SD package (DONE):** `/tmp/sd/hack/` on the SD card
  holds the whole mod — `bin/` (static armv6 busybox 1.36.1 full applets,
  dropbearmulti 2018.76 + persistent host key, fshare2fifo, rRTSPServer,
  curl http-only), `root/.ssh/authorized_keys`, `boot.sh` (busybox into
  /bin, telnetd 9999, dropbear 2222 via pidfile, spawns start-rtsp.sh) and
  `start-rtsp.sh` (waits for the app's frame buffer, then producer → fifo →
  server, idempotent). `debug.sh` on the SD root is now a one-liner exec'ing
  boot.sh. Builds via `tools/build-busybox.sh` / `tools/build-curl.sh`
  (WSL + musl.cc arm-linux-musleabi, soft-float static — see
  `tools/build-armv6.sh`). Idempotency verified by double-running boot.sh
  live. Reboot-tested twice: first reboot exposed that the frame buffer
  exists before the app writes frames (producer died with "no H.264 stream
  found") — fixed with a retry loop in start-rtsp.sh; second reboot came up
  fully unattended (producer retried 3×, then stream decoded immediately).
  **Deferred, with reasons:** mediamtx (LIVE555 works; a static Go armv6
  binary would add ~10 MB to a 35 MB-RAM device — revisit only if HLS/
  WebRTC is needed); sftp-server (dropbear has no sftp subsystem — `scp -O`
  covers file transfer).
- **Phase 2 — video out (DONE):** `src/fshare2fifo` (producer) →
  `/tmp/h264_high_fifo` → LIVE555 rRTSPServer (armv6 static musl build via
  `tools/build-armv6.sh`). **`rtsp://10.1.2.19/ch0_0.h264` streams the live
  camera** (ffmpeg-verified). Join quality hard-won, four stacked bugs:
  (1) producer now waits for a complete SPS→PPS→IDR chain before emitting;
  (2) emits **4-byte** start codes — LIVE555's framer discards input until
  it sees `00 00 00 01`, and the ring's 3-byte form made it sync on
  accidental alignments; (3) the server's startup drain now keeps the tail
  from the last complete chain (1 MB fifo via server-side F_SETPIPE_SZ;
  the producer's write-end call fails with EEXIST) and waits ~3 s for a
  chain when the window has none; (4) the fifo unlock thread no longer
  reads (it ate the first 1024 bytes = the stream head).
  **Remaining:** intermittent full-frame concealment bursts under 1×
  pacing (the ffplay case) and a multi-second end-to-end delay. Root
  causes found and being fixed:
  1. The ring carries **two interleaved 1080p streams** whose slices
     share the `9a 00` header shape AND the same frame numbers; their
     fn sequences jump when mixed (decoder "top block unavailable" +
     full-frame conceals). The c0 table's TYPE field (offset 24,
     0x0400 hi-res / 0x0800 low-res / 0x0100 audio) settles the split:
     every hi-res slice is >= 955 bytes, every low-res slice <= 475, so
     the producer emits slices >= 700 bytes ONLY (mains-only; the small
     slices are ALL the low-res stream's — the "twin slice" theory was
     reverted, it leaked 0x0800 slices when frame numbers collided).
     The pic_order_cnt byte and the frame-number ranges are both
     era-dependent and unusable as discriminators. ffplay 60 s:
     0 errors, 0 conceals.
  2. Lap-detection false positives under pacing: the server drains the
     1 MB fifo in ~2.3 s bursts, so every normal block tripped the
     1.2 s lap threshold → re-sync storm → the accumulating delay.
     Threshold raised to ~3 s (LAP_TICKS 130).
  3. The stream structure is era/mode-dependent (boot-time record
     streams with 26/34-byte headers and `6a 8a 33 f?` magics vs the
     steady-state raw Annex-B mode; P/B splits that vary per session).

  **Upstream code found (2026-08, user-supplied links) — the same fshare
  design across YI platforms:**
  - [roleoroleo/yi-hack-Allwinner-v2 rRTSPServer.h](https://github.com/roleoroleo/yi-hack-Allwinner-v2/blob/master/src/rRTSPServer/include/rRTSPServer.h#L35):
    `BUFFER_FILE /dev/shm/fshare_frame_buf`, read/write lock files
    (`fshare_read_lock`/`fshare_write_lock`), stream offsets 300/368
    (autodetect), and a **frame_header struct (20/22/24/26/28-byte
    variants) with `len`, `counter`, `time`, `type`, `stream_counter`**.
    The `type` flags: `0x0400` high-res video, `0x0800` low-res video,
    `0x0100` AAC audio, `0x0002` SPS, `0x0008` VPS — the stream
    discriminator our producer needs. Write index at buffer offset 16.
  - [BenjaminFaal/yi-hack-Allwinner imggrabber.c](https://github.com/BenjaminFaal/yi-hack-Allwinner/blob/master/src/snapshot/snapshot/imggrabber.c#L40):
    same buffer SIZE (1,786,156 = ours exactly), ring wraps to
    `BUF_OFFSET 300`, and the app writes **`/tmp/iframe.idx`**: two
    `{sps_addr, sps_len, pps_addr, pps_len, idr_addr, idr_len}` records
    (high + low resolution) — the app's own SPS/PPS/IDR index.
  - [run-my-job/yi-hack-Allwinner rRTSPServer.cpp](https://github.com/run-my-job/yi-hack-Allwinner/blob/master/src/rRTSPServer/src/rRTSPServer.cpp#L18):
    the record-walking reader: walk frames via the header `len`, filter
    by `type`, wrap-aware memcpy — the exact design to port.
  - [roleoroleo/yi-hack-MStar system.sh](https://github.com/roleoroleo/yi-hack-MStar/blob/master/src/static/static/home/yi-hack/script/system.sh#L22):
    frame index at buffer offset 0x1C (ours: 0x18) — same concept,
    platform-shifted layout; each platform's header layout differs, so
    the Fullhan layout still needs mapping from our captures.

  **Next steps:** (1) check `/tmp/iframe.idx` on the unit — if the app
  writes it, joins become exact (no chain scanning); (2) re-dump the
  FF-record 34-byte headers and look for the `type` field matching the
  0x0400/0x0800/0x0002 pattern (candidates: the `6a 8a 3b xx` "const"
  or the tail u16s); (3) if the type decodes, port the record-walking
  reader (rRTSPServer.cpp design) — exact stream selection, no
  size-threshold heuristics, robust across the camera's mode changes.
  Seed data: `tools/sps-scan.py`, `tools/nalscan.py`, `tools/ringdiff.py`,
  `analysis/s*.bin`.
- **Phase 3 — HA integration:** RTSP (generic camera) or ONVIF (minimal
  ws-discovery + SOAP service, or yi-hack's `wsd_simple_server` /
  `onvif_notify_server` if portable). PTZ does not need ONVIF: `pwmv2_fullhan`
  + `write_dev`/`write_gpio` via MQTT/shell.
- **Phase 4 — only if needed:** initramfs patch, with UART (`ttyS0,115200`)
  uboot console + verified full flash backup as the recovery net.

## Open questions

- Does the app encrypt the h264 in the fshare buffer, or is the random
  0x12C–0x46xx region unrelated? (our capture decodes as valid H.264, so
  the stream region is cleartext)
- Where are the UART pads for a uboot console? (before Phase 4)
- How does the official `update.sh` flash images — can that path be reused
  safely?
- Semaphore protocol details for multi-reader fshare access (single reader
  works by polling).
