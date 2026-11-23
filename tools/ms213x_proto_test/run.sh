#!/bin/sh
# Host test for the MS2130 USB flash protocol (firmware/main/ms213x_proto.c) against a model of the chip.
set -e
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -O2 -Wall -Wextra -I../../firmware/main test.c ../../firmware/main/ms213x_proto.c -o "$out/test"
"$out/test"
