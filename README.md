# yi-hack for the YI Outdoor 1080p PTZ (Fullhan FH8626V100)

[![build](https://github.com/TFyre/yi-outdoor-1080p-ptz/actions/workflows/build.yml/badge.svg)](https://github.com/TFyre/yi-outdoor-1080p-ptz/actions/workflows/build.yml)

A yi-hack-v5-style firmware mod for the **YI Outdoor 1080p PTZ** camera
(the `b221fp` board, Fullhan **FH8626V100** SoC): RTSP streaming, SSH, a
web UI, PTZ control, ONVIF, and an opt-in cloud kill switch — all running
from a FAT32 microSD card, leaving the stock flash untouched.

> **⚠️ This is a hobbyist firmware mod.** It may void your warranty,
> break with stock firmware updates, and — if you do something wrong —
> brick your device. It ships with no warranty of any kind. It is not
> affiliated with or endorsed by YI Technology.

## Why this repo exists

Upstream [yi-hack-v5](https://github.com/alienatedsec/yi-hack-v5) (a
Hi3518ev200 project) does not support this unit — its "Yi Outdoor"
support is the older h30 model. This unit is a different SoC entirely
(see [Issue #457](https://github.com/alienatedsec/yi-hack-v5/issues/457)
for the prior recon). The interesting work here is the reverse
engineering: the camera's shared-memory H.264/AAC frame buffer
(`/dev/shm/fshare_frame_buf`) is fully documented in
[docs/fshare-protocol.md](docs/fshare-protocol.md), and everything in
`src/` builds on that spec.

## Features

- **RTSP** at `rtsp://<camera-ip>/ch0_0.h264` — 1080p H.264 + AAC audio,
  served by a LIVE555 server fed from the app's own frame buffer
  (no re-encoding).
- **SSH** — dropbear on port 2222 (key auth; pubkey template in
  `deploy/hack/root/.ssh/authorized_keys.example`), plus a telnet
  bootstrap shell on 9999 via the SD `debug.sh`.
- **Web UI** — busybox httpd on port 8080: live view link, PTZ pad,
  white-LED control.
- **PTZ** — `src/ptz/ptz.c` drives the motor through the stock app's own
  IPC message queues (so the app keeps handling calibration, limits, and
  position persistence).
- **ONVIF Profile S** — `src/onvif/onvif.c`, a hand-rolled SOAP daemon
  on port 8082; works with Home Assistant, ONVIF Device Manager, Blue Iris.
- **Cloud kill switch** (opt-in) — `hack/boot.sh`'s `block-cloud` flag
  kills the cloud clients, bind-mounts a neutralized `wp_cmd` watchdog
  config, sinks the four xiaoyi hostnames in `/etc/hosts`, and replaces
  the lost clock with `ntpd`. Remove the flag and reboot = stock.

## Device facts

| | |
|---|---|
| SoC | Fullhan FH8626V100 — single **ARM1176** core (ARMv6 + VFPv2), ~35 MB RAM |
| Kernel | 4.9.129, uClibc userspace |
| Flash | SPI NOR (MTD: bootstrap, uboot-env, uboot, kernel+initramfs, `/home` squashfs ro, `/backup` jffs2 rw, mfc, conf) |
| Stock fw | `5.0.00.00_202204281015` |
| UART | `ttyS0,115200` — the unbrick path |

**No prebuilt binary from any mainstream toolchain runs here**: ARMv7
instructions, VFPv3-only code, and 64-bit `time_t` issues all bite.
Everything in `hack/bin/` is a static musl soft-float ARMv6 build (see
`tools/build-armv6.sh`).

## Repo layout

```
deploy/hack/    the SD tree (mirrors /tmp/sd/hack/ on the card)
  boot.sh         boot orchestrator (busybox, dropbear, httpd, ONVIF,
                  cloud block — idempotent)
  start-rtsp.sh   the RTSP chain: f2f_audio → rRTSPServer → fshare2fifo,
                  plus an uptime watch loop
  etc/            watchdog config shadow (cloud block)
  www/            web UI (busybox httpd CGI)
src/            camera-side tools (C, static ARMv6)
  fshare2fifo/    ring → fifo producer (video + audio modes)
  onvif/          ONVIF Profile S daemon
  ptz/            PTZ / LED driver (stock app IPC)
  ring-capture/   ring forensics probes
  cpldio/         /dev/cpld_periph ioctl probe (dev-only; see PLAN.md)
tools/           build scripts, dev scripts, offline repro tooling
  vendor/         patched live555 files (LGPL-3.0-or-later, see below)
docs/            fshare-protocol.md — the authoritative ring spec
PLAN.md          the live working plan / reverse-engineering log
CLAUDE.md        device facts + operational notes for contributors
```

`analysis/` and `backup/` are gitignored scratch (ring snapshots,
stock-firmware dumps, test captures).

## Building

Binaries are not committed — build them (in WSL, Ubuntu distro):

```
wsl.exe -d Ubuntu -- bash -lc "cd /mnt/c/<repo-path> && tools/build-armv6.sh <target>"
```

Targets: `fshare2fifo` (and its `f2f_audio` alias), `onvif`, `ptz`,
`cpldio`. The dependency toolchains (busybox, curl, dropbear) build with
their own scripts in `tools/`. See [docs/fshare-protocol.md](docs/fshare-protocol.md)
and the build scripts for the musl.cc ARMv6 toolchain details.

## Installing

1. Build the `hack/bin/` binaries (above).
2. Copy your SSH pubkey to `deploy/hack/root/.ssh/authorized_keys`
   (see the `.example`).
3. Copy `debug.sh` to the root of a FAT32 SD card and the contents of
   `deploy/hack/` to `/tmp/sd/hack/` (or use `tools/install-sd.sh`).
4. Insert the card, power the camera — RTSP, SSH (2222), the web pad
   (8080), and ONVIF (8082) come up on their own.

Client playback recipes (the source runs ~19.5 fps — players assuming
25/30 fps look fast): see the RTSP section of CLAUDE.md.

## Related projects

- [alienatedsec/yi-hack-v5](https://github.com/alienatedsec/yi-hack-v5) —
  the upstream GPL-3.0 project this repo follows in spirit (Hi3518ev200
  based; does not support this unit). Its `ipc_cmd` envelope protocol is
  reused by `src/ptz/`.
- [Issue #457 — "Yi Outdoor 1080p PTZ (b221fp)"](https://github.com/alienatedsec/yi-hack-v5/issues/457) —
  prior recon of this exact unit.
- [roleoroleo/yi-hack-Allwinner-v2](https://github.com/roleoroleo/yi-hack-Allwinner-v2) —
  the Allwinner-v2 port (`rRTSPServer.h` referenced during the RTSP port).
- [BenjaminFaal/yi-hack-Allwinner](https://github.com/BenjaminFaal/yi-hack-Allwinner) —
  snapshot/imggrabber work referenced during recon.
- [run-my-job/yi-hack-Allwinner](https://github.com/run-my-job/yi-hack-Allwinner) —
  `rRTSPServer.cpp` referenced during the RTSP port.
- [roleoroleo/yi-hack-MStar](https://github.com/roleoroleo/yi-hack-MStar) —
  the MStar port (`system.sh` referenced during the SD-package design).
- [LIVE555 / live555MediaServer](http://www.live555.com/) — the RTSP
  media framework; `tools/vendor/` carries patched copies of its sources
  (LGPL-3.0-or-later).
- Builds also use: [busybox](https://busybox.net/),
  [dropbear](https://matt.ucc.asn.au/dropbear/dropbear.html),
  [curl](https://curl.se/), and the [musl.cc](https://musl.cc/) ARMv6
  cross toolchains.

## License

GPL-3.0-or-later (see [LICENSE](LICENSE)), matching upstream
yi-hack-v5. Exceptions: `tools/vendor/*` are modified
[LIVE555](http://www.live555.com/) sources, which remain under their own
**LGPL-3.0-or-later** license with headers intact (live555 2020.01.19,
as bundled by upstream yi-hack-v5; the patches applied by
`tools/build-armv6.sh` and `tools/build-rtspserver-native.sh` are
described in PLAN.md's RTSP sections). This repo redistributes no
compiled binaries, only source — if you attach release binaries to your
own releases, the usual GPL source-offer obligations for
busybox/dropbear/curl apply.
