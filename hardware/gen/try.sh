#!/bin/sh
# one routing run: ./try.sh [n]  -> fails, DRC counts, tidiness summary and the n worst nets
PY=${KICAD_PYTHON:-/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3}
CLI=${KICAD_CLI:-$(command -v kicad-cli || echo /Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli)}
cd "$(dirname "$0")"
$PY stage.py ${2:-all} > ../out/route.log 2>&1
grep -v wxApp ../out/route.log | grep -E "routed [0-9]+ nets|FAILED|Error|assert" ; grep -A12 Traceback ../out/route.log | tail -6
$CLI pcb drc --format json --severity-all --all-track-errors --schematic-parity -o ../out/drc.json ../hdmilight.kicad_pcb 2>/dev/null | grep Found | tr '\n' ' '; echo
python3 - <<'PYEOF'
import json, collections
d = json.load(open('../out/drc.json'))
c = collections.Counter(v['type'] for v in d['violations'])
print(dict(c), 'unconnected:', len(d.get('unconnected_items', [])))
PYEOF
$PY netstat.py ${1:-14} 2>&1 | grep -v wxApp
