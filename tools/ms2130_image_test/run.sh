#!/bin/sh
# Host test for the MS2130 image check used by the flash writer (firmware/main/ms2130_image.c).
# Also checks the real dump when it is present (ms2130/work is not in git).
set -e
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -O2 -Wall -Wextra test.c ../../firmware/main/ms2130_image.c -o "$out/test"
"$out/test" $(ls ../../ms2130/work/*.bin 2>/dev/null)
