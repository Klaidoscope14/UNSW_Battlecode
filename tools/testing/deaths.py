#!/usr/bin/env python3
"""deaths.py BASE NAME [DEF=VAL ...] : counterfactual check (R9). Re-plays every game of results/BASE.txt in which our
queen died, with fleet/ built with the given #defines (bot in work/NAME), and reports per game: the old death
(round, cause) and the new result. Games are deterministic, so with no defines the same deaths come back."""
import os, re, sys, subprocess, shutil, glob, collections
from concurrent.futures import ThreadPoolExecutor
T = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(T))
env = dict(os.environ, PATH=os.path.expanduser('~/.local/bin') + ':' + os.environ['PATH'], UNSWBC_NO_UPDATE='1')
base, name = sys.argv[1:3]; defs = sys.argv[3:]
games = []
for l in open(f'{T}/results/{base}.txt'):
    m = re.search(r'/(\w+)\s+(\w+) ([AB]) s(\d+): (\S) \(([^)]*)\).*Qdead:(.*) r(\d+)', l)
    if m: games.append((m.group(1), m.group(2), m.group(3), int(m.group(4)), m.group(5), m.group(7).strip(), int(m.group(8))))
B = f'{T}/work/{name}'; shutil.rmtree(B, ignore_errors=True); os.makedirs(B)
for f in glob.glob(f'{ROOT}/fleet/*.hpp') + [f'{ROOT}/fleet/bot.toml']: shutil.copy(f, B)
with open(f'{B}/main.cpp', 'w') as w:
    w.write(''.join(f'#define {d.split("=")[0]} {d.split("=", 1)[1]}\n' for d in defs) + open(f'{ROOT}/fleet/main.cpp').read())
subprocess.run(['unswbc', 'run', 'maps/default_small.map', B, f'{T}/sparring/vib2', '--seed', '1', '--no-replay'], cwd=ROOT, env=env, capture_output=True)
def play(g):
    opp, mp, side, seed = g[:4]
    a, b = (B, f'{T}/sparring/{opp}') if side == 'A' else (f'{T}/sparring/{opp}', B)
    for _ in range(3):
        r = subprocess.run(['unswbc', 'run', f'maps/{mp}.map', a, b, '--seed', str(seed), '--no-replay'], cwd=ROOT, env=env, capture_output=True, text=True)
        txt = r.stdout + r.stderr
        if 'wins after' in txt or 'draw' in txt.lower(): break
    w = re.search(r'team (A|B) wins after (\d+) rounds \(([^)]*)\)', txt)
    res = ('W' if w.group(1) == side else 'L') if w else '?'
    q = '0' if side == 'A' else '1'
    qm = re.search(r'round (\d+): bot %s \(team %s\) died: (.*)' % (q, side), txt)
    return g, res, (int(qm.group(1)), qm.group(2).strip()) if qm else None
with ThreadPoolExecutor(8) as ex: res = list(ex.map(play, games))
c = collections.Counter()
for g, r, qd in res:
    c['games'] += 1; c['still_dies'] += qd is not None; c['W'] += r == 'W'; c['was_W'] += g[4] == 'W'
    print(f"{g[1]:>16} {g[0]:>8} {g[2]} s{g[3]}: was {g[4]} queen died r{g[6]} ({g[5]}) -> now {r}, " + (f"queen died r{qd[0]} ({qd[1]})" if qd else "queen alive"))
print(f"{base} -> {name}: {c['games']} queen-death games; queen still dies in {c['still_dies']}; wins {c['was_W']} -> {c['W']}")
