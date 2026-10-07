#!/usr/bin/env python3
"""contact.py OUTDIR [OUTDIR2] : the queen's contact episodes from diag.py logs ("C" lines, R9).
An episode starts at a contact turn (enemy head within its full strike reach + 3) after >= 5 rounds without one.
Survival = the queen is still alive 10 rounds after the episode started (truth from the engine output).
Also: how often the R8 trigger fired, how often the search (log-only or on) disagreed with the plain score,
search effort (BFS cells) and aborts. With two folders, prints both for comparison."""
import sys, glob, re, os, collections
def analyse(d):
    st = collections.Counter(); work = []
    for out in sorted(glob.glob(f'{d}/*.out')):
        base = out[:-4]; side = base[-2]; q = '0' if side == 'A' else '1'
        m = re.search(r'round (\d+): bot %s \(team %s\) died' % (q, side), open(out).read()); death = int(m.group(1)) if m else None
        rounds = []
        for l in open(base + '.diag'):
            if not l.startswith(f'C {side} '): continue
            p = l.split(); r = int(p[2]); kv = dict(re.findall(r'([a-z]+)(-?[\w.]+)', ' '.join(p[3:]).split('|')[0]))
            rounds.append(r); st['turns'] += 1
            if kv.get('trig') == '1':
                st['trig'] += 1
                if kv.get('ran') == '1':
                    st['ran'] += 1; work.append(int(kv['work']))
                    if kv['search'] != kv['heur']: st['search_disagrees'] += 1
                    if kv['final'] != kv['heur']: st['final_changed'] += 1
                if kv.get('abort') == '1': st['aborted'] += 1
        last = -100
        for r in rounds:
            if r - last >= 5:
                st['episodes'] += 1
                if death is None or death > r + 10: st['survived10'] += 1
                elif death is not None: st['died_within10'] += 1
            last = r
        st['games'] += 1; st['queen_deaths'] += death is not None
    s = dict(st)
    if st['episodes']: s['survival10'] = round(st['survived10'] / st['episodes'], 3)
    if work: work.sort(); s['work_p50'] = work[len(work) // 2]; s['work_max'] = work[-1]
    return s
for d in sys.argv[1:]: print(os.path.basename(d.rstrip('/')), analyse(d))
