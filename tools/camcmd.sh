#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# One-shot command runner over the camera's telnet shell (busybox ash).
# Usage: camcmd.sh "df -h; cat /proc/mtd"
# Output is returned on stdout; the connection is closed when the command finishes.
#
# Note: we wait on a probe echo, not on the shell prompt — the prompt has no
# trailing newline, so line-based reads can never "see" it.
set -u

HOST=${CAM_HOST:-10.1.2.19}
PORT=${CAM_PORT:-9999}
CMD=${1:-"id"}

exec 3<>/dev/tcp/$HOST/$PORT || { echo "connect failed to $HOST:$PORT" >&2; exit 1; }

# Prove the shell is up: its echo of our probe ends with a newline.
printf 'echo __CAMCMD_READY__\n' >&3
ready=0
for _ in $(seq 1 20); do
  IFS= read -r -t 1 line <&3 && case "${line%$'\r'}" in
    *__CAMCMD_READY__*) ready=1; break ;;
  esac
done
[ "$ready" = 1 ] || { echo "shell not ready on $HOST:$PORT" >&2; exit 1; }

printf '%s\n' "$CMD" >&3
printf '%s\n' 'echo __CAMCMD_DONE__' >&3

while IFS= read -r -t 20 line <&3; do
  line=${line%$'\r'}
  case "$line" in
    *__CAMCMD_DONE__*) break ;;
    *__CAMCMD_READY__*) continue ;;
  esac
  printf '%s\n' "$line"
done
exec 3<&-
