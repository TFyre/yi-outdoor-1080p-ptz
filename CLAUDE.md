# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Firmware modification project for the **YI Outdoor 1080p PTZ** camera (yitechnology.com/outdoor-ptz): a yi-hack-v5-style mod — RTSP, SSH, web UI, PTZ control — that runs from a FAT32 microSD card and leaves the stock flash untouched.

The repository is tooling + docs so far (see `tools/`); there is no build system or test suite yet.

## Commits

Use Conventional Commits: `type(scope): description` (e.g. `feat(rtsp): …`, `fix: …`, `docs: …`, `chore: …`).

## Device facts (verified on the unit)

- SoC: Fullhan **FH8626V100** — single ARMv6 core, ~35 MB RAM (MemTotal: 35904 kB). Not the Allwinner `r40ga` and not the Hi3518ev200 `h30ga` outdoor models; do not mix them up.
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

- **Telnet (bootstrap)**: `debug.sh` on the SD card root runs at boot — installs busybox 1.36.1 to `/bin` and starts `telnetd -l/bin/ash -p9999`. Connect: `telnet 10.1.2.19 9999` (no password). Non-interactive: `bash tools/camcmd.sh "command"`.
- **SSH (preferred)**: dropbear is started by `debug.sh` at boot. The host key persists on the SD at `/tmp/sd/yi-hack-v5/etc/dropbear/ecdsa.key` (stable across reboots, so host-key pinning works). dropbearkey cannot write to vfat directly (its chmod fails), so the boot script generates in tmpfs and copies with `cat` when the key is missing. Manual restart:
  ```
  nohup /tmp/sd/yi-hack-v5/bin/dropbearmulti dropbear -r /tmp/sd/yi-hack-v5/etc/dropbear/ecdsa.key -E -p 2222 >/tmp/db.log 2>&1 &
  ```
  Client side (this machine):
  ```
  ssh -i ~/.ssh/yi_cam_rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o StrictHostKeyChecking=accept-new -p 2222 root@10.1.2.19
  scp -O -P 2222 -i ~/.ssh/yi_cam_rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o StrictHostKeyChecking=accept-new root@10.1.2.19:<file> <dest>
  ```
  `-O` forces legacy SCP protocol — there is no sftp-server on the camera. Client key: `~/.ssh/yi_cam_rsa` (pubkey installed as `/root/.ssh/authorized_keys` on the camera).

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

Nothing of the RTSP chain is persistent across reboots yet — see Phase 1.

## Backup

Full stock backup taken (all MTD partitions + file-level tars of `/home` and `/backup`), md5-verified on both ends:
- On the SD card: `/tmp/sd/backup/mtd/`
- Local copy (gitignored via `backup/`): `backup/mtd/`

Keep stock-firmware dumps out of git.

## Upstream and prior work

- The camera is **unsupported** by upstream [alienatedsec/yi-hack-v5](https://github.com/alienatedsec/yi-hack-v5) (a Hi3518ev200 project, GPL-3.0; its "Yi Outdoor" support is the older h30 model, base fw 3.0.0.0D). Porting the hack to this Fullhan unit is the core challenge.
- [Issue #457](https://github.com/alienatedsec/yi-hack-v5/issues/457) (closed, not planned) documents prior recon of this exact unit. Baseline tried: release 0.4.1 (`yi_outdoor_0.4.1.tgz`), payload at `/tmp/sd/yi-hack-v5/` (binaries in `bin/`, busybox in `busybox/`). The SD also holds later experiments (mediamtx armv6, ffmpeg, h201c/r10m/cloud-dome tarballs).
