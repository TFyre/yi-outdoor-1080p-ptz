#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Prepare an SD card for the camera: copy debug.sh + the hack package onto
# a FAT32 card. For a fresh camera this IS the entire install — the stock
# firmware runs debug.sh at boot, which brings up telnet 9999, SSH 2222 and
# the RTSP chain from the card. Nothing is ever flashed to the camera, and
# no prior access (telnet/SSH) is needed on a new unit.
#
# Usage (Git Bash on Windows, card mounted as a drive):
#   tools/install-sd.sh /e            install/update the package on drive E:
#   tools/install-sd.sh --check /e    verify a card that should be ready
#
# The card must be FAT32 (right-click → Format in Explorer if in doubt).
# The dropbear host key is copied along so SSH host-key pinning keeps
# working on a new camera; delete hack/etc/dropbear/ecdsa.key from the
# card if you prefer per-camera keys (boot.sh regenerates a fresh one).
set -u

cd "$(dirname "$0")/.."

usage() {
    echo "usage: $0 <drive> | --check <drive>   (e.g. $0 /e)"
    exit 1
}

[ $# -ge 1 ] || usage
CHECK=0
if [ "$1" = "--check" ]; then
    CHECK=1
    shift
    [ $# -ge 1 ] || usage
fi
DRIVE="$1"

[ -d "$DRIVE" ] || { echo "error: $DRIVE is not a mounted drive" >&2; exit 1; }
echo "== target: $DRIVE"

if [ ! -d deploy/hack/bin ] || [ ! -f deploy/hack/bin/busybox ] \
   || [ ! -f deploy/hack/bin/rRTSPServer ] || [ ! -f deploy/hack/bin/dropbearmulti ]; then
    echo "error: deploy/hack/bin is incomplete — run the build scripts in WSL first:" >&2
    echo "  build-armv6.sh all; build-busybox.sh; build-curl.sh   (dropbearmulti: see CLAUDE.md)" >&2
    exit 1
fi

if [ "$CHECK" = 1 ]; then
    echo "== checking existing card content"
    for f in debug.sh hack/boot.sh hack/start-rtsp.sh hack/bin/busybox \
             hack/bin/fshare2fifo hack/bin/f2f_audio \
             hack/bin/rRTSPServer hack/bin/dropbearmulti \
             hack/root/.ssh/authorized_keys; do
        if [ -f "$DRIVE/$f" ]; then
            echo "   ok  $f"
        else
            echo "  MISS $f"
        fi
    done
    echo "== done (card is ready if nothing is MISSing)"
    exit 0
fi

if [ -f "$DRIVE/debug.sh" ] || [ -d "$DRIVE/hack" ]; then
    echo "!! existing package found on the card — it will be overwritten."
    printf "continue? [y/N] "
    read -r answer
    case "$answer" in
        y|Y) ;;
        *) echo "aborted"; exit 1 ;;
    esac
fi

echo "== copying package"
rm -rf "$DRIVE/hack"
cp -r deploy/hack "$DRIVE/hack"
cp debug.sh "$DRIVE/debug.sh"
echo "   ok"

if [ ! -f "$DRIVE/hack/etc/dropbear/ecdsa.key" ]; then
    echo "   note: no host key on card — boot.sh will generate one (SSH clients"
    echo "         will see a changed host key once)"
fi

echo "== done. Next steps:"
echo "   1. eject the card, put it in the camera, power on"
echo "   2. wait ~60s, then:  bash tools/watch-boot.sh"
echo "   3. stream: rtsp://10.1.2.19/ch0_0.h264"
exit 0
