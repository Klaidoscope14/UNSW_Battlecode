"""queen_deaths.py FOLDER : every death of OUR queen in the <Map>/Elo-*.txt logs under FOLDER.

Per death: game, round, length, cause, what she was doing, whether their queen was already dead,
the killer (length, did it sprint, how many rounds it was within 4 cells before), enemy heads
within 6 cells in the last 5 rounds, teammates within 3, open sides of her cell, distance from her
spawn, and the last 6 statuses. Then counts by pattern.
"""
import sys, os, re, glob, hashlib, collections
from ladderlog import Game, board


def status_word(text):
    for w in re.findall(r'[a-z][a-z-]+', text or ''):
        if w not in ('cmb', 'col', 'exp', 'prt', 'nrm', 'war', 'keep', 'farm'):
            return w
    return ''


def one(path, boards):
    g = Game(path); m = g.map; Q = g.US
    hist = collections.deque(maxlen=8)
    near_hist = collections.defaultdict(int)   # enemy id -> consecutive rounds within 4
    statuses = collections.deque(maxlen=6)
    res = {}
    last_move = {}

    def onround(gg, r):
        if Q not in g.alive:
            return
        qh = g.body[Q][0]
        enem = [(m.dist(qh, g.body[j][0]), len(g.body[j]), j) for j in g.alive if g.tm(j) != g.US and g.body[j]]
        mates = sum(1 for j in g.alive if j != Q and g.tm(j) == g.US and g.body[j] and m.dist(qh, g.body[j][0]) <= 3)
        close = {j for d, l, j in enem if d <= 4}
        for j in list(near_hist):
            if j not in close:
                near_hist[j] = 0
        for j in close:
            near_hist[j] += 1
        hist.append((r - 1, len(g.body[Q]), qh, sorted(enem)[:3], mates))
        if r - 1 >= 1 and boards is not None:
            res['board'] = board(g, qh, 6)

    def onevent(gg, r, kind, i, d):
        if kind == 'log' and i == Q:
            statuses.append(status_word(d))
        elif kind == 'move':
            last_move[i] = (r, d['of'])
        elif kind == 'die' and i == Q:
            res.update(r=r, why=d['why'], L=len(d['body']), cell=d['body'][0] if d['body'] else None)
            # the killer: an enemy that died head-on the same round next to her
            res['killer'] = None
            for rr, t in g.events:
                if rr == r:
                    mm = re.match(r'dragon (\d+) dies\. Reason: head-on', t)
                    if mm and g.tm(int(mm.group(1))) != g.US:
                        k = int(mm.group(1))
                        res['killer'] = (k, len(g.body.get(k, [])), last_move.get(k, (0, 1))[1] >= 2 and last_move.get(k, (0, 1))[0] == r,
                                         near_hist.get(k, 0))
            res['hist'] = list(hist); res['stat'] = list(statuses)
            res['eq_dead'] = g.OPP not in g.alive and g.OPP in g.died
            res['eq_died'] = g.died.get(g.OPP)
            res['open'] = m.open_sides(*res['cell']) if res['cell'] else None
            res['home'] = m.dist(res['cell'], g.spawn[g.US]) if res['cell'] else None
    g.run(onround=onround, onevent=onevent)
    return g, res


def main():
    root = sys.argv[1]
    seen = set(); out = []
    for p in sorted(glob.glob(os.path.join(root, '*', 'Elo-*.txt'))):
        if sum(1 for _ in open(p)) < 5:
            continue
        h = hashlib.md5(open(p, 'rb').read()).hexdigest()
        if h in seen:
            continue
        seen.add(h)
        g, r = one(p, {})
        if 'r' in r:
            out.append((os.path.relpath(p, root)[:-4], g, r))
    pat = collections.Counter()
    for name, g, r in out:
        k = r['killer']
        hist = r['hist']
        enem5 = max((sum(1 for d, l, j in h[3] if d <= 6) for h in hist[-5:]), default=0)
        mates = max((h[4] for h in hist[-5:]), default=0)
        st = r['stat']
        print(f'{name:<30} r{r["r"]:<4} L{r["L"]:<3} {r["why"]:<18} doing "{st[-1] if st else ""}"  their queen '
              f'{"dead since r" + str(r["eq_died"]) if r["eq_dead"] else "alive"}  open {r["open"]} home-dist {r["home"]}')
        if k:
            print(f'      killer #{k[0]} L{k[1]} {"SPRINT" if k[2] else "walk"}; within 4 of her for {k[3]} rounds before; '
                  f'enemy heads within 6 (max, last 5r) {enem5}; teammates within 3: {mates}; statuses {st}')
        else:
            print(f'      no killer (self/wall/body); enemy heads within 6 {enem5}; teammates within 3: {mates}; statuses {st}')
        lens = [h[1] for h in hist]
        print(f'      her length last rounds {lens}; nearest enemies at r-1 {hist[-1][3] if hist else []}')
        # patterns
        if r['why'] != 'head-on collision':
            pat['self / boxed (wall, body, itself)'] += 1
        elif k and k[3] >= 3:
            pat['stalked: killer within 4 for 3+ rounds'] += 1
        elif k and k[2]:
            pat['sprint strike from range'] += 1
        else:
            pat['walk-in head-on, killer close < 3 rounds'] += 1
    print('\nPATTERNS', dict(pat))
    print('their queen already dead at the time:', sum(1 for _, g, r in out if r['eq_dead']), 'of', len(out))
    print('status at death:', dict(collections.Counter((r['stat'][-1] if r['stat'] else '') for _, g, r in out)))


if __name__ == '__main__':
    main()
