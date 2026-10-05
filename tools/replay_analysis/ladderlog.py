"""Parse and replay a ladder match log (<Map>.txt exported from the replay viewer).

Header lines:
  WHO STARTED MATCH : <team>        (the starter is team A = map team 0; its dragon 0 is its queen)
  REASON FOR WIN/LOSS : <text>      (from our point of view)
  Even numbered dragon represents X , odd represents Y.   (true for the starting dragons only:
                                                            later ids are shared by both teams)
Then "<round>\t<event>" lines.

Teams are tracked properly: starting dragons by the map file, children inherit the parent's team.
Bodies are rebuilt move by move (multi-step moves, eating, paid steps, splits).
"""
import re, collections, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAPDIR = os.path.join(ROOT, 'maps')
MAPS = {'around_unsw': 'unsw', 'australia': 'australia', 'autarky': 'autarky', 'default': 'default', 'deafult': 'default',
        'devil': 'devil', 'islands': 'islands', 'maze': 'maze', 'portals': 'portals', 'prisoners_dilemma': 'dilemma',
        'queen_of_spades': 'queen_of_spades', 'schooltime': 'schooltime', 'slithery_fight': 'slithery_fight',
        'stripes': 'stripes', 'tower_defense': 'tower_defense', 'trauma': 'trauma', 'trophy': 'trophy',
        'weakhold': 'weakhold', 'stronghold': 'stronghold', 'big_empty': 'big_empty'}
DIRS = {'N': (0, -1), 'E': (1, 0), 'S': (0, 1), 'W': (-1, 0)}
OURS = ('Us', 'Low Cortisol', 'Low_Cortisol')


class Map:
    def __init__(s, name):
        s.kelp = set(); s.portal = {}; s.tiles = {}; s.dragons = []; s.sym = '?'
        for l in open(os.path.join(MAPDIR, name + '.map')):
            p = l.split()
            if not p:
                continue
            if p[0] == 'MAP':
                s.W, s.H = int(p[1]), int(p[2])
            elif p[0] == 'SYMMETRY':
                s.sym = p[1]
            elif p[0] == 'EDGE' and p[1] != 'COUNT':
                if p[2] == '1':
                    s.kelp.add(int(p[1]))
                elif p[2] == '2':
                    s.portal[int(p[1])] = p[3]
            elif p[0] == 'TILE' and p[1] != 'COUNT' and len(p) == 5 and p[3] != '0':
                s.tiles[(int(p[1]), int(p[2]))] = (int(p[3]), int(p[4]))
            elif p[0] == 'DRAGON' and p[1] != 'COUNT':
                t = int(p[1]); n = int(p[2]); xs = list(map(int, p[3:3 + 2 * n]))
                s.dragons.append((t, [(xs[2 * i], xs[2 * i + 1]) for i in range(n)]))
        s.N = s.W * s.H

    # Edge numbering verified against every "hit a wall" death (84/84 on 5 maps).
    def eidx(s, x, y, d):
        if d == 'S':
            y += 1; d = 'N'
        if d == 'E':
            x += 1; d = 'W'
        return (2 * y + (0 if d == 'N' else 1)) * (s.W + 1) + x

    def kelp_edge(s, x, y, d):
        return s.eidx(x, y, d) in s.kelp

    def portal_edge(s, x, y, d):
        return s.eidx(x, y, d) in s.portal

    def open_sides(s, x, y):
        return sum(1 for d in 'NESW' if not s.kelp_edge(x, y, d))

    def nb(s, x, y, d):
        dx, dy = DIRS[d]
        return ((x + dx) % s.W, (y + dy) % s.H)

    def dist(s, a, b):
        dx = abs(a[0] - b[0]); dy = abs(a[1] - b[1])
        return min(dx, s.W - dx) + min(dy, s.H - dy)


RX_MOVE = re.compile(r'dragon (\d+) moves ([NESW]) to \((\d+),(\d+)\)(.*)')
RX_SPLIT = re.compile(r'dragon (\d+) splits off dragon (\d+), (\d+) cells')
RX_DIE = re.compile(r'dragon (\d+)( \(queen\))? dies\. Reason: (.*)')
RX_ASK = re.compile(r'dragon (\d+) asks to (?:move ([NESW]+)|split off (\d+) cells)')
RX_LOG = re.compile(r'dragon (\d+): (.*)')
RX_PING = re.compile(r'dragon (\d+) pings (\d+) ([NESW]) and (.*)')
RX_PEARL = re.compile(r'\((\d+),(\d+)\) pearl (appears|taken)')
RX_STEP = re.compile(r'step (\d+) of (\d+)')


class Game:
    def __init__(s, path):
        s.path = path
        s.name = os.path.basename(path)[:-4]
        s.opp_name = os.path.basename(os.path.dirname(os.path.abspath(path))).replace('_Match_Replays', '').replace('_Replays', '')
        lines = open(path).read().split('\n')
        starter = lines[0].split(':', 1)[1].strip()
        s.starter_us = any(o == starter for o in OURS)
        s.result_line = lines[1].strip()
        s.won = lines[1].startswith('REASON FOR WIN')
        s.reason = lines[1].split(':', 1)[1].strip()
        s.map = Map(MAPS.get(s.name.lower(), s.name.lower()))  # local logs use the map file name
        s.US = 0 if s.starter_us else 1      # map team index of our team
        s.OPP = 1 - s.US
        s.team = {}; s.body = {}; s.born = {}; s.died = {}; s.deathwhy = {}; s.parent = {}; s.queen_ids = {0, 1}
        for i, (t, b) in enumerate(s.map.dragons):
            s.team[i] = t; s.body[i] = list(b); s.born[i] = 0
        s.spawn = {t: s.map.dragons[t][1][0] for t in (0, 1)}   # each queen's start
        s.events = []
        for l in lines[3:]:
            p = l.split('\t', 1)
            if len(p) < 2 or not p[0].strip().isdigit():
                continue
            s.events.append((int(p[0]), p[1]))
        s.maxround = max(r for r, _ in s.events)

    def tm(s, i):
        return s.team.get(i, i % 2)

    def side(s, i):
        return 'us' if s.tm(i) == s.US else 'opp'

    def half(s, cell):
        """'us' if the cell is nearer our queen's start than theirs, 'opp' if nearer theirs, '=' on the line."""
        a = s.map.dist(cell, s.spawn[s.US]); b = s.map.dist(cell, s.spawn[s.OPP])
        return 'us' if a < b else 'opp' if b < a else '='

    def run(s, onround=None, onevent=None):
        """Replay all events. onround(game, r) runs at the START of round r (state = end of r-1).
        onevent(game, r, kind, id, data) runs after the state change of each event."""
        cur = 0
        s.pearls = set(); s.alive = set(s.body); s.ind = {}; s.lastask = {}
        for r, t in s.events:
            while r > cur:
                cur += 1
                if onround:
                    onround(s, cur)
            m = RX_MOVE.match(t)
            if m:
                i = int(m.group(1)); x, y = int(m.group(3)), int(m.group(4)); rest = m.group(5)
                if i not in s.body:
                    s.body[i] = [(x, y)]; s.team[i] = i % 2; s.born[i] = 0; s.alive.add(i)
                b = s.body[i]
                ml = re.search(r'length (\d+)', rest)
                newL = int(ml.group(1)) if ml else len(b) + (1 if 'eats a pearl' in rest else 0)  # local logs omit the length
                b.insert(0, (x, y))
                while len(b) > newL:
                    b.pop()
                st = RX_STEP.search(rest)
                info = {'dir': m.group(2), 'to': (x, y), 'eat': 'eats a pearl' in rest,
                        'step': int(st.group(1)) if st else 1, 'of': int(st.group(2)) if st else 1,
                        'paid': 'pays' in rest, 'len': len(b)}
                if info['eat']:
                    s.pearls.discard((x, y))
                if onevent:
                    onevent(s, r, 'move', i, info)
                continue
            m = RX_SPLIT.match(t)
            if m:
                p, c, k = int(m.group(1)), int(m.group(2)), int(m.group(3))
                b = s.body.get(p, [])
                child = b[-k:][::-1] if len(b) >= k else [b[-1]] * k if b else [(0, 0)] * k
                s.body[p] = b[:-k] if len(b) > k else b[:1]
                s.body[c] = child; s.team[c] = s.tm(p); s.born[c] = r; s.parent[c] = p; s.alive.add(c)
                if onevent:
                    onevent(s, r, 'split', p, {'child': c, 'k': k, 'parentLen': len(b)})
                continue
            m = RX_DIE.match(t)
            if m:
                i = int(m.group(1))
                if onevent:
                    onevent(s, r, 'die', i, {'why': m.group(3), 'queen': bool(m.group(2)), 'body': list(s.body.get(i, []))})
                s.died[i] = r; s.deathwhy[i] = m.group(3); s.alive.discard(i)
                continue
            m = RX_ASK.match(t)
            if m:
                i = int(m.group(1)); s.lastask[i] = (r, m.group(2) or ('split' + m.group(3)))
                if onevent:
                    onevent(s, r, 'ask', i, s.lastask[i][1])
                continue
            m = RX_PEARL.match(t)
            if m:
                c = (int(m.group(1)), int(m.group(2)))
                if m.group(3) == 'appears':
                    s.pearls.add(c)
                else:
                    s.pearls.discard(c)
                if onevent:
                    onevent(s, r, 'pearl', None, {'cell': c, 'what': m.group(3)})
                continue
            m = RX_PING.match(t)
            if m:
                if onevent:
                    onevent(s, r, 'ping', int(m.group(1)), {'msg': int(m.group(2)), 'dir': m.group(3), 'res': m.group(4)})
                continue
            m = RX_LOG.match(t)
            if m:
                s.ind[int(m.group(1))] = m.group(2)
                if onevent:
                    onevent(s, r, 'log', int(m.group(1)), m.group(2))
        if onround:
            onround(s, cur + 1)

    def occupied(s):
        occ = {}
        for j in s.alive:
            for k, c in enumerate(s.body[j]):
                occ[c] = (j, k)
        return occ


def board(g, center, radius=6, mark=None):
    """ASCII board around center. Q/q our queen, K/k their queen, U/u ours, E/e theirs (capital = head),
    * pearl, | and - kelp edges, P portal edge."""
    m = g.map; occ = g.occupied(); cx, cy = center; out = []
    for dy in range(-radius, radius + 1):
        y = (cy + dy) % m.H
        top = '   '
        row = f'{y:3d}'
        for dx in range(-radius, radius + 1):
            x = (cx + dx) % m.W
            top += ' ' + ('-' if m.kelp_edge(x, y, 'N') else 'P' if m.portal_edge(x, y, 'N') else ' ')
            ch = '.'
            if (x, y) in occ:
                j, k = occ[(x, y)]
                ours = g.tm(j) == g.US
                if j == g.US:
                    ch = 'q'
                elif j == g.OPP:
                    ch = 'k'
                else:
                    ch = 'u' if ours else 'e'
                if k == 0:
                    ch = ch.upper()
            elif (x, y) in g.pearls:
                ch = '*'
            if mark and (x, y) in mark:
                ch = mark[(x, y)]
            row += ('|' if m.kelp_edge(x, y, 'W') else 'P' if m.portal_edge(x, y, 'W') else ' ') + ch
        out.append(top)
        out.append(row)
    return '\n'.join(out)
