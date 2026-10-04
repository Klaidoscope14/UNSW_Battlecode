"""Deeper per-game questions.  python3 deep.py <log.txt>...
 1. enemy queen death: who/what killed her, could any of our dragons see it (7x7 view), when (if ever) we bunkered
 2. self-inflicted deaths (hit itself / no valid action / hit a wall): forced (no free neighbour) or chosen;
    next to a teammate; drop re-eaten by the same team within 3 rounds
 3. our "doomed" deaths: age of the dragon, open sides at the death cell, what it was doing 3-8 turns before
 4. our snipes / hunts / regicides: what they traded with, fleet ratio at the time
 5. pearl efficiency: pearls per dragon-turn, early game (r<=50) and overall; races lost (our head was nearer
    when the pearl appeared, theirs took it) and races won
 6. opponent sprint attacks (multi-step moves that end in a head-on) and their reach
 7. opponent camping: share of their dragon-turns with the head within 2 of a spawn tile and no pearl eaten
"""
import sys, collections, statistics
from ladderlog import Game, board


def free_neighbours(g, i, occ):
    b = g.body[i]
    if not b:
        return 0
    x, y = b[0]; n = 0
    for d in 'NESW':
        if g.map.kelp_edge(x, y, d):
            continue
        c = g.map.nb(x, y, d)
        if c in occ:
            j, k = occ[c]
            if not (k == len(g.body[j]) - 1 and len(g.body[j]) > 1):
                continue
        n += 1
    return n


def run(path, out):
    g = Game(path); m = g.map
    P = lambda *a: print(*a, file=out)
    eq_death = {}; our_bunker = []
    self_deaths = collections.Counter(); self_examples = []
    doomed = []; status_hist = collections.defaultdict(lambda: collections.deque(maxlen=10))
    attacks = []
    turns = {'us': collections.Counter(), 'opp': collections.Counter()}
    eats = {'us': collections.Counter(), 'opp': collections.Counter()}
    races = collections.Counter(); appear_near = {}
    sprint_hits = collections.Counter(); last_move = {}
    camp_turns = collections.Counter(); spawn_cells = set(m.tiles)
    pending = []; drops = {}
    counts = {}

    def onround(gg, r):
        rr = r - 1
        a = sum(1 for i in g.alive if g.side(i) == 'us'); b = sum(1 for i in g.alive if g.side(i) == 'opp')
        counts[rr] = (a, b)
        # resolve self-death drops: re-eaten by own team within 3 rounds?
        for d in list(pending):
            if rr - d['r'] > 3:
                d['reeaten'] = False
                pending.remove(d)
                self_deaths[(d['side'], d['why'], d['forced'], d['mate'], 'not re-eaten')] += 1

    def onevent(gg, r, kind, i, d):
        if kind == 'log' and g.side(i) == 'us':
            status_hist[i].append((r, d))
            if i == g.US and 'bunker' in d:
                our_bunker.append(r)
        elif kind == 'move':
            s = g.side(i)
            last_move[i] = (r, d)
            if d['step'] == 1:
                ph = 'early' if r <= 50 else 'all'
                turns[s]['early' if r <= 50 else 'late'] += 1
                if s == 'opp' and any(m.dist(d['to'], c) <= 2 for c in spawn_cells) and not d['eat']:
                    camp_turns['near-spawn'] += 1
                if s == 'opp':
                    camp_turns['all'] += 1
            if d['eat']:
                eats[s]['early' if r <= 50 else 'late'] += 1
                c = d['to']
                if c in appear_near:
                    winner_near = appear_near.pop(c)
                    races[(winner_near, 'taken by ' + s)] += 1
                for p in pending:
                    if c in p['cells'] and s == p['side']:
                        pending.remove(p)
                        self_deaths[(p['side'], p['why'], p['forced'], p['mate'], 're-eaten')] += 1
                        break
        elif kind == 'pearl' and d['what'] == 'appears' and d['cell'] in m.tiles:
            c = d['cell']; best = {}
            for j in g.alive:
                s = g.side(j); dd = m.dist(g.body[j][0], c)
                best[s] = min(best.get(s, 99), dd)
            if best:
                u, o = best.get('us', 99), best.get('opp', 99)
                appear_near[c] = 'we were nearer' if u < o else 'they were nearer' if o < u else 'tie'
        elif kind == 'die':
            s = g.side(i); why = d['why']; body = d['body']
            occ = g.occupied()
            L = len(body)
            if body and why in ('hit itself', 'gave no valid action', 'hit a wall'):
                fr = free_neighbours(g, i, occ)
                mate = any(g.side(j) == s and j != i and m.dist(g.body[j][0], body[0]) <= 3 for j in g.alive)
                pending.append({'r': r, 'side': s, 'why': why, 'forced': 'forced' if fr == 0 else 'had a free cell',
                                'mate': 'mate<=3' if mate else 'alone', 'cells': set(body[::2])})
            if i == g.OPP:
                seers = [j for j in g.alive if g.side(j) == 'us' and j != i and body and
                         max(min(abs(g.body[j][0][0] - body[0][0]), m.W - abs(g.body[j][0][0] - body[0][0])),
                             min(abs(g.body[j][0][1] - body[0][1]), m.H - abs(g.body[j][0][1] - body[0][1]))) <= 3]
                killer = None
                eq_death.update({'r': r, 'why': why, 'L': L, 'cell': body[0] if body else None, 'seers': seers,
                                 'board': board(g, body[0], 5) if body else ''})
            if s == 'us':
                hist = list(status_hist[i])
                last = hist[-1][1] if hist else ''
                age = r - g.born.get(i, 0)
                if 'doomed' in last or why in ('hit a wall', 'hit a body', 'hit itself') and 'recycle' not in last and 'feed' not in last:
                    cell = body[0] if body else None
                    before = [h for (rr, h) in hist[:-1]][-6:]
                    doomed.append((r, i, why, L, age, m.open_sides(*cell) if cell else -1,
                                   collections.Counter(w for w in (' '.join(h.split()[2:3]) for h in before)).most_common(3),
                                   [h for h in before][-3:]))
                for key in ('snipe', 'hunt', 'regicide', 'intercept', 'attack', 'pack', 'last-attack'):
                    if key in last.split():
                        lm = last_move.get(i)
                        a, b = counts.get(r - 1, (0, 0))
                        attacks.append((r, key, L, why, a, b))
                        break
            lm = last_move.get(i)
            if why == 'head-on collision' and lm and lm[0] == r and lm[1]['of'] >= 2:
                sprint_hits[(s, lm[1]['of'], L)] += 1

    g.run(onround, onevent)

    P('=' * 100)
    P(f'DEEP  {g.opp_name} / {g.name}   result: {g.result_line}')
    P('')
    P('1. ENEMY QUEEN')
    if eq_death:
        P(f'   died round {eq_death["r"]}: {eq_death["why"]} at length {eq_death["L"]} at {eq_death["cell"]}; '
          f'our dragons with her in their 7x7 view at that moment: {len(eq_death["seers"])} {eq_death["seers"][:6]}')
        for line in eq_death['board'].split('\n'):
            P('      ' + line)
    else:
        P('   survived')
    P(f'   our queen showed "bunker": {len(our_bunker)} turns' + (f' (first r{our_bunker[0]})' if our_bunker else ''))
    P('')
    P('2. SELF-INFLICTED / WALL DEATHS  (side, cause, forced?, teammate within 3?, drop re-eaten by own team <=3 rounds?)')
    for k, v in sorted(self_deaths.items(), key=lambda kv: (-kv[1])):
        P(f'   {v:5d}  {k}')
    P('')
    P('3. OUR NON-COMBAT DEATHS  (round, id, cause, length, age in rounds, open sides at head, what it did before)')
    ages = [d[4] for d in doomed]
    if doomed:
        P(f'   {len(doomed)} deaths; median age {statistics.median(ages)} rounds; died within 10 rounds of birth: '
          f'{sum(1 for a in ages if a <= 10)}; head on a corridor/dead-end cell (<=2 open sides): {sum(1 for d in doomed if 0 <= d[5] <= 2)}')
        prev = collections.Counter()
        for d in doomed:
            for w, n in d[6]:
                prev[w] += n
        P(f'   activity in the turns before (role/mode words): {prev.most_common(8)}')
        for d in doomed[:15]:
            P(f'   r{d[0]:<4} #{d[1]:<5} {d[2]:<20} L{d[3]:<3} age {d[4]:<4} open {d[5]}  last: {d[7]}')
    P('')
    P('4. OUR ATTACK DEATHS  (what the dragon was doing; fleet us/opp just before)')
    ac = collections.Counter((a[1], a[3]) for a in attacks)
    for k, v in ac.most_common():
        P(f'   {v:4d}  {k}')
    behind = sum(1 for a in attacks if a[5] > a[4])
    P(f'   attacks made while the opponent already had more dragons: {behind}/{len(attacks)}')
    for a in attacks[:12]:
        P(f'   r{a[0]:<4} {a[1]:<10} L{a[2]:<3} {a[3]:<20} fleet {a[4]}/{a[5]}')
    P('')
    P('5. PEARL EFFICIENCY')
    for s in ('us', 'opp'):
        te, tl = turns[s]['early'], turns[s]['late']; ee, el = eats[s]['early'], eats[s]['late']
        P(f'   {s}: rounds<=50: {ee} pearls / {te} dragon-turns = {100 * ee / max(1, te):.1f}%;  '
          f'after: {el} / {tl} = {100 * el / max(1, tl):.1f}%')
    P(f'   races (who was nearer when the pearl appeared -> who ate it): {dict(races)}')
    P('')
    P('6. HEAD-ONS STRAIGHT AFTER A MULTI-STEP MOVE  (side, steps, length)')
    for k, v in sprint_hits.most_common(10):
        P(f'   {v:4d}  {k}')
    P('')
    P(f'7. OPPONENT: dragon-turns with the head within 2 of a spawn tile without eating: '
      f'{100 * camp_turns["near-spawn"] / max(1, camp_turns["all"]):.0f}%  (spawn tiles cover {len(spawn_cells)} of {m.N} cells)')


if __name__ == '__main__':
    for p in sys.argv[1:]:
        run(p, sys.stdout)
