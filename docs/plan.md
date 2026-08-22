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
  camera** (ffmpeg-verified). Known issues: join starts mid-GOP (decoder
  artifacts until first IDR), occasional torn frames at ring wrap —
  polish by waiting for SPS/IDR before emitting.
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
