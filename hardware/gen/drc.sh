#!/bin/sh
# run KiCad DRC and summarise
CLI=${KICAD_CLI:-$(command -v kicad-cli || echo /Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli)}
cd "$(dirname "$0")/.." && "$CLI" pcb drc --format json --severity-all --all-track-errors -o out/drc.json hdmilight.kicad_pcb >/dev/null 2>&1
python3 - "$@" <<'PY'
import json, collections, sys
d=json.load(open('out/drc.json'))
v=d.get('violations',[]); u=d.get('unconnected_items',[])
c=collections.Counter(x['type'] for x in v)
print(len(v),'violations,',len(u),'unconnected:',dict(c))
def loc(i):
    p=i.get('pos',{}); return f"({p.get('x',0):.2f},{p.get('y',0):.2f})"
skip=set(sys.argv[1:])
shown=0
for x in v:
    if x['type'] in skip: continue
    shown+=1
    if shown>45: continue
    print(' ',x['type'],'|',x['description'][:58],'|',' || '.join(i['description'][:62]+loc(i) for i in x['items']))
for x in u: print('  UNCONN',' || '.join(i['description'][:62]+loc(i) for i in x['items']))
PY
