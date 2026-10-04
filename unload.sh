#!/usr/bin/env bash
# Remove the chatlog kernel module. Run as: sudo scripts/unload.sh
set -euo pipefail
rmmod chatlog && echo "chatlog unloaded"
