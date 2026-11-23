#!/bin/sh
# idf.py for the oneboard (revision A: splitter, capture chip and ESP32-S31 on one PCB): its own build directory and sdkconfig, so it never
# disturbs the dev-kit builds.
#   ./idf-oneboard.sh set-target esp32s31     (once)
#   ./idf-oneboard.sh build
#   ./idf-oneboard.sh -p /dev/cu.usbmodemXXXX flash monitor
cd "$(dirname "$0")"
exec idf.py --preview -B build-oneboard -D SDKCONFIG=sdkconfig.oneboard -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.oneboard" "$@"
