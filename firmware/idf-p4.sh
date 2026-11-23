#!/bin/sh
# idf.py for the ESP32-P4 dev kits (external HDMI splitter + USB grabber): its own build directory, the default sdkconfig.
#   ./idf-p4.sh set-target esp32p4      (once)
#   ./idf-p4.sh build
#   ./idf-p4.sh -p /dev/cu.usbmodemXXXX flash monitor
# The installed P4 board is updated over Wi-Fi only: tools/ota_paced.py.
cd "$(dirname "$0")"
exec idf.py -B build-esp32p4 "$@"
