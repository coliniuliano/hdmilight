#!/bin/sh
# Disassemble and decompile an MS2130 flash image: decomp.sh IMAGE [OUT]   (OUT defaults to ../../work/decomp)
# Needs Ghidra and a JDK (brew install ghidra openjdk); GHIDRA and JAVA_HOME override where they are looked for.
set -e
here=$(cd "$(dirname "$0")" && pwd)
image=$1
out=${2:-$here/../../work/decomp}
[ -f "$image" ] || { echo "usage: decomp.sh IMAGE [OUT]" >&2; exit 2; }
mkdir -p "$out"; out=$(cd "$out" && pwd)
GHIDRA=${GHIDRA:-/opt/homebrew/opt/ghidra/libexec}
if [ -z "$JAVA_HOME" ]; then
  for j in /opt/homebrew/opt/openjdk /opt/homebrew/opt/openjdk@*; do
    [ -x "$j/libexec/openjdk.jdk/Contents/Home/bin/java" ] && JAVA_HOME=$j/libexec/openjdk.jdk/Contents/Home && break
  done
fi
export JAVA_HOME
export PATH="$JAVA_HOME/bin:$PATH"

python3 "$here/ms8051.py" prepare "$image" "$out"
rm -rf "$out/ghidra" && mkdir "$out/ghidra"
"$GHIDRA/support/analyzeHeadless" "$out/ghidra" ms2130 -import "$out/code_patched.bin" \
  -processor "8051:BE:16:default" -loader BinaryLoader -loader-baseAddr 0 -scriptPath "$here" \
  -preScript Setup8051.java "$out/entries.txt" "$out/insn.txt" "$out/inline.txt" \
  -postScript Dump8051.java "$out" > "$out/ghidra.log" 2>&1 || { tail -20 "$out/ghidra.log"; exit 1; }
if grep -q "SCRIPT ERROR" "$out/ghidra.log"; then grep -A3 "SCRIPT ERROR" "$out/ghidra.log"; exit 1; fi

python3 - "$out" <<'EOF'
import json, sys
out = sys.argv[1]
want = set(json.load(open(out + '/flow.json'))['insn'])
got = {int(l.split()[0], 16) for l in open(out + '/listing.txt') if not l.startswith(';')}
missing = want - got
print(f"Ghidra disassembled {len(want & got)} of {len(want)} traced instructions" + (f"; MISSING {len(missing)}" if missing else ""))
print(f"{sum(1 for _ in open(out + '/funcs.txt'))} functions -> {out}/decomp.c, listing.txt, funcs.txt")
sys.exit(1 if missing else 0)
EOF
