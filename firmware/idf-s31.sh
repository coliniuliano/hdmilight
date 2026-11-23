#!/bin/sh
# idf.py for the ESP32-S31 dev kit (Function-CoreBoard-1 with an external HDMI splitter + USB grabber): its own build
# directory and sdkconfig, so it never disturbs the P4 or the oneboard builds.
#   ./idf-s31.sh set-target esp32s31    (once)
#   ./idf-s31.sh build
#   ./idf-s31.sh -p /dev/cu.usbserial-XXXX flash monitor
cd "$(dirname "$0")"
exec idf.py --preview -B build-esp32s31 -D SDKCONFIG=sdkconfig.esp32s31 "$@"
