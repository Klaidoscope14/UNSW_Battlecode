// L4 TACTICS - move candidates, adjacency and threat terms, the queen's two-turn lookahead,
// her free exits, strike paths.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

// Decision
struct Cand
{
    int d = 0, dest = -1;
    bool legal = false, blind = false, attack = false, eat = false;
    bool ok1 = false, ok2 = false;
    int area = 0;
    double dive = -1e9; // net segments won by entering a dead end (+0.5 if we get out whole); non-queens
    bool surv = false;  // queen: some way on through known or unexplored water
    int dt = INF;       // path distance to the target after this move
    bool noLoop = false; // queen (length <= 3): no free 2x2 reachable afterwards
    double score = -1e18;
    std::array<double, 4> bands{}; // score split by severity band (BAND_LIFE .. BAND_STYLE)
    std::vector<int> nb1;
};

bool is_adjacent_enemy_head(int cell)
{
    for (auto const& h : heads)
        if (!h.mine && manhattan(h.cell, cell) <= 1)
            return true;
    return false;
}

// Can a child made of the rear `childLen` segments get out? Its head is my
// tail and it moves later this round; my front part stays where it is.
bool child_can_escape(int childLen, bool growth = false)
{
    if (static_cast<int>(body.size()) < L)
        return true; // tail is out of view, well away from whatever traps the head
    std::vector<int> child(body.rbegin(), body.rbegin() + childLen);
    WallScope ws;
    for (int i = 0; i < L - childLen; i++)
        ws.wall(body[i]);
    BodyScope const sb(child, childLen);
    int const need = growth && SPLIT_ROOM ? std::max(6, 2 * childLen + 4) : std::min(childLen + 1, 6);
    FloodRes const f = flood(child[0], child.back(), std::max(childLen + 8, need + 2));
    // A growth split is optional: its child must have room to live, not just
    // to take a step (children born into pockets died within a few rounds).
    return (f.tail && !(growth && SPLIT_ROOM)) || f.area >= need;
}

// P3c: after a move to `dest` with body `nb` (newLen long), how many cells
// can she put her head on next turn (free + paid steps, her own body in the
// way except its last segment) that no enemy can reach within two turns and
// that are not dead ends? Capped at 8.
int look2_safe_cells(int dest, std::vector<int> const& nb, int newLen)
{
    l2S++;
    int const reachQ = std::min(LOOK2_QUEEN_REACH, (newLen + 3) / 4 + (LOOK2_PAID ? std::max(0, newLen - 2) : 0));
    for (int i = 0; i + 1 < newLen && i < static_cast<int>(nb.size()); i++)
        l2Stamp[nb[i]] = l2S; // her body after the move (the last segment moves on)
    l2Q.clear();
    l2Q.push_back(dest);
    l2Stamp[dest] = l2S;
    l2Dist[dest] = 0;
    int safe = 0;
    for (std::size_t i = 0; i < l2Q.size() && safe < 8; i++)
    {
        int const c = l2Q[i];
        if (l2Dist[c] >= reachQ)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int const n = cross(c, k);
            if (n < 0 || l2Stamp[n] == l2S || (vis[n] && occId[n] >= 0 && occId[n] != me))
                continue;
            l2Stamp[n] = l2S;
            l2Dist[n] = l2Dist[c] + 1;
            l2Q.push_back(n);
            if (qThreat2[n] >= INF && seenRound[n] >= 0 && open_sides(n) >= 2)
                safe++;
        }
    }
    return safe;
}

double queen_threat_pen(int n)
{
    if (!QUEEN_HARD_THREAT)
    {
        if (qThreat[n] == 0)
            return 3e6;
        if (qThreat[n] < INF)
            return PAID_THREAT_PEN / static_cast<double>(qThreat[n]);
        return 0;
    }
    // Every enemy moves after the queen, and a head-on kills both whatever the
    // lengths, so paying segments to reach her is free for the attacker:
    // anything within free + paid reach is as good as a kill.
    double p = 0;
    if (qThreat[n] == 0)
        p = 3e6;
    else if (qThreat[n] < INF)
        p = HARD_THREAT_PEN - 20000.0 * std::min(qThreat[n], 10);
    if (qThreatCnt[n] > 1)
        p += THREAT_EXTRA_ATTACKER * (qThreatCnt[n] - 1);
    if (qThreat[n] == INF && qMargin[n] <= QUEEN_MARGIN)
        p += QUEEN_MARGIN_PEN * (QUEEN_MARGIN + 1 - qMargin[n]);
    return p;
}

// The part of queen_threat_pen that is only caution (not inside any reach yet).
double queen_margin_pen(int n)
{
    if (QUEEN_HARD_THREAT && qThreat[n] == INF && qMargin[n] <= QUEEN_MARGIN)
        return QUEEN_MARGIN_PEN * (QUEEN_MARGIN + 1 - qMargin[n]);
    return 0;
}

double end_threat(int n)
{
    double t = 0;
    for (auto const& hh : heads)
    {
        if (hh.mine)
        {
            if (manhattan(hh.cell, n) <= 1)
                t += 4000;
            continue;
        }
        if (amQueen && QUEEN_THREAT_MAP)
            continue; // added once below from the threat map
        int const reach = my.leader ? enemy_reach(hh) : 1;
        if (manhattan(hh.cell, n) <= reach)
            t += amQueen ? 3e6 : my.leader ? 300000 : THREAT_PEN;
    }
    if (amQueen && QUEEN_THREAT_MAP)
        t += queen_threat_pen(n);
    if (my.leader)
        for (int tc : enemyTails)
            if (manhattan(tc, n) <= 1)
                t += amQueen ? 400000 : 100000;
    return t;
}

// Shortest step sequence that ends in the enemy queen's head. Other dragons
// stand still during our turn; our own body follows us. Steps past the free
// ones cost a segment each, which does not matter: a head-on kills us anyway.
std::vector<int> regicide_path(int qcell, int maxSteps)
{
    set_body(body, L);
    std::unordered_map<int, int> dist, par, pdir;
    std::vector<int> q{body[0]};
    dist[body[0]] = 0;
    for (std::size_t i = 0; i < q.size(); i++)
    {
        int const c = q[i], dc = dist[c];
        if (dc >= maxSteps)
            continue;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n < 0 || dist.count(n))
                continue;
            if (n == qcell)
            {
                std::vector<int> dirs{d};
                for (int x = c; x != body[0]; x = par[x])
                    dirs.push_back(pdir[x]);
                std::reverse(dirs.begin(), dirs.end());
                return dirs;
            }
            if (!vis[n] || (occId[n] >= 0 && occId[n] != me) || !passable(n, dc + 1))
                continue;
            dist[n] = dc + 1;
            par[n] = c;
            pdir[n] = d;
            q.push_back(n);
        }
    }
    return {};
}

// Free cells next to the queen's head for her next move, with our body as it
// will be after a candidate move (newBody, head first; empty = as now).
int queen_exits(int qh, std::vector<int> const& newBody)
{
    int k = 0;
    for (int d = 0; d < 4; d++)
    {
        int const n = step_open(qh, d); // unexplored: may be open
        if (n < 0)
            continue;
        if (vis[n] && occId[n] >= 0 && occId[n] != me)
            continue; // another dragon (or her own body) is there
        if (std::find(newBody.begin(), newBody.end(), n) != newBody.end())
            continue; // we will be there
        k++;
    }
    return k;
}

} // namespace
