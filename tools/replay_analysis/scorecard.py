"""scorecard.py LOG... : a one-page scorecard per game plus a summary table across games.

Each game is checked against fixed targets (from the ladder analyses):
  T1  their queen died first and we still lost ............................ must not happen
  T2  our optional attack deaths while they really had more dragons ......... <= 2
  T3  pearls per dragon-turn in rounds 1-50 ................................. >= theirs / 1.5
  T4  nearest head of ours when a pearl spawns (median) ..................... <= 3 cells
  T5  our head next to the tile when a pearl spawns ......................... >= 20% of spawns
  T6  rounds our queen spends inside an enemy's strike reach ................ <= 10%
  T7  idle circling: our non-queen dragon-turns spent looping a 2x2
      without eating for 8+ rounds (posts / bunkers / rest excluded) ........ <= 10%
  T8  our queen alive at the end (or outliving theirs) ....................... yes
Also reported: deaths within 10 rounds of birth, rounds with 3 or fewer dragons, fountain pearls,
queen loops (intentional) and the idle-circling share of the opponent.

  python3 scorecard.py Stochastic_Parrot_Replays/*.txt
  python3 scorecard.py --summary-only <folder>/*.txt
"""
import sys, os, re, collections, statistics
from ladderlog import Game, MAPS, MAPDIR

ATTACK = ('snipe', 'attack', 'hunt', 'pack', 'pack-strike', 'intercept', 'escort-hit', 'rescue', 'hunter', 'lurk', 'push')
INTENT_LOOP = ('post', 'bunker', 'rest', 'late-safe', 'guard', 'post-trim')
IDLE_WINDOW = 8


def reach(L):
    return (L + 3) // 4 + max(0, L - 2)


def status_word(text):
    for w in re.findall(r'[a-z][a-z-]+', text or ''):
        if w not in ('cmb', 'col', 'exp', 'prt', 'nrm', 'war', 'keep', 'farm'):
            return w
    return ''


def fast_tiles(g):
    out = set()
    for l in open(os.path.join(MAPDIR, MAPS.get(g.name.lower(), g.name.lower()) + '.map')):
        p = l.split()
        if p and p[0] == 'TILE' and p[1] != 'COUNT' and p[3] != '0' and int(p[4]) <= 5:
            out.add((int(p[1]), int(p[2])))
    return out


def in_2x2(g, cells):
    """All cells inside one 2x2 square (wrap-aware)."""
    x0, y0 = cells[0]
    xs = set(); ys = set()
    for x, y in cells:
        dx = (x - x0) % g.map.W; dy = (y - y0) % g.map.H
        dx = dx - g.map.W if dx > g.map.W // 2 else dx
        dy = dy - g.map.H if dy > g.map.H // 2 else dy
        xs.add(dx); ys.add(dy)
    return max(xs) - min(xs) <= 1 and max(ys) - min(ys) <= 1


def score(path):
    g = Game(path); m = g.map
    S = ('us', 'opp')
    fast = fast_tiles(g)
    turns = collections.Counter(); early_turns = collections.Counter(); early_eat = collections.Counter()
    eaten = collections.Counter(); fountain_eat = collections.Counter()
    near_spawn = {s: [] for s in S}; camp = collections.Counter(); spawns = 0
    q_rounds = collections.Counter(); q_exposed = collections.Counter()
    hist = collections.defaultdict(lambda: collections.deque(maxlen=IDLE_WINDOW))
    last_eat = {}
    idle = collections.Counter(); loop_turns = collections.Counter(); intent = collections.Counter()
    q_loop = 0
    small_fleet = 0; rounds = 0
    counts = {}
    attacks = []          # (round, word, outnumbered)
    regicides = 0
    infant = 0; our_deaths = 0
    status = {}

    def onround(gg, r):
        nonlocal small_fleet, rounds, q_loop
        rr = r - 1
        ids = {s: [i for i in g.alive if g.side(i) == s] for s in S}
        counts[rr] = (len(ids['us']), len(ids['opp']))
        if rr >= 1:
            rounds += 1
            small_fleet += len(ids['us']) <= 3
        for s in S:
            qi = g.US if s == 'us' else g.OPP
            if qi in g.alive:
                q_rounds[s] += 1
                qh = g.body[qi][0]
                other = 'opp' if s == 'us' else 'us'
                if any(m.dist(qh, g.body[j][0]) <= reach(len(g.body[j])) + 1 for j in ids[other]):
                    q_exposed[s] += 1
        for i in g.alive:
            if not g.body[i]:
                continue
            h = hist[i]; h.append(g.body[i][0])
            if rr < 1 or len(h) < IDLE_WINDOW:
                continue
            s = g.side(i)
            looping = in_2x2(g, list(h)) and rr - last_eat.get(i, -99) >= IDLE_WINDOW
            if i in (0, 1):
                if looping and s == 'us':
                    q_loop += 1
                continue
            loop_turns[s] += 1
            if looping:
                if s == 'us' and status_word(status.get(i, '')) in INTENT_LOOP:
                    intent[s] += 1
                else:
                    idle[s] += 1

    def onevent(gg, r, kind, i, d):
        nonlocal spawns, regicides, infant, our_deaths
        if kind == 'log':
            status[i] = d
        elif kind == 'move':
            s = g.side(i)
            if d['step'] == 1:
                turns[s] += 1
                if r <= 50:
                    early_turns[s] += 1
            if d['eat']:
                eaten[s] += 1; last_eat[i] = r
                if r <= 50:
                    early_eat[s] += 1
                if d['to'] in fast:
                    fountain_eat[s] += 1
        elif kind == 'pearl' and d['what'] == 'appears' and d['cell'] in m.tiles:
            spawns += 1
            for s in S:
                hs = [g.body[j][0] for j in g.alive if g.side(j) == s and g.body[j]]
                if hs:
                    nd = min(m.dist(h, d['cell']) for h in hs)
                    near_spawn[s].append(nd)
                    camp[s] += nd <= 1
        elif kind == 'die' and g.side(i) == 'us':
            our_deaths += 1
            if d['why'] != 'head-on collision' and r - g.born.get(i, 0) <= 10 and g.born.get(i, 0) > 0:
                infant += 1
            if d['why'] == 'head-on collision' and i not in (0, 1):
                w = status_word(status.get(i, ''))
                if w == 'regicide':
                    regicides += 1
                elif w in ATTACK:
                    u, o = counts.get(r - 1, (0, 0))
                    attacks.append((r, w, o > u))

    g.run(onround=onround, onevent=onevent)

    qd_us, qd_opp = g.died.get(g.US), g.died.get(g.OPP)
    eff = {s: eaten_rate(early_eat[s], early_turns[s]) for s in S}
    med = {s: statistics.median(near_spawn[s]) if near_spawn[s] else float('nan') for s in S}
    adj = {s: camp[s] / max(1, spawns) for s in S}
    expo = q_exposed['us'] / max(1, q_rounds['us'])
    idle_us = idle['us'] / max(1, loop_turns['us']); idle_opp = idle['opp'] / max(1, loop_turns['opp'])
    att_out = sum(1 for a in attacks if a[2])
    lost_after = (not g.won) and qd_opp is not None and (qd_us is None or qd_opp < qd_us)
    queen_ok = qd_us is None or (qd_opp is not None and qd_opp < qd_us)
    T = [
        ('T1', 'their queen died first and we still lost', 'no' if not lost_after else 'YES', not lost_after),
        ('T2', 'optional attack deaths while outnumbered', f'{att_out} (of {len(attacks)} attacks)', att_out <= 2),
        ('T3', 'pearls / dragon-turn r1-50 (us vs them)', f'{eff["us"]:.1%} vs {eff["opp"]:.1%}', eff['us'] * 1.5 >= eff['opp']),
        ('T4', 'nearest head at spawn, median (us vs them)', f'{med["us"]:g} vs {med["opp"]:g}', med['us'] <= 3),
        ('T5', 'head next to the spawn tile (us vs them)', f'{adj["us"]:.0%} vs {adj["opp"]:.0%}', adj['us'] >= 0.20),
        ('T6', 'our queen inside enemy strike reach', f'{expo:.0%} of {q_rounds["us"]} rounds', expo <= 0.10),
        ('T7', 'idle circling (us vs them)', f'{idle_us:.1%} vs {idle_opp:.1%}', idle_us <= 0.10),
        ('T8', 'our queen alive at the end / outlived theirs',
         'alive' if qd_us is None else f'died r{qd_us} ({g.deathwhy.get(g.US, "")})' + (' after theirs' if queen_ok else ''), queen_ok),
    ]
    info = {
        'pearls eaten (us / them)': f'{eaten["us"]} / {eaten["opp"]}',
        'fountain pearls (us / them)': f'{fountain_eat["us"]} / {fountain_eat["opp"]}' if fast else 'map has no fountains',
        'their queen': 'alive' if qd_opp is None else f'died r{qd_opp} ({g.deathwhy.get(g.OPP, "")})',
        'regicide strikes (our dragon died hitting their queen)': str(regicides),
        'our deaths / non-combat deaths within 10 rounds of birth': f'{our_deaths} / {infant}',
        'rounds with 3 or fewer of our dragons': f'{small_fleet} of {rounds}',
        'our queen looping a 2x2 without eating (rounds)': str(q_loop),
        'intentional loops (post / bunker / rest / guard) dragon-turns': str(intent['us']),
    }
    return g, T, info


def eaten_rate(e, t):
    return e / t if t else 0.0


def page(g, T, info):
    out = []
    side = 'we moved first (A)' if g.starter_us else 'we moved second (B)'
    out.append('=' * 92)
    out.append(f'SCORECARD  {g.opp_name} / {g.name}   {side}   last round {g.maxround}')
    out.append(f'{g.result_line}')
    out.append('-' * 92)
    for tid, what, val, ok in T:
        out.append(f'  {"PASS" if ok else "FAIL"}  {tid}  {what:<46} {val}')
    out.append('-' * 92)
    for k, v in info.items():
        out.append(f'        {k:<58} {v}')
    passed = sum(1 for t in T if t[3])
    out.append(f'  targets met: {passed}/{len(T)}')
    return '\n'.join(out)


def main():
    args = sys.argv[1:]
    summary_only = '--summary-only' in args
    paths = [a for a in args if not a.startswith('--')]
    rows = []
    for p in paths:
        g, T, info = score(p)
        if not summary_only:
            print(page(g, T, info))
        rows.append((g, T))
    if len(rows) > 1 or summary_only:
        print('=' * 92)
        print(f'{"SUMMARY":<10}' + '  '.join(f'{t[0]:>3}' for t in rows[0][1]) + '   result')
        for g, T in rows:
            marks = '  '.join(' ok' if t[3] else ' X ' for t in T)
            res = 'WIN ' if g.won else 'LOSS'
            print(f'{g.name[:9]:<10}{marks}   {res} {g.result_line.split(":", 1)[-1].strip()[:40]}')
        tot = [sum(1 for g, T in rows if T[k][3]) for k in range(len(rows[0][1]))]
        print(f'{"met":<10}' + '  '.join(f'{v}/{len(rows)}'.rjust(3) for v in tot))


if __name__ == '__main__':
    main()
