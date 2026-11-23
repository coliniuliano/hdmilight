"""Compare the netlist KiCad exports from the schematic with design.PARTS."""
import os, re, shutil, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import design
# kicad-cli: $KICAD_CLI, else on PATH, else KiCad.app on macOS
CLI = os.environ.get("KICAD_CLI") or shutil.which("kicad-cli") or "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
root = os.path.join(HERE, "..")
subprocess.run([CLI, "sch", "export", "netlist", "--format", "kicadsexpr", "-o", os.path.join(root, "out", "sch.net"),
                os.path.join(root, "hdmilight.kicad_sch")], capture_output=True)
t = open(os.path.join(root, "out", "sch.net")).read()
t = t[t.index("(nets"):]
nets = {}
for blk in re.split(r"\(net\s*\(code", t)[1:]:
    name = re.search(r'\(name "([^"]*)"\)', blk).group(1).lstrip("/")
    nets[name] = set(re.findall(r'\(node\s*\(ref "([^"]+)"\)\s*\(pin "([^"]+)"\)', blk))
want = {n: set(v) for n, v in design.nets().items()}
bad = 0
for n in sorted(set(nets) | set(want)):
    if n.startswith("unconnected-"):
        continue
    a, b = nets.get(n, set()), want.get(n, set())
    if a != b:
        bad += 1
        if bad <= 10:
            print("DIFF", n, "only in schematic:", sorted(a - b)[:5], "only in design:", sorted(b - a)[:5])
# the same comparison ignoring names: every design net must be exactly one schematic net
sch_sets = {frozenset(v) for k, v in nets.items() if len(v) > 1 or not k.startswith("unconnected-")}
conn = [n for n, v in want.items() if frozenset(v) not in sch_sets]
print(f"{len([n for n in nets if not n.startswith('unconnected-')])} schematic nets, {len(want)} design nets, "
      f"{bad} name mismatches, {len(conn)} connectivity mismatches {conn[:8]}")
