#!/usr/bin/env python3
"""live_config.py [-j N] : which config.hpp switches actually change the compiled bot (the "live configuration").
For every `#define NAME VALUE` in fleet/config.hpp, compile fleet/main.cpp with that one value changed
(0 <-> 1, other integers +1, decimals x1.5 or 0 -> 1) and compare the generated assembly with the unchanged build.
Identical assembly = the switch has no effect in the current build: an OFF feature's own parameter, a debug-only
switch, or a setting bypassed by another switch. Prints DEAD / LIVE per switch and each dead switch's use sites."""
import os, re, subprocess, sys, hashlib, tempfile
from concurrent.futures import ThreadPoolExecutor
T = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(T)); SRC = os.path.join(ROOT, 'fleet')
J = int(sys.argv[sys.argv.index('-j') + 1]) if '-j' in sys.argv else 3
defs = []
for line in open(os.path.join(SRC, 'config.hpp')):
    m = re.match(r'#define (\w+) (\S+)', line)
    if m:
        defs.append((m.group(1), m.group(2)))

def perturb(v):
    s = v.rstrip('ULul')
    try:
        i = int(s, 0)
        return '1' if i == 0 else ('0' if i == 1 else str(i + 1))
    except ValueError:
        pass
    try:
        f = float(s.rstrip('f'))
        return '1.0' if f == 0 else repr(f * 1.5)
    except ValueError:
        return None

def asm(extra):
    with tempfile.NamedTemporaryFile(suffix='.s', delete=False) as tf:
        out = tf.name
    r = subprocess.run(['clang++', '-std=c++20', '-O1', '-w', '-S', '-o', out] + extra + ['main.cpp'], cwd=SRC, capture_output=True, text=True)
    if r.returncode != 0:
        os.unlink(out)
        return 'ERROR'
    h = hashlib.sha1()
    for l in open(out):
        if not l.lstrip().startswith(('.file', '.ident', '.loc', '#')):
            h.update(l.encode())
    os.unlink(out)
    return h.hexdigest()

base = asm([])
def check(d):
    name, v = d
    p = perturb(v)
    if p is None:
        return name, v, 'SKIP'
    h = asm([f'-D{name}={p}'])
    return name, v, 'ERROR' if h == 'ERROR' else ('LIVE' if h != base else 'DEAD')
with ThreadPoolExecutor(J) as ex:
    res = list(ex.map(check, defs))
uses = {}
for f in sorted(os.listdir(SRC)):
    if f.endswith(('.hpp', '.cpp')) and f != 'config.hpp':
        for i, l in enumerate(open(os.path.join(SRC, f)), 1):
            for w in set(re.findall(r'\b[A-Z][A-Z0-9_]{2,}\b', l)):
                uses.setdefault(w, []).append(f'{f}:{i}: {l.strip()[:150]}')
dead = [r for r in res if r[2] == 'DEAD']
print(f'{len(res)} switches: {sum(r[2] == "LIVE" for r in res)} live, {len(dead)} dead, '
      f'{sum(r[2] == "SKIP" for r in res)} skipped, {sum(r[2] == "ERROR" for r in res)} errors')
for name, v, st in res:
    if st != 'LIVE':
        print(f'\n{st} {name} = {v}')
        for u in uses.get(name, ['  (no use outside config.hpp)'])[:6]:
            print('   ', u)
