"""master.py FOLDER : one row per game (all <Map>/Elo-*.txt under FOLDER, duplicates skipped) with the
facts needed to explain a loss, written to FOLDER/analysis/master.csv, plus a readable summary by loss
type and by map on stdout.

Columns (us = our team, them = the opponent):
  game, map, elo, team, first, loss, rounds
  qU_died/qU_cause/qU_len/qU_status/qU_max     our queen: death round, cause, length then, what it was doing, max length
  qT_died/qT_cause/qT_len/qT_max/qT_by_us       their queen; killed in a head-on with one of ours?
  queen_first                                   whose queen died first ('us', 'them', 'none')
  n50..nEnd_us/them, tl.., lg..                 dragons / total length / longest at r50, 100, 200, 300, end
  lead4                                         first round they had 4+ more dragons
  pearls_us/them, eff50_us/them, fount_us/them  pearls eaten, pearls per dragon-turn in r1-50, fountain pearls
  deaths_us_<cause>, headon_attack/pearl/other  our deaths; our head-on deaths by what our dragon was doing
  infant_us                                     non-combat deaths within 10 rounds of birth
  small_rounds                                  rounds with 3 or fewer of our dragons
  giant_r                                       first round their longest dragon was 5+ longer than ours
  agg_dragon/agg_queen                          their approach lift toward our dragons / our queen (3-5 cells)
  portal_us/them, reach_us/them                 portal crossings; share of the map's cells their heads visited
  st_<word>                                     share of our dragon-turns with that status word
"""
import sys, os, re, glob, csv, hashlib, collections, statistics
from ladderlog import Game, DIRS

CHECK = (50, 100, 200, 300)
ATTACK = ('snipe', 'attack', 'hunt', 'pack', 'pack-strike', 'intercept', 'escort-hit', 'rescue', 'hunter', 'lurk', 'push', 'regicide', 'last-attack')
WORDS = ('pearl', 'explore', 'camp', 'rest', 'home', 'late-safe', 'bunker', 'post', 'guard', 'guard-line', 'screen', 'hunt',
         'hunt-queen', 'hunter', 'pack', 'lurk', 'feed', 'feed-leader', 'defend', 'portal', 'escort-hit', 'rescue', 'doomed', 'grow-fleet')


def status_word(text):
    for w in re.findall(r'[a-z][a-z-]+', text or ''):
        if w not in ('cmb', 'col', 'exp', 'prt', 'nrm', 'war', 'keep', 'farm'):
            return w
    return ''


def fast_tiles(g):
    from ladderlog import MAPDIR
    out = set()
    for l in open(os.path.join(MAPDIR, g.mapkey + '.map')):
        p = l.split()
        if p and p[0] == 'TILE' and p[1] != 'COUNT' and p[3] != '0' and int(p[4]) <= 5:
            out.add((int(p[1]), int(p[2])))
    return out


def canon_team(name):
    n = re.sub(r'\.txt$', '', name or '?').strip()
    n = re.sub(r'[^0-9A-Za-z]+', ' ', n).strip().lower()
    return {'migabot': 'miga bot'}.get(n.replace(' ', ''), n) or '?'


def row_for(path):
    g = Game(path); m = g.map; S = ('us', 'opp')
    fast = fast_tiles(g)
    R = {'game': os.path.basename(path)[:-4], 'map': g.mapkey, 'elo': getattr(g, 'opp_elo', ''), 'team': canon_team(g.opp_name),
         'first': 'us' if g.starter_us else 'them'}
    snap = {}; lead4 = None; giant = None; small = 0
    status = {}; stc = collections.Counter(); st_total = 0
    eaten = collections.Counter(); e50 = collections.Counter(); t50 = collections.Counter(); fount = collections.Counter()
    deaths = collections.Counter(); headon = collections.Counter(); infant = 0
    portal = collections.Counter(); visited = {'us': set(), 'opp': set()}; prev = {}
    qmax = {'us': 0, 'opp': 0}
    pre = {}; appr = collections.defaultdict(lambda: [0, 0.0])
    qinfo = {}

    def onround(gg, r):
        nonlocal lead4, giant, small, st_total
        rr = r - 1
        ids = {s: [i for i in g.alive if g.side(i) == s and g.body[i]] for s in S}
        if rr in CHECK or rr >= 1:
            snap[rr] = {s: (len(ids[s]), sum(len(g.body[i]) for i in ids[s]), max((len(g.body[i]) for i in ids[s]), default=0)) for s in S}
        a, b = len(ids['us']), len(ids['opp'])
        if rr >= 1:
            if lead4 is None and b >= a + 4:
                lead4 = rr
            if a <= 3:
                small += 1
            if giant is None and snap[rr]['opp'][2] >= snap[rr]['us'][2] + 5:
                giant = rr
        for s in S:
            qi = g.US if s == 'us' else g.OPP
            if qi in g.alive:
                qmax[s] = max(qmax[s], len(g.body[qi]))
            for i in ids[s]:
                visited[s].add(g.body[i][0])
        for i in ids['us']:
            st_total += 1
            stc[status_word(status.get(i, ''))] += 1
        # approach tendencies of their heads (3-5 cells from one of ours)
        pre.clear()
        heads = {i: g.body[i][0] for i in ids['us'] + ids['opp']}
        for i in ids['opp']:
            best = None
            for k in ids['us']:
                d = m.dist(heads[i], heads[k])
                if 3 <= d <= 5 and (best is None or d < best[0]):
                    best = (d, k)
            if best:
                pre[i] = (heads[i], best[0], heads[best[1]], best[1] == g.US)

    def onevent(gg, r, kind, i, d):
        nonlocal infant
        if kind == 'log':
            status[i] = d
        elif kind == 'move':
            s = g.side(i)
            p = prev.get(i)
            if p is not None and m.portal_edge(p[0], p[1], d['dir']):
                dx, dy = DIRS[d['dir']]
                if ((p[0] + dx) % m.W, (p[1] + dy) % m.H) != d['to']:
                    portal[s] += 1
            prev[i] = d['to']
            if d['step'] == 1 and r <= 50:
                t50[s] += 1
            if d['eat']:
                eaten[s] += 1
                if r <= 50:
                    e50[s] += 1
                if d['to'] in fast:
                    fount[s] += 1
            if s == 'opp' and d['of'] == 1 and i in pre:
                h, d0, tgt, isq = pre.pop(i)
                opts = [m.nb(h[0], h[1], dd) for dd in 'NESW' if not m.kelp_edge(h[0], h[1], dd)]
                base = sum(1 for o in opts if m.dist(o, tgt) < d0) / max(1, len(opts))
                a = appr['queen' if isq else 'dragon']
                a[0] += m.dist(d['to'], tgt) < d0; a[1] += base
        elif kind == 'split':
            prev[d['child']] = None
        elif kind == 'die':
            s = g.side(i)
            if i in (0, 1):
                qinfo[s] = (r, d['why'], len(d['body']), status_word(status.get(i, '')) if s == 'us' else '', d['body'][0] if d['body'] else None)
            if s == 'us':
                deaths[d['why']] += 1
                if d['why'] == 'head-on collision':
                    w = status_word(status.get(i, ''))
                    headon['attack' if w in ATTACK else 'pearl' if w == 'pearl' else 'other'] += 1
                elif r - g.born.get(i, 0) <= 10 and g.born.get(i, 0) > 0:
                    infant += 1

    g.run(onround=onround, onevent=onevent)
    last = max(snap)
    # end state -> loss type
    us_alive = snap[last]['us'][0]
    qU_alive = g.US in g.alive; qT_alive = g.OPP in g.alive
    if us_alive == 0:
        loss = 'elimination'
    elif qU_alive or qT_alive:
        loss = 'longer queen'
    else:
        loss = 'longest dragon'
    R.update(loss=loss, rounds=g.maxround)
    qu = qinfo.get('us'); qt = qinfo.get('opp')
    R.update(qU_died=qu[0] if qu else '', qU_cause=qu[1] if qu else '', qU_len=qu[2] if qu else (len(g.body[g.US]) if qU_alive else ''),
             qU_status=qu[3] if qu else '', qU_max=qmax['us'],
             qT_died=qt[0] if qt else '', qT_cause=qt[1] if qt else '', qT_len=qt[2] if qt else (len(g.body[g.OPP]) if qT_alive else ''),
             qT_max=qmax['opp'])
    # their queen killed by one of ours (head-on, one of our dragons died next to her the same round)?
    by_us = ''
    if qt and qt[1] == 'head-on collision':
        by_us = 'no'
        for rr, t in g.events:
            if rr == qt[0]:
                mm = re.match(r'dragon (\d+)( \(queen\))? dies\. Reason: head-on', t)
                if mm and g.tm(int(mm.group(1))) == g.US:
                    by_us = 'yes'
    R['qT_by_us'] = by_us
    R['queen_first'] = 'none' if not qu and not qt else 'us' if qu and (not qt or qu[0] < qt[0]) else 'them' if qt and (not qu or qt[0] < qu[0]) else 'same'
    for c in CHECK + ('End',):
        rr = last if c == 'End' else min(c, last)
        for s, tag in (('us', 'us'), ('opp', 'them')):
            n, tl, lg = snap[rr][s]
            R[f'n{c}_{tag}'] = n; R[f'tl{c}_{tag}'] = tl; R[f'lg{c}_{tag}'] = lg
    R.update(lead4=lead4 if lead4 is not None else '', giant_r=giant if giant is not None else '',
             pearls_us=eaten['us'], pearls_them=eaten['opp'],
             eff50_us=round(e50['us'] / max(1, t50['us']), 3), eff50_them=round(e50['opp'] / max(1, t50['opp']), 3),
             fount_us=fount['us'], fount_them=fount['opp'], infant_us=infant, small_rounds=small,
             headon_attack=headon['attack'], headon_pearl=headon['pearl'], headon_other=headon['other'],
             portal_us=portal['us'], portal_them=portal['opp'],
             reach_us=round(len(visited['us']) / m.N, 2), reach_them=round(len(visited['opp']) / m.N, 2))
    for k in ('head-on collision', 'hit itself', 'hit a wall', 'hit a body'):
        R['deaths_' + k.split()[-1]] = deaths[k]
    for k, (c, b) in (('dragon', appr['dragon']), ('queen', appr['queen'])):
        R['agg_' + k] = round(c / b, 2) if b >= 5 else ''
    for w in WORDS:
        R['st_' + w] = round(stc[w] / max(1, st_total), 3)
    return R


def main():
    root = sys.argv[1]
    rows = []; seen = set()
    for p in sorted(glob.glob(os.path.join(root, '*', 'Elo-*.txt'))):
        if sum(1 for _ in open(p)) < 5:
            continue
        h = hashlib.md5(open(p, 'rb').read()).hexdigest()
        if h in seen:
            continue
        seen.add(h)
        r = row_for(p); r['game'] = os.path.relpath(p, root)[:-4]
        rows.append(r)
    os.makedirs(os.path.join(root, 'analysis'), exist_ok=True)
    keys = list(rows[0].keys())
    with open(os.path.join(root, 'analysis', 'master.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=keys); w.writeheader(); w.writerows(rows)
    print(f'{len(rows)} games -> {root}/analysis/master.csv ({len(keys)} columns)')


if __name__ == '__main__':
    main()
