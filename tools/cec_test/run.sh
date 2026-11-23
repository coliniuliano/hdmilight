#!/bin/sh
# Host test for the CEC decoder, frame timings and TV power-state logic (firmware/main/cec_core.c).
set -e
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -O2 -Wall -Wextra -I../../firmware/main test.c ../../firmware/main/cec_core.c -o "$out/test"
"$out/test"
