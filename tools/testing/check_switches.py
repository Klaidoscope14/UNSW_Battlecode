#!/usr/bin/env python3
"""check_switches.py [DIR] : every #define in the bot's sources must have a unique name (default DIR = fleet).
Guarded switches (#ifndef X / #define X) silently shadow each other when a name is reused (the MAJ_MIN_UNITS
12 vs 8 bug). Exit code 1 and a list when a name is defined more than once."""
import collections, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
d = os.path.join(ROOT, sys.argv[1] if len(sys.argv) > 1 else "fleet")
seen = collections.defaultdict(list)
for f in sorted(os.listdir(d)):
    if not f.endswith((".cpp", ".hpp", ".h")) or f == "helper.hpp":
        continue
    for i, line in enumerate(open(os.path.join(d, f)), 1):
        m = re.match(r"\s*#\s*define\s+([A-Za-z_]\w*)", line)
        if m:
            seen[m.group(1)].append(f"{f}:{i}")
dup = {k: v for k, v in seen.items() if len(v) > 1}
for k, v in sorted(dup.items()):
    print(f"DUPLICATE {k}: {', '.join(v)}")
print(f"{len(seen)} switches, {len(dup)} duplicated")
sys.exit(1 if dup else 0)
