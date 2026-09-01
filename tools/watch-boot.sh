#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Two-phase boot watcher: waits for the current dropbear to go down (reboot),
# then for it to come back, then proves SSH auth still works post-boot.
# Usage: tools/watch-boot.sh   (runs until the camera comes back)
set -u
HOST=10.1.2.19
PORT=2222
up() { (exec 3<>/dev/tcp/$HOST/$PORT) 2>/dev/null; }

while up; do sleep 2; done
echo "[$(date +%T)] port $PORT down - reboot in progress"
until up; do sleep 2; done
echo "[$(date +%T)] port $PORT back up"
ssh -i ~/.ssh/yi_cam_rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=8 \
    -p $PORT root@$HOST 'echo BOOT_OK; ps | grep -v grep | grep -i dropbear; echo ---DBLOG---; cat /tmp/db.log' 2>&1
