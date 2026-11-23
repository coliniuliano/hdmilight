#!/bin/sh
# Oneboard build only: the MS2130 capture chip's boot flash through the ESP32, over Wi-Fi.
#
# Over USB (a programmed chip only: a blank one does not enumerate, use SPI or tools/ms2130_serial.py for that):
#   tools/ms2130_flash.sh <host> usb-read out.bin   dump the 64 KB flash (changes nothing: try this first)
#   tools/ms2130_flash.sh <host> usb-write image    check, erase, write and verify an MS2130 image
#   tools/ms2130_flash.sh <host> usb-status         state of the running or last job
# Over SPI (the MS2130 is switched off meanwhile and comes back by itself):
#   tools/ms2130_flash.sh <host> id                 JEDEC id of the flash (proves the SPI path before anything is erased)
#   tools/ms2130_flash.sh <host> read out.bin       dump the first 64 KB
#   tools/ms2130_flash.sh <host> write image        check, erase, write and verify an MS2130 image
#
# The host has no default (every board answers to "hdmilight" until it is renamed); it is asked for when left out.
# The image to write is a full 64 KB dump of a working MS2130 (ms2130/tools/ms2130_image.py breaks one down, and
# steve-m/ms213x_flash with ms2130/tools/ms213x_flash-macos-fullread.patch takes one off a USB dongle). Dumps are
# kept out of git, in ms2130/work/, because a full dump includes the chip's user-data area, which holds its EDID
# and can hold HDCP keys.
set -e
case "$1" in
  id|read|write|usb-read|usb-write|usb-status)
    [ -t 0 ] || { echo "usage: tools/ms2130_flash.sh <host> id|read|write|usb-read|usb-write|usb-status [file]"; exit 2; }
    printf "Oneboard to talk to (hostname or IP): "; read -r HOST;;
  *) HOST=$1; [ $# -gt 0 ] && shift;;
esac
[ -n "$HOST" ] || { echo "usage: tools/ms2130_flash.sh <host> id|read|write|usb-read|usb-write|usb-status [file]"; exit 2; }
status() { curl -s -m 10 "http://$HOST/api/ms2130/usb"; }
field() { python3 -c 'import sys,json; print(json.load(sys.stdin).get(sys.argv[1], ""))' "$1"; }
wait_job() {    # poll until the job is no longer running; succeed only if it finished
  while :; do
    j=$(status); s=$(echo "$j" | field state)
    [ "$s" = running ] || break
    printf '\r%s %s %%   ' "$(echo "$j" | field stage)" "$(echo "$j" | field pct)"; sleep 1
  done
  printf '\r%s: %s\n' "$s" "$(echo "$j" | field message)"
  [ "$s" = done ]
}
case "$1" in
  id)    curl -s -m 20 "http://$HOST/api/ms2130/flash?id=1"; echo;;
  read)  [ -n "$2" ] || { echo "read needs an output file"; exit 2; }
         curl -s -f -m 60 -o "$2" "http://$HOST/api/ms2130/flash" && echo "$(wc -c < "$2") bytes -> $2";;
  write) [ -f "$2" ] || { echo "write needs an image file"; exit 2; }
         curl -s -m 120 --data-binary "@$2" -H 'Content-Type: application/octet-stream' "http://$HOST/api/ms2130/flash"; echo;;
  usb-status) status; echo;;
  usb-read) [ -n "$2" ] || { echo "usb-read needs an output file"; exit 2; }
         curl -s -m 20 -X POST "http://$HOST/api/ms2130/usb?op=read" >/dev/null
         wait_job && curl -s -f -m 60 -o "$2" "http://$HOST/api/ms2130/usb/dump" && echo "$(wc -c < "$2") bytes -> $2";;
  usb-write) [ -f "$2" ] || { echo "usb-write needs an image file"; exit 2; }
         r=$(curl -s -m 60 --data-binary "@$2" -H 'Content-Type: application/octet-stream' "http://$HOST/api/ms2130/usb?op=write")
         e=$(echo "$r" | field error); [ -z "$e" ] || { echo "$e"; exit 1; }
         wait_job;;
  *)     sed -n 2,14p "$0"; exit 2;;
esac
