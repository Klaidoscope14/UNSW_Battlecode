// L1 TOPOLOGY - corridors, pockets and dead ends, free loops for the queen, corridor tests.
// (Architecture_v2 step 7b replaces the walks below with bridge / loop lookups.)
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

bool topo_loop_near(std::vector<int> const& b, int len); // static topology, below

// Open sides of a cell (kelp and unexplored edges excluded): <= 2 is a corridor.
int open_sides(int c)
{
    int k = 0;
    for (int d = 0; d < 4; d++)
        k += cross(c, d) >= 0;
    return k;
}

// A cell inside a 1-wide dead end (a pocket): following the corridor cells
// from it reaches a dead end before any open water.
bool in_pocket(int c)
{
    if (open_sides(c) >= 3)
        return false;
    std::vector<int> q{c};
    std::unordered_set<int> seen{c};
    for (std::size_t i = 0; i < q.size() && q.size() <= 12; i++)
    {
        int const x = q[i];
        int const os = open_sides(x);
        if (os <= 1)
            return true;
        if (os >= 3)
            continue; // opens out this way
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(x, d);
            if (n >= 0 && seen.insert(n).second)
                q.push_back(n);
        }
    }
    return false;
}

// Stepping from `from` into `to`: does the 1-wide corridor ahead end in a
// dead end before it opens out? (Unexplored edges count as closed.)
bool pocket_ahead(int from, int to)
{
    int prev = from, c = to;
    for (int k = 0; k < 24; k++)
    {
        int const os = open_sides(c);
        if (os <= 1)
            return true;
        if (os >= 3)
            return false;
        int next = -1;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n >= 0 && n != prev)
                next = n;
        }
        if (next < 0)
            return true;
        prev = c;
        c = next;
    }
    return false;
}

// F7: stepping from `from` into `to`, does a FULLY SEEN 1-wide corridor end
// in a dead end? Any unexplored edge on the way: unknown, not a dead end.
bool known_dead_end(int from, int to)
{
    int prev = from, c = to;
    for (int k = 0; k < 24; k++)
    {
        int open = 0, unk = 0, next = -1;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n == -2 || n == -3)
                unk++;
            else if (n >= 0)
            {
                open++;
                if (n != prev)
                    next = n;
            }
        }
        if (unk > 0 || open >= 3)
            return false;
        if (open <= 1 || next < 0)
            return true;
        prev = c;
        c = next;
    }
    return false;
}

// A queen can never be trapped while she can reach a free loop longer than
// she is: she can circle it for ever and turn round there (a 2x2 for length
// <= 3, verified in the engine; a k x k ring holds 4(k-1) cells). Unexplored
// edges do not count here: pockets with unseen walls are how queens die.
bool ring_free(int tl, int k)
{
    std::vector<int> ring;
    int c = tl;
    int const dirs[4] = {1, 2, 3, 0}; // E along the top, S down the right, W along the bottom, N up the left
    for (int side = 0; side < 4; side++)
        for (int i = 0; i < k - 1; i++)
        {
            ring.push_back(c);
            if (edgeType[edge_code(c, dirs[side])] != E_OPEN)
                return false;
            c = nb(c, dirs[side]);
        }
    if (c != tl)
        return false;
    for (int x : ring)
        if ((vis[x] && occId[x] >= 0 && occId[x] != me) || wallStamp[x] == wstamp)
            return false;
    return true;
}

bool loop_reachable(std::vector<int> const& b, int len)
{
    int k = 2;
    while (4 * (k - 1) <= len)
        k++;
    if (k > 5)
        return !TOPO_LOOP || topo_loop_near(b, len); // long queens: no k x k ring test; 7b: a loop region longer than her
    BodyScope const sb(b, len);
    std::unordered_map<int, int> dist{{b[0], 0}};
    std::vector<int> q{b[0]};
    for (std::size_t i = 0; i < q.size(); i++)
    {
        int const c = q[i], dc = dist[c];
        if (ring_free(c, k))
            return true;
        if (dc >= QUEEN_LOOP_DEPTH)
            continue;
        for (int d = 0; d < 4; d++)
        {
            if (edgeType[edge_code(c, d)] != E_OPEN)
                continue;
            int const n = nb(c, d);
            if (dist.count(n) || !passable(n, dc + 1))
                continue;
            dist[n] = dc + 1;
            q.push_back(n);
        }
    }
    return false;
}

// Optional queen split: does she still have a loop with the child's body (the
// tail part) standing where it is?
bool split_keeps_loop(int child)
{
    if (!QUEEN_LOOP_SAFE || static_cast<int>(body.size()) < L)
        return true;
    std::vector<int> front(body.begin(), body.begin() + (L - child));
    WallScope ws;
    for (int i = L - child; i < L; i++)
        ws.wall(body[i]);
    return loop_reachable(front, L - child);
}

// Does a 1-wide corridor lead from `start` to `goal` (within maxSteps)?
// Walks only corridor cells (<= 2 open sides) and stops at visible dragons,
// so it finds the queen's head from the side she is facing.
bool corridor_leads_to(int start, int goal, int maxSteps)
{
    std::unordered_map<int, int> dist{{start, 0}};
    std::vector<int> q{start};
    for (std::size_t i = 0; i < q.size(); i++)
    {
        int const c = q[i], dc = dist[c];
        if (c == goal)
            return true;
        if (dc >= maxSteps || (c != start && open_sides(c) > 2))
            continue;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n < 0 || dist.count(n))
                continue;
            if (n != goal && vis[n] && occId[n] >= 0)
                continue;
            dist[n] = dc + 1;
            q.push_back(n);
        }
    }
    return false;
}


// ---------------------------------------------------------------------------
// Static topology of the known map (Architecture_v2 step 7b)
// ---------------------------------------------------------------------------
// A dragon can never turn round where there is no cycle: its own tail blocks
// the cell it came from. So known water splits into LOOP REGIONS (2-edge-
// connected components: every edge in one lies on a cycle) joined by BRIDGES.
// Crossing a bridge leads into a BRANCH. A branch with no loop longer than the
// dragon and no unexplored edge is a trap it can only leave by splitting.
// Edges are the known crossings (open water and portals with a known far end);
// an unexplored edge or an unlocated portal marks its cell as FRONTIER (the
// map may go on there). Rebuilt lazily whenever an edge is learned.
struct Branch
{
    int cells = 0;
    int maxCap = 0;    // largest loop region in the branch (0: no cycle at all)
    int spawns = 0;    // known spawn tiles
    bool frontier = false;
};
Branch merge(Branch a, Branch const& b)
{
    a.cells += b.cells;
    a.maxCap = std::max(a.maxCap, b.maxCap);
    a.spawns += b.spawns;
    a.frontier = a.frontier || b.frontier;
    return a;
}
struct Topology
{
    int version = -1;
    std::vector<int> comp;            // cell -> loop region
    std::vector<int> cap;             // region -> cells (its longest cycle is at most this; a region of 1 has none)
    std::vector<char> bridge;         // edge key -> crossing it splits the known map
    std::vector<int> parent;          // region -> parent region in the bridge forest (-1: a root)
    std::vector<Branch> own, down, up; // region itself / its subtree / everything beyond its parent bridge
} topo;

// One physical edge seen from either side: a portal's two edges count as one.
int edge_key(int c, int d)
{
    int const code = edge_code(c, d);
    return edgeType[code] == E_PORTAL && edgePartner[code] >= 0 ? std::min(code, edgePartner[code]) : code;
}

void topo_build()
{
    topo.version = edgeVersion;
    static std::vector<int> start, to, key, tin, low, order;
    start.assign(N + 1, 0);
    to.clear();
    key.clear();
    static std::vector<char> front;
    front.assign(N, 0);
    for (int c = 0; c < N; c++)
    {
        start[c] = static_cast<int>(to.size());
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n == -2 || n == -3)
                front[c] = 1;
            else if (n >= 0 && n != c)
            {
                to.push_back(n);
                key.push_back(edge_key(c, d));
            }
        }
    }
    start[N] = static_cast<int>(to.size());

    // Bridges: iterative Tarjan (no recursion - the wasm stack is small). The
    // edge we arrived by is skipped by its key, so parallel edges are cycles.
    topo.bridge.assign(2 * N, 0);
    tin.assign(N, -1);
    low.assign(N, 0);
    struct Frame
    {
        int v, parentKey, next;
    };
    static std::vector<Frame> st;
    st.clear();
    int timer = 0;
    for (int s = 0; s < N; s++)
    {
        if (tin[s] >= 0)
            continue;
        tin[s] = low[s] = timer++;
        st.push_back({s, -1, start[s]});
        while (!st.empty())
        {
            int const v = st.back().v;
            if (st.back().next < start[v + 1])
            {
                int const i = st.back().next++;
                int const u = to[i];
                if (key[i] == st.back().parentKey)
                    continue;
                if (tin[u] >= 0)
                    low[v] = std::min(low[v], tin[u]);
                else
                {
                    tin[u] = low[u] = timer++;
                    st.push_back({u, key[i], start[u]});
                }
            }
            else
            {
                int const pk = st.back().parentKey;
                st.pop_back();
                if (!st.empty())
                {
                    int const p = st.back().v;
                    low[p] = std::min(low[p], low[v]);
                    if (low[v] > tin[p])
                        topo.bridge[pk] = 1;
                }
            }
        }
    }

    // Loop regions: connected by non-bridge edges.
    topo.comp.assign(N, -1);
    topo.cap.clear();
    topo.own.clear();
    static std::vector<int> q;
    for (int s = 0; s < N; s++)
    {
        if (topo.comp[s] >= 0)
            continue;
        int const id = static_cast<int>(topo.cap.size());
        Branch b;
        q.assign(1, s);
        topo.comp[s] = id;
        for (std::size_t k = 0; k < q.size(); k++)
        {
            int const c = q[k];
            b.cells++;
            b.spawns += nextSpawn[c] >= 0 || gapHi[c] > 0;
            b.frontier = b.frontier || front[c];
            for (int i = start[c]; i < start[c + 1]; i++)
                if (!topo.bridge[key[i]] && topo.comp[to[i]] < 0)
                {
                    topo.comp[to[i]] = id;
                    q.push_back(to[i]);
                }
        }
        b.maxCap = b.cells >= 2 ? b.cells : 0;
        topo.cap.push_back(b.cells);
        topo.own.push_back(b);
    }

    // Bridge forest over the regions (flat arrays, reused between builds: on a half-explored big map most
    // unexplored cells are regions of their own); branch summaries in both directions.
    int const R0 = static_cast<int>(topo.cap.size());
    static std::vector<int> fStart, fAdj, chBuf;
    fStart.assign(R0 + 1, 0);
    for (int c = 0; c < N; c++)
        for (int i = start[c]; i < start[c + 1]; i++)
            if (topo.bridge[key[i]])
                fStart[topo.comp[c] + 1]++;
    for (int r = 0; r < R0; r++)
        fStart[r + 1] += fStart[r];
    fAdj.assign(fStart[R0], 0);
    {
        static std::vector<int> fill;
        fill.assign(fStart.begin(), fStart.end() - 1);
        for (int c = 0; c < N; c++)
            for (int i = start[c]; i < start[c + 1]; i++)
                if (topo.bridge[key[i]])
                    fAdj[fill[topo.comp[c]]++] = topo.comp[to[i]];
    }
    topo.parent.assign(R0, -2);
    topo.down.assign(R0, Branch{});
    topo.up.assign(R0, Branch{});
    order.clear();
    for (int r = 0; r < R0; r++)
    {
        if (topo.parent[r] != -2)
            continue;
        topo.parent[r] = -1;
        std::size_t const first = order.size();
        order.push_back(r);
        for (std::size_t k = first; k < order.size(); k++)
            for (int i = fStart[order[k]]; i < fStart[order[k] + 1]; i++)
                if (topo.parent[fAdj[i]] == -2)
                {
                    topo.parent[fAdj[i]] = order[k];
                    order.push_back(fAdj[i]);
                }
    }
    for (int k = static_cast<int>(order.size()) - 1; k >= 0; k--) // children before parents
    {
        int const x = order[k];
        topo.down[x] = merge(topo.down[x], topo.own[x]);
        if (topo.parent[x] >= 0)
            topo.down[topo.parent[x]] = merge(topo.down[topo.parent[x]], topo.down[x]);
    }
    static std::vector<Branch> pre;
    for (int p : order) // parents before children
    {
        if (fStart[p] == fStart[p + 1])
            continue; // a leaf (most regions): no children to fill in
        chBuf.clear();
        for (int i = fStart[p]; i < fStart[p + 1]; i++)
            if (topo.parent[fAdj[i]] == p && std::find(chBuf.begin(), chBuf.end(), fAdj[i]) == chBuf.end())
                chBuf.push_back(fAdj[i]);
        Branch const base = merge(topo.up[p], topo.own[p]);
        pre.assign(chBuf.size() + 1, Branch{});
        for (std::size_t i = 0; i < chBuf.size(); i++)
            pre[i + 1] = merge(pre[i], topo.down[chBuf[i]]);
        Branch suf;
        for (int i = static_cast<int>(chBuf.size()) - 1; i >= 0; i--)
        {
            topo.up[chBuf[i]] = merge(merge(base, pre[i]), suf);
            suf = merge(suf, topo.down[chBuf[i]]);
        }
    }
}

// Rebuilt at most every TOPO_EVERY rounds (CPU). A stale topology can only MISS a new trap, never invent one: a
// branch judged a trap is fully explored, so no new edge can touch it or reconnect it.
int topoBuiltR = -1000;
void topo_refresh()
{
    if (static_cast<int>(topo.comp.size()) != N || (topo.version != edgeVersion && R - topoBuiltR >= TOPO_EVERY))
    {
        topo_build();
        topoBuiltR = R;
    }
}

// The branch a step from `from` in direction d leads into; false when the step
// does not cross a bridge (it stays inside a loop region).
bool topo_beyond(int from, int d, Branch& out)
{
    topo_refresh();
    int const n = cross(from, d);
    if (n < 0 || !topo.bridge[edge_key(from, d)])
        return false;
    int const a = topo.comp[from], b = topo.comp[n];
    if (topo.parent[b] == a)
        out = topo.down[b];
    else if (topo.parent[a] == b)
        out = topo.up[a];
    else
        return false;
    return true;
}

// A known trap for a dragon of length len: the step crosses a bridge into a
// fully explored branch that has no loop longer than the dragon.
bool topo_trap(int from, int d, int len)
{
    Branch b;
    return topo_beyond(from, d, b) && !b.frontier && b.maxCap <= len;
}

// Long queens (no k x k ring test): a loop region longer than her within
// QUEEN_LOOP_DEPTH steps of her head through known open water.
bool topo_loop_near(std::vector<int> const& b, int len)
{
    topo_refresh();
    BodyScope const sb(b, len);
    std::unordered_map<int, int> dist{{b[0], 0}};
    std::vector<int> q{b[0]};
    for (std::size_t i = 0; i < q.size(); i++)
    {
        int const c = q[i], dc = dist[c];
        if (topo.cap[topo.comp[c]] >= std::max(2, len + 1) || topo.own[topo.comp[c]].frontier)
            return true;
        if (dc >= QUEEN_LOOP_DEPTH)
            continue;
        for (int d = 0; d < 4; d++)
        {
            if (edgeType[edge_code(c, d)] != E_OPEN)
                continue;
            int const n = nb(c, d);
            if (dist.count(n) || !passable(n, dc + 1))
                continue;
            dist[n] = dc + 1;
            q.push_back(n);
        }
    }
    return false;
}


#ifdef TOPO_DIAG
// Debug: the known map as the topology sees it ('o' loop region, '.' no cycle,
// '?' frontier, ' ' unknown cell, '=' cell on a bridge end), plus counts.
void topo_render(std::FILE* f)
{
    topo_refresh();
    int bridges = 0, loopCells = 0, treeCells = 0;
    for (int k = 0; k < 2 * N; k++)
        bridges += topo.bridge[k];
    std::fprintf(f, "round %d dragon %d: %d regions, %d bridge edges\n", R, me, static_cast<int>(topo.cap.size()), bridges);
    for (int y = 0; y < H; y++)
    {
        std::string row;
        for (int x = 0; x < W; x++)
        {
            int const c = cell_of(x, y), r = topo.comp[c];
            bool known = false, onBridge = false;
            for (int d = 0; d < 4; d++)
            {
                known = known || cross(c, d) != -2;
                onBridge = onBridge || (cross(c, d) >= 0 && topo.bridge[edge_key(c, d)]);
            }
            bool cellFront = false;
            for (int d = 0; d < 4; d++)
                cellFront = cellFront || cross(c, d) == -2 || cross(c, d) == -3;
            char ch = !known ? ' ' : topo.cap[r] >= 2 ? (onBridge ? '=' : 'o') : '.';
            if (known && cellFront && ch != '.')
                ch = ch == '=' ? '+' : '?';
            loopCells += topo.cap[r] >= 2;
            treeCells += known && topo.cap[r] < 2;
            row += ch;
        }
        std::fprintf(f, "%s\n", row.c_str());
    }
    std::fprintf(f, "loop cells %d, no-cycle cells %d\n", loopCells, treeCells);
    // Raw dump for the offline check (tools: the Python reference recomputes bridges from these edges).
    std::fprintf(f, "W %d H %d\nEDGES ", W, H);
    for (int k = 0; k < 2 * N; k++)
        std::fputc('0' + edgeType[k], f);
    std::fprintf(f, "\nPARTNERS");
    for (int k = 0; k < 2 * N; k++)
        if (edgePartner[k] > k)
            std::fprintf(f, " %d:%d", k, edgePartner[k]);
    std::fprintf(f, "\nBRIDGES");
    for (int k = 0; k < 2 * N; k++)
        if (topo.bridge[k])
            std::fprintf(f, " %d", k);
    std::fprintf(f, "\nCAPS");
    for (int c = 0; c < N; c++)
        std::fprintf(f, " %d", topo.cap[topo.comp[c]]);
    std::fprintf(f, "\n");
}
#endif

} // namespace
