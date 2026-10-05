"""ingest.py FOLDER : check every <Map>/Elo-<rating>[-<team>].txt game log under FOLDER.

For each log: map, opponent rating and team, who moved first, the loss type (normalised from the free
text), rounds played, and replay checks:
  - nonadj   steps whose new head is not next to the old one and not a known portal crossing
  - wallbad  "hit a wall" deaths where the map has no kelp on the edge the dragon asked to cross
  - end      who is alive at the end (our queen / their queen / fleet sizes / longest dragons) and
             whether that agrees with the stated loss reason
Logs without game events are listed as unusable.
"""
import sys, os, re, glob, collections
from ladderlog import Game, DIRS

LOSS_TYPES = (('elimination', r'elimin'), ('longer queen', r'queen'), ('longest dragon', r'dragon'))


def loss_type(text):
    t = text.lower()
    for name, rx in LOSS_TYPES:
        if re.search(rx, t):
            return name
    return '?'


def canon_team(name):
    n = re.sub(r'\.txt$', '', name or '?').strip()
    n = re.sub(r'[^0-9A-Za-z]+', ' ', n).strip().lower()
    return {'migabot': 'miga bot', 'starstruck': 'starstruck'}.get(n.replace(' ', ''), n) or '?'


def check(path):
    g = Game(path); m = g.map
    nonadj = 0; wallbad = 0; prev = {}

    def oe(gg, r, kind, i, d):
        nonlocal nonadj, wallbad
        if kind == 'move':
            p = prev.get(i)
            if p is not None and m.dist(p, d['to']) != 1:
                dx, dy = DIRS[d['dir']]
                plain = ((p[0] + dx) % m.W, (p[1] + dy) % m.H)
                if not m.portal_edge(p[0], p[1], d['dir']) or plain == d['to']:
                    nonadj += 1
            prev[i] = d['to']
        elif kind == 'split':
            prev[d['child']] = gg.body[d['child']][0] if gg.body.get(d['child']) else None
        elif kind == 'die' and d['why'] == 'hit a wall' and d['body']:
            a = gg.lastask.get(i)
            if a and a[0] == r and a[1] and a[1][0] in 'NESW':
                hx, hy = d['body'][0]
                if not m.kelp_edge(hx, hy, a[1][0]):
                    wallbad += 1
    g.run(onevent=oe)
    us = [i for i in g.alive if g.tm(i) == g.US]; op = [i for i in g.alive if g.tm(i) == g.OPP]
    lu = max((len(g.body[i]) for i in us), default=0); lo = max((len(g.body[i]) for i in op), default=0)
    qu = len(g.body[g.US]) if g.US in g.alive else 0; qo = len(g.body[g.OPP]) if g.OPP in g.alive else 0
    lt = loss_type(g.reason)
    # what the end state says
    if not us:
        endt = 'elimination'
    elif g.maxround >= 500 and qu != qo:
        endt = 'longer queen' if qo > qu else 'WIN?'
    elif g.maxround >= 500:
        endt = 'longest dragon' if lo > lu else ('total length?' if lo == lu else 'WIN?')
    else:
        endt = 'queen death?' if not qu else '?'
    agree = endt == lt or (lt == 'longer queen' and endt.startswith('queen'))
    return g, dict(map=g.mapkey, elo=getattr(g, 'opp_elo', None), team=canon_team(g.opp_name), first='us' if g.starter_us else 'them',
                   loss=lt, rounds=g.maxround, nonadj=nonadj, wallbad=wallbad, us=len(us), them=len(op), qu=qu, qo=qo, lu=lu, lo=lo,
                   endt=endt, agree=agree, fixed=g.header_fixed)


def main():
    root = sys.argv[1]
    rows = []; bad = []; seen = {}
    for p in sorted(glob.glob(os.path.join(root, '*', 'Elo-*.txt'))):
        n = sum(1 for _ in open(p))
        if n < 5:
            bad.append((p, f'{n} lines: no game events'))
            continue
        import hashlib
        h = hashlib.md5(open(p, 'rb').read()).hexdigest()
        if h in seen:
            bad.append((p, f'duplicate of {os.path.relpath(seen[h], root)}'))
            continue
        seen[h] = p
        try:
            g, r = check(p)
            r['file'] = os.path.relpath(p, root)
            rows.append(r)
        except Exception as ex:  # noqa
            bad.append((p, repr(ex)))
    print(f'{"file":<34}{"map":<14}{"elo":>5} {"team":<18}{"first":<6}{"loss type":<15}{"end state":<15}{"rnd":>4} '
          f'{"us":>3}{"them":>5} {"qU":>3}{"qT":>3} {"lU":>3}{"lT":>4} {"nonadj":>6}{"wall?":>6}')
    for r in rows:
        flag = ('' if r['agree'] else '  <-- reason vs end state') + ('  (header starter corrected from our QUEEN status)' if r['fixed'] else '')
        print(f'{r["file"]:<34}{r["map"]:<14}{r["elo"] or 0:>5} {r["team"][:17]:<18}{r["first"]:<6}{r["loss"]:<15}{r["endt"]:<15}{r["rounds"]:>4} '
              f'{r["us"]:>3}{r["them"]:>5} {r["qu"]:>3}{r["qo"]:>3} {r["lu"]:>3}{r["lo"]:>4} {r["nonadj"]:>6}{r["wallbad"]:>6}{flag}')
    print(f'\nusable logs: {len(rows)}   unusable: {len(bad)}')
    for p, why in bad:
        print(f'  UNUSABLE {os.path.relpath(p, root)}: {why}')
    c = collections.Counter(r['loss'] for r in rows)
    print('loss types (from the header):', dict(c))
    print('loss types (from the end state):', dict(collections.Counter(r['endt'] for r in rows)))


if __name__ == '__main__':
    main()
