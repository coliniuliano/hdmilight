#!/bin/sh
# Reference check: compile HyperHDR v22.0.0.0's own colour math against ../../core and diff the results.
# Everything upstream comes from a shallow clone of the pinned tag, made here: whole files where they compile
# (ColorSpace.cpp, YuvConverter.cpp, InfiniteHybridRgbInterpolator.cpp), and hash-pinned line ranges for the four
# functions that do not (hdr_to_srgb / doToneMapping / prepareLCH, and the output processing). No HyperHDR source
# is kept in this repository; see README.md beside this script.
# Needs a C++20 compiler with a working libc++ and git. Apple's Command Line Tools can ship empty c++/v1
# include dirs, in which case point CXX at a full toolchain (Homebrew LLVM on macOS is the default below).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CORE="$HERE/../../core"
WORK=${WORK:-/tmp/hl_refcheck}
CXX=${CXX:-/opt/homebrew/opt/llvm/bin/clang++}
CC=${CC:-clang}
mkdir -p "$WORK"
if [ ! -d "$WORK/HyperHDR" ]; then
  git clone -q --depth 1 -b v22.0.0.0 https://github.com/awawa-dev/HyperHDR.git "$WORK/HyperHDR"
  git -C "$WORK/HyperHDR" submodule update -q --init --depth 1 external/linalg
fi
H="$WORK/HyperHDR"

# Four of the functions this check needs live inside LutCalibrator.cpp and InfiniteProcessing.cpp, which cannot be
# compiled on their own (Qt). They are cut out of the pinned clone here rather than kept as copies in this
# repository. Each extract is checked against the hash it was written against, so a moved line range, an edited
# file or a re-pointed tag fails loudly instead of silently compiling the wrong text.
if command -v shasum >/dev/null 2>&1; then sha256() { shasum -a 256 | cut -d' ' -f1; }
else sha256() { sha256sum | cut -d' ' -f1; }; fi
GEN="$WORK/extracts"
rm -rf "$GEN" && mkdir -p "$GEN"
extract() {   # extract <name.inc> <path under sources/> <first,last> <sha256 of those lines>
  sed -n "$3p" "$H/sources/$2" > "$GEN/$1"
  got=$(sha256 < "$GEN/$1")
  if [ "$got" != "$4" ]; then
    echo "$1: sources/$2 lines $3 are not what this check was written against (tag v22.0.0.0)"
    echo "  expected sha256 $4"
    echo "  got             $got"
    exit 1
  fi
}
extract ref_tonemapping.inc lut-calibrator/LutCalibrator.cpp 537,671 \
        54224beec276de665212bf9bcaa538d1a5eb36ac22915471a6e5a1c42b7f8016
extract ref_hdr_to_srgb.inc lut-calibrator/LutCalibrator.cpp 703,828 \
        6f66dd69044b310a819ae6be5090abe592c315bc76e8f631efdd5ec8df3b7ece
extract ref_preplch.inc     lut-calibrator/LutCalibrator.cpp 829,871 \
        40b45cab17eb5622afe32811118272b3e8d652cf71ebc470931abb8c279b675a
extract ref_processing.inc  infinite-color-engine/InfiniteProcessing.cpp 240,668 \
        81c124765b99d0eb346193561c01269b7e56eeb1b55698c2faca9a5468745bea

rm -rf "$WORK/obj" && mkdir -p "$WORK/obj"
for f in "$CORE"/src/*.c; do $CC -std=c11 -O2 -I"$CORE/include" -c "$f" -o "$WORK/obj/$(basename "${f%.c}").o"; done
SYSROOT=$(xcrun --show-sdk-path 2>/dev/null || true)
$CXX -std=c++20 -O1 ${SYSROOT:+-isysroot "$SYSROOT"} -DPCH_ENABLED -include "$HERE/stubs/prelude.h" \
  -I"$HERE/stubs" -I"$HERE" -I"$GEN" -I"$H/include" -I"$H/external/linalg" -I"$CORE/include" -Wno-everything \
  "$HERE/harness.cpp" \
  "$H/sources/infinite-color-engine/ColorSpace.cpp" \
  "$H/sources/infinite-color-engine/YuvConverter.cpp" \
  "$H/sources/infinite-color-engine/InfiniteHybridRgbInterpolator.cpp" \
  "$WORK"/obj/*.o -o "$WORK/refcheck"
"$WORK/refcheck"
