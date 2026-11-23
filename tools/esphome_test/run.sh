#!/bin/sh
# Tests for firmware/main/esphome_api.c against aioesphomeapi (Home Assistant's ESPHome client), all on 127.0.0.1:
#   1. protocol: handshake, device info, entities, commands, state pushes, reconnect      (test.py)
#   2. robustness: bad frames, slow frames, idle timeout, eviction, connection storm, CPU  (robustness.py)
#   3. starvation: server out of file descriptors must not spin and must recover          (robustness.py fd)
# Needs a Python with aioesphomeapi: PYTHON=/path/to/venv/bin/python ./run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
fw="$here/../../firmware/main"
out=$(mktemp -d)
py=${PYTHON:-python3}
pids=""
trap 'for p in $pids; do kill $p 2>/dev/null; done; rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -DHL_ESPH_BIND_ADDR=INADDR_LOOPBACK -I"$fw" \
   "$fw/esphome_api.c" "$here/host_main.c" -o "$out/host_esphome"

start() {   # port idle_ms [fd limit]
    if [ -n "$3" ]; then (ulimit -n "$3"; exec "$out/host_esphome" "$1" "$2") > "$out/server-$1.log" 2>&1 &
    else "$out/host_esphome" "$1" "$2" > "$out/server-$1.log" 2>&1 & fi
    pids="$pids $!"; last=$!; sleep 0.3
}
fail() { echo "--- server log"; cat "$out/server-$1.log"; exit 1; }

echo "== protocol";   start 16053 0;    "$py" -I "$here/test.py"       16053 $last            || fail 16053
echo "== robustness"; start 16054 1500; "$py" -I "$here/robustness.py" 16054 $last 1500       || fail 16054
echo "== starvation"; start 16055 6000 7; "$py" -I "$here/robustness.py" 16055 $last 6000 fd  || fail 16055
echo "all suites passed"
