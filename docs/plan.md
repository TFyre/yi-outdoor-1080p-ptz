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

- **Phase 0 — read-only recon (current):** pull firmware artifacts to
  `analysis/` (gitignored), unpack the RTS3903N tarball and mtd3's initramfs,
  string-scan app binaries for RTSP/ONVIF leads, read `/backup/init.sh` and
  `/home/app/script/update.sh`.
- **Phase 1 — clean SD package:** one folder `/tmp/sd/hack/` with full-applet
  static busybox, dropbear + keys, sftp-server, mediamtx, static curl, one
  `boot.sh`, and a debug.sh one-liner. Build env: WSL2/Docker (Windows host).
- **Phase 2 — video out:** via option 1 or 2.
- **Phase 3 — HA integration:** RTSP (generic camera) or ONVIF (minimal
  ws-discovery + SOAP service, or yi-hack's `wsd_simple_server` /
  `onvif_notify_server` if portable). PTZ does not need ONVIF: `pwmv2_fullhan`
  + `write_dev`/`write_gpio` via MQTT/shell.
- **Phase 4 — only if needed:** initramfs patch, with UART (`ttyS0,115200`)
  uboot console + verified full flash backup as the recovery net.

## Open questions

- What exactly is `Yi-RTS3903N-RTSPServerV03.tar.gz`? (Phase 0)
- Does `tserver`/`p2p_tnp` offer a local RTSP/RTP path? (Phase 0)
- How does the official `update.sh` flash images — can that path be reused
  safely? (Phase 0)
- Where are the UART pads for a uboot console? (before Phase 4)
