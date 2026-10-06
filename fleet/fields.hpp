// L1 FIELDS - per-turn maps: distances from my head, pearl richness, Voronoi territory, enemy
// strike reach (queen threat map, one and two turns), distances from our queen.
// Field cache (Architecture_v2 3c): each field is computed at most once per turn (or less often), on first use:
//   distHead            plan_bfs(head) in prepare_turn, every turn
//   vorOurs / vorEnemy  compute_voronoi(), once per round (vorRound)
//   regionVal           update_region_value(), every 5 rounds
//   qThreat* / qMargin  compute_queen_threat(), once per turn, queen only
//   qDist               queen_dist_bfs(), only when QUEEN_CLEAR is on
//   topology            topo_refresh() (topology.hpp), when an edge was learned (at most every TOPO_EVERY rounds)
// Per-candidate searches (flood, survival, dives, plan_bfs from a candidate cell) are not fields: they depend on
// the simulated body and run inside a BodyScope.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

std::vector<int> distHead, distTmp;
std::vector<int> skipPearl; // round until which this dragon ignores the pearl (only reachable at a loss)

// P7: how pearl-rich the area round each cell is (expected pearls per round
// from known spawn tiles within RV_RAD, fountains most), normalised to [0, 1].
// Recomputed every 5 rounds.
std::vector<float> regionVal;
int regionValRound = -100;
double rv_size_factor() { return std::clamp(static_cast<double>(N) / RV_FULL_CELLS, 0.4, 1.0); }
void update_region_value()
{
    if (R - regionValRound < 5 && static_cast<int>(regionVal.size()) == N)
        return;
    regionValRound = R;
    regionVal.assign(N, 0.0f);
    for (int t = 0; t < N; t++)
    {
        if (!(nextSpawn[t] >= 0 || pearlState[t] != 0 || gapHi[t] > 0))
            continue;
        double const rate = gapHi[t] > 0 ? 1.0 / std::max(3, gapHi[t]) : 1.0 / 25.0;
        int const tx = t % W, ty = t / W;
        for (int dy = -RV_RAD; dy <= RV_RAD; dy++)
            for (int dx = -RV_RAD + std::abs(dy); dx <= RV_RAD - std::abs(dy); dx++)
            {
                int const x = tx + dx, y = ty + dy;
                if (x < 0 || y < 0 || x >= W || y >= H)
                    continue;
                regionVal[y * W + x] += static_cast<float>(rate * (RV_RAD + 1 - std::abs(dx) - std::abs(dy)) / (RV_RAD + 1));
            }
    }
    float mx = 0;
    for (float v : regionVal)
        mx = std::max(mx, v);
    if (mx > 0)
        for (float& v : regionVal)
            v /= mx;
}
double rv_norm(int c) { return REGION_VALUE && static_cast<int>(regionVal.size()) == N ? regionVal[c] : 0.0; }

// A: Voronoi fields - path distance from the nearest of our heads (vorOurs)
// and of theirs (vorEnemy) to every cell, over known water (unexplored
// edges open, bodies passable: they move). Seeds: heads in view and
// position reports up to VOR_REP_AGE rounds old.
std::vector<int> vorOurs, vorEnemy, vorQ;
int vorRound = -1;
void vor_bfs(std::vector<int> const& seeds, std::vector<int>& dist)
{
    dist.assign(N, INF);
    vorQ.clear();
    for (int c : seeds)
        if (c >= 0 && c < N && dist[c] != 0)
        {
            dist[c] = 0;
            vorQ.push_back(c);
        }
    for (std::size_t i = 0; i < vorQ.size(); i++)
    {
        int const c = vorQ[i];
        for (int k = 0; k < 4; k++)
        {
            int const n = step_open(c, k); // unexplored edge: assume open
            if (n < 0 || dist[n] <= dist[c] + 1)
                continue;
            dist[n] = dist[c] + 1;
            vorQ.push_back(n);
        }
    }
}
void compute_voronoi()
{
    if (!VORONOI || vorRound == R)
        return;
    vorRound = R;
    std::vector<int> ours, theirs;
    std::unordered_set<int> seen;
    for (auto const& h : heads)
    {
        (h.mine ? ours : theirs).push_back(h.cell);
        seen.insert(h.id);
    }
    if (!body.empty())
        ours.push_back(body[0]);
    for (auto const& [id, st] : fleet)
        if (!seen.count(id) && R - st.round <= VOR_REP_AGE)
            ours.push_back(st.cell);
    for (auto const& e : enemyReps)
        if (!seen.count(e.id) && R - e.round <= VOR_REP_AGE)
            theirs.push_back(e.cell);
    vor_bfs(ours, vorOurs);
    vor_bfs(theirs, vorEnemy);
}
// Margin of a cell: > 0 we get there first by that many steps, < 0 they do.
int vor_margin(int c)
{
    if (!VORONOI || static_cast<int>(vorEnemy.size()) != N || vorEnemy[c] >= INF)
        return 99;
    return vorEnemy[c] - vorOurs[c];
}

// Sprints: the first ceil(L/4) steps of a turn are free.
int free_steps() { return (L + 3) / 4; }
int enemy_reach(HeadInfo const& h) { return std::max(1, (h.len + 3) / 4); }

// Danger of ending a turn on cell n: next to a teammate head, or where an
// enemy head can reach us (leaders assume the enemy sprints its free steps).
// Queen threat map: the fewest paid sprint steps an enemy needs to put its
// head on a cell (0 = within its free steps, INF = out of reach). A head-on
// kills both dragons whatever their lengths, and a dragon may sprint past its
// free steps by paying one segment per step down to length 2.
std::vector<int> qThreat, qThreatVis, thStamp, thDist, thQ;
std::vector<int> qThreatCnt, qMargin; // enemies able to reach a cell; steps beyond the nearest reach
std::vector<int> qThreat2;            // P3c: < INF where some enemy can put its head within two turns
std::vector<int> l2Stamp, l2Dist, l2Q;
int l2S = 1;
bool look2Active = false;             // some enemy is close enough for the lookahead to matter
int thS = 1;

// Cells an enemy at src can put its head on this turn (freeR free steps, up to
// maxR with paid ones). `cnt`/`margin` (optional) also count attackers and
// record how far beyond the reach a cell lies, up to QUEEN_MARGIN more steps.
void reach_bfs(int src, int freeR, int maxR, std::vector<int>& out, std::vector<int>* cnt = nullptr, std::vector<int>* margin = nullptr)
{
    thS++;
    thQ.clear();
    thQ.push_back(src);
    thStamp[src] = thS;
    thDist[src] = 0;
    int const lim = maxR + (margin ? QUEEN_MARGIN : 0);
    for (std::size_t i = 0; i < thQ.size(); i++)
    {
        int const c = thQ[i], dc = thDist[c];
        if (dc >= lim)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int const n = step_open(c, k); // unexplored edge: assume the enemy can pass
            if (n < 0 || thStamp[n] == thS)
                continue;
            thStamp[n] = thS;
            thDist[n] = dc + 1;
            if (dc + 1 <= maxR)
            {
                out[n] = std::min(out[n], std::max(0, dc + 1 - freeR));
                if (cnt)
                    (*cnt)[n]++;
            }
            else if (margin)
                (*margin)[n] = std::min((*margin)[n], dc + 1 - maxR);
            if (vis[n] && occId[n] >= 0)
                continue; // a body: the enemy cannot go through it
            thQ.push_back(n);
        }
    }
}

void compute_queen_threat()
{
    std::fill(qThreat.begin(), qThreat.end(), INF);
    std::fill(qThreatVis.begin(), qThreatVis.end(), INF);
    std::fill(qThreatCnt.begin(), qThreatCnt.end(), 0);
    std::fill(qMargin.begin(), qMargin.end(), INF);
    std::vector<int>* cnt = QUEEN_HARD_THREAT ? &qThreatCnt : nullptr;
    std::vector<int>* mar = QUEEN_HARD_THREAT ? &qMargin : nullptr;
    // P3a: her last segment leaves its cell when she steps, so an enemy can
    // come through it (a U-turn is no shield).
    int tailCell = -1, tailOcc = -1;
    if (THREAT_TAIL_FREE && amQueen && static_cast<int>(body.size()) >= L && L >= 2)
    {
        tailCell = body[L - 1];
        tailOcc = occId[tailCell];
        if (tailOcc == me)
            occId[tailCell] = -1;
        else
            tailCell = -1;
    }
    std::unordered_set<int> seenIds;
    if (QUEEN_LOOK2)
        std::fill(qThreat2.begin(), qThreat2.end(), INF);
    look2Active = false;
    for (auto const& h : heads)
    {
        if (h.mine)
            continue;
        seenIds.insert(h.id);
        int const len = std::max(h.len, h.estLen);
        int const f = std::max(1, (len + 3) / 4);
        int const m = std::min(PAID_REACH_CAP, f + (PAID_REACH ? std::max(0, len - 2) : 0));
        reach_bfs(h.cell, f, f, qThreatVis);
        reach_bfs(h.cell, f, std::max(f, m), qThreat, cnt, mar);
        if (QUEEN_LOOK2)
        {
            int const m2 = std::min(PAID_REACH_CAP, m + f); // two turns: free steps twice, the paid budget once
            reach_bfs(h.cell, 2 * f, m2, qThreat2);
            if (!body.empty() && manhattan(h.cell, body[0]) <= m2 + LOOK2_QUEEN_REACH + 1)
                look2Active = true;
        }
    }
    // Enemies the fleet reported in the last two rounds: they may have moved
    // a free step or two since.
    for (auto const& e : enemyReps)
    {
        int const age = R - e.round;
        if (age < 1 || age > REP_MAX_AGE || seenIds.count(e.id))
            continue;
        int const el = std::max(e.len, e.estLen);
        int const f = std::max(1, (el + 3) / 4) * (age + 1);
        int const m = std::min(PAID_REACH_CAP, f + (PAID_REACH ? std::max(0, el - 2) : 0));
        reach_bfs(e.cell, f, std::max(f, m), qThreat, cnt, mar);
        if (QUEEN_LOOK2)
        {
            int const f1 = std::max(1, (el + 3) / 4);
            int const m2 = std::min(PAID_REACH_CAP, m + f1);
            reach_bfs(e.cell, f + f1, m2, qThreat2);
            if (!body.empty() && manhattan(e.cell, body[0]) <= m2 + LOOK2_QUEEN_REACH + 1)
                look2Active = true;
        }
    }
    if (tailCell >= 0)
        occId[tailCell] = tailOcc;
}

// Path distances from the queen's head out to `maxR` steps (known water;
// unexplored edges count as open). Used by teammates to give her room.
std::vector<int> qDist;
void queen_dist_bfs(int src, int maxR)
{
    std::fill(qDist.begin(), qDist.end(), INF);
    thQ.clear();
    thQ.push_back(src);
    qDist[src] = 0;
    for (std::size_t i = 0; i < thQ.size(); i++)
    {
        int const c = thQ[i];
        if (qDist[c] >= maxR)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int const n = step_open(c, k);
            if (n < 0 || qDist[n] != INF)
                continue;
            qDist[n] = qDist[c] + 1;
            thQ.push_back(n);
        }
    }
}

} // namespace
