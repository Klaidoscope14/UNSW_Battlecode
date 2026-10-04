"""Opponent (and our) profile.  python3 profile.py <log.txt>...
 - fleet length mix at checkpoints (share of L2 / L3 / L4-5 / L6-9 / L10+)
 - dragon lifespans and births/deaths
 - the queen: length over time, how far she roams, how often she moves vs stays in a small area
 - attacks on our queen: enemy heads within 3 of our queen per round (pressure) and who killed her
 - paid steps: what they were used for (ate a pearl / head-on / neither)
"""
import sys, collections, statistics
from ladderlog import Game

CHECK = [50, 100, 200, 300, 400, 499]


def bucket(L):
    return 'L2' if L <= 2 else 'L3' if L == 3 else 'L4-5' if L <= 5 else 'L6-9' if L <= 9 else 'L10+'


def run(path, out):
    g = Game(path); m = g.map
    P = lambda *a: print(*a, file=out)
    mix = {}; qpath = {'us': [], 'opp': []}
    pressure = {'us': collections.Counter(), 'opp': collections.Counter()}
    paid_use = {'us': collections.Counter(), 'opp': collections.Counter()}
    last_paid = {}
    births = {'us': 0, 'opp': 0}

    def onround(gg, r):
        rr = r - 1
        if rr in CHECK:
            for s in ('us', 'opp'):
                c = collections.Counter(bucket(len(g.body[i])) for i in g.alive if g.side(i) == s)
                n = max(1, sum(c.values()))
                mix[(rr, s)] = (sum(c.values()), {k: round(100 * c[k] / n) for k in ('L2', 'L3', 'L4-5', 'L6-9', 'L10+') if c[k]})
        for s, qi in (('us', g.US), ('opp', g.OPP)):
            if qi in g.alive:
                h = g.body[qi][0]
                qpath[s].append((rr, h, len(g.body[qi])))
                other = 'opp' if s == 'us' else 'us'
                n3 = sum(1 for j in g.alive if g.side(j) == other and m.dist(g.body[j][0], h) <= 3)
                pressure[s][min(n3, 3)] += 1

    def onevent(gg, r, kind, i, d):
        if kind == 'move' and d['paid']:
            last_paid[i] = r
            if d['eat']:
                paid_use[g.side(i)]['ate a pearl on the paid step'] += 1
            else:
                paid_use[g.side(i)]['paid step'] += 1
        elif kind == 'die' and last_paid.get(i) == r:
            paid_use[g.side(i)]['died the same turn (' + d['why'] + ')'] += 1
        elif kind == 'split':
            births[g.side(i)] += 1

    g.run(onround, onevent)
    life = {'us': [], 'opp': []}
    for i, b in g.born.items():
        end = g.died.get(i, g.maxround)
        life[g.side(i)].append(end - b)
    P('=' * 100)
    P(f'PROFILE {g.opp_name} / {g.name}')
    for s in ('us', 'opp'):
        P(f'  {s}: dragons ever {len(life[s])}, born by split {births[s]}, died {sum(1 for i in g.died if g.side(i) == s)}, '
          f'median lifespan {statistics.median(life[s]) if life[s] else 0} rounds')
        P('     length mix: ' + '  '.join(f'r{rr}: n{mix[(rr, s)][0]} {mix[(rr, s)][1]}' for rr in CHECK if (rr, s) in mix))
    for s in ('us', 'opp'):
        qp = qpath[s]
        if not qp:
            continue
        lens = [x[2] for x in qp]
        moves = [m.dist(qp[k][1], qp[k - 1][1]) for k in range(1, len(qp))]
        box = []
        for k in range(10, len(qp)):
            xs = [qp[j][1] for j in range(k - 10, k + 1)]
            box.append(max(m.dist(a, b) for a in xs for b in xs))
        P(f'  {s} queen: rounds {len(qp)}; length at r50/100/200/300/400: '
          + '/'.join(str(next((x[2] for x in qp if x[0] == t), '-')) for t in (50, 100, 200, 300, 400))
          + f'; max {max(lens)}; stays inside a 4-cell spread over 10 rounds {100 * sum(1 for b in box if b <= 4) / max(1, len(box)):.0f}% of the time'
          + f'; enemy heads within 3 of her: 0:{pressure[s][0]} 1:{pressure[s][1]} 2:{pressure[s][2]} 3+:{pressure[s][3]}')
    for s in ('us', 'opp'):
        if paid_use[s]:
            P(f'  {s} paid steps: {dict(paid_use[s])}')


if __name__ == '__main__':
    for p in sys.argv[1:]:
        run(p, sys.stdout)
