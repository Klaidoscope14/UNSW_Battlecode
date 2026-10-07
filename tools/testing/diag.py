#!/usr/bin/env python3
"""diag.py NAME OUTDIR MAPS OPPS SEEDS [DEF=VAL ...] : build fleet/ with QDIAG (+ #defines) into work/NAME and play
every map x opponent x side x seed with FLEET_DIAG=<OUTDIR>/<map>_<opp>_<side><seed>.diag; the engine's output goes to
the matching .out (who died when: the truth the diagnostics are checked against). MAPS / OPPS comma-separated, or
"suite" for the 17 suite maps / 3 sparring bots. Set QDIAG_ALL=1 to log the queen every turn (big files)."""
import os, sys, subprocess, shutil, glob
from concurrent.futures import ThreadPoolExecutor

class _R:  # stand-in result for a game that hung (killed at the timeout)
    stdout = ''; stderr = 'timeout'
def run_to(cmd, **kw):
    try:
        return subprocess.run(cmd, **kw)
    except subprocess.TimeoutExpired:
        return _R()
T = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(T))
env = dict(os.environ, PATH=os.path.expanduser('~/.local/bin') + ':' + os.environ['PATH'], UNSWBC_NO_UPDATE='1')
SUITE_MAPS = 'australia,autarky,default,devil,dilemma,islands,maze,portals,queen_of_spades,schooltime,slithery_fight,stripes,tower_defense,trauma,trophy,unsw,weakhold'
name, out, maps, opps, seeds = sys.argv[1:6]; defs = sys.argv[6:]
maps = SUITE_MAPS if maps == 'suite' else maps; opps = 'vib2,swarm2,fleet11' if opps == 'suite' else opps
B = f'{T}/work/{name}'; shutil.rmtree(B, ignore_errors=True); os.makedirs(B); os.makedirs(out, exist_ok=True)
for f in glob.glob(f'{ROOT}/fleet/*.hpp') + [f'{ROOT}/fleet/bot.toml']: shutil.copy(f, B)
with open(f'{B}/main.cpp', 'w') as w:
    w.write('#define QDIAG 1\n' + ''.join(f'#define {d.split("=")[0]} {d.split("=", 1)[1]}\n' for d in defs) + open(f'{ROOT}/fleet/main.cpp').read())
r = subprocess.run(['unswbc', 'run', 'maps/default_small.map', B, f'{T}/sparring/vib2', '--seed', '1', '--no-replay'], cwd=ROOT, env=env, capture_output=True, text=True)
if 'wins' not in r.stdout + r.stderr: print('BUILD FAILED', (r.stdout + r.stderr)[-2000:]); sys.exit(1)
jobs = [(m, o, s, sd) for m in maps.split(',') for o in opps.split(',') for s in 'AB' for sd in range(1, int(seeds) + 1)]
def play(j):
    m, o, side, sd = j; base = f'{out}/{m}_{o}_{side}{sd}'
    a, b = (B, f'{T}/sparring/{o}') if side == 'A' else (f'{T}/sparring/{o}', B)
    for _ in range(3):  # an engine error under load: play it again
        open(base + '.diag', 'w').close()
        r = run_to(['unswbc', 'run', f'maps/{m}.map', a, b, '--seed', str(sd), '--no-replay'], cwd=ROOT, env=dict(env, FLEET_DIAG=base + '.diag'), capture_output=True, text=True, timeout=600)
        if 'wins after' in r.stdout + r.stderr or 'draw' in (r.stdout + r.stderr).lower(): break
    open(base + '.out', 'w').write(r.stdout + r.stderr)
with ThreadPoolExecutor(6) as ex: list(ex.map(play, jobs))  # <= 6 games: the user process limit is 1333 and each dragon is a process
print('done', len(jobs))
