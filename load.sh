set -euo pipefail
cd "$(dirname "$0")/../driver"

[ -f chatlog.ko ] || make
lsmod | grep -q '^chatlog' && rmmod chatlog      
insmod chatlog.ko "$@"                          

for _ in $(seq 20); do [ -e /dev/chatlog ] && break; sleep 0.1; done
chmod 666 /dev/chatlog
echo "Loaded. /dev/chatlog ready:"; ls -l /dev/chatlog
dmesg | tail -n 2
