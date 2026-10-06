// L1 TOPOLOGY - corridors, pockets and dead ends, free loops for the queen, corridor tests.
// (Architecture_v2 step 7b replaces the walks below with bridge / loop lookups.)
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

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
        return true; // very long queens: no ring test (rare)
    set_body(b, len);
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
    wstamp++;
    for (int i = L - child; i < L; i++)
        wallStamp[body[i]] = wstamp;
    bool const ok = loop_reachable(front, L - child);
    wstamp++;
    set_body(body, L);
    return ok;
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

} // namespace
