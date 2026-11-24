# Stock system notes — boot chain, services, credentials

Verified 2026-11-24 by extracting the stock rootfs out of the mtd3 dump
(`backup/mtd/`, gitignored) and by reading the app's own diagnostic
snapshots left on the SD card. Firmware 5.0.00.00_202204281015, board
`b221fp`.

## 1. Boot chain

```
init (/etc/inittab)
  ├─ ::sysinit:/etc/init.d/rcS
  │    ├─ S01udev            tmpfs /dev, devpts, udevd
  │    ├─ S02init_rootfs     mount -o loop /dev/mtdblock4 /home   (squashfs)
  │    │                     mount -t jffs2 /dev/mtdblock5 /backup
  │    │                     . /backup/init.sh
  │    └─ S03network         ifconfig lo up
  └─ ::respawn:/sbin/inetd -f -e /etc/inetd.conf
```

`/backup/init.sh` (on the jffs2 `backup` partition, writable — the only
persistent stock config area) is where the important pieces happen:

1. `mount tmpfs /tmp -t tmpfs -o size=32m`, `mkdir /tmp/sd`
2. **Mount the SD card**:
   ```sh
   if [ -b /dev/mmcblk0p1 ]; then mount -t vfat /dev/mmcblk0p1 /tmp/sd
   else                           mount -t vfat /dev/mmcblk0 /tmp/sd; fi
   ```
   This card is a **superfloppy** (FAT32 on the whole device, no partition
   table), so it takes the `/dev/mmcblk0` branch. A partitioned card takes
   the `p1` branch — both are supported.
3. `checkdisk` — see §4.
4. gpio/pinctrl setup, wifi/4G driver selection, optional firmware update.
5. `/home/app/lower_half_init.sh`:
   ```sh
   ./log_server & ./dispatch & sleep 2 ./rmm & sleep 2 ./mp4record &
   ./cloud & ./p2p_tnp & ./oss & ./watch_process &
   chmod 777 /tmp/sd/debug.sh
   sh /tmp/sd/debug.sh &          # <- the hack's entry point
   ```

So **the SD hack runs only if the card mounted**: `/tmp/sd/debug.sh` is
the last line of `lower_half_init.sh`, and `exec`s `/tmp/sd/hack/boot.sh`.
If the mount failed, `/tmp/sd` is an empty directory on tmpfs, the
`chmod`/`sh` fail silently, and the camera comes up as plain stock.

## 2. Network services (stock inetd)

`/etc/inetd.conf` ships exactly two entries:

| Port | Service | State |
|---|---|---|
| 23 | `/sbin/telnetd -i` | **listening** (busybox applet, symlink to `/bin/busybox`) |
| 21 | `/sbin/ftpd /home` | **broken by design** — `/sbin/ftpd` does not exist in the image |

The port-21 banner is literally
`inetd: can't execute '/sbin/ftpd': No such file or directory`. That is
a vendor leftover, not damage — do not chase it.

Everything else the camera originally offered (RTSP, ONVIF, web UI, SSH,
telnet-9999) comes from the SD hack.

## 3. Root login: there is no password to find

The stock rootfs has **`/etc/passwd` with `root:x:0:0:...` and no
`/etc/shadow` at all** — not in the initramfs, not in `/home`, not in
`/backup`. busybox `login` treats the `x` field as "look the hash up in
shadow", finds no file, and denies the login.

Consequences, verified live:

- Port 23 presents a normal telnet negotiation and a `(none) login:`
  prompt, accepts the username `root`, prompts for a password, and then
  **can never authenticate — with any password**, because the comparison
  data does not exist.
- So there is **no stock password to recover from the dumps**, and the
  telnet prompt is a dead end for recovery/remote access.
- It also means nobody (including an attacker) can log in there: the
  camera's only network shell has always been the hack's.

The console shell in `/etc/inittab` (`::respawn:-/bin/sh`) is on the
serial console (`ttyS0`, 115200), not the network.

## 4. `checkdisk`: it can reformat the card

`/home/base/tools/checkdisk` runs at every boot after the SD mount:

- `umount /tmp/sd`, then `/home/base/tools/fsck.fat -a /dev/mmcblk0{,p1}`
  (or `exfatfsck` for exFAT)
- remount, then a write test: `echo 123 > /tmp/sd/.yunyi_test_...`,
  umount, mount again
- on a card it judges bad it has a repair path that runs
  `fdisk /dev/mmcblk0 < /home/base/tools/fdisk.input` followed by
  `mkfs.vfat -v ...`

**Operational rule:** power-cycling the camera while the card looks bad
can therefore wipe the card. Image it before repeated boot attempts.

## 5. Recording

The stock app records continuously to the card:

- `mp4record` writes **1-minute 1080p mp4 segments** to
  `/tmp/sd/record/YYYY.../HH/MM...mp4` (observed ~110 hourly directories,
  ~12 GB on a 15 GB card).
- `watch_process` keeps every app process alive: every 10 s it re-runs
  the commands listed in `/home/app/wp_cmd` (ro squashfs). Its
  `process:[rmm] cmd:[reboot]` entry is the camera's self-recovery
  watchdog — **always keep it**.
- The hack's flags filter that file at boot
  (`deploy/hack/etc/wp_cmd.orig` is the pristine copy):
  - `$HACK/block-cloud` → removes `cloud`, `cloudAPI`, `p2p_tnp`
  - `$HACK/no-record` → removes `mp4record` **and** mounts a 1 MB tmpfs
    over `/tmp/sd/record`, so even a stray recorder writes to RAM.

This continuous recording is what wears SD cards out; see the README /
plan for the "camera as sensor, record elsewhere" architecture.

## 6. Reproducing the rootfs extraction

All offsets are for this firmware; re-scan if it ever changes.

```sh
# 1. mtd3.bin is a u-boot uImage: 64-byte header + 2,242,968-byte payload
dd if=backup/mtd/mtd3.bin of=/tmp/kernel.img bs=1 skip=64 count=2242968

# 2. The kernel is self-decompressing. The xz magic appears twice around
#    offset 13.9k: the FIRST is the decompressor's constant (preceded by
#    the string "Bug in the XZ decompressor"), the SECOND is the payload.
tail -c +13877 /tmp/kernel.img | xz -dc --single-stream > /tmp/vmlinux.bin
#    -> 5,153,696 bytes of ARM code

# 3. The initramfs is another xz stream inside vmlinux.bin, at 4,503,160.
#    (The hit at 3,869,452 is a constant, not a stream.)
tail -c +4503161 /tmp/vmlinux.bin | xz -dc --single-stream > /tmp/initramfs.cpio
#    -> 1,492,480-byte newc cpio ("070701")

mkdir /tmp/rootfs && cd /tmp/rootfs && cpio -idm < /tmp/initramfs.cpio
```

Notes:

- `backup/mtd/initramfs.xz` was cut 5 bytes into the first magic, which
  is why a plain `xz -t` calls it corrupt — it is not the useful file.
- `xz --single-stream` matters: the payload is followed by flash padding
  (zeros / 0xFF), which makes a normal `xz -t` complain.
- The extracted rootfs is a standard busybox 1.26.2 system: `/bin/busybox`
  + applet symlinks, `/etc/inittab`, `/etc/init.d/S0*`.

## 7. Credentials hygiene

The app's rotating logs (`/tmp/sd/log/log1..5.txt`) and the `wpa_supplicant`
config copied into its diagnostic tars contain the **cloud account
credentials and the WiFi PSK**. They were copied to `backup/logs/`
(gitignored) for analysis — never publish them, and do not commit that
directory.
