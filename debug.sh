#!/bin/sh
# debug.sh — the stock firmware runs this from the SD card root at boot.
# It is just the entry point: everything lives in the hack package
# (see /tmp/sd/hack/boot.sh).
if [ -f /tmp/sd/hack/boot.sh ]; then
  exec /tmp/sd/hack/boot.sh
fi
# Fallback: no hack package on the SD — bare telnet so we can still get in.
/bin/busybox telnetd -l /bin/sh -p 9999 &
