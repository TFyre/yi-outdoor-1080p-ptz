#!/bin/bash
# Pull a raw ring snapshot from the camera to the local machine.
# Usage: tools/grab-ring.sh [output.bin]   (default: analysis/ring.bin)
OUT="${1:-analysis/ring.bin}"
SSH="ssh -i ~/.ssh/yi_cam_rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o StrictHostKeyChecking=accept-new -o ConnectTimeout=20 -p 2222 root@10.1.2.19"
SCP="scp -O -P 2222 -i ~/.ssh/yi_cam_rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o StrictHostKeyChecking=accept-new -o ConnectTimeout=20"
$SSH "cp /dev/shm/fshare_frame_buf /tmp/ring.bin && ls -la /tmp/ring.bin"
$SCP root@10.1.2.19:/tmp/ring.bin "$OUT"
ls -la "$OUT"
