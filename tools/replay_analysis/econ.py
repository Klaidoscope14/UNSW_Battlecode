"""Why does one side eat more?  python3 econ.py <log.txt>...
Per side, over dragon-turns: distance from the head to the nearest spawn tile and to the nearest present pearl,
pearls left next to a head (adjacent at round start, not eaten that round by anyone of that side),
and samples of races lost while our head was nearer (with our nearest dragon's status)."""
import sys, collections, statistics
from ladderlog import Game
from analyze import our_reason


def run(path, out):
    g = Game(path); m = g.map
    P = lambda *a: print(*a, file=out)
    spawn = list(m.tiles)
    # distance to the nearest spawn tile, precomputed per cell (torus manhattan, kelp ignored)
    dsp = {}
    for x in range(m.W):
        for y in range(m.H):
            dsp[(x, y)] = min((m.dist((x, y), c) for c in spawn), default=99)
    stats = {s: {'turns': 0, 'dsp': collections.Counter(), 'dpe': collections.Counter(), 'adj_left': 0, 'adj': 0}
             for s in ('us', 'opp')}
    opening = {s: collections.Counter() for s in ('us', 'opp')}
    eaten_this_round = set(); adj_snapshot = []
    lost = []; appear_info = {}

    def onround(gg, r):
        nonlocal adj_snapshot
        # settle last round's "adjacent pearls"
        for s, i, c in adj_snapshot:
            stats[s]['adj'] += 1
            if c not in eaten_this_round:
                stats[s]['adj_left'] += 1
        eaten_this_round.clear(); adj_snapshot = []
        pearls = list(g.pearls)
        for i in g.alive:
            s = g.side(i); h = g.body[i][0]
            st = stats[s]; st['turns'] += 1
            st['dsp'][min(dsp.get(h, 99), 9)] += 1
            dp = min((m.dist(h, c) for c in pearls), default=99)
            st['dpe'][min(dp, 15)] += 1
            if r <= 51:
                opening[s][our_reason(g.ind.get(i, '')) if s == 'us' else 'x'] += 1
            for c in pearls:
                if m.dist(h, c) == 1:
                    adj_snapshot.append((s, i, c))
                    break

    def onevent(gg, r, kind, i, d):
        if kind == 'move' and d['eat']:
            eaten_this_round.add(d['to'])
            c = d['to']
            if c in appear_info and g.side(i) == 'opp':
                ra, uid, ud, od, ust = appear_info[c]
                if ud < od:
                    lost.append((ra, r, c, uid, ud, od, ust))
            appear_info.pop(c, None)
        elif kind == 'pearl' and d['what'] == 'appears' and d['cell'] in m.tiles:
            c = d['cell']; bu = (99, None); bo = 99
            for j in g.alive:
                dd = m.dist(g.body[j][0], c)
                if g.side(j) == 'us' and dd < bu[0]:
                    bu = (dd, j)
                elif g.side(j) == 'opp':
                    bo = min(bo, dd)
            appear_info[c] = (r, bu[1], bu[0], bo, g.ind.get(bu[1], '') if bu[1] is not None else '')

    g.run(onround, onevent)
    P('=' * 100)
    P(f'ECON  {g.opp_name} / {g.name}')
    for s in ('us', 'opp'):
        st = stats[s]; n = max(1, st['turns'])
        near_sp = sum(v for k, v in st['dsp'].items() if k <= 1) / n
        near_pe = sum(v for k, v in st['dpe'].items() if k <= 3) / n
        far_pe = sum(v for k, v in st['dpe'].items() if k >= 10) / n
        P(f'  {s}: dragon-turns {st["turns"]}; head within 1 of a spawn tile {100 * near_sp:.0f}%; '
          f'a pearl within 3: {100 * near_pe:.0f}%; nearest pearl 10+ away: {100 * far_pe:.0f}%; '
          f'pearl adjacent at round start but not eaten by the side that round: {st["adj_left"]}/{st["adj"]}')
    P(f'  our status words, rounds 1-50: {opening["us"].most_common(8)}')
    P(f'  races lost while our head was nearer: {len(lost)}; samples (appeared, eaten, cell, our id, our dist, their dist, our status):')
    for x in lost[:10]:
        P(f'     r{x[0]}->r{x[1]} {x[2]} #{x[3]} {x[4]} vs {x[5]}  "{x[6]}"')
    sts = collections.Counter(our_reason(x[6]) for x in lost)
    P(f'  what our nearest dragon was doing in those lost races: {sts.most_common(8)}')


if __name__ == '__main__':
    for p in sys.argv[1:]:
        run(p, sys.stdout)
