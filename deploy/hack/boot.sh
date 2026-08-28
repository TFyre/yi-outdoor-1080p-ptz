#!/bin/sh
# /tmp/sd/hack/boot.sh — camera boot orchestrator.
#
# The stock firmware runs /tmp/sd/debug.sh at boot; that one-liner execs
# this script. Idempotent: safe to re-run by hand while the camera is up.
# Fast: the stock boot may wait for debug.sh to finish, so anything slow
# (the RTSP chain, which needs the app's frame buffer) runs detached.

HACK=/tmp/sd/hack
BIN=$HACK/bin

# 1. busybox 1.36.1 full-applet set into /bin. Stock busybox is 1.26.2 and
#    lacks mkfifo/head/wc/uname/base64/... /bin is the ramdisk (lost on
#    reboot), so this is redone every boot — by design. cp+mv (not plain
#    cp): overwriting a binary that running shells execute fails with
#    ETXTBSY, rename does not.
cp $BIN/busybox /bin/busybox1.36.1.new
mv /bin/busybox1.36.1.new /bin/busybox1.36.1
rm -f /bin/ash
/bin/busybox1.36.1 --install -s /bin

# 2. telnet fallback shell (bootstrap if SSH ever breaks)
/bin/busybox1.36.1 telnetd -l /bin/ash -p 9999 &

# 3. dropbear SSH on 2222. The host key lives on the SD so it is stable
#    across reboots (dropbearkey cannot write to vfat directly — its chmod
#    fails with EPERM — so generate in tmpfs and cat onto the SD).
if [ ! -f $HACK/etc/dropbear/ecdsa.key ]; then
  mkdir -p $HACK/etc/dropbear /tmp/dropbear
  $BIN/dropbearmulti dropbearkey -t ecdsa -f /tmp/dropbear/ecdsa.key
  cat /tmp/dropbear/ecdsa.key > $HACK/etc/dropbear/ecdsa.key
fi
mkdir -p /root/.ssh
cat $HACK/root/.ssh/authorized_keys > /root/.ssh/authorized_keys
chmod 700 /root/.ssh
chmod 600 /root/.ssh/authorized_keys
# dropbearmulti applets in PATH: client-side scp -O execs a remote `scp -t`
ln -sf $BIN/dropbearmulti /bin/scp
ln -sf $BIN/dropbearmulti /bin/dbclient
# Start only if no pidfile-tracked instance is alive (idempotent re-run).
# Killing an existing listener is deliberately avoided: its child sessions
# share its comm name, so a pidof-based kill would take down live SSH
# sessions with it.
if [ -s /tmp/dropbear.pid ] && [ -d /proc/$(cat /tmp/dropbear.pid) ]; then
  : # dropbear already running
else
  nohup $BIN/dropbearmulti dropbear -P /tmp/dropbear.pid \
    -r $HACK/etc/dropbear/ecdsa.key -E -p 2222 >/tmp/db.log 2>&1 &
fi

# 4. RTSP chain — detached: the stock app (rmm) creates the shared frame
#    buffer only once the camera pipeline is up, well after boot finishes.
#    Auto-start is OPT-IN: it runs only when the flag file auto-rtsp
#    exists on the SD (enable: touch /tmp/sd/hack/auto-rtsp). Without it
#    boot leaves the chain down — bring it up by hand with
#    `sh /tmp/sd/hack/start-rtsp.sh`.
if [ -f $HACK/auto-rtsp ]; then
  nohup $HACK/start-rtsp.sh >/tmp/rtsp-boot.log 2>&1 &
fi

# 5. Web UI (PTZ pad + CGI) on 8080 — tiny, always on. The CGI drives
#    the motor through the app's own mqueue (see src/ptz/).
if ! pidof httpd >/dev/null; then
  nohup /bin/busybox1.36.1 httpd -p 8080 -h $HACK/www >/tmp/httpd.log 2>&1 &
fi

exit 0
