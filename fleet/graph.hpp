// L1 GRAPH - body occupancy over time, flood fill, planning BFS, the queen survival search,
// step simulation and dead-end dives.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

// Occupancy, flood fill and path finding
// Bodies are time-aware: the segment i places from the tail (tail = 0) is
// still there when the head tries to enter at step t < i + 2, because
// collision is checked before the tail moves. Other dragons get the same rule
// (plus one step of margin in case they eat) when their whole body is in view,
// and are solid otherwise.
std::vector<int> ownStamp, ownRel, wallStamp;
int stamp = 1, wstamp = 1;

void set_body(std::vector<int> const& b, int len)
{
    stamp++;
    for (int i = 0; i < static_cast<int>(b.size()); i++)
    {
        ownStamp[b[i]] = stamp;
        ownRel[b[i]] = (len - 1 - i) + 2;
    }
}

bool passable(int c, int t)
{
    if ((otherRel[c] && t < otherRel[c]) || wallStamp[c] == wstamp)
        return false;
    return ownStamp[c] != stamp || t >= ownRel[c];
}

struct FloodRes
{
    int area = 0;
    bool tail = false;
    bool frontier = false;
};

std::vector<int> bfsQ, bfsT, visStamp;
int vstamp = 1;
bool ignoreContested = false;

// Cells reachable from `start` using only edges we know are crossable.
FloodRes flood(int start, int tailCell, int cap)
{
    FloodRes r;
    vstamp++;
    bfsQ.clear();
    bfsT.clear();
    bfsQ.push_back(start);
    bfsT.push_back(0);
    visStamp[start] = vstamp;
    for (std::size_t i = 0; i < bfsQ.size() && r.area < cap; i++)
    {
        int const c = bfsQ[i], t = bfsT[i];
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n == -2)
            {
                r.frontier = true; // unexplored water beyond: count as open
                continue;
            }
            if (n < 0 || visStamp[n] == vstamp)
                continue;
            if (!passable(n, t + 1) || (t == 0 && contested[n] && !ignoreContested))
                continue;
            visStamp[n] = vstamp;
            r.area++;
            if (n == tailCell)
                r.tail = true;
            bfsQ.push_back(n);
            bfsT.push_back(t + 1);
        }
    }
    return r;
}

// Optimistic shortest paths for planning: unknown edges count as open, kelp and
// unlocated portals as closed, known portals as shortcuts.
std::vector<int> planSteps;
std::vector<std::vector<int>> planRing;

void plan_bfs(int start, std::vector<int>& dist)
{
    std::fill(dist.begin(), dist.end(), INF);
    if (UNKNOWN_EDGE_COST > 1)
    {
        // Dial's algorithm: known edges cost 1, edges nobody has seen cost
        // more (in a maze most of them turn out to be kelp). Bodies are still
        // timed by the number of steps, not by the cost.
        int const K = UNKNOWN_EDGE_COST + 1;
        planRing.resize(K);
        for (auto& b : planRing)
            b.clear();
        dist[start] = 0;
        planSteps[start] = 0;
        planRing[0].push_back(start);
        int pending = 1;
        for (int cost = 0; pending > 0; cost++)
        {
            auto& bk = planRing[cost % K];
            for (std::size_t i = 0; i < bk.size(); i++)
            {
                int const c = bk[i];
                if (dist[c] != cost)
                    continue;
                for (int d = 0; d < 4; d++)
                {
                    int n = cross(c, d);
                    int w = 1;
                    if (n == -2)
                    {
                        n = nb(c, d);
                        w = UNKNOWN_EDGE_COST;
                    }
                    int const nt = planSteps[c] + 1;
                    if (n < 0 || cost + w >= dist[n] || !passable(n, nt))
                        continue;
                    dist[n] = cost + w;
                    planSteps[n] = nt;
                    planRing[(cost + w) % K].push_back(n);
                    pending++;
                }
            }
            pending -= static_cast<int>(bk.size());
            bk.clear();
        }
        return;
    }
    bfsQ.clear();
    bfsQ.push_back(start);
    dist[start] = 0;
    for (std::size_t i = 0; i < bfsQ.size(); i++)
    {
        int const c = bfsQ[i];
        int const t = dist[c] + 1;
        for (int d = 0; d < 4; d++)
        {
            int n = cross(c, d);
            if (n == -2)
                n = nb(c, d);
            if (n < 0 || dist[n] != INF || !passable(n, t))
                continue;
            dist[n] = t;
            bfsQ.push_back(n);
        }
    }
}

// Distance to a cell, or to its nearest neighbour if the cell itself is
// occupied (used for enemy heads and teammates).
int dist_to(std::vector<int> const& dist, int target)
{
    if (dist[target] < INF)
        return dist[target];
    int best = INF;
    for (int d = 0; d < 4; d++)
        best = std::min(best, dist[nb(target, d)] + (dist[nb(target, d)] < INF ? 1 : 0));
    return best;
}

bool fountain(int c)
{
    return FOUNTAINS && gapHi[c] > 0 && gapHi[c] <= FOUNT_GAP; // refills (almost) at once: a pocket worth mining
}
bool in_pocket(int c);

bool pearl_at(int c)
{
    return pearlState[c] == 1 && (vis[c] || R - pearlRound[c] <= 3);
}

// Queen survival search
// Area alone cannot tell a dead-end corridor from open water, and a snake
// cannot turn round in a corridor. So for the queen we look for any path of
// `need` further moves through known water. Body cells (set_body) free up
// over time; cells the head passes through stay occupied for `len` moves.
std::vector<int> pathStamp, pathTime;
int pstamp = 1, survNodes = 0;

int survCap = SURV_NODE_CAP; // node budget of the current survival search
bool survive_rec(int h, int t, int need, int len)
{
    if (t >= need || ++survNodes > survCap)
        return true; // long enough (or out of budget: assume fine)
    for (int d = 0; d < 4; d++)
    {
        int n = cross(h, d);
        if (n == -2)
        {
            if (SURV_UNKNOWN_SAFE)
                return true;
            // Unexplored edge: assume open water, but our own body (often out
            // of view on a long queen) and other dragons still block.
            n = nb(h, d);
        }
        if (n < 0)
            continue;
        int const nt = t + 1;
        if (!passable(n, nt) || (nt == 1 && contested[n]))
            continue;
        if (pathStamp[n] == pstamp && nt < pathTime[n] + len + 1)
            continue;
        int const saveS = pathStamp[n], saveT = pathTime[n];
        pathStamp[n] = pstamp;
        pathTime[n] = nt;
        bool const ok = survive_rec(n, nt, need, len);
        pathStamp[n] = saveS;
        pathTime[n] = saveT;
        if (ok)
            return true;
    }
    return false;
}

// b is the body after the move (head first), with its length.
bool queen_can_survive(std::vector<int> const& b, int len, int nodeCap = SURV_NODE_CAP)
{
    set_body(b, len);
    pstamp++;
    survNodes = 0;
    survCap = nodeCap;
    pathStamp[b[0]] = pstamp;
    pathTime[b[0]] = 0;
    return survive_rec(b[0], 0, std::min(2 * len + 6, 50), len);
}

// Body after a sequence of steps (free steps only), or empty if a step is illegal.
std::vector<int> simulate_steps(std::vector<int> b, int len, std::vector<int> const& dirs, int& outLen)
{
    std::vector<int> eaten;
    for (int d : dirs)
    {
        int const n = cross(b[0], d);
        if (n < 0)
            return {};
        bool const eat = pearl_at(n) && std::find(eaten.begin(), eaten.end(), n) == eaten.end();
        if (eat)
            eaten.push_back(n);
        b.insert(b.begin(), n);
        if (eat)
            len++;
        else if (static_cast<int>(b.size()) > len)
            b.pop_back();
    }
    outLen = len;
    return b;
}

// Dead-end dives
// A dead end that holds pearls can still be worth entering when the pearls pay
// for the way out. At the bottom the survival split leaves the front two
// segments behind and the rest leaves as a child, whose head is our old tail
// outside the dead end. This searches our own moves through known water;
// other dragons keep their bodies. Value = net segments won.
int diveNodes = 0, diveCap = DIVE_NODE_CAP;

// After a split of all but the front 2 segments, can the child get away?
bool split_child_escapes(std::vector<int> const& b, int len)
{
    if (static_cast<int>(b.size()) < len)
        return true; // tail out of view, well away from the dead end
    int const childLen = len - 2;
    std::vector<int> child(b.rbegin(), b.rbegin() + childLen);
    wstamp++;
    for (int i = 0; i < len - childLen; i++)
        wallStamp[b[i]] = wstamp;
    set_body(child, childLen);
    FloodRes const f = flood(child[0], child.back(), childLen + 8);
    wstamp++;
    return f.tail || f.area >= std::min(childLen + 1, 6);
}

double dive_rec(std::vector<int> const& b, int len, int depth, int eaten, std::vector<int>& eatenCells)
{
    int const h = b[0];
    set_body(b, len);
    int const tailCell = static_cast<int>(b.size()) >= len ? b.back() : -1;
    FloodRes const f = flood(h, tailCell, 2 * len + 4);
    if (f.tail || f.area + (f.frontier ? len : 0) >= 2 * len + 4 || (FRONTIER_OPEN && !isLeader && f.frontier))
        return eaten + 0.5; // back in open water: everything eaten is profit, nobody lost
    if (depth >= DIVE_DEPTH || ++diveNodes > diveCap)
        return eaten - 3; // unresolved: assume the split and a bit more
    double best = -1e9;
    for (int d = 0; d < 4; d++)
    {
        int const n = cross(h, d);
        if (n < 0)
            continue;
        set_body(b, len);
        if (!passable(n, 1))
            continue;
        bool const fount = fountain(n) && !(vis[n] && occId[n] >= 0 && occId[n] != me); // refills before we get there
        bool const eat = (pearl_at(n) || fount) && std::find(eatenCells.begin(), eatenCells.end(), n) == eatenCells.end();
        std::vector<int> nb2;
        nb2.reserve(b.size() + 1);
        nb2.push_back(n);
        nb2.insert(nb2.end(), b.begin(), b.end());
        int const nl = len + (eat ? 1 : 0);
        if (!eat && static_cast<int>(b.size()) >= len)
            nb2.pop_back();
        if (eat)
            eatenCells.push_back(n);
        best = std::max(best, dive_rec(nb2, nl, depth + 1, eaten + (eat ? 1 : 0), eatenCells));
        if (eat)
            eatenCells.pop_back();
    }
    if (best > -1e9)
        return best;
    // Stuck: the survival split saves all but the front two segments.
    if (len >= 4 && units < unitLimit && split_child_escapes(b, len))
        return eaten - 2;
    return eaten - len;
}

} // namespace
