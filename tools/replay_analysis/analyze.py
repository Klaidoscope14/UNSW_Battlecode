"""Per-game metrics for a ladder log.  python3 analyze.py <log.txt> [more logs...]
Writes the report to stdout. See README.md for what each section means."""
import sys, re, collections, statistics
from ladderlog import Game, board

PHASES = [(0, 50), (50, 100), (100, 200), (200, 300), (300, 400), (400, 501)]
MARKS = [1, 10, 20, 30, 50, 75, 100, 125, 150, 200, 250, 300, 350, 400, 450, 500]


def phase(r):
    for a, b in PHASES:
        if a <= r < b:
            return f'{a}-{b - 1 if b < 501 else 500}'
    return '?'


def reach(L):
    return (L + 3) // 4 + max(0, L - 2)   # max steps in one turn (free + paid)


def our_reason(text):
    """Our indicator: '[QUEEN |LEAD ]role mode reason L# [s#] [SCR|GRD]' -> reason."""
    p = text.split()
    p = [w for w in p if w not in ('QUEEN', 'LEAD', 'SCR', 'GRD') and not re.fullmatch(r'L\d+|s\d', w)]
    return ' '.join(p[2:]) if len(p) > 2 else ' '.join(p)


def med(v):
    return statistics.median(v) if v else float('nan')


def analyze(path, out):
    g = Game(path); m = g.map
    S = ('us', 'opp')
    P = lambda *a: print(*a, file=out)

    timeline = []
    eaten = {s: 0 for s in S}; eaten_at = {s: {} for s in S}
    eaten_half = {s: collections.Counter() for s in S}
    drops_eaten = {s: collections.Counter() for s in S}   # by whose death the drop came from
    near_spawn = {s: [] for s in S}; camp = {s: 0 for s in S}; spawns = 0
    appear = {}; latency = {s: [] for s in S}
    splits = {s: [] for s in S}; split_phase = {s: collections.Counter() for s in S}
    turns = {s: 0 for s in S}; multi = {s: 0 for s in S}; paid = {s: 0 for s in S}; portal = {s: 0 for s in S}
    deaths = {s: collections.Counter() for s in S}; death_phase = {s: collections.Counter() for s in S}
    death_list = []
    our_reasons = collections.defaultdict(collections.Counter)
    opp_ind = collections.Counter()
    death_reason = collections.Counter()        # (cause, our reason at death)
    hit_body_owner = collections.Counter()
    qlen = {s: [] for s in S}; qdeath = {}
    q_exposed = {s: 0 for s in S}; q_rounds = {s: 0 for s in S}; q_escort = {s: [] for s in S}
    q_corridor = {s: 0 for s in S}; q_spawn_dist = {s: [] for s in S}
    q_tail = {s: collections.deque(maxlen=8) for s in S}
    q_board = {}
    drop_cells = {}                             # cell -> (round, side of the dead dragon, was_queen)
    recycled = {s: 0 for s in S}; selfish = {s: 0 for s in S}
    last_move = {}                              # id -> info of the latest step
    sprint_kills = collections.Counter()
    q_gain = {s: collections.Counter() for s in S}   # queen length gains: 'drop' / 'spawn'
    max_units = {s: 0 for s in S}
    count_hist = []
    pending_die = []
    validation = {'moves': 0, 'nonadj': 0}
    prev_head = {}

    def alive_side(s):
        return [i for i in g.alive if g.side(i) == s]

    def onround(gg, r):
        rr = r - 1
        # pair the head-on deaths of the round that just ended
        if pending_die:
            ho = [d for d in pending_die if d['why'] == 'head-on collision']
            used = set()
            for a in ho:
                if a['id'] in used:
                    continue
                cand = [b for b in ho if b['id'] != a['id'] and b['id'] not in used and m.dist(a['cell'], b['cell']) <= 2]
                if cand:
                    b = min(cand, key=lambda b: m.dist(a['cell'], b['cell']))
                    used |= {a['id'], b['id']}
                    if a['side'] != b['side']:
                        u, v = (a, b) if a['side'] == 'us' else (b, a)
                        lab = lambda d: 'Q' if d['queen'] else 'L2' if d['len'] <= 2 else 'L3-5' if d['len'] <= 5 else 'L6-9' if d['len'] <= 9 else 'L10+'
                        sprint_kills[('trade', 'us:' + lab(u), 'opp:' + lab(v), 'opp-sprinted' if v['sprint'] else 'opp-walked',
                                      'us-sprinted' if u['sprint'] else 'us-walked')] += 1
                    else:
                        sprint_kills[('friendly', a['side'])] += 1
                else:
                    sprint_kills[('unpaired', a['side'], 'Q' if a['queen'] else 'L%d' % a['len'])] += 1
            pending_die.clear()
        ids = {s: alive_side(s) for s in S}
        for s in S:
            max_units[s] = max(max_units[s], len(ids[s]))
        count_hist.append((rr, len(ids['us']), len(ids['opp'])))
        if rr in MARKS or rr == g.maxround:
            row = [rr]
            for s in S:
                lens = [len(g.body[i]) for i in ids[s]]
                qi = g.US if s == 'us' else g.OPP
                row += [len(ids[s]), sum(lens), len(g.body[qi]) if qi in g.alive else 0, max(lens or [0]), eaten[s]]
            timeline.append(row)
        for s in S:
            qi = g.US if s == 'us' else g.OPP
            if qi not in g.alive:
                continue
            qh = g.body[qi][0]; L = len(g.body[qi])
            q_rounds[s] += 1
            qlen[s].append(L)
            q_spawn_dist[s].append(m.dist(qh, g.spawn[g.US if s == 'us' else g.OPP]))
            if m.open_sides(*qh) <= 2:
                q_corridor[s] += 1
            other = 'opp' if s == 'us' else 'us'
            exposed = False; nearE = []
            for j in ids[other]:
                d = m.dist(qh, g.body[j][0])
                if d <= reach(len(g.body[j])) + 1:
                    exposed = True
                if d <= 8:
                    nearE.append((d, len(g.body[j]), j))
            if exposed:
                q_exposed[s] += 1
            q_escort[s].append(sum(1 for j in ids[s] if j != qi and m.dist(qh, g.body[j][0]) <= 3))
            q_tail[s].append((rr, L, qh, sorted(nearE)[:4], g.ind.get(qi, '')))
            if rr >= 1:
                q_board[s] = (rr, board(g, qh, 6))

    def onevent(gg, r, kind, i, d):
        nonlocal spawns
        if kind == 'move':
            s = g.side(i)
            validation['moves'] += 1
            ph = prev_head.get(i)
            if ph is not None and m.dist(ph, d['to']) != 1:
                validation['nonadj'] += 1
            # a step that is not the plain neighbour of the old head went through a portal
            if ph is not None:
                dx, dy = {'N': (0, -1), 'E': (1, 0), 'S': (0, 1), 'W': (-1, 0)}[d['dir']]
                if ((ph[0] + dx) % m.W, (ph[1] + dy) % m.H) != d['to']:
                    portal[s] += 1
            prev_head[i] = d['to']
            if d['step'] == 1:
                turns[s] += 1
                if d['of'] >= 2:
                    multi[s] += 1
            if d['paid']:
                paid[s] += 1
            last_move[i] = (r, d)
            if d['eat']:
                eaten[s] += 1
                c = d['to']
                eaten_half[s][g.half(c)] += 1
                if c in appear:
                    latency[s].append(r - appear.pop(c))
                if c in drop_cells:
                    dr, dside, dq = drop_cells.pop(c)
                    drops_eaten[s][('own' if dside == s else 'enemy') + ('-queen' if dq else '')] += 1
                    if dside == s and r - dr <= 3:
                        recycled[s] += 1
                    if (i == g.US and s == 'us') or (i == g.OPP and s == 'opp'):
                        q_gain[s]['drop'] += 1
                elif (i == g.US and s == 'us') or (i == g.OPP and s == 'opp'):
                    q_gain[s]['spawn'] += 1
        elif kind == 'pearl' and d['what'] == 'appears':
            c = d['cell']
            if c in m.tiles:
                spawns += 1
                appear[c] = r
                for s in S:
                    hs = [g.body[j][0] for j in g.alive if g.side(j) == s]
                    if hs:
                        nd = min(m.dist(h, c) for h in hs)
                        near_spawn[s].append(nd)
                        if nd <= 1:
                            camp[s] += 1
        elif kind == 'split':
            s = g.side(i)
            splits[s].append((d['k'], d['parentLen']))
            split_phase[s][phase(r)] += 1
        elif kind == 'log':
            if g.side(i) == 'us':
                our_reasons[phase(r)][our_reason(d)] += 1
            else:
                opp_ind[re.sub(r'\d+', 'N', d)] += 1
        elif kind == 'die':
            s = g.side(i); why = d['why']; body = d['body']
            deaths[s][why] += 1; death_phase[s][(phase(r), why)] += 1
            L = len(body); cell = body[0] if body else None
            lm = last_move.get(i)
            sprint = bool(lm and lm[0] == r and lm[1]['of'] >= 2)
            q = (i == g.US) or (i == g.OPP)
            pending_die.append({'id': i, 'side': s, 'why': why, 'cell': cell, 'len': L, 'queen': q, 'sprint': sprint})
            reason = our_reason(g.ind.get(i, '')) if s == 'us' else ''
            if s == 'us':
                death_reason[(why, reason)] += 1
            death_list.append((r, s, i, why, L, q, reason))
            if why == 'hit a body' and i in g.lastask and cell:
                a = g.lastask[i][1]
                if a and a[0] in 'NESW':
                    tgt = m.nb(cell[0], cell[1], a[-1]) if len(a) == 1 else None
                    if tgt:
                        occ = g.occupied()
                        if tgt in occ:
                            j = occ[tgt][0]
                            hit_body_owner[(s, 'own' if g.side(j) == s else 'enemy')] += 1
            for k, c in enumerate(body):
                if k % 2 == 0:
                    drop_cells[c] = (r, s, q)
            if why in ('hit itself', 'gave no valid action'):
                selfish[s] += 1
            if q:
                qdeath[s] = (r, why, L, list(q_tail[s]), q_board.get(s))

    g.run(onround, onevent)

    # ------------------------------------------------------------------ report
    P('=' * 100)
    P(f'GAME  {g.opp_name} / {g.name}   map {m.W}x{m.H} sym {m.sym}   we {"started (team A)" if g.starter_us else "moved second (team B)"}')
    P(f'RESULT {g.result_line}   last round {g.maxround}')
    P(f'replay check: {validation["moves"]} steps, {validation["nonadj"]} non-adjacent (= portal jumps or parser gaps)')
    P('')
    P('TIMELINE  (dragons / total length / queen length / longest / pearls eaten so far)')
    P(f'{"round":>5} | {"US":^36} | {"OPP":^36}')
    for row in timeline:
        P(f'{row[0]:>5} | ' + ' '.join(f'{v:>6}' for v in row[1:6]) + ' | ' + ' '.join(f'{v:>6}' for v in row[6:11]))
    P(f'max fleet: us {max_units["us"]}  opp {max_units["opp"]}')
    lead = next((rr for rr, a, b in count_hist if b > a + 3), None)
    P(f'first round opp had 4+ more dragons than us: {lead}')
    P('')
    P('ECONOMY')
    P(f'  pearls eaten: us {eaten["us"]}  opp {eaten["opp"]}')
    P(f'  eaten on OUR half / THEIR half:  us {eaten_half["us"]["us"]}/{eaten_half["us"]["opp"]}   opp {eaten_half["opp"]["us"]}/{eaten_half["opp"]["opp"]}')
    P(f'  spawns {spawns}; nearest head at spawn (median): us {med(near_spawn["us"])}  opp {med(near_spawn["opp"])}; '
      f'head adjacent at spawn: us {100 * camp["us"] / max(1, spawns):.0f}%  opp {100 * camp["opp"] / max(1, spawns):.0f}%')
    P(f'  spawn->eaten latency (median rounds): us {med(latency["us"])}  opp {med(latency["opp"])}')
    P(f'  death-drop pearls eaten: us {dict(drops_eaten["us"])}  opp {dict(drops_eaten["opp"])}')
    P(f'  own drops re-eaten within 3 rounds (recycling): us {recycled["us"]}  opp {recycled["opp"]}')
    P(f'  queen length gains: us {dict(q_gain["us"])}  opp {dict(q_gain["opp"])}')
    P('')
    P('SPLITS  (child size x parent length)')
    for s in S:
        ks = collections.Counter(k for k, _ in splits[s]); pl = collections.Counter(p for _, p in splits[s])
        P(f'  {s}: {len(splits[s])} splits; child sizes {dict(sorted(ks.items())[:10])}; parent lengths {dict(sorted(pl.items())[:12])}')
        P(f'      by phase {dict(split_phase[s])}')
    P('')
    P('MOVEMENT')
    for s in S:
        P(f'  {s}: {turns[s]} dragon-turns; multi-step {100 * multi[s] / max(1, turns[s]):.1f}%; paid steps {paid[s]}; portal jumps {portal[s]}')
    P('')
    P('DEATHS by cause')
    for s in S:
        P(f'  {s}: {dict(deaths[s].most_common())}   self-inflicted (hit itself / no valid action): {selfish[s]}')
    P('  by phase:')
    for s in S:
        byp = collections.defaultdict(dict)
        for (ph, why), n in death_phase[s].items():
            byp[ph][why] = n
        for ph in [f'{a}-{b - 1 if b < 501 else 500}' for a, b in PHASES]:
            if ph in byp:
                P(f'    {s} {ph:>8}: {byp[ph]}')
    P(f'  "hit a body": whose body: {dict(hit_body_owner)}')
    P('  head-on pairs (us vs opp lengths, who sprinted):')
    for k, n in sprint_kills.most_common(25):
        P(f'    {n:4d}  {k}')
    P('')
    P('OUR DEATHS by what the dragon was doing (its last status text)')
    for (why, rsn), n in death_reason.most_common(25):
        P(f'    {n:4d}  {why:<22} while "{rsn}"')
    P('')
    P('QUEENS')
    for s in S:
        n = max(1, q_rounds[s])
        P(f'  {s}: alive {q_rounds[s]} rounds; length max {max(qlen[s] or [0])} end {qlen[s][-1] if qlen[s] else 0}; '
          f'inside an enemy strike reach {100 * q_exposed[s] / n:.0f}% of rounds; corridor cells {100 * q_corridor[s] / n:.0f}%; '
          f'own dragons within 3 (median) {med(q_escort[s])}; distance from start (median) {med(q_spawn_dist[s])}')
        if s in qdeath:
            r, why, L, tail, bd = qdeath[s]
            P(f'    DIED round {r}: {why} at length {L}')
            for rr, LL, h, near, ind in tail:
                P(f'      r{rr:<4} L{LL:<3} head {h}  nearest enemy heads (dist,len,id): {near}  "{ind}"')
            if bd:
                P(f'    board at end of round {bd[0]} (Q/q ours, K/k theirs, U/u ours, E/e theirs, * pearl, |- kelp):')
                for line in bd[1].split('\n'):
                    P('      ' + line)
    P('')
    P('OUR STATUS TEXTS by phase (top 8)')
    for ph in [f'{a}-{b - 1 if b < 501 else 500}' for a, b in PHASES]:
        if our_reasons[ph]:
            tot = sum(our_reasons[ph].values())
            P(f'  {ph:>8}: ' + ', '.join(f'{k} {100 * v / tot:.0f}%' for k, v in our_reasons[ph].most_common(8)))
    P('')
    if opp_ind:
        P('OPPONENT STATUS TEXTS (top 15, digits as N)')
        for k, v in opp_ind.most_common(15):
            P(f'    {v:6d}  {k}')
        P('')
    ours = [d for d in death_list if d[1] == 'us']
    P(f'OUR LAST 12 DEATHS (round, id, cause, length, queen?, status)')
    for r, s, i, why, L, q, rsn in ours[-12:]:
        P(f'    r{r:<4} #{i:<5} {why:<22} L{L:<3} {"QUEEN" if q else "":<5} "{rsn}"')
    P('')
    P('DRAGON COUNT every 25 rounds (us / opp)')
    P('    ' + '  '.join(f'r{rr}:{a}/{b}' for rr, a, b in count_hist if rr % 25 == 0))
    return g


if __name__ == '__main__':
    for p in sys.argv[1:]:
        analyze(p, sys.stdout)
