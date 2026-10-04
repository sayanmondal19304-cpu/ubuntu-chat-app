#!/usr/bin/env bash
# Build (if needed) and load the chatlog kernel module. Run as: sudo scripts/load.sh
set -euo pipefail
cd "$(dirname "$0")/../driver"

[ -f chatlog.ko ] || make
lsmod | grep -q '^chatlog' && rmmod chatlog      # reload if already loaded
insmod chatlog.ko "$@"                           # e.g.  buf_size=131072

# wait for udev to create the node, then let normal users use it (demo setting)
for _ in $(seq 20); do [ -e /dev/chatlog ] && break; sleep 0.1; done
chmod 666 /dev/chatlog
echo "Loaded. /dev/chatlog ready:"; ls -l /dev/chatlog
dmesg | tail -n 2
