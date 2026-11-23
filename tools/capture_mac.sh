#!/bin/sh
# Grab frames from the MS2130 on macOS for the host simulator.
#   tools/capture_mac.sh [seconds] [outdir]
# Lists AVFoundation devices first; set DEV to the dongle's index if it is not 0.
# AVFoundation decodes the dongle's MJPEG itself and only exposes raw pixel formats,
# so the frames are re-encoded JPEGs of the dongle's output, not its own bitstream.
# (Whether the dongle's JPEGs carry DHT tables is answered by the P4 log instead.)
set -e
SECS=${1:-5}
OUT=${2:-/tmp/hl_frames}
DEV=${DEV:-0}
mkdir -p "$OUT"
echo "--- AVFoundation devices ---"
ffmpeg -hide_banner -f avfoundation -list_devices true -i "" 2>&1 | sed -n '/video devices/,/audio devices/p' || true
echo "--- capturing ${SECS}s from device ${DEV} at 640x480 @30 (uyvy422, re-encoded to JPEG q2) ---"
ffmpeg -hide_banner -y -f avfoundation -framerate 30 -video_size 640x480 -pixel_format uyvy422 \
       -i "${DEV}" -t "$SECS" -q:v 2 -f image2 "$OUT/frame_%04d.jpg"
echo "--- advertised modes ---"
ffmpeg -hide_banner -f avfoundation -video_size 1x1 -i "${DEV}" -t 0.1 -f null - 2>&1 \
  | grep -o -E '[0-9]+x[0-9]+@\[[0-9.]+' | sed 's/@\[/ /' \
  | awk '{r[$1]=r[$1]" "$2} END{for(k in r) print k":"r[k]}' | sort -t x -k1,1n -k2,2n
ls "$OUT" | wc -l
