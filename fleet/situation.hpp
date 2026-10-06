// L2 SITUATION - roles by id, fleet size targets, phases, majority, leaders / giant / heir,
// our half vs theirs, enemy count, the round-500 tiebreak guess, where the enemy queen is.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

// Fleet state: danger, transition, leaders, mode (sections 11-16)
Role role_for(int id)
{
    std::uint32_t const h = static_cast<std::uint32_t>(id) * 2654435761u;
    int const r = static_cast<int>((h >> 16) % 10);
    if (r < 3) return ROLE_COMBAT;
    if (r < 4) return ROLE_PORTAL;
    if (r < 6) return ROLE_EXPLORER;
    return ROLE_COLLECTOR;
}

// Fleet size the expansion phase aims for.
int expansion_target() { return std::min(unitLimit - QUEEN_SLOTS - 1, std::max(4, N / (DYN_ALLOC ? DYN_DENSITY : EXP_DENSITY))); }
// Growth splits allowed: before the switch round (dynamic) or EXP_END.
bool growth_open() { return DYN_ALLOC ? !switched : R < EXP_END; }
// Fleet floor once dragons feed the queen: more enemies, more dragons kept.
int cons_floor() { return DYN_ALLOC ? 10 + std::min(10, pressureMax / 3) : CONSOL_MIN_UNITS; }

void compute_fleet_state()
{
    int const head = body[0];
    std::unordered_set<int> recent;
    for (auto const& e : enemyReps)
        if (R - e.round <= 60)
            recent.insert(e.id);
    distinctEnemies = static_cast<int>(recent.size());
    // Dynamic allocation: the more enemies we meet, the longer numbers matter
    // and the later we turn them into length. Sticky once switched.
    if (DYN_ALLOC)
    {
        if (firstR < 0)
            firstR = R;
        pressureMax = std::max(pressureMax, distinctEnemies);
        switchRound = SWITCH_FIXED > 0 ? SWITCH_FIXED : SW_LO + (SW_HI - SW_LO) * std::min(pressureMax, SW_E_HI) / SW_E_HI;
        bool const informed = heardPressure || R - firstR >= 8 || firstR <= 1;
        if (!switched && R >= switchRound && informed)
        {
            switched = true;
            queue_fact(msg_phase(), 70, 3);
        }
        if (R % 3 == me % 3)
            queue_fact(msg_phase(), 50, 1); // often: newborns and the queen hear little
    }
    enemyEst = distinctEnemies;
    if (ENEMY_EST && distinctEnemies > 0)
    {
        // We hear of the enemies our fleet happens to see; a swarm is mostly
        // out of view. Scale by the share of the map our dragons watch.
        double const cover = std::max(EST_MIN_COVER_X100 / 100.0, std::min(1.0, units * static_cast<double>(EST_VIEW_CELLS) / N));
        enemyEst = std::max(distinctEnemies, static_cast<int>(distinctEnemies / cover + 0.5));
    }
    outnumbered = enemyEst > units;
    lateSafe = LATE_SAFE && R >= 500 - LATE_SAFE_ROUNDS;
    survivalMode = SURVIVAL && (units <= SURV_UNITS || (distinctEnemies >= SURV_RATIO_MIN && units * SURV_RATIO < distinctEnemies) || lateSafe);

    // Section 12: more opponents around -> keep the big fleet for longer.
    int const d = distinctEnemies;
    int const thr = (d >= 12 ? 400 : d >= 7 ? 360 : d >= 3 ? 310 : d >= 1 ? 250 : 200) - TRANS_SHIFT;
    if (!transitioned)
        transitionRound = thr;
    if (!transitioned && R >= transitionRound)
        transitioned = true;

    localThreat = 0;
    std::unordered_set<int> counted;
    for (auto const& h : heads)
        if (!h.mine)
        {
            localThreat++;
            counted.insert(h.id);
        }
    for (auto const& e : enemyReps)
        if (R - e.round <= 6 && !counted.count(e.id) && manhattan(e.cell, head) <= 8)
        {
            localThreat++;
            counted.insert(e.id);
        }
    if (echoEnemy >= 2)
        localThreat++;

    // P2: the home queen's leash follows the threat around her. Contact pulls
    // her home; quiet lets her eat further out (unlimited in the opening).
    if (units >= ADAPT_FLEET)
        fleetEstablished = true;
    queenOpening = QUEEN_ADAPT && !fleetEstablished && R < ADAPT_OPEN_END;
    queenLeash = QUEEN_LEASH;
    if (QUEEN_ADAPT && amQueen)
    {
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, head) <= THREAT_RADIUS)
                lastThreatR = R;
        for (auto const& e : enemyReps)
            if (R - e.round <= 3 && manhattan(e.cell, head) <= THREAT_RADIUS)
                lastThreatR = R;
        int const quiet = R - lastThreatR - QUIET_ROUNDS;
        if (quiet >= 0 && queenOpening)
            queenLeash = INF;
        else if (quiet >= 0 && R < ADAPT_END_ROUND)
            queenLeash = std::min(LEASH_MAX, QUEEN_LEASH + quiet / 2);
    }

    // Expansion (numbers first): lasts until the fleet first reaches its
    // target. The queen splits too until QUEEN_GROW_ROUND; if the enemy queen
    // is still alive by then she grows instead (once it is dead, staying alive
    // is all that counts, so she keeps splitting while there is room).
    if (EXPANSION)
    {
        if (expansionPhase && (!growth_open() || units >= expansion_target()))
            expansionPhase = false;
        queenSplits = (DYN_ALLOC ? !switched : R < QUEEN_GROW_ROUND) || enemyQueenDead;
    }
    else
        expansionPhase = queenSplits = false;

    // OPEN_PLAN: majority (with hysteresis) ends the opening plan.
    {
        double const ratio = static_cast<double>(units) / std::max(1, est_enemy_units());
        if (!haveMajority && units >= OPEN_MAJ_UNITS && ratio >= MAJ_ON)
            haveMajority = true;
        else if (haveMajority && ratio < MAJ_OFF)
            haveMajority = false;
        openPlan = OPEN_PLAN && !haveMajority && !switched && R < OPEN_PLAN_END;
    }
    // Hunters: a fixed share of the fleet (by id) goes after the enemy queen
    // once the opening is over. The queen and the leaders never hunt.
    {
        std::uint32_t const hh = static_cast<std::uint32_t>(me) * 2246822519u + 3266489917u;
        bool const active = HUNTERS && !(enemyQueenDead && !ourQueenDead) && (R >= HUNT_START || !expansionPhase) && R >= OPEN_FARM_ROUND &&
                            !(HUNT_GATE && (survivalMode || outnumbered) && !ourQueenDead);
        int const slot = static_cast<int>((hh >> 13) % 10);
        hunterSlot = !amQueen && slot < HUNTER_SHARE;
        isHunter = active && !amQueen && !isLeader && slot < HUNTER_SHARE && !openPlan; // OPEN_PLAN: multiply first
        // Guards: their own slice of the fleet; once the enemy queen is dead
        // the hunters come home too.
        isGuard = GUARDS && !amQueen && !isLeader && !ourQueenDead && !isHunter &&
                  ((slot >= HUNTER_SHARE && slot < HUNTER_SHARE + GUARD_SHARE) || (enemyQueenDead && slot < HUNTER_SHARE)) &&
                  (GUARD_MIN_UNITS == 0 || enemyQueenDead || (units >= GUARD_MIN_UNITS && !expansionPhase)) && units > GUARD_FLEET_MIN &&
                  (R >= OPEN_FARM_ROUND || enemyQueenDead);
        isScreen = SCREENS && isGuard &&
                   (slot == HUNTER_SHARE || (SCREENS_MORE && units < 20 && slot == HUNTER_SHARE + 1 && (me & 1)));
        isDefender = OWN_HALF && !amQueen && !isLeader && !isGuard && !isHunter && slot == DEFENDER_SLOT && R >= HUNT_START;
    }
    isWall = false; // set below once the leader election is done

    // Too many leaders (they could not hear each other at election time):
    // step down if enough longer leaders are known.
    amQueen = me == queenId;
    if (amQueen)
        isLeader = true; // the queen leads for the whole game
    else if (isLeader)
    {
        // F1: any longer teammate counts (not just leaders): with patchy sonar
        // few leaders are ever heard of, and nobody stepped down.
        int longer = 0;
        for (auto const& [id, s] : fleet)
            if (id != me && id != queenId && (s.leader || GIANT_ONE) && R - s.round <= 30 && (s.len > L || (s.len == L && id < me)))
                longer++;
        if (longer >= LEADER_COUNT || (GIANT_ONE && !roleLeader && L < LEADER_MIN_LEN - 1))
            isLeader = false;
    }

    // Leader election (section 11): the longest dragons we know of.
    if (transitioned && !expansionPhase && !isLeader && L >= (GIANT_ONE ? LEADER_MIN_LEN : 3))
    {
        int freshLeaders = 0;
        std::vector<std::pair<int, int>> ranking{{-L, me}};
        for (auto const& [id, s] : fleet)
        {
            if (id == me || R - s.round > 30)
                continue;
            if (s.leader)
                freshLeaders++;
            else
                ranking.push_back({-s.len, id});
        }
        int const slots = LEADER_COUNT - freshLeaders;
        std::sort(ranking.begin(), ranking.end());
        for (int i = 0; i < slots && i < static_cast<int>(ranking.size()); i++)
            if (ranking[i].second == me)
                isLeader = true;
    }

    // Heir: once the enemy queen is dead ours only has to live, and the
    // longest of the rest grows (insurance for the longest-dragon tiebreak).
    isHeir = false;
    if (HEIR && enemyQueenDead && !ourQueenDead && !amQueen && !(GIANT_ONE && units < GIANT_MIN_UNITS))
    {
        isHeir = true;
        for (auto const& [id, s] : fleet)
            if (id != me && id != queenId && R - s.round <= 10 && (s.len > L || (s.len == L && id < me)))
                isHeir = false;
        if (isHeir)
            isLeader = true;
    }

    // P5 giant: from GIANT_ROUND the longest of the rest stops splitting and
    // grows on every map, so the queen is never our only long dragon.
    isGiant = false;
    if (GIANT_ALL && R >= GIANT_ROUND && !amQueen && !ourQueenDead && L >= GIANT_MIN_LEN && !isHeir && !(GIANT_ONE && units < GIANT_MIN_UNITS))
    {
        isGiant = true;
        for (auto const& [id, s] : fleet)
        {
            if (id == me || id == queenId)
                continue;
            if (R - s.round <= 10 && (s.len > L || (s.len == L && id < me)))
                isGiant = false;
            // F1: another leader we heard of lately that is at least as long: it is the giant, not us
            if (GIANT_ONE && s.leader && R - s.round <= 30 && (s.len > L || (s.len == L && id < me)))
                isGiant = false;
        }
        if (isGiant)
            isLeader = true;
    }
    // F1: the giant / heir role ended - so does the leadership it gave (it
    // left half the fleet as non-splitting "leaders" on big maps).
    if (GIANT_ONE && !amQueen)
    {
        if (roleLeader && !isGiant && !isHeir)
            isLeader = false;
        roleLeader = isGiant || isHeir;
    }

    isWall = wall_now() && !amQueen && !isLeader && !isHunter && !isGuard && !isDefender && wall_slot(me) && L >= WALL_MIN_LEN;

    // Big open map late in the game: length decides the tiebreaks there and
    // long dragons do not trap themselves, so the fleet turns into length.
    // Small or kelp-dense maps keep their numbers.
    openMap = consolidate = false;
    if (N >= OPEN_MIN_CELLS)
    {
        int known = 0, kelp = 0;
        for (int code = 0; code < 2 * N; code++)
            if (edgeType[code] != E_UNK)
            {
                known++;
                kelp += edgeType[code] == E_KELP;
            }
        openMap = known > 0 && kelp <= OPEN_KELP_MAX * known;
        consolidate = CONSOLIDATE && openMap && R >= CONSOL_ROUND;
    }
    if (DYN_ALLOC)
        consolidate = switched; // every map: after the switch, length

    // Majority on a smaller map: hunt them down, win by elimination.
    majorityHunt = MAJ_HUNT && N <= SMALL_MAP_CELLS && !survivalMode && distinctEnemies >= 1 && units >= MAJ_MIN_UNITS &&
                   units * 10 >= MAJ_RATIO_X10 * enemyEst;

    // Modes (sections 13-15).
    bool enemyNearby = false;
    for (auto const& e : enemyReps)
        if (R - e.round <= 30 && manhattan(e.cell, head) <= 12)
            enemyNearby = true;
    if (localThreat >= 2 || (localThreat >= 1 && role == ROLE_COMBAT))
        mode = MODE_COMBAT;
    else if (transitioned && !enemyNearby && localThreat == 0)
        mode = MODE_FARM;
    else if (transitioned)
        mode = MODE_PRESERVE;
    else
        mode = MODE_NORMAL;
}

// Our half: nearer our queen's start than its mirror image (theirs).
int enemy_spawn_guess()
{
    if (ourQueenSpawn < 0)
        return -1;
    if (symKnown >= 0)
        return mirror_cell(symKnown, ourQueenSpawn);
    for (int k = 0; k < 3; k++)
        if (symMask & (1 << k))
            return mirror_cell(k, ourQueenSpawn);
    return -1;
}
int halfMargin(int c) // > 0 on our side, < 0 on theirs
{
    int const es = enemy_spawn_guess();
    if (es < 0 || es == ourQueenSpawn)
        return 0;
    return manhattan(c, es) - manhattan(c, ourQueenSpawn);
}

int est_enemy_units()
{
    return std::max(enemyEst, 1);
}

// Best guess of the enemy queen's head: in view, recently reported, or the
// mirror image of our queen's start (one guess per symmetry still possible).
// Their feeders die next to their queen: three or more recent death drops
// close together (and not ours) mark where she probably is.
int feeder_queen_guess()
{
    if (!FEEDER_TRAIL || R < 100)
        return -1;
    std::vector<int> ev;
    for (auto const& e : dropEv)
    {
        if (R - e.round > 20 || e.nearOurQueen)
            continue;
        bool ours = false; // one of our dragons died there (a trade or an accident)
        for (auto const& dg : dangers)
            if (std::abs(dg.round - e.round) <= 3 && manhattan(dg.cell, e.cell) <= 3)
                ours = true;
        if (!ours)
            ev.push_back(e.cell);
    }
    int best = -1, bestCnt = 2;
    for (int a : ev)
    {
        int cnt = 0;
        for (int b : ev)
            if (b != a && manhattan(a, b) <= 5)
                cnt++;
        if (cnt >= bestCnt)
        {
            bestCnt = cnt;
            best = a;
        }
    }
    return best;
}

int enemy_queen_target()
{
    for (auto const& h : heads)
        if (!h.mine && h.id == enemyQueenId)
            return h.cell;
    int best = -1, bestRound = -1;
    for (auto const& e : enemyReps)
        if (e.id == enemyQueenId && R - e.round <= 30 && e.round > bestRound)
        {
            best = e.cell;
            bestRound = e.round;
        }
    if (best >= 0)
        return best;
    int const trail = feeder_queen_guess();
    if (trail >= 0)
        return trail;
    if (ourQueenSpawn < 0)
        return -1;
    std::vector<int> ks;
    for (int k = 0; k < 3; k++)
        if (symMask & (1 << k))
            ks.push_back(k);
    if (ks.empty())
        return -1;
    return mirror_cell(ks[me % ks.size()], ourQueenSpawn);
}

// Running guess of the round-500 tiebreaks: queen length, then longest
// dragon, then total length. -1 = unknown.
struct TieState
{
    int ourQ = -1, theirQ = -1, ourLong = 0, theirLong = -1;
};
TieState tie_state()
{
    TieState t;
    if (ourQueenDead)
        t.ourQ = 0;
    else if (me == queenId)
        t.ourQ = L;
    else if (auto const q = fleet.find(queenId); q != fleet.end() && R - q->second.round <= 60)
        t.ourQ = q->second.len;
    if (enemyQueenDead)
        t.theirQ = 0;
    else if (auto const e = enemyLenMem.find(enemyQueenId); e != enemyLenMem.end() && R - e->second.round <= 150)
        t.theirQ = e->second.len;
    t.ourLong = L;
    for (auto const& [id, st] : fleet)
        if (R - st.round <= 60)
            t.ourLong = std::max(t.ourLong, st.len);
    for (auto const& [id, lm] : enemyLenMem)
        if (R - lm.round <= 100)
            t.theirLong = std::max(t.theirLong, lm.len);
    return t;
}

// Giant (insurance for the longest-dragon tiebreak): our longest non-queen
// leader we know of, -1 if none.
int giant_id()
{
    int best = -1, bestLen = 0;
    if (isLeader && !amQueen)
    {
        best = me;
        bestLen = L;
    }
    for (auto const& [id, st] : fleet)
        if (id != queenId && st.leader && R - st.round <= 30 && (st.len > bestLen || (st.len == bestLen && id < best)))
        {
            best = id;
            bestLen = st.len;
        }
    return best;
}

// Is our queen already long enough? Past max(QUEEN_FEED_CAP, their queen's
// known length + margin) feeding her more only adds risk.
bool queen_fed_enough()
{
    TieState const t = tie_state();
    if (t.ourQ < 0)
        return false;
    int const cap = std::max(QUEEN_FEED_CAP, t.theirQ >= 0 ? t.theirQ + QUEEN_FEED_MARGIN : 0);
    return t.ourQ >= cap;
}

} // namespace
