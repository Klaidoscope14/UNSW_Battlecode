#!/usr/bin/env python3
"""topo_check.py DUMP : independent check of the bot's topology (Architecture_v2 7b).

Build a copy of the bot with `#define TOPO_DIAG <round>` and play one game with FLEET_DIAG=<file>. Our queen writes
her known edges and her computed bridges / loop-region sizes at that round. This script recomputes both from the
edges with a separate recursive Tarjan and reports any difference."""
import sys
sys.setrecursionlimit(100000)

E_UNK, E_OPEN, E_KELP, E_PORTAL = 0, 1, 2, 3
DX, DY = (0, 1, 0, -1), (-1, 0, 1, 0)


def check(block):
    kv = {}
    for line in block:
        parts = line.split(' ', 1)
        kv[parts[0]] = parts[1] if len(parts) > 1 else ''
    W, H = int(block[[i for i, l in enumerate(block) if l.startswith('W ')][0]].split()[1]), \
        int(block[[i for i, l in enumerate(block) if l.startswith('W ')][0]].split()[3])
    N = W * H
    et = [int(ch) for ch in kv['EDGES'].strip()]
    partner = [-1] * (2 * N)
    for pr in kv['PARTNERS'].split():
        a, b = map(int, pr.split(':'))
        partner[a], partner[b] = b, a
    bot_bridges = set(map(int, kv['BRIDGES'].split()))
    bot_caps = list(map(int, kv['CAPS'].split()))

    def nb(c, d):
        x, y = (c % W + DX[d]) % W, (c // W + DY[d]) % H
        return y * W + x

    def code(c, d):
        return [c * 2, nb(c, 1) * 2 + 1, nb(c, 2) * 2, c * 2 + 1][d]

    def cross(c, d):
        k = code(c, d)
        t = et[k]
        if t == E_OPEN: return nb(c, d)
        if t == E_KELP: return -1
        if t == E_UNK: return -2
        p = partner[k]
        if p < 0: return -3
        pc = p >> 1
        if (p & 1) == 0:
            return pc if d == 2 else nb(pc, 0)
        return pc if d == 1 else nb(pc, 3)

    def key(c, d):
        k = code(c, d)
        return min(k, partner[k]) if et[k] == E_PORTAL and partner[k] >= 0 else k

    adj = [[] for _ in range(N)]
    for c in range(N):
        for d in range(4):
            n = cross(c, d)
            if n >= 0 and n != c:
                adj[c].append((n, key(c, d)))
    tin, low, bridges, t = [-1] * N, [0] * N, set(), [0]

    def dfs(v, pk):
        tin[v] = low[v] = t[0]; t[0] += 1
        for u, k in adj[v]:
            if k == pk: continue
            if tin[u] >= 0:
                low[v] = min(low[v], tin[u])
            else:
                dfs(u, k)
                low[v] = min(low[v], low[u])
                if low[u] > tin[v]: bridges.add(k)
    for s in range(N):
        if tin[s] < 0: dfs(s, -1)
    comp = [-1] * N; size = []
    for s in range(N):
        if comp[s] >= 0: continue
        cid = len(size); stack = [s]; comp[s] = cid; n = 0
        while stack:
            c = stack.pop(); n += 1
            for u, k in adj[c]:
                if k not in bridges and comp[u] < 0:
                    comp[u] = cid; stack.append(u)
        size.append(n)
    caps = [size[comp[c]] for c in range(N)]
    ok_b, ok_c = bridges == bot_bridges, caps == bot_caps
    print(f"{block[0]}  bridges {len(bridges)} (bot {len(bot_bridges)}) {'OK' if ok_b else 'MISMATCH'}; "
          f"region sizes {'OK' if ok_c else 'MISMATCH (%d cells)' % sum(a != b for a, b in zip(caps, bot_caps))}")
    return ok_b and ok_c


blocks, cur = [], None
for line in open(sys.argv[1]).read().splitlines():
    if line.startswith('round '):
        cur = [line]; blocks.append(cur)
    elif cur is not None:
        cur.append(line)
res = [check(b) for b in blocks]
print("ALL OK" if all(res) else "PROBLEM")
sys.exit(0 if all(res) else 1)
