// L3 COMMAND - team-level assignment every dragon computes the same way: pearl matching,
// hunter / wall slots, pack membership, prey assignment.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

// Target selection (sections 6-10, 18)
struct PearlCand
{
    int cell, appear;
    double conf;
};

// C: bounded path BFS from a teammate's head (unexplored edges open, bodies
// passable - they move), for its distance to each candidate pearl.
std::vector<int> mStamp, mDist, mQ;
int mS = 0;
void match_bfs(int src)
{
    if (static_cast<int>(mStamp.size()) != N)
    {
        mStamp.assign(N, 0);
        mDist.assign(N, 0);
    }
    mS++;
    mQ.clear();
    mQ.push_back(src);
    mStamp[src] = mS;
    mDist[src] = 0;
    for (std::size_t i = 0; i < mQ.size(); i++)
    {
        int const c = mQ[i];
        if (mDist[c] >= MATCH_DEPTH)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int n = cross(c, k);
            if (n == -2)
                n = nb(c, k);
            if (n < 0 || mStamp[n] == mS)
                continue;
            mStamp[n] = mS;
            mDist[n] = mDist[c] + 1;
            mQ.push_back(n);
        }
    }
}

// Implicit, communication-light assignment: every dragon solves the same
// greedy matching of known teammates to known pearls, so dragons split up
// instead of all chasing one pearl (sections 6, 7, 9).
int choose_pearl(std::vector<PearlCand> const& pearls)
{
    struct Agent
    {
        int id, cell, fixedEta;
        bool leader;
    };
    std::vector<Agent> agents{{me, body[0], -1, isLeader}};
    std::unordered_set<int> seen{me};
    for (auto const& h : heads)
        if (h.mine && !seen.count(h.id))
        {
            seen.insert(h.id);
            auto it = fleet.find(h.id);
            agents.push_back({h.id, h.cell, -1, it != fleet.end() && it->second.leader});
        }
    for (auto const& [id, s] : fleet)
        if (!seen.count(id) && R - s.round <= 4 && agents.size() < 24)
        {
            seen.insert(id);
            agents.push_back({id, s.cell, -1, s.leader});
        }

    struct Pair
    {
        double cost;
        int agent, pearl;
    };
    // C: each teammate's path distance to each pearl (manhattan ignores kelp:
    // in mazes it sent the wrong dragon and lost races we were nearer for).
    std::vector<std::vector<int>> pathEta;
    if (MATCH_PATHS)
    {
        pathEta.assign(agents.size(), {});
        for (std::size_t ai = 0; ai < agents.size(); ai++)
        {
            if ((agents[ai].id == me && !MATCH_SELF_SAME) || manhattan(agents[ai].cell, body[0]) > 2 * MATCH_DEPTH)
                continue; // far teammates cannot compete for our pearls: manhattan is enough
            bool anyNear = false;
            for (auto const& p : pearls)
                if (manhattan(agents[ai].cell, p.cell) <= MATCH_DEPTH)
                    anyNear = true;
            if (!anyNear)
                continue;
            match_bfs(agents[ai].cell);
            pathEta[ai].assign(pearls.size(), -1);
            for (std::size_t pi = 0; pi < pearls.size(); pi++)
                if (mStamp[pearls[pi].cell] == mS)
                    pathEta[ai][pi] = mDist[pearls[pi].cell];
        }
    }
    std::vector<Pair> pairs;
    for (int pi = 0; pi < static_cast<int>(pearls.size()); pi++)
    {
        auto const& p = pearls[pi];
        auto const cl = claims.find(p.cell);
        for (int ai = 0; ai < static_cast<int>(agents.size()); ai++)
        {
            auto const& a = agents[ai];
            int eta;
            if (a.id == me && !(MATCH_PATHS && MATCH_SELF_SAME))
                eta = distHead[p.cell];
            else if (cl != claims.end() && cl->second.id == a.id)
                eta = std::max(0, cl->second.eta - (R - cl->second.round));
            else if (MATCH_PATHS)
            {
                int const pe = pathEta[ai].empty() ? -1 : pathEta[ai][pi];
                eta = pe >= 0 ? pe : std::max(manhattan(a.cell, p.cell), MATCH_DEPTH + 1);
            }
            else
                eta = manhattan(a.cell, p.cell);
            if (eta >= INF)
                continue;
            int const wait = p.appear - R;
            // Future spawn: travel plus half the time spent waiting there.
            double cost = (CAMP_JOB && wait > 0 ? eta + 0.5 * std::max(0, wait - eta) : std::max(eta, wait)) + 1.0;
            cost /= p.conf;
            if (a.leader)
                cost *= a.id == queenId ? (EXPANSION && queenSplits ? 1.0 : 0.3) : 0.6; // feed the queen (once she grows), then the leaders
            pairs.push_back({cost + a.id * 1e-7, ai, pi});
        }
        // A teammate we cannot see but who claimed this pearl still counts.
        if (cl != claims.end() && !seen.count(cl->second.id))
        {
            double const cost = std::max(0, cl->second.eta - (R - cl->second.round)) + 1.0;
            pairs.push_back({cost / p.conf, -1, pi});
        }
    }
    std::sort(pairs.begin(), pairs.end(), [](Pair const& a, Pair const& b) { return a.cost < b.cost; });
    std::vector<char> agentUsed(agents.size(), 0), pearlUsed(pearls.size(), 0);
    for (auto const& pr : pairs)
    {
        if (pearlUsed[pr.pearl] || (pr.agent >= 0 && agentUsed[pr.agent]))
            continue;
        pearlUsed[pr.pearl] = 1;
        if (pr.agent < 0)
            continue;
        agentUsed[pr.agent] = 1;
        if (agents[pr.agent].id == me)
            return pr.pearl;
    }
    return -1;
}

// Packs: hunters (and, once our queen is dead, every small dragon) share out
// prey the same way pearls are shared: every dragon solves the same greedy
// matching from what the fleet knows, so packs form without negotiation.
int hunter_slot(int id)
{
    std::uint32_t const hh = static_cast<std::uint32_t>(id) * 2246822519u + 3266489917u;
    return static_cast<int>((hh >> 13) % 10);
}

bool hunts_now()
{
    return HUNTERS && !(enemyQueenDead && !ourQueenDead) && (R >= HUNT_START || !expansionPhase) && R >= OPEN_FARM_ROUND;
}

// P8 wall: when it forms (later on bigger maps) and who is in it.
int wall_round() { return std::min(260, 120 + N / 40); }
bool wall_now() { return WALL && HUNTERS && !openPlan && N >= WALL_MIN_CELLS && R >= wall_round() && units >= WALL_FORM_RATIO * est_enemy_units(); }
bool wall_slot(int id)
{
    int const sl = hunter_slot(id);
    return sl >= WALL_SLOT_LO && sl <= WALL_SLOT_HI;
}

// Would teammate `id` (length len) be in a pack right now?
bool pack_member(int id, int len, bool leader)
{
    if (id == queenId || leader)
        return false;
    if (ENDGAME_PACKS && ourQueenDead && len <= 6)
        return true;
    if (wall_now() && wall_slot(id) && len >= WALL_MIN_LEN)
        return true;
    return hunts_now() && hunter_slot(id) < HUNTER_SHARE && len >= HUNTER_LEN;
}

struct Prey
{
    int cell, id, len;
    bool queen;
};
std::vector<Prey> preyList;
int myPrey = -1, myPackSize = 0, myPackRank = 0;

void assign_prey()
{
    preyList.clear();
    myPrey = -1;
    myPackSize = 0;
    myPackRank = 0;
    // Our queen is dead: only the longest dragon can still win it. If theirs
    // has not been seen for 100 rounds she is most likely dead too (theirs
    // die early), so their longest dragons come first.
    bool const giantHunt = GIANT_HUNT && ourQueenDead && R >= GIANT_HUNT_ROUND && (enemyQueenDead || R - lastEqSeenR >= 100);
    int qt = enemyQueenDead || giantHunt ? -1 : enemy_queen_target();
    // Big map, her position not seen lately: she hides at home among her
    // swarm, and the long walk there only feeds it (ladder: never reached).
    if (HUNT_FAR_SMALL && qt >= 0 && N > SMALL_MAP_CELLS && !ourQueenDead && R - lastEqSeenR > HUNT_FRESH)
        qt = -1;
    if (qt >= 0)
        preyList.push_back({qt, enemyQueenId, 0, true});
    // Long enemy dragons: when the queens no longer decide the game, or while
    // nobody knows where the enemy queen is.
    if (ourQueenDead || enemyQueenDead || qt < 0)
        for (auto const& e : enemyReps)
            if (e.id != enemyQueenId && R - e.round <= 6 && e.len >= (giantHunt ? GIANT_PREY_MIN : PREY_MIN_LEN))
                preyList.push_back({e.cell, e.id, e.len, false});
    if (preyList.empty())
        return;

    struct Agent
    {
        int id, cell;
    };
    std::vector<Agent> agents{{me, body[0]}};
    for (auto const& [id, st] : fleet)
        if (id != me && R - st.round <= 4 && pack_member(id, st.len, st.leader))
            agents.push_back({id, st.cell});
    struct Pair
    {
        double cost;
        int a, p;
    };
    std::vector<Pair> pairs;
    for (int pi = 0; pi < static_cast<int>(preyList.size()); pi++)
        for (int ai = 0; ai < static_cast<int>(agents.size()); ai++)
        {
            int const d = agents[ai].id == me ? dist_to(distHead, preyList[pi].cell) : manhattan(agents[ai].cell, preyList[pi].cell);
            if (d >= INF)
                continue;
            double const c = d - (preyList[pi].queen ? 8.0 : (giantHunt ? 1.0 : 0.25) * preyList[pi].len) + agents[ai].id * 1e-6;
            pairs.push_back({c, ai, pi});
        }
    std::sort(pairs.begin(), pairs.end(), [](Pair const& x, Pair const& y) { return x.cost < y.cost; });
    std::vector<int> taken(preyList.size(), 0);
    std::vector<char> used(agents.size(), 0);
    std::vector<int> assignedTo(agents.size(), -1);
    for (auto const& pr : pairs)
    {
        int const cap = preyList[pr.p].queen ? 1000 : PACK_SIZE;
        if (used[pr.a] || taken[pr.p] >= cap)
            continue;
        used[pr.a] = 1;
        assignedTo[pr.a] = pr.p;
        taken[pr.p]++;
        if (agents[pr.a].id == me)
            myPrey = pr.p;
    }
    if (myPrey >= 0)
    {
        myPackSize = taken[myPrey];
        // Rank in the pack (by id): each member takes its own side of her.
        int rank = 0;
        for (std::size_t a = 0; a < agents.size(); a++)
            if (assignedTo[a] == myPrey && agents[a].id < me)
                rank++;
        myPackRank = rank;
    }
}

// Pack mates (other than us) within `r` of a cell, from fleet reports.
int mates_near(int cell, int r)
{
    int k = 0;
    for (auto const& [id, st] : fleet)
        if (id != me && R - st.round <= 3 && pack_member(id, st.len, st.leader) && manhattan(st.cell, cell) <= r)
            k++;
    return k;
}

} // namespace
