// L6 ACT - split gates, issuing moves / sprints / splits, the sprint planner.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

// Numbers are the goal, but a crowded map kills small dragons for nothing.
bool room_for_more()
{
    if (EXPANSION && growth_open())
        return units < expansion_target();
    return units < std::min(unitLimit, std::max(2, N / SPLIT_DENSITY));
}

// The engine refuses every split once the team holds its unit limit. The
// queen's survival split must never be refused, so the others leave her
// QUEEN_SLOTS free (several dragons can split in the same round).
bool split_allowed(Controller& ct, int child)
{
    return ct.can_split(child) && (amQueen || units < unitLimit - QUEEN_SLOTS);
}

void act_move(Controller& ct, int d, char const* why)
{
    ct.make_move(dir_of(d));
    note = why;
    prevBody = body;
    lastDirs = {d};
    if (traceF)
        traceAct = std::string("M") + DCH[d];
}

void act_split(Controller& ct, int child, char const* why)
{
    ct.do_split(child);
    note = why;
    prevBody = body;
    lastDirs.clear();
    if (traceF)
        traceAct = "S" + std::to_string(child);
}

struct SprintPlan
{
    std::vector<int> dirs;
    double value = -1e18;
    bool kill = false;
};

std::vector<int> distGoal;

// Depth-first search over step sequences through cells in view. Other dragons
// do not move during our turn, so what we see is exact; our own body shifts
// step by step. Steps past freeN cost a segment each.
void sprint_dfs(std::vector<int> const& b, int len, int step, int maxSteps, int freeN, int eaten,
                std::vector<int>& dirs, std::vector<int>& eatenCells, bool useGoal, int goalStart, SprintPlan& best)
{
    int const h = b[0];
    for (int d = 0; d < 4; d++)
    {
        int const n = cross(h, d);
        if (n < 0 || !vis[n])
            continue;
        if (occId[n] >= 0 && occId[n] != me)
        {
            // Head-on with the enemy queen: worth any non-queen dragon.
            bool const canPay = step + 1 <= freeN || len >= 3;
            if (canPay && occHead[n] && occId[n] == enemyQueenId && !occMine[n] && !amQueen && !enemyQueenDead && !best.kill)
            {
                dirs.push_back(d);
                best.dirs = dirs;
                best.kill = true;
                best.value = 1e12;
                dirs.pop_back();
            }
            continue;
        }
        set_body(b, len);
        if (!passable(n, 1))
            continue;
        bool const eat = pearl_at(n) && std::find(eatenCells.begin(), eatenCells.end(), n) == eatenCells.end();
        std::vector<int> nb2;
        nb2.reserve(b.size() + 1);
        nb2.push_back(n);
        nb2.insert(nb2.end(), b.begin(), b.end());
        int nl = len + (eat ? 1 : 0);
        if (!eat && static_cast<int>(b.size()) >= len)
            nb2.pop_back();
        if (step + 1 > freeN) // a paid step also removes one more tail segment
        {
            if (static_cast<int>(nb2.size()) >= nl)
                nb2.pop_back();
            nl--;
            if (nl < 2)
                continue;
        }
        if (eat)
            eatenCells.push_back(n);
        dirs.push_back(d);
        int const eatenNow = eaten + (eat ? 1 : 0);
        if (step + 1 >= 2 && !best.kill)
        {
            int const tailCell = static_cast<int>(nb2.size()) >= nl ? nb2.back() : -1;
            set_body(nb2, nl);
            int const cap = std::min(N, 3 * nl + 24);
            FloodRes const f = flood(n, tailCell, cap);
            int const area = f.area + (f.frontier && !amQueen ? nl : 0);
            if (f.tail || area >= 2 * nl + 4)
            {
                double v = eatenNow * 3000.0 + std::min(area, cap) * 15.0 - (L + eatenNow - nl) * 4000.0 - end_threat(n);
                if (QUEEN_EXIT_GUARD && !amQueen)
                    for (auto const& qh : heads)
                        if (qh.mine && qh.id == queenId && manhattan(qh.cell, n) <= 2)
                        {
                            std::vector<int> nowBody(body.begin(), body.begin() + std::min<int>(body.size(), L));
                            std::vector<int> after(nb2.begin(), nb2.begin() + std::min<int>(nb2.size(), nl));
                            int const before = queen_exits(qh.cell, nowBody), now = queen_exits(qh.cell, after);
                            if (now < before && now < 2)
                                v -= QUEEN_EXIT_PEN; // a sprint must not end in the queen's last way out
                        }
                if (useGoal && distGoal[n] < INF)
                    v += 150.0 * (goalStart - distGoal[n]);
                if (v > best.value)
                {
                    best.value = v;
                    best.dirs = dirs;
                }
            }
        }
        if (step + 1 < maxSteps && !best.kill)
            sprint_dfs(nb2, nl, step + 1, maxSteps, freeN, eatenNow, dirs, eatenCells, useGoal, goalStart, best);
        dirs.pop_back();
        if (eat)
            eatenCells.pop_back();
    }
}

void act_moves(Controller& ct, std::vector<int> const& dirs, char const* why)
{
    std::vector<Direction> ds;
    for (int d : dirs)
        ds.push_back(dir_of(d));
    ct.make_moves(ds);
    note = why;
    prevBody = body;
    lastDirs = dirs;
    if (traceF)
    {
        traceAct = "M";
        for (int d : dirs)
            traceAct += DCH[d];
    }
}

} // namespace
