#!/bin/sh
# Upload a firmware image to a running board over Wi-Fi and wait for it to come back.
#   tools/ota.sh <host> <image>      e.g. tools/ota.sh 192.168.1.250 firmware/build-oneboard/hdmilight_v3.bin
# No defaults: every board answers to "hdmilight" until it is renamed, and each kind of board has its own image.
# With no host on the command line it asks for one.
set -e
HOST=$1
if [ -z "$HOST" ]; then
  [ -t 0 ] || { echo "usage: tools/ota.sh <host> <image>"; exit 2; }
  printf "Board to update (hostname or IP): "; read -r HOST
  [ -n "$HOST" ] || { echo "no host given"; exit 2; }
fi
IMG=$2
if [ -z "$IMG" ] || [ ! -f "$IMG" ]; then
  echo "usage: tools/ota.sh <host> <image>     the image must be named; built ones:"
  ls "$(dirname "$0")"/../firmware/build-*/hdmilight_v3.bin 2>/dev/null | sed 's/^/  /'
  exit 2
fi
echo "before: $(curl -s -m 5 "http://$HOST/api/status" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d.get("build"))')"
curl -s -m 120 --data-binary "@$IMG" -H 'Content-Type: application/octet-stream' "http://$HOST/api/update"; echo
echo "rebooting..."; sleep 25
for i in 1 2 3 4 5 6; do
  if b=$(curl -s -m 5 "http://$HOST/api/status" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d.get("build"))' 2>/dev/null); then echo "after:  $b"; exit 0; fi
  sleep 5
done
echo "board did not answer"; exit 1
