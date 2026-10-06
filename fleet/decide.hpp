// DECIDE - the per-turn decision, in stages: turn setup and candidate analysis (L4), the reflex
// table (L4), the target steps (L5), then feeding / splitting / move scoring (L6).
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

// Per-turn values shared between the stages of decide() (reset every turn).
struct Turn
{
    int head{};
    int facing{};
    bool queenStarving{};
    bool sealedNoSplit{};
    std::array<Cand, 4> cands{};
    int cap{};
    bool anyLegal{};
    bool anySafe{};
    bool attackAvail{};
    int attackDir{};
    int attackLen{};
    double diveMin{};
    bool diving{};
    bool nextToEnemy{};
    bool queenAdjacent{};
    bool allIn{};
    int qNow{};
    std::unordered_set<int> stalkers{};
    bool wantAttack{};
    int freeN{};
    bool packer{};
    std::array<int, 4> qBox{};
    bool bunkerFresh{};
    int postDir{};
    bool capFeed{};
    std::vector<PearlCand> pearls{};
    int target{};
    int portalDir{};
    Intent why{};
    int pi{};
    bool hunter{};
    bool huntOk{};
    int feedId{};
    int feedLeader{};
    bool earlyFeed{};
    bool feedPhase{};
    int guardQ{};
    bool tieBunker{};
    bool bunkerMode{};
    int bunkerDir{};
    bool inBox{};
    bool campWait{};
    int bestD{};
};
Turn tn;

// Turn setup: queen spawn, starvation, sealed start, periodic news, threat map, distances.
bool prepare_turn(Controller& ct)
{
    auto& head = tn.head;
    auto& facing = tn.facing;
    auto& queenStarving = tn.queenStarving;
    auto& sealedNoSplit = tn.sealedNoSplit;
    head = body[0];
    facing = dir_index(ct.get_dir());
    claimTarget = -1;
    if (amQueen && ourQueenSpawn < 0)
        ourQueenSpawn = head;
    if (L > lastLenSeen)
        lastEatR = R; // grew: ate something
    lastLenSeen = L;
    // Starving queen: she is (almost) the only eater and has not eaten for a
    // while. Her portal / unexplored / loop rules would leave her at length
    // 2-3 for ever (no split, no fleet), so with no enemy in view they relax.
    bool enemyInView = false;
    for (auto const& h : heads)
        if (!h.mine)
            enemyInView = true;
    // OPEN_PLAN: a queen with no teammate near her cannot wait for others to
    // find the pearls - she explores herself (the starving relaxations).
    bool queenAlone = false;
    if (OPEN_PLAN && OPEN_QUEEN && sit.openPlan && amQueen && !enemyInView && !sit.lateSafe)
    {
        queenAlone = true;
        for (auto const& h : heads)
            if (h.mine && h.id != me && manhattan(h.cell, body[0]) <= QUEEN_ISO_RAD)
                queenAlone = false;
        for (auto const& [id, st] : fleet)
            if (id != me && R - st.round <= 5 && manhattan(st.cell, body[0]) <= QUEEN_ISO_RAD)
                queenAlone = false;
    }
    queenStarving = (STARVE_RELAX && amQueen && units <= 2 && R >= 30 && R - lastEatR >= STARVE_ROUNDS && !enemyInView && !sit.lateSafe) || queenAlone;
    queenStarvingG = queenStarving;
    headHist.push_back(head);
    if (headHist.size() > 16)
        headHist.erase(headHist.begin());
    // First look: is the queen in a pocket that kelp closes off inside her
    // view, with no portal? A split child there could never get out, so she
    // keeps her length and just circles (she needs one free cell to move).
    if (QUEEN_SEALED_NOSPLIT && amQueen && !sealedChecked)
    {
        sealedChecked = true;
        bool closed = true;
        std::vector<int> reg{head};
        std::unordered_set<int> inReg{head};
        for (std::size_t i = 0; i < reg.size() && closed; i++)
            for (int d = 0; d < 4 && closed; d++)
            {
                std::uint8_t const et = edgeType[edge_code(reg[i], d)];
                if (et == E_KELP)
                    continue;
                if (et != E_OPEN)
                {
                    closed = false; // portal or unexplored edge: not sealed
                    break;
                }
                int const n = nb(reg[i], d);
                if (!vis[n])
                {
                    closed = false; // the pocket runs out of view
                    break;
                }
                if (inReg.insert(n).second)
                    reg.push_back(n);
            }
        queenSealed = closed;
        sealedCells = closed ? static_cast<int>(reg.size()) : 0;
    }
    sealedNoSplit = amQueen && queenSealed && L <= sealedCells - 1;
    if (ourQueenSpawn < 0 && R <= 1)
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                ourQueenSpawn = h.cell;
    if (HUNTERS && ourQueenSpawn >= 0 && (R % 20 == me % 20 || (amQueen && R < 5)))
        queue_fact(msg_qspawn(ourQueenSpawn), amQueen ? 60 : 40, 1);
    // OPEN_PLAN zones: standing in a rich spot - tell the fleet.
    if (ZONES && !body.empty() && R % ZONE_MSG_EVERY == me % ZONE_MSG_EVERY)
    {
        int const t = spawn_tiles_near(body[0], RV_RAD);
        if (t >= ZONE_TILES)
            queue_fact(msg_zone(body[0], t), 45, 1);
    }
    if (EQ_DEATH_DETECT && sit.enemyQueenDead && R % (EQ_INFER ? 3 : 8) == me % (EQ_INFER ? 3 : 8))
        queue_fact(msg_queen_dead(true), EQ_INFER ? 85 : 55, EQ_INFER ? 2 : 1); // keep telling: relays are lossy (F2: the queen must hear it)
    if (EQ_DEATH_DETECT && sit.ourQueenDead && R % 8 == (me + 4) % 8)
        queue_fact(msg_queen_dead(false), 55, 1);

    if (amQueen && QUEEN_THREAT_MAP)
    {
        compute_queen_threat();
        for (int c = 0; c < N; c++)
            if (qThreatVis[c] == 0 && !(vis[c] && occId[c] >= 0))
                contested[c] = 1;
    }
    set_body(body, L);
    plan_bfs(head, distHead);
    return false;
}

// L4 candidate analysis: the four moves (legal, safe, area, survival, loop) and dead-end dives.
bool analyse_moves(Controller& ct)
{
    auto& head = tn.head;
    auto& cands = tn.cands;
    auto& cap = tn.cap;
    auto& anyLegal = tn.anyLegal;
    auto& anySafe = tn.anySafe;
    auto& attackAvail = tn.attackAvail;
    auto& attackDir = tn.attackDir;
    auto& attackLen = tn.attackLen;
    auto& diveMin = tn.diveMin;
    auto& diving = tn.diving;
    // ---- Candidate moves ------------------------------------------------
    cap = std::min(N, 3 * L + 24);
    anyLegal = false, anySafe = false, attackAvail = false;
    attackDir = -1, attackLen = 0;
    for (int d = 0; d < 4; d++)
    {
        Cand& cd = cands[d];
        cd.d = d;
        int const dest = cross(head, d);
        cd.dest = dest;
        if (dest == -1 || dest == -2)
            continue;
        if (dest == -3)
        {
            cd.legal = true;
            cd.blind = true;
            anyLegal = true;
            continue;
        }
        if (vis[dest] && occId[dest] >= 0 && occId[dest] != me)
        {
            if (occHead[dest] && !occMine[dest])
            {
                cd.attack = true;
                attackAvail = true;
                attackDir = d;
                for (auto const& h : heads)
                    if (h.cell == dest)
                        attackLen = h.len;
            }
            continue;
        }
        // B1b: a cell we can see with no dragon on it is always enterable (that is the engine's rule); our body
        // model can hold stale segments after a portal crossing, so vision decides for visible cells.
        bool const visiblyFree = LEGAL_BY_VISION && vis[dest] && occId[dest] < 0 && wallStamp[dest] != wstamp;
        if ((!passable(dest, 1) && !visiblyFree) || (vis[dest] && occId[dest] == me))
            continue;
        cd.legal = true;
        anyLegal = true;
        cd.eat = pearl_at(dest);
        std::vector<int> nb1;
        nb1.reserve(body.size() + 1);
        nb1.push_back(dest);
        nb1.insert(nb1.end(), body.begin(), body.end());
        int const nl = L + (cd.eat ? 1 : 0);
        if (!cd.eat && static_cast<int>(body.size()) >= L)
            nb1.pop_back();
        int const tailCell = static_cast<int>(nb1.size()) >= nl ? nb1.back() : -1;
        set_body(nb1, nl);
        FloodRes f1 = flood(dest, tailCell, cap);
        cd.area = f1.area + (f1.frontier && !amQueen ? nl : 0); // the queen trusts only known water
        // A snake cannot turn round in a corridor, so raw area is only trusted
        // when there is room to circle; reaching our own tail is always an exit.
        bool const frontierExit = FRONTIER_OPEN && !my.leader; // small dragons may explore narrow passages
        cd.ok1 = f1.tail || cd.area >= 2 * nl + 4 || (frontierExit && f1.frontier);

        // Depth 2: is there a follow-up that keeps us free?
        for (int d2 = 0; d2 < 4 && !cd.ok2; d2++)
        {
            int const n2 = cross(dest, d2);
            if (n2 < 0)
                continue;
            set_body(nb1, nl);
            if (!passable(n2, 1) || contested[n2] || (vis[n2] && occId[n2] == me && ownStamp[n2] != stamp))
                continue;
            bool const eat2 = pearl_at(n2);
            std::vector<int> nb2{n2};
            nb2.insert(nb2.end(), nb1.begin(), nb1.end());
            int const nl2 = nl + (eat2 ? 1 : 0);
            if (!eat2 && static_cast<int>(nb1.size()) >= nl)
                nb2.pop_back();
            int const tail2 = static_cast<int>(nb2.size()) >= nl2 ? nb2.back() : -1;
            set_body(nb2, nl2);
            FloodRes const f2 = flood(n2, tail2, cap);
            if (f2.tail || f2.area + (f2.frontier && !amQueen ? nl2 : 0) >= 2 * nl2 + 2 || (frontierExit && f2.frontier))
                cd.ok2 = true;
        }
        if (amQueen && cd.ok2 && !queen_can_survive(nb1, nl))
            cd.ok1 = cd.ok2 = false; // a corridor with no way back
        // Pocket check: a portal box or a small walled area can look roomy to
        // the flood (it counts water beyond the box's other portals, and our
        // tail as an exit). Can the body really keep moving in there?
        if (POCKET_CHECK && !amQueen && cd.ok2)
        {
            bool const viaPortal = dest != nb(head, d);
            bool const smallEnclosed = !f1.frontier && f1.area < 3 * nl;
            if ((viaPortal || smallEnclosed) && !queen_can_survive(nb1, nl, POCKET_NODE_CAP))
                cd.ok1 = cd.ok2 = false; // a trap - the dead-end dive logic decides if its pearls are worth it
        }
        // F7: a 1-wide dead end we can see to its bottom is a trap whatever
        // the flood says (a snake cannot turn round in it); the dive logic
        // below decides whether its pearls pay for the split.
        if (SLOT_CHECK && !amQueen && cd.ok1 && dest == nb(head, d) && known_dead_end(head, dest))
            cd.ok1 = cd.ok2 = false;
        // 7b: the same for a dead-end branch of any shape (a tree of corridors, a room with no loop longer than us).
        if (TOPO_TRAP && !amQueen && cd.ok1 && topo_trap(head, d, nl))
            cd.ok1 = cd.ok2 = false;
        if (QUEEN_LOOP_SAFE && amQueen)
            cd.noLoop = !loop_reachable(nb1, nl);
        if (cd.ok2)
            anySafe = true;
        cd.nb1 = std::move(nb1);
    }
    // Dead ends with pearls (non-queens): worth it when the pearls pay for
    // the survival split at the bottom (the user's compensation rule).
    double bestDive = -1e9;
    // Each dive node floods ~2L+4 cells, so the node budget shrinks with
    // length: a long dragon costs no more per candidate than a short one.
    diveCap = DIVE_NODE_CAP * 12 / std::max(12, 2 * L + 4);
    // Long dragons never mine dead ends (a giant in a pocket is a giant lost),
    // and their dive search is the costliest thing a turn can do.
    if (DIVE_ENABLE && !amQueen && L <= DIVE_MAX_LEN)
        for (auto& cd : cands)
            if (cd.legal && !cd.blind && !cd.ok1 && !cd.nb1.empty() && !contested[cd.dest])
            {
                diveNodes = 0;
                std::vector<int> ec;
                if (cd.eat)
                    ec.push_back(cd.dest);
                cd.dive = dive_rec(cd.nb1, L + (cd.eat ? 1 : 0), 1, cd.eat ? 1 : 0, ec);
                bestDive = std::max(bestDive, cd.dive);
            }
    // Pocket mining: with a teammate close by, a dive that only breaks even is
    // worth it - the stub left inside recycles into pearls for the teammate.
    bool mateNearHead = false;
    for (auto const& h : heads)
        if (h.mine && h.id != me && manhattan(h.cell, head) <= 5)
            mateNearHead = true;
    diveMin = mateNearHead && !amQueen ? DIVE_MATE_GAIN : DIVE_MIN_GAIN;
    diving = bestDive >= diveMin;
    // The queen with no safe move: still tell a corridor that leads on from a
    // known dead end (unexplored water counts as a way on here).
    if (QUEEN_SURV_RANK && amQueen && !anySafe)
        for (auto& cd : cands)
            if (cd.legal && !cd.blind && !cd.nb1.empty())
            {
                ignoreContested = false;
                std::vector<int> const& b1 = cd.nb1;
                set_body(b1, L + (cd.eat ? 1 : 0));
                pstamp++;
                survNodes = 0;
                pathStamp[b1[0]] = pstamp;
                pathTime[b1[0]] = 0;
                int const len1 = L + (cd.eat ? 1 : 0);
                cd.surv = survive_rec(b1[0], 0, std::min(2 * len1 + 6, 50), len1);
            }
    set_body(body, L);
#ifdef FLEET_DEBUG
    {
        std::string dbg = "head " + std::to_string(head % W) + "," + std::to_string(head / W) + " body" + std::to_string(body.size()) + "/" + std::to_string(L);
        for (auto const& cd : cands)
        {
            int const code = edge_code(head, cd.d);
            dbg += std::string(" ") + DCH[cd.d] + ":dest" + std::to_string(cd.dest) + " et" + std::to_string(edgeType[code]) +
                   " oe" + std::to_string(obsEdge[code]) + " L" + std::to_string(cd.legal) + " ok" + std::to_string(cd.ok1) + std::to_string(cd.ok2);
            if (cd.dest >= 0)
                dbg += " occ" + std::to_string(occId[cd.dest]) + " own" + std::to_string(ownStamp[cd.dest] == stamp) + "/" + std::to_string(ownRel[cd.dest]);
            if (cd.dive > -1e9)
                dbg += " dive" + std::to_string(cd.dive).substr(0, 4);
        }
        for (auto const& h : heads)
            dbg += " H" + std::to_string(h.id) + (h.mine ? "m@" : "e@") + std::to_string(h.cell % W) + "," + std::to_string(h.cell / W) + "l" + std::to_string(h.len);
        for (int tc : enemyTails)
            dbg += " T@" + std::to_string(tc % W) + "," + std::to_string(tc / W);
        ct.output_log(dbg);
    }
#endif
    return false;
}

// L4 contact: enemy heads next to us, stalkers of our queen, which head-on trades we want.
bool read_contact(Controller& ct)
{
    auto& head = tn.head;
    auto& cands = tn.cands;
    auto& attackAvail = tn.attackAvail;
    auto& attackDir = tn.attackDir;
    auto& attackLen = tn.attackLen;
    auto& nextToEnemy = tn.nextToEnemy;
    auto& queenAdjacent = tn.queenAdjacent;
    auto& allIn = tn.allIn;
    auto& qNow = tn.qNow;
    auto& stalkers = tn.stalkers;
    auto& wantAttack = tn.wantAttack;
    auto& freeN = tn.freeN;
    nextToEnemy = is_adjacent_enemy_head(head);
    queenAdjacent = false;
    for (auto const& cd : cands)
        if (cd.attack && occId[cd.dest] == enemyQueenId && !sit.enemyQueenDead)
        {
            queenAdjacent = true;
            attackDir = cd.d;
        }
    allIn = sit.ourQueenDead && !sit.enemyQueenDead; // we lose at round 500 unless their queen dies
    // v2.14 anti-escort: an enemy head that stays within 2 of our queen for a
    // few rounds is stalking her (runs beside her along a wall until she is
    // boxed). The queen reports it; any short teammate strikes it.
    qNow = -1;
    if (!sit.ourQueenDead)
    {
        if (amQueen)
            qNow = head;
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qNow = h.cell;
        auto const qf = fleet.find(queenId);
        if (qNow < 0 && qf != fleet.end() && R - qf->second.round <= 1)
            qNow = qf->second.cell;
    }
    if (ANTI_ESCORT && qNow >= 0)
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, qNow) <= 2)
            {
                auto& es = escortSeen[h.id];
                if (es.first != R)
                {
                    es.second = es.first == R - 1 ? es.second + 1 : 1;
                    es.first = R;
                }
                if (es.second >= ESCORT_ROUNDS)
                    stalkers.insert(h.id);
            }
    if (amQueen)
        for (auto const& h : heads)
            if (stalkers.count(h.id))
            {
                queue_fact(msg_danger(h.cell, 1), 88, 2);
                queue_fact(msg_enemy(h.cell, h.id, h.len), 86, 2);
            }
    int stalkerDir = -1;
    for (auto const& cd : cands)
        if (cd.attack && stalkers.count(occId[cd.dest]))
            stalkerDir = cd.d;
    // A head-on kills both dragons. While expanding, numbers are what we are
    // building, so only trade up: the enemy queen, a longer enemy, or when we
    // clearly outnumber them.
    bool const evenTrades = !(EXPANSION && sit.expansion);
    // v2.14: their dragons rarely attack ours, so a 1-for-1 only helps a swarm
    // that out-produces us. Trade up only (or with a clear majority).
    // Not outnumbered (by the scaled count): even trades as before (an
    // opponent that attacks our dragons must be answered).
    bool const tradeUp = !TRADE_UP || allIn || sit.majorityHunt || attackLen >= TRADE_UP_RATIO * L || (!sit.outnumbered && !sit.survival);
    wantAttack = attackAvail && !amQueen &&
                            ((queenAdjacent) || (stalkerDir >= 0 && !my.leader) ||
                             (!my.leader && tradeUp && (my.role == ROLE_COMBAT || sit.mode == MODE_COMBAT || allIn) &&
                              (evenTrades ? (L <= attackLen + 1 || units > est_enemy_units() + 1 || L <= 4 || allIn)
                                          : (attackLen > L || units > est_enemy_units() + 1 || allIn)) &&
                              // Swarms out-produce us: a 1-for-1 trade only pays while we are ahead.
                              (!SAFE_TRADES || allIn || (!sit.survival && (attackLen > L || !sit.outnumbered)))));
    if (!queenAdjacent && stalkerDir >= 0 && !my.leader)
        attackDir = stalkerDir;
    freeN = std::min(free_steps(), MAX_SPRINT);
    return false;
}

// Reflex: strike the visible enemy queen with free + paid steps.
bool rx_regicide_paid(Controller& ct)
{
    auto& head = tn.head;
    // ---- 0. Sprint into the enemy queen ("one smells blood") -----------------
    if (PAID_REGICIDE && !amQueen && !sit.enemyQueenDead)
        for (auto const& h : heads)
            if (!h.mine && h.id == enemyQueenId)
            {
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(h.cell, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(head, 1), 70, 2);
                    if (EQ_DEATH_DETECT)
                        queue_fact(msg_queen_dead(true), 99, 4); // certain: she moved before us
                    return act_moves(ct, path, "regicide");
                }
            }
    return false;
}

// Reflex: strike a stalker running beside our queen.
bool rx_escort_hit(Controller& ct)
{
    auto& stalkers = tn.stalkers;
    if (ANTI_ESCORT && !amQueen && !my.leader && L <= 8 && !stalkers.empty())
        for (auto const& h : heads)
            if (!h.mine && stalkers.count(h.id))
            {
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(h.cell, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(h.cell, 1), 75, 2);
                    return act_moves(ct, path, "escort-hit");
                }
            }
    return false;
}

// Reflex: a short dragon strikes a longer enemy head (never trades down).
bool rx_snipe(Controller& ct)
{
    // Snipe: a short dragon that can put its head on an enemy head this turn
    // (free + paid steps) and is no longer than it strikes: a head-on kills
    // both, so it never trades down. Witnesses report the death drops.
    if (SNIPE && !amQueen && !my.leader && L <= SNIPE_MAX_LEN &&
        (!SAFE_TRADES || (!sit.survival && units >= est_enemy_units() + 4 && units >= 8)))
    {
        std::vector<HeadInfo const*> targets;
        for (auto const& h : heads)
            if (!h.mine && h.id != enemyQueenId && (sit.majorityHunt || (TRADE_UP && sit.outnumbered ? h.len >= TRADE_UP_RATIO * L : h.len >= L)))
                targets.push_back(&h);
        std::sort(targets.begin(), targets.end(), [](HeadInfo const* a, HeadInfo const* b) { return a->len > b->len; });
        int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
        for (auto const* h : targets)
        {
            std::vector<int> const path = regicide_path(h->cell, reach);
            set_body(body, L);
            if (!path.empty())
            {
                queue_fact(msg_danger(h->cell, 1), 70, 2);
                return act_moves(ct, path, "snipe");
            }
        }
    }
    return false;
}

// Reflex: pack members strike their prey (or any long enemy) in reach.
bool rx_pack_strike(Controller& ct)
{
    auto& allIn = tn.allIn;
    auto& packer = tn.packer;
    // Pack members: work out the shared prey, and strike a long enemy dragon
    // when its head is within reach (a short dragon for a long one).
    packer = HUNTERS && !amQueen && !my.leader && !my.guard && (my.hunter || my.wall || (ENDGAME_PACKS && sit.ourQueenDead && L <= 6)) &&
                        (allIn || !(SAFE_TRADES && (sit.survival || sit.outnumbered)));
    if (packer)
    {
        assign_prey();
        int const preyId = myPrey >= 0 ? preyList[myPrey].id : -1;
        for (auto const& h : heads)
            if (!h.mine && h.id != enemyQueenId && (h.id == preyId || h.len >= std::max(PREY_MIN_LEN, L + 4)))
            {
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(h.cell, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(h.cell, 1), 70, 2);
                    return act_moves(ct, path, "pack-strike");
                }
            }
    }
    return false;
}

// Reflex: free-step sprint into the enemy queen.
bool rx_regicide_sprint(Controller& ct)
{
    auto& head = tn.head;
    auto& queenAdjacent = tn.queenAdjacent;
    auto& freeN = tn.freeN;
    if (!amQueen && !sit.enemyQueenDead && !queenAdjacent)
    {
        bool near = false;
        for (auto const& h : heads)
            if (!h.mine && h.id == enemyQueenId && manhattan(h.cell, head) <= std::min(freeN + 1, 3))
                near = true;
        if (near)
        {
            SprintPlan kp;
            std::vector<int> dirs, eatenCells;
            sprint_dfs(body, L, 0, std::min(freeN + 1, 3), freeN, 0, dirs, eatenCells, false, 0, kp);
            set_body(body, L);
            if (kp.kill)
            {
                queue_fact(msg_danger(head, 1), 70, 2);
                if (EQ_DEATH_DETECT)
                    queue_fact(msg_queen_dead(true), 99, 4);
                return act_moves(ct, kp.dirs, "regicide");
            }
        }
    }
    return false;
}

// Reflex (STALKER_INTERCEPT, off): strike heads that threaten our queen.
bool rx_intercept(Controller& ct)
{
    // Short teammates strike enemy heads that have our queen within their
    // reach (one of ours for one attacker; the queen moves first and cannot
    // dodge a sprint).
    if (STALKER_INTERCEPT && !amQueen && !my.leader && !sit.ourQueenDead && L <= 6)
    {
        int qc = -1;
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qc = h.cell;
        auto const q = fleet.find(queenId);
        if (qc < 0 && q != fleet.end() && R - q->second.round <= 2)
            qc = q->second.cell;
        if (qc >= 0)
            for (auto const& h : heads)
            {
                if (h.mine)
                    continue;
                int const el = std::max(h.len, h.estLen);
                if (manhattan(h.cell, qc) > (el + 3) / 4 + std::max(0, el - 2) + 1)
                    continue;
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(h.cell, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(h.cell, 1), 75, 2);
                    return act_moves(ct, path, "intercept");
                }
            }
    }
    return false;
}

// Reflex (QUEEN_PAID_ESCAPE, off): the queen sprints out of every reach.
bool rx_queen_paid_escape(Controller& ct)
{
    auto& cands = tn.cands;
    auto& anyLegal = tn.anyLegal;
    // The queen: every single step ends inside some enemy's reach. Sprint out
    // with free steps, paying segments if needed (length is cheaper than her).
    if (QUEEN_PAID_ESCAPE && QUEEN_HARD_THREAT && QUEEN_THREAT_MAP && amQueen && anyLegal)
    {
        double bestSingle = 1e18;
        for (auto const& cd : cands)
            if (cd.legal && !cd.blind && cd.dest >= 0)
                bestSingle = std::min(bestSingle, queen_threat_pen(cd.dest));
        if (bestSingle >= 0.5 * HARD_THREAT_PEN)
        {
            SprintPlan ep;
            std::vector<int> dirs, eatenCells;
            int const steps = std::min(4, free_steps() + std::max(0, L - 2));
            if (steps >= 2)
                sprint_dfs(body, L, 0, steps, free_steps(), 0, dirs, eatenCells, false, 0, ep);
            set_body(body, L);
            if (!ep.dirs.empty() && ep.value > -0.5 * HARD_THREAT_PEN)
            {
                int el = 0;
                std::vector<int> const endBody = simulate_steps(body, L, ep.dirs, el);
                bool const ok = !endBody.empty() && queen_can_survive(endBody, el) &&
                                !(QUEEN_LOOP_SAFE && !loop_reachable(endBody, el));
                set_body(body, L);
                if (ok)
                    return act_moves(ct, ep.dirs, "q-escape");
            }
        }
    }
    return false;
}

// Reflex: no legal move - unwind split, queen stall, last attack, save split, or die harmlessly.
bool rx_no_legal_move(Controller& ct)
{
    auto& head = tn.head;
    auto& facing = tn.facing;
    auto& anyLegal = tn.anyLegal;
    auto& attackAvail = tn.attackAvail;
    auto& attackDir = tn.attackDir;
    auto& nextToEnemy = tn.nextToEnemy;
    // ---- 1. Death is certain: survival split (sections 3, 12) --------------
    if (!anyLegal)
    {
        // Boxed in by our own body: split so that the blocking segment becomes
        // the child's tail. The child moves later this round, which frees that
        // cell, and the front part keeps as many segments as possible.
        if (static_cast<int>(body.size()) >= L)
        {
            int bestKeep = -1;
            for (int d = 0; d < 4; d++)
            {
                int const n = cross(head, d);
                if (n < 0)
                    continue;
                for (int i = 2; i <= L - 2; i++)
                    if (body[i] == n && i > bestKeep)
                        bestKeep = i;
            }
            if (bestKeep >= 2 && split_allowed(ct, L - bestKeep) && (!amQueen || bestKeep >= L - 2 || bestKeep >= 4 || L < 6))
                return act_split(ct, L - bestKeep, "unwind-split");
        }
        // Stalling only helps if something around the head will move away:
        // another dragon, or our own last two segments (they leave as the child).
        bool willClear = false;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(head, d);
            if (n < 0)
                continue;
            if (otherRel[n])
                willClear = true;
            for (int i = std::max(0, static_cast<int>(body.size()) - 2); i < static_cast<int>(body.size()); i++)
                if (body[i] == n && static_cast<int>(body.size()) >= L)
                    willClear = true;
        }
        if (amQueen && willClear && L >= 4 && split_allowed(ct, 2))
        {
            // Splitting is not a move: the queen keeps its head, survives this
            // turn and keeps all but 2 segments (the child takes the tail).
            queue_fact(msg_danger(head, nextToEnemy ? 1 : 2), 80, 3);
            return act_split(ct, 2, "queen-stall");
        }
        if (attackAvail && !my.leader && L < 6 && !(QUEEN_NO_SUICIDE && amQueen && L >= 4 && split_allowed(ct, L - 2)))
            return act_move(ct, attackDir, "last-attack");
        if (L >= 4 && split_allowed(ct, L - 2))
        {
            queue_fact(msg_danger(head, nextToEnemy ? 1 : 2), 80, 3);
            return act_split(ct, L - 2, "save-split");
        }
        if (attackAvail)
            return act_move(ct, attackDir, "last-attack");
        // Die without taking a teammate along: never into a friendly head
        // (least of all the queen's); kelp, bodies or our own neck are harmless.
        int deathDir = -1, deathRank = 99;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(head, d);
            int rank = 2;
            if (n >= 0 && vis[n] && occId[n] >= 0 && occHead[n] && occMine[n])
                rank = occId[n] == queenId ? 9 : 8;
            else if (n == -3 || (n >= 0 && !vis[n]))
                rank = 0; // unknown far side: might even survive
            else if (n >= 0 && occId[n] >= 0 && !occMine[n])
                rank = 1; // enemy body
            if (rank < deathRank)
            {
                deathRank = rank;
                deathDir = d;
            }
        }
        return act_move(ct, deathDir >= 0 ? deathDir : facing, "doomed");
    }
    return false;
}

// Reflex: no safe single step - sprint to safety.
bool rx_escape_sprint(Controller& ct)
{
    auto& anySafe = tn.anySafe;
    auto& diving = tn.diving;
    auto& freeN = tn.freeN;
    if (!anySafe && !diving && (freeN >= 2 || L >= 5))
    {
        SprintPlan ep;
        std::vector<int> dirs, eatenCells;
        sprint_dfs(body, L, 0, std::min(freeN + (L >= 5 ? 1 : 0), 3), freeN, 0, dirs, eatenCells, false, 0, ep);
        set_body(body, L);
        bool escOk = ep.kill || ep.value > (amQueen && QUEEN_THREAT_MAP ? -1e6 : QUEEN_EXIT_GUARD ? -0.5 * QUEEN_EXIT_PEN : -1e17);
        if (escOk && amQueen && QUEEN_LOOP_SAFE && !ep.kill && !ep.dirs.empty())
        {
            int el = 0;
            std::vector<int> const endBody = simulate_steps(body, L, ep.dirs, el);
            if (!endBody.empty() && !loop_reachable(endBody, el))
                escOk = false; // an escape into a pocket only delays it
            set_body(body, L);
        }
        if (escOk)
            return act_moves(ct, ep.dirs, ep.kill ? "regicide" : "escape");
    }
    return false;
}

// Reflex: trapped - survival split (a pearl one step away is eaten first).
bool rx_trap_split(Controller& ct)
{
    auto& head = tn.head;
    auto& cands = tn.cands;
    auto& anySafe = tn.anySafe;
    auto& diving = tn.diving;
    auto& nextToEnemy = tn.nextToEnemy;
    if (!anySafe && !diving && !amQueen && L >= 4 && split_allowed(ct, L - 2) && child_can_escape(L - 2))
    {
        // Trapped but a pearl is one step away: eat it first (compensation),
        // the survival split is still available next turn.
        bool anyBlind = false, pearlFirst = false;
        for (auto const& cd : cands)
        {
            anyBlind |= cd.blind;
            pearlFirst |= cd.legal && cd.eat;
        }
        if (!anyBlind && !pearlFirst)
        {
            queue_fact(msg_danger(head, nextToEnemy ? 1 : 2), 80, 3);
            return act_split(ct, L - 2, "trap-split");
        }
    }
    return false;
}

// Reflex: the queen is about to be struck - her length lives on in a child.
bool rx_insure_split(Controller& ct)
{
    auto& head = tn.head;
    auto& cands = tn.cands;
    auto& anyLegal = tn.anyLegal;
    // Insurance: the queen is about to be struck - every move ends inside an
    // enemy's reach and a visible enemy head is within its free steps of
    // her. She splits off all but 2: if she dies, the length lives on in the
    // child (the tail end, away from the striker) instead of vanishing.
    if (QUEEN_INSURE && !QUEEN_NO_SUICIDE && amQueen && anyLegal && QUEEN_THREAT_MAP && L >= QUEEN_INSURE_LEN && static_cast<int>(body.size()) >= L)
    {
        bool allHit = true, closeStriker = false;
        for (auto const& cd : cands)
            if (cd.legal && (cd.blind || cd.dest < 0 || qThreat[cd.dest] >= INF))
                allHit = false;
        for (auto const& h : heads)
            if (!h.mine)
            {
                int const el = std::max(h.len, h.estLen);
                if (manhattan(h.cell, head) <= (el + 3) / 4 + 1)
                    closeStriker = true;
            }
        if (allHit && closeStriker && split_allowed(ct, L - 2) && child_can_escape(L - 2))
        {
            queue_fact(msg_danger(head, 1), 80, 3);
            return act_split(ct, L - 2, "insure-split");
        }
    }
    return false;
}

// Reflex: boxed in for good next to a teammate - become pearls for it.
bool rx_recycle(Controller& ct)
{
    auto& head = tn.head;
    auto& cands = tn.cands;
    auto& anySafe = tn.anySafe;
    auto& diving = tn.diving;
    // Recycle: boxed in for good (every move leaves less room than our own
    // length) with a teammate close by. The drop is the same either way;
    // dying now, next to the teammate, means it actually gets eaten.
    if (RECYCLE && !anySafe && !diving && !amQueen && !my.leader && L >= 3 && body.size() >= 2 && (!SURV_ECON || units >= RECYCLE_MIN_UNITS))
    {
        bool boxed = true, pearlFirst = false;
        for (auto const& cd : cands)
        {
            if (!cd.legal)
                continue;
            if (cd.blind || cd.ok1 || cd.area >= L)
                boxed = false;
            pearlFirst |= cd.eat;
        }
        bool mateNear = false;
        for (auto const& h : heads)
            if (h.mine && h.id != me)
                for (int i = 0; i < L && i < static_cast<int>(body.size()); i++)
                    if (manhattan(h.cell, body[i]) <= RECYCLE_MATE_DIST)
                        mateNear = true;
        if (boxed && !pearlFirst && mateNear)
            for (int d = 0; d < 4; d++)
                if (nb(head, d) == body[1])
                {
                    queue_fact(msg_danger(head, 2), 60, 1);
                    return act_move(ct, d, "recycle");
                }
    }
    return false;
}

// Reflex: a wanted head-on attack.
bool rx_attack(Controller& ct)
{
    auto& head = tn.head;
    auto& attackDir = tn.attackDir;
    auto& wantAttack = tn.wantAttack;
    // ---- 2. Head-on attack (sections 15, 16) ------------------------------
    if (wantAttack)
    {
        queue_fact(msg_danger(head, 1), 70, 2);
        return act_move(ct, attackDir, "attack");
    }
    return false;
}

// Target: guards infer the queen's bunker box from vision; cap feeding.
bool tg_bunker_watch(Controller& ct)
{
    auto& head = tn.head;
    auto& qBox = tn.qBox;
    auto& bunkerFresh = tn.bunkerFresh;
    auto& postDir = tn.postDir;
    auto& capFeed = tn.capFeed;
    // ---- Target ----------------------------------------------------------
    // Cap feeding: once the fleet is at its cap, more pearls cannot become
    // dragons, so the spare ones go to the queen - only while she is safe
    // (known position, no enemy reported near her lately) and we are not in
    // survival mode. Self-throttling: a feed drops the fleet below the cap.
    // Sonar rarely reaches a guard, so a guard that sees her also works it out:
    // late in the game (or once a bunker is likely), a length-3 queen whose
    // whole body sits inside one 2x2 box two rounds running is bunkered there.
    if (BUNKER_GUARDS && !amQueen && !sit.ourQueenDead && (sit.lateSafe || sit.enemyQueenDead || R >= TIE_BUNKER_ROUND))
        for (auto const& h : heads)
            if (h.mine && h.id == queenId && h.len == 3)
            {
                std::vector<int> qcells;
                for (int c = 0; c < N; c++)
                    if (vis[c] && occId[c] == queenId)
                        qcells.push_back(c);
                if (static_cast<int>(qcells.size()) != 3)
                    break;
                int const tls[4] = {h.cell, nb(h.cell, 3), nb(h.cell, 0), nb(nb(h.cell, 3), 0)};
                std::array<int, 4> bx{};
                for (int tl : tls)
                {
                    if (!box_cells(tl, bx))
                        continue;
                    bool all = true;
                    for (int c : qcells)
                        if (std::find(bx.begin(), bx.end(), c) == bx.end())
                            all = false;
                    if (!all)
                        continue;
                    if (inferTl == tl && inferR == R - 1)
                    {
                        qBunkerTl = tl;
                        qBunkerR = R;
                    }
                    inferTl = tl;
                    inferR = R;
                    break;
                }
            }
    // The queen is bunkered (announced or seen within the last 5 rounds): her box.
    bunkerFresh = BUNKER_GUARDS && !amQueen && !sit.ourQueenDead && qBunkerTl >= 0 && R - qBunkerR <= 5 && box_cells(qBunkerTl, qBox);
    postDir = -1;
    capFeed = false;
    // Open maps only: on crowded maps the feeders' bodies wall her in.
    if (CAP_FEED && sit.openMap && !sit.ourQueenDead && !sit.survival && R >= CAP_FEED_ROUND && !room_for_more())
    {
        int qk = amQueen ? head : -1;
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qk = h.cell;
        auto const qf = fleet.find(queenId);
        if (qk < 0 && qf != fleet.end() && R - qf->second.round <= 30)
            qk = qf->second.cell;
        if (qk >= 0)
        {
            capFeed = !queen_fed_enough();
            for (auto const& e : enemyReps)
                if (R - e.round <= 10 && manhattan(e.cell, qk) <= 10)
                    capFeed = false;
        }
    }
    return false;
}

// Target (economy): the matched pearl, with commitment.
bool tg_pearl(Controller& ct)
{
    auto& pearls = tn.pearls;
    auto& target = tn.target;
    auto& portalDir = tn.portalDir;
    auto& why = tn.why;
    auto& pi = tn.pi;
    pearls = gather_pearls();
    target = -1;
    portalDir = -1;
    why = I_EXPLORE;
    pi = choose_pearl(pearls);
    // Commitment: switching pearls every other turn wastes the moves made so
    // far (in a maze the two pearls are often in different corridors).
    if (TARGET_COMMIT && lastPearl >= 0 && (!amQueen || QUEEN_COMMIT))
    {
        int prevIdx = -1;
        for (int k = 0; k < static_cast<int>(pearls.size()); k++)
            if (pearls[k].cell == lastPearl)
                prevIdx = k;
        if (prevIdx >= 0 && prevIdx != pi && distHead[lastPearl] < INF)
        {
            int const dPrev = distHead[lastPearl];
            int const dNew = pi >= 0 ? distHead[pearls[pi].cell] : INF;
            bool const clearlyBetter = pi >= 0 && dNew + COMMIT_MARGIN <= dPrev && dNew * 10 <= dPrev * 7;
            if (!clearlyBetter)
                pi = prevIdx;
        }
    }
    if (pi >= 0)
    {
        target = pearls[pi].cell;
        claimTarget = target;
        claimEta = distHead[target];
        why = I_PEARL;
    }
    return false;
}

// Target (queen): late / lead safety, evasion, flight from a crowd.
bool tg_queen_caution(Controller& ct)
{
    auto& head = tn.head;
    auto& target = tn.target;
    auto& why = tn.why;
    // Last rounds: a living queen beats any pearl (their queen is often dead
    // already and we cannot know it). Only pearls right next to her that no
    // enemy can reach; otherwise she keeps to the ground she is on.
    // v2.14 lead safe: ahead on queen length from LEAD_ROUND (their queens are
    // short and hidden), so a pearl trip can only lose the game.
    bool leadSafe = false;
    if (LEAD_SAFE && amQueen && !sit.ourQueenDead && !sit.enemyQueenDead && R >= LEAD_ROUND)
    {
        TieState const lt = tie_state();
        leadSafe = (lt.theirQ >= 0 && L > lt.theirQ) || (lt.theirQ < 0 && L >= LEAD_MIN_LEN);
    }
    if (amQueen && (sit.lateSafe || leadSafe) && target >= 0)
    {
        bool risky = distHead[target] > 3 || (QUEEN_THREAT_MAP && qThreat[target] < INF);
        for (auto const& e : enemyReps)
            if (R - e.round <= 10 && manhattan(e.cell, target) <= 8)
                risky = true;
        if (FEED_FIX && QUEEN_THREAT_MAP && pearl_at(target) && distHead[target] <= FEED_EAT_DIST && qThreat[target] >= INF)
            risky = false; // a drop right by her that no enemy can reach this turn
        if (risky)
        {
            target = -1;
            claimTarget = -1;
            why = I_LATE_SAFE;
        }
    }
    if (QUEEN_EVADE && QUEEN_HARD_THREAT && amQueen && target >= 0 && (qThreat[target] < INF || qMargin[target] <= 1))
    {
        target = -1;
        claimTarget = -1;
        why = I_EVADE;
    }
    // F5: a crowd of enemy heads round the queen (swarm maps): no pearl, no
    // rest - walk to the safest open water nearby, away from them and toward
    // our own dragons.
    if (QUEEN_CROWD > 0 && amQueen && !sit.ourQueenDead)
    {
        int crowd = 0;
        std::unordered_set<int> counted;
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, head) <= CROWD_RAD)
            {
                crowd++;
                counted.insert(h.id);
            }
        for (auto const& e : enemyReps)
            if (R - e.round <= 2 && !counted.count(e.id) && manhattan(e.cell, head) <= CROWD_RAD)
            {
                crowd++;
                counted.insert(e.id);
            }
        if (crowd >= QUEEN_CROWD)
        {
            int const f = choose_flee();
            if (f >= 0)
            {
                target = f;
                claimTarget = -1;
                why = I_FLEE;
            }
        }
    }
    return false;
}

// Target: combat dragons and majority hunters chase enemies.
bool tg_hunt(Controller& ct)
{
    auto& head = tn.head;
    auto& allIn = tn.allIn;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& hunter = tn.hunter;
    auto& huntOk = tn.huntOk;
    // Combat dragons hunt enemies, preferring the ones near our leaders. When
    // we clearly outnumber them, small dragons also hunt the long enemies.
    bool const outnumber = units > sit.enemyEst + 3 && sit.distinctEnemies > 0;
    hunter = !my.leader && ((sit.mode == MODE_COMBAT && my.role == ROLE_COMBAT) || (outnumber && L <= 5));
    // v2.12 gate, also for this chase: never while outnumbered or in survival mode.
    huntOk = allIn || !(HUNT_GATE && (sit.survival || sit.outnumbered));
    // Majority on a smaller map: every spare dragon hunts, any enemy will do.
    bool const majHunter = sit.majorityHunt && !amQueen && !my.leader && !my.guard;
    if ((hunter && huntOk) || majHunter ? (target < 0 || distHead[target] > (majHunter ? 2 : 4)) : false)
    {
        int best = -1, bestD = majHunter ? 24 : 16;
        for (auto const& e : enemyReps)
        {
            if (R - e.round > 3)
                continue;
            if (!majHunter && (TRADE_UP && sit.outnumbered ? e.len < std::max(6, TRADE_UP_RATIO * L) : (!(sit.mode == MODE_COMBAT && my.role == ROLE_COMBAT) && e.len < 6)))
                continue;
            int dd = dist_to(distHead, e.cell);
            if (majHunter && dd < INF)
            {
                // One hunter per enemy: leave it to a teammate that is nearer
                // (several converging on one target run into each other).
                bool nearerMate = false;
                for (auto const& h : heads)
                    if (h.mine && h.id != me && h.id != queenId && manhattan(h.cell, e.cell) < manhattan(head, e.cell))
                        nearerMate = true;
                for (auto const& [id, st] : fleet)
                    if (id != me && id != queenId && R - st.round <= 2 && manhattan(st.cell, e.cell) + 1 < manhattan(head, e.cell))
                        nearerMate = true;
                if (nearerMate)
                    continue;
            }
            for (auto const& [id, s] : fleet)
                if (s.leader && R - s.round <= 10 && manhattan(s.cell, e.cell) <= 6)
                    dd -= 6;
            if (dd < bestD)
            {
                bestD = dd;
                best = e.cell;
            }
        }
        if (best >= 0)
        {
            target = best;
            why = I_HUNT;
        }
    }
    return false;
}

// Target: chase the enemy queen when she is close.
bool tg_hunt_queen(Controller& ct)
{
    auto& allIn = tn.allIn;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& hunter = tn.hunter;
    auto& huntOk = tn.huntOk;
    // The enemy queen decides the game: chase her when she is close, always
    // for combat dragons, and with everyone once our own queen is gone.
    if (!amQueen && !sit.enemyQueenDead && (!my.leader || allIn))
    {
        int qc = -1, qround = -1;
        for (auto const& e : enemyReps)
            if (e.id == enemyQueenId && R - e.round <= 4 && e.round > qround)
            {
                qc = e.cell;
                qround = e.round;
            }
        if (qc >= 0)
        {
            int const dq = dist_to(distHead, qc);
            int const range = allIn ? 60 : HUNT_QUEEN_RANGE;
            if (dq <= range && (dq <= 6 || allIn || (huntOk && R >= OPEN_FARM_ROUND && (my.role == ROLE_COMBAT || hunter))))
            {
                target = qc;
                why = I_HUNT_QUEEN;
            }
        }
    }
    return false;
}

// Target: feeders walk to the queen / giant.
bool tg_feed(Controller& ct)
{
    auto& head = tn.head;
    auto& bunkerFresh = tn.bunkerFresh;
    auto& capFeed = tn.capFeed;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& feedId = tn.feedId;
    auto& feedLeader = tn.feedLeader;
    auto& earlyFeed = tn.earlyFeed;
    auto& feedPhase = tn.feedPhase;
    // Farming endgame (section 13): feeders walk to our queen (or, without
    // her, to the nearest leader).
    feedLeader = -1, feedId = -1;
    // When feeding starts: open maps turn numbers into length from
    // CONSOL_ROUND; behind on queen length we start earlier (sooner still on
    // open maps); otherwise only the last FEED_WINDOW rounds.
    TieState const ts = TIE_TRACK ? tie_state() : TieState{};
    bool const behindQ = TIE_TRACK && !sit.ourQueenDead && !sit.enemyQueenDead &&
                         ((ts.ourQ >= 0 && ts.theirQ >= 0 && ts.ourQ <= ts.theirQ) ||
                          (QUEEN_HOME && R >= LEAD_ROUND && ts.ourQ <= 3) ||
                          (FEED_FIX && R >= LEAD_ROUND && ts.theirQ < 0 && ts.ourQ >= 0 && ts.ourQ < FEED_UNKNOWN_GOAL)); // v2.14: a home queen eats little (unknown counts as short)
    int feedStart = sit.consolidate ? (DYN_ALLOC ? sit.switchRound : CONSOL_ROUND) : 500 - FEED_WINDOW;
    if (behindQ)
        feedStart = std::min(feedStart, sit.openMap ? FEED_BEHIND_OPEN : FEED_BEHIND_CONGESTED);
    if (capFeed)
        feedStart = std::min(feedStart, R); // the fleet is full: its spare pearls go to the queen
    earlyFeed = R < 500 - FEED_WINDOW; // guards, hunters and defenders keep their jobs until the last rounds
    // A length-2 dragon cannot feed (it would leave one pearl and could not
    // even squeeze into its neck usefully): it would only crowd her.
    feedPhase = !my.leader && L >= 3 && R >= feedStart && sit.mode != MODE_COMBAT && sit.localThreat == 0 &&
                           !(my.guard && bunkerFresh) && // her posts come first
                           !(sit.majorityHunt && earlyFeed) && // winning on numbers: keep hunting until the last rounds
                           !(earlyFeed && ((my.guard && !(DYN_ALLOC && sit.switched && !my.screen)) || my.hunter || my.wall || my.defender)) && !(FEED_ONLY_IF_CONTEST && sit.enemyQueenDead && !sit.ourQueenDead);
    if (feedPhase)
    {
        int bestD = INF, longestId = -1, longestLen = -1;
        // Giant: on open maps a share of the feeders grows it too (it wins
        // the longest-dragon tiebreak if the queens end up dead or tied).
        int const gid = GIANT && (sit.consolidate || (GIANT_ALL && R >= GIANT_ROUND)) && !sit.ourQueenDead && (me % GIANT_SHARE == 0) ? giant_id() : -1;
        if (gid >= 0 && gid != me)
        {
            for (auto const& h : heads)
                if (h.mine && h.id == gid)
                {
                    feedLeader = h.cell;
                    feedId = gid;
                }
            auto const g = fleet.find(gid);
            if (feedLeader < 0 && g != fleet.end() && R - g->second.round <= 30)
            {
                feedLeader = g->second.cell;
                feedId = gid;
            }
        }
        if (sit.ourQueenDead && R >= 500 - LATE_LONGEST_ONLY)
            for (auto const& [id, s] : fleet)
                if (s.leader && R - s.round <= 5 && s.len > longestLen)
                {
                    longestLen = s.len;
                    longestId = id;
                }
        // A queen long enough to win on length gets no more: the extra length
        // only makes her harder to keep alive. Late on open maps the giant
        // takes it instead; otherwise the dragon keeps its length.
        bool const queenFull = !sit.ourQueenDead && queen_fed_enough();
        if (queenFull && feedId < 0 && sit.consolidate)
        {
            int const g2 = giant_id();
            auto const g = fleet.find(g2);
            if (g2 >= 0 && g2 != me && g != fleet.end() && R - g->second.round <= 30)
            {
                feedLeader = g->second.cell;
                feedId = g2;
            }
        }
        for (auto const& [id, s] : fleet)
        {
            if (feedId >= 0 || queenFull)
                break; // feeding the giant, or nobody
            bool const ok = sit.ourQueenDead ? s.leader && (longestId < 0 || id == longestId) : id == queenId;
            if (ok && R - s.round <= (DYN_ALLOC && sit.switched ? 100 : sit.consolidate || capFeed ? 30 : 5) && manhattan(s.cell, head) < bestD)
            {
                bestD = manhattan(s.cell, head);
                feedLeader = s.cell;
                feedId = id;
            }
        }
        for (auto const& h : heads)
            if (h.mine && !queenFull && !(gid >= 0 && feedId == gid) && (sit.ourQueenDead ? (fleet.count(h.id) && fleet[h.id].leader && (longestId < 0 || h.id == longestId)) : h.id == queenId))
            {
                feedLeader = h.cell;
                feedId = h.id;
            }
        // v2.14: she keeps to her home, so without a fresh report go there and
        // look (the sacrifice itself waits until she is in view).
        if (QUEEN_HOME && feedLeader < 0 && feedId < 0 && !sit.ourQueenDead && !queenFull && ourQueenSpawn >= 0 && distHead[ourQueenSpawn] < INF)
        {
            feedLeader = ourQueenSpawn;
            feedId = queenId;
        }
        // Open map, nowhere to find her: the longest leader in view grows instead.
        if (sit.consolidate && feedLeader < 0)
        {
            int bestLen = 0;
            for (auto const& h : heads)
                if (h.mine && h.id != me && fleet.count(h.id) && fleet[h.id].leader && h.len > bestLen)
                {
                    bestLen = h.len;
                    feedLeader = h.cell;
                    feedId = h.id;
                }
        }
        // Queue: a swarm of feeders walls her in, so with enough of them
        // around her already the rest wait their turn further out.
        int crowd = 0;
        if (feedLeader >= 0)
            for (auto const& h : heads)
                if (h.mine && h.id != me && h.id != feedId && manhattan(h.cell, feedLeader) <= 5)
                    crowd++;
        bool const waitTurn = feedLeader >= 0 && crowd >= FEED_CROWD && manhattan(head, feedLeader) > 4;
        if (feedLeader >= 0 && !waitTurn && (target < 0 || distHead[target] > 6))
        {
            target = feedLeader;
            why = I_FEED;
        }
    }
    return false;
}

// Target: pack prey, lurking, regrouping.
bool tg_pack(Controller& ct)
{
    auto& head = tn.head;
    auto& packer = tn.packer;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& feedPhase = tn.feedPhase;
    // Packs: go for the shared prey; the first one there waits a little for
    // a mate so the pack arrives together (alone, it just goes in).
    if (packer && !feedPhase && (L >= HUNTER_LEN || sit.ourQueenDead || my.wall) && myPrey >= 0 &&
        (target < 0 || why != I_PEARL || distHead[target] > 2))
    {
        Prey const& pr = preyList[myPrey];
        if (dist_to(distHead, pr.cell) < INF)
        {
            target = pr.cell;
            why = pr.queen ? I_HUNTER : I_PACK;
            bool preyInView = false;
            for (auto const& h : heads)
                if (!h.mine && h.id == pr.id)
                    preyInView = true;
            // Lurk: her position is fresh but she is out of our view. Wait just
            // outside her 7x7 view, each pack member on its own side; the
            // moment she steps toward one she is in its view and reach, and the
            // regicide sprint fires. Two sides: she cannot step away from both.
            int lurkCell = -1;
            if (LURK && pr.queen && !preyInView)
            {
                int qc = -1, qr = -1;
                for (auto const& e : enemyReps)
                    if (e.id == enemyQueenId && e.round > qr)
                    {
                        qr = e.round;
                        qc = e.cell;
                    }
                if (qc >= 0 && R - qr <= LURK_FRESH)
                {
                    static int const off[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                    int const r = myPackRank % 4;
                    int const ix = ((qc % W + off[r][0] * LURK_DIST) % W + W) % W;
                    int const iy = ((qc / W + off[r][1] * LURK_DIST) % H + H) % H;
                    int const ideal = cell_of(ix, iy);
                    int bestScore = INF;
                    for (int dy = -2; dy <= 2; dy++)
                        for (int dx = -2 + std::abs(dy); dx <= 2 - std::abs(dy); dx++)
                        {
                            int const c = cell_of(((ix + dx) % W + W) % W, ((iy + dy) % H + H) % H);
                            if (distHead[c] >= INF || cheb(c, qc) < LURK_DIST)
                                continue;
                            int const sc = distHead[c] + 2 * manhattan(c, ideal);
                            if (sc < bestScore)
                            {
                                bestScore = sc;
                                lurkCell = c;
                            }
                        }
                }
            }
            if (lurkCell >= 0)
            {
                target = lurkCell;
                why = I_LURK;
            }
            else if (PACK_REGROUP > 0 && myPackSize >= 2 && !preyInView && manhattan(head, pr.cell) <= PACK_REGROUP &&
                     mates_near(pr.cell, PACK_REGROUP + 3) == 0)
            {
                target = -1;
                why = I_REGROUP;
            }
        }
    }
    return false;
}

// Target: the wall line.
bool tg_wall(Controller& ct)
{
    auto& packer = tn.packer;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& feedPhase = tn.feedPhase;
    // P8 wall: no prey known - the wall advances on their home, side by side.
    if (my.wall && packer && !feedPhase && myPrey < 0 && (target < 0 || why != I_PEARL || distHead[target] > 2))
    {
        int const g = wall_goal();
        if (g >= 0)
        {
            target = g;
            why = I_WALL;
        }
    }
    return false;
}

// Target: guards intercept, post round the bunker, screen, or stay near the queen.
bool tg_guard(Controller& ct)
{
    auto& head = tn.head;
    auto& qBox = tn.qBox;
    auto& bunkerFresh = tn.bunkerFresh;
    auto& postDir = tn.postDir;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& feedPhase = tn.feedPhase;
    auto& guardQ = tn.guardQ;
    // Guards: intercept enemies closing on the queen, otherwise screen her
    // from a few tiles out (crowding her walls her in).
    guardQ = -1;
    if (my.guard && !feedPhase)
    {
        int qCellG = -1;
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qCellG = h.cell;
        auto const q = fleet.find(queenId);
        if (qCellG < 0 && q != fleet.end() && R - q->second.round <= (my.screen ? 60 : 3))
            qCellG = q->second.cell;
        if (qCellG < 0 && bunkerFresh)
            qCellG = qBunkerTl; // she is in her announced box
        if (qCellG < 0 && BUNKER_GUARDS && sit.lateSafe && q != fleet.end() && R - q->second.round <= 60)
            qCellG = q->second.cell; // last rounds: find her (she bunkers), then post
        guardQ = qCellG;
        if (qCellG >= 0)
        {
            int threat = -1, threatD = INF;
            for (auto const& h : heads)
                if (!h.mine && manhattan(h.cell, qCellG) <= GUARD_INTERCEPT && manhattan(h.cell, qCellG) < threatD)
                {
                    threat = h.cell;
                    threatD = manhattan(h.cell, qCellG);
                }
            if (threat >= 0)
            {
                // A head-on kills both: one guard for one attacker near the queen.
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(threat, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(threat, 1), 75, 2);
                    return act_moves(ct, path, "intercept");
                }
                target = threat;
                why = I_INTERCEPT;
            }
            else if (bunkerFresh)
            {
                // Post: a 2x2 next to her box covering the most of its open
                // sides, not held by a teammate. Circled at length <= 3 it
                // keeps 3 of its 4 cells filled: no enemy reaches her there.
                std::vector<int> expo;
                for (int c : qBox)
                    for (int d = 0; d < 4; d++)
                    {
                        int const n = cross(c, d);
                        if (n >= 0 && std::find(qBox.begin(), qBox.end(), n) == qBox.end() &&
                            std::find(expo.begin(), expo.end(), n) == expo.end())
                            expo.push_back(n);
                    }
                std::array<int, 4> best{};
                int bestTl = -1;
                double bestScore = -1e18;
                std::array<int, 4> pb{};
                for (int e : expo)
                {
                    int const tls[4] = {e, nb(e, 3), nb(e, 0), nb(nb(e, 3), 0)};
                    for (int tl : tls)
                    {
                        if (!box_cells(tl, pb))
                            continue;
                        bool bad = false;
                        int cover = 0, dmin = INF;
                        for (int c : pb)
                        {
                            if (std::find(qBox.begin(), qBox.end(), c) != qBox.end())
                                bad = true; // never on her box
                            if (vis[c] && occId[c] >= 0 && occId[c] != me && !(occMine[c] && occId[c] == queenId))
                                bad = true; // someone is there (a teammate holds it, or an enemy)
                            if (pearlState[c] == 1 && std::find(body.begin(), body.end(), c) == body.end())
                                bad = true; // a pearl in the loop would make a length-3 guard 4
                            if (std::find(expo.begin(), expo.end(), c) != expo.end())
                                cover++;
                            dmin = std::min(dmin, c == head ? 0 : distHead[c]);
                        }
                        for (auto const& [id, st] : fleet)
                            if (id != me && id != queenId && R - st.round <= 2 && std::find(pb.begin(), pb.end(), st.cell) != pb.end())
                                bad = true; // reported holding it
                        if (bad || dmin >= INF)
                            continue;
                        double const sc = 100.0 * cover - dmin;
                        if (sc > bestScore)
                        {
                            bestScore = sc;
                            bestTl = tl;
                            best = pb;
                        }
                    }
                }
                if (bestTl >= 0)
                {
                    bool const inPost = std::find(best.begin(), best.end(), head) != best.end();
                    target = -1;
                    why = I_POST;
                    if (inPost && L >= 4)
                    {
                        int const child = L >= 6 ? L - 3 : L - 2; // keep 3 (2 from length 4-5)
                        if (split_allowed(ct, child) && child_can_escape(child))
                            return act_split(ct, child, "post-trim");
                    }
                    if (inPost && L <= 3)
                    {
                        for (int d = 0; d < 4; d++)
                        {
                            int const n = cross(head, d);
                            if (n < 0 || std::find(best.begin(), best.end(), n) == best.end())
                                continue;
                            if (std::find(body.begin(), body.end(), n) != body.end() || (vis[n] && occId[n] >= 0))
                                continue;
                            if (pearl_at(n) && L >= 3)
                                continue;
                            postDir = d;
                        }
                    }
                    else if (!inPost)
                    {
                        int bd = INF;
                        for (int c : best)
                            if (distHead[c] < bd)
                            {
                                bd = distHead[c];
                                target = c;
                            }
                    }
                }
            }
            else if (my.screen && screen_post(qCellG) >= 0)
            {
                // Stand watch out toward the enemy: attackers are seen (and
                // reported to her) before they are within striking reach.
                int const post = screen_post(qCellG);
                if (manhattan(head, post) > 1)
                {
                    target = post;
                    why = I_SCREEN;
                }
                else if (target >= 0 && distHead[target] > 3)
                    target = -1;
            }
            else if (manhattan(head, qCellG) > GUARD_RING && distHead[qCellG] < INF)
            {
                target = qCellG;
                why = I_GUARD;
            }
            else
            {
                if (target >= 0 && distHead[target] > 4)
                    target = -1; // stay around here; no long trips while on duty
                // Line up with her: a ping only reaches a dragon on the same
                // row or column, so a guard there can pass her its reports.
                int const qx = qCellG % W, qy = qCellG / W, hx = head % W, hy = head / W;
                if (SCREENS_MORE && target < 0 && hx != qx && hy != qy)
                {
                    int best = -1, bd = INF;
                    for (int axis = 0; axis < 2; axis++)
                        for (int off = -GUARD_RING; off <= GUARD_RING; off++)
                        {
                            if (std::abs(off) < 3)
                                continue;
                            int const c = axis == 0 ? cell_of(((qx + off) % W + W) % W, qy) : cell_of(qx, ((qy + off) % H + H) % H);
                            if (distHead[c] < bd && (!vis[c] || occId[c] < 0))
                            {
                                bd = distHead[c];
                                best = c;
                            }
                        }
                    if (best >= 0 && bd <= 6)
                    {
                        target = best;
                        why = I_GUARD_LINE;
                    }
                }
            }
        }
    }

    if (guardQ >= 0 && target < 0 && why != I_POST)
        why = I_GUARD; // idle on station: no exploring away from the queen
    return false;
}

// Target: rescue a stalked queen.
bool tg_rescue(Controller& ct)
{
    auto& qNow = tn.qNow;
    auto& stalkers = tn.stalkers;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& feedPhase = tn.feedPhase;
    // v2.14 rescue: a stalker reported next to our queen (or seen): short
    // teammates nearby come and strike it (one of ours for her life).
    if (ANTI_ESCORT && !amQueen && !my.leader && L <= 8 && !sit.ourQueenDead && !feedPhase && why != I_INTERCEPT &&
        why != I_POST)
    {
        auto const qf = fleet.find(queenId);
        int const qc = qNow >= 0 ? qNow : (qf != fleet.end() && R - qf->second.round <= 3 ? qf->second.cell : -1);
        if (qc >= 0)
        {
            int best = -1, bd = RESCUE_RANGE + 1;
            for (auto const& dg : dangers)
                if (dg.cause == 1 && R - dg.round <= 1 && manhattan(dg.cell, qc) <= 3)
                {
                    int const dd = dist_to(distHead, dg.cell);
                    if (dd < bd)
                    {
                        bd = dd;
                        best = dg.cell;
                    }
                }
            for (auto const& h : heads)
                if (!h.mine && stalkers.count(h.id) && distHead[h.cell] < bd)
                {
                    bd = distHead[h.cell];
                    best = h.cell;
                }
            if (best >= 0)
            {
                target = best;
                why = I_RESCUE;
            }
        }
    }
    return false;
}

// Target (queen): stay put once theirs is dead; bunker box.
bool tg_queen_bunker(Controller& ct)
{
    auto& head = tn.head;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& tieBunker = tn.tieBunker;
    auto& bunkerMode = tn.bunkerMode;
    auto& bunkerDir = tn.bunkerDir;
    auto& inBox = tn.inBox;
    // Once the enemy queen is dead ours only has to stay alive: no long
    // trips for pearls, the guards come to her.
    if (GUARDS && amQueen && sit.enemyQueenDead && target >= 0 && distHead[target] > 3)
        target = -1;

    // Bunker: head for the chosen box, then circle it.
    // Tie bunker: late in the game with both queens short and theirs no
    // longer than ours, queen length is a tie at best and the next tiebreak
    // is the longest dragon, so ours just circles at her length (<= 3).
    tieBunker = false;
    if (TIE_BUNKER && BUNKER && amQueen && !sit.enemyQueenDead && !sit.ourQueenDead && R >= TIE_BUNKER_ROUND && L <= 3)
    {
        auto const it = enemyLenMem.find(enemyQueenId);
        tieBunker = TIE_FORCE || (it != enemyLenMem.end() && it->second.exact && R - it->second.round <= TIE_SEEN_ROUNDS && it->second.len <= L);
    }
    bool const lateBunker = LATE_SAFE && BUNKER && amQueen && sit.lateSafe && L <= 3 && !sit.ourQueenDead; // circle at her length
    // F2: their queen was seen and has not been seen for EQ_GONE_ROUNDS - most
    // likely dead (queens that roam get killed; we lost games where only ours
    // had to live). Bunker without the trim, and only if ours is not shorter
    // than theirs was.
    bool eqProbDead = false;
    if (EQ_INFER && amQueen && !sit.enemyQueenDead && !sit.ourQueenDead && R >= EQ_INFER_ROUND && (sit.lastEqSeenR >= 0 || EQ_INFER_UNSEEN) && R - sit.lastEqSeenR >= EQ_GONE_ROUNDS)
    {
        auto const it = enemyLenMem.find(enemyQueenId);
        eqProbDead = it == enemyLenMem.end() || it->second.len <= L;
    }
    bunkerMode = BUNKER && amQueen && (sit.enemyQueenDead || tieBunker || lateBunker || eqProbDead) && !sit.ourQueenDead;
    bunkerDir = -1;
    inBox = false;
    if (bunkerMode)
    {
        std::array<int, 4> bx{};
        if (bunkerBox < 0 || !box_cells(bunkerBox, bx) || !box_usable(bx))
            bunkerBox = choose_bunker();
        if (bunkerBox >= 0 && box_cells(bunkerBox, bx))
        {
            inBox = std::find(bx.begin(), bx.end(), head) != bx.end();
            why = I_BUNKER;
            target = -1;
            if (inBox && L <= 3)
            {
                for (int d = 0; d < 4; d++)
                {
                    int const n = cross(head, d);
                    if (n < 0 || std::find(bx.begin(), bx.end(), n) == bx.end())
                        continue;
                    if (std::find(body.begin(), body.end(), n) != body.end())
                        continue; // the tail still blocks the cell it leaves
                    if (vis[n] && occId[n] >= 0)
                        continue;
                    if (pearl_at(n) && L >= 3)
                    {
                        bunkerBox = -1; // a pearl in the loop would jam it: pick another box next turn
                        continue;
                    }
                    bunkerDir = d;
                }
            }
            else if (!inBox)
            {
                int bd = INF;
                for (int x : bx)
                    if (distHead[x] < bd)
                    {
                        bd = distHead[x];
                        target = x;
                    }
            }
        }
    }
    return false;
}

// Target (queen): rest spot, then home.
bool tg_queen_rest(Controller& ct)
{
    auto& head = tn.head;
    auto& queenStarving = tn.queenStarving;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& bunkerMode = tn.bunkerMode;
    // v2.14 rest: with nothing safe to eat she waits in open water near home
    // (drifting along walls with no goal is how queens end up in corners).
    if (QUEEN_HOME && amQueen && !bunkerMode && target < 0 && !queenStarving && ourQueenSpawn >= 0)
    {
        if (restCell < 0 || R % 25 == 0 || (vis[restCell] && occId[restCell] >= 0 && occId[restCell] != me) || distHead[restCell] >= INF)
            restCell = choose_rest();
        if (restCell >= 0 && manhattan(head, restCell) > 1 && distHead[restCell] < INF)
        {
            target = restCell;
            why = I_REST;
        }
    }
    // v2.14 home: with nothing to eat inside her leash the queen walks back
    // home (their stalkers hunt queens that roam).
    if (QUEEN_HOME && amQueen && !bunkerMode && target < 0 && !queenStarving && ourQueenSpawn >= 0 && manhattan(head, ourQueenSpawn) > queenLeash + 4 && queenLeash < INF &&
        distHead[ourQueenSpawn] < INF)
    {
        target = ourQueenSpawn;
        why = I_HOME;
    }
    return false;
}

// Target: portal scouts and the portal rush.
bool tg_portal(Controller& ct)
{
    auto& target = tn.target;
    auto& portalDir = tn.portalDir;
    auto& why = tn.why;
    // Portals: whole regions (and most of the pearls on some maps) lie behind
    // portals whose far end nobody has seen. Scouts open them early: one
    // crossing teaches everyone the pair, and paths then use it.
    // Only worth the risk when the portals lead somewhere we cannot walk to:
    // our reachable area (unexplored edges count as open) is a small part of
    // the map.
    int reachable = 0;
    for (int c = 0; c < N; c++)
        reachable += distHead[c] < INF;
    bool const boxedIn = reachable * 100 < SCOUT_REACH_PCT * N;
    int const scoutMaxLen = POCKET_CHECK ? BLIND_PORTAL_MAX_LEN : 12;
    bool const scout = PORTAL_SCOUTS && (boxedIn || PORTAL_ALWAYS) && !amQueen && !my.leader && L <= scoutMaxLen && (my.hunterSlot || my.role == ROLE_PORTAL) &&
                       (why == I_PEARL || why == I_EXPLORE);
    if (!amQueen && !my.leader && L <= scoutMaxLen && (scout || (target < 0 && (boxedIn || my.role == ROLE_PORTAL))) &&
        !(my.guard && why != I_EXPLORE))
    {
        int pd = -1;
        int const pg = choose_portal_goal(pd);
        bool take = pg >= 0 && (target < 0 || why != I_PEARL || distHead[pg] + SCOUT_PEARL_MARGIN <= distHead[target]);
        if (!PORTAL_SCOUTS)
            take = take && target < 0 && my.role == ROLE_PORTAL;
        if (take)
        {
            target = pg;
            portalDir = pd;
            why = I_PORTAL;
        }
    }
    // P9 portal rush: an unexplored portal right here - the nearest short
    // dragon goes through now (anything but a pearl next to it waits).
    if (PORTAL_RUSH && !amQueen && !my.leader && L <= scoutMaxLen && !(my.guard && why != I_EXPLORE) &&
        why != I_PORTAL && !(why == I_PEARL && target >= 0 && distHead[target] <= PORTAL_RUSH_PEARL))
    {
        int pd = -1;
        int const pg = choose_portal_goal(pd);
        if (pg >= 0 && distHead[pg] <= PORTAL_RUSH_DIST)
        {
            bool nearer = false;
            for (auto const& h : heads)
                if (h.mine && h.id != me && h.id != queenId && manhattan(h.cell, pg) < distHead[pg])
                    nearer = true;
            if (!nearer)
            {
                target = pg;
                portalDir = pd;
                why = I_PORTAL;
            }
        }
    }
    return false;
}

// Target: idle hunter-group dragons push toward the enemy side while expanding.
bool tg_push(Controller& ct)
{
    auto& target = tn.target;
    auto& why = tn.why;
    if (AGGRO_EXPANSION && target < 0 && my.hunterSlot && sit.expansion && !my.leader && !my.guard && !(SAFE_TRADES && (sit.survival || sit.outnumbered)))
    {
        // Take the middle early: idle hunter-group dragons head for the enemy
        // side (the mirror image of our queen's start), past the centre.
        int const qt = sit.enemyQueenDead ? -1 : enemy_queen_target();
        if (qt >= 0 && distHead[qt] < INF)
        {
            target = qt;
            why = I_PUSH;
        }
    }
    return false;
}

// Target: defenders hold our half.
bool tg_defend(Controller& ct)
{
    auto& target = tn.target;
    auto& why = tn.why;
    auto& feedPhase = tn.feedPhase;
    if (my.defender && !feedPhase && why != I_INTERCEPT &&
        (target < 0 || halfMargin(target) <= 0 || distHead[target] > 10))
    {
        int const dg = choose_camp_goal(true);
        if (dg >= 0)
        {
            target = dg;
            why = I_DEFEND;
        }
    }
    return false;
}

// Target (economy): opening camp by a spawn tile.
bool tg_camp_open(Controller& ct)
{
    auto& target = tn.target;
    auto& why = tn.why;
    // Opening: pearls appear on spawn tiles, so an idle dragon waits by one
    // rather than wandering off to explore (we lost most opening pearl races).
    if (target < 0 && R < OPEN_CAMP_ROUNDS && !amQueen && !my.guard && why == I_EXPLORE)
    {
        int const cg = choose_camp_goal();
        if (cg >= 0 && distHead[cg] <= 20)
        {
            target = cg;
            why = I_CAMP;
        }
    }
    return false;
}

// Target (economy): rich zones.
bool tg_zone(Controller& ct)
{
    auto& head = tn.head;
    auto& target = tn.target;
    auto& why = tn.why;
    // OPEN_PLAN zones: idle collectors, and collectors whose pearl is far,
    // go to the zone that needs them most (allocation by richness).
    if (ZONES && !amQueen && !my.leader && !my.guard && !my.hunter && !my.wall && !my.defender &&
        ((target < 0 && why == I_EXPLORE) || (why == I_PEARL && target >= 0 && distHead[target] > ZONE_OVERRIDE)))
    {
        int const z = choose_zone();
        if (z >= 0 && manhattan(head, z) > 2 && (target < 0 || distHead[z] + 2 < distHead[target]))
        {
            target = z;
            why = I_ZONE;
        }
    }
    return false;
}

// Target (economy): outposts.
bool tg_outpost(Controller& ct)
{
    auto& head = tn.head;
    auto& target = tn.target;
    auto& why = tn.why;
    // P8 outposts: an idle collector holds the richest spot nobody holds yet.
    if (OUTPOSTS && target < 0 && !amQueen && !my.leader && !my.guard && !my.hunter && !my.wall && why == I_EXPLORE)
    {
        int const op = choose_outpost();
        if (op >= 0 && manhattan(head, op) > 2)
        {
            target = op;
            why = I_OUTPOST;
        }
    }
    return false;
}

// Target (economy): explore, or camp when nothing is unseen.
bool tg_explore(Controller& ct)
{
    auto& queenStarving = tn.queenStarving;
    auto& target = tn.target;
    auto& why = tn.why;
    if (target < 0 && !(GUARDS && amQueen && sit.enemyQueenDead) && !(amQueen && QUEEN_AVOID_UNKNOWN && !queenStarving) && why != I_GUARD &&
        why != I_REGROUP && why != I_EVADE)
    {
        target = choose_explore_goal();
        // Nothing unseen left to look at: wait by a spawn tile instead of
        // revisiting old ground (pearls appear there, not on the way).
        if (IDLE_CAMP && !amQueen && !my.guard && (target < 0 || seenRound[target] >= 0))
        {
            int const cg = choose_camp_goal();
            if (cg >= 0)
            {
                target = cg;
                why = I_CAMP;
            }
        }
    }
    return false;
}

// Commit: loop-stall, camp give-up, stall and chase bookkeeping for the chosen target.
bool commit_target(Controller& ct)
{
    auto& target = tn.target;
    auto& why = tn.why;
    // v2.14 loop stall: circling one 2x2 for rounds without eating while the
    // pearl stays out of reach (a maze target that keeps flipping): drop it.
    if (LOOP_STALL && why == I_PEARL && target >= 0 && distHead[target] > 3 && R - lastEatR >= LOOP_STALL_ROUNDS &&
        static_cast<int>(headHist.size()) >= LOOP_STALL_ROUNDS)
    {
        bool tight = true;
        for (std::size_t i = headHist.size() - LOOP_STALL_ROUNDS; i < headHist.size() && tight; i++)
            for (std::size_t j = i + 1; j < headHist.size(); j++)
                if (manhattan(headHist[i], headHist[j]) > 2)
                    tight = false;
        if (tight)
            skipPearl[target] = R + 30;
    }
    // F6: camped next to a tile for CAMP_GIVEUP rounds and nothing spawned:
    // the timing guess was wrong - leave it alone for a while.
    if (CAMP_GIVEUP > 0 && why == I_CAMP && target >= 0)
    {
        if (target != campTile)
        {
            campTile = target;
            campArriveR = -1;
        }
        if (distHead[target] <= 1 && campArriveR < 0)
            campArriveR = R;
        if (campArriveR >= 0 && R - campArriveR >= CAMP_GIVEUP && !pearl_at(target))
        {
            skipPearl[target] = R + 40;
            campTile = -1;
        }
    }
    // Stalled: no step closer to this pearl for STALL_ROUNDS turns (it is
    // behind kelp we keep running into): leave it to others for a while.
    if (why == I_PEARL && target >= 0)
    {
        int const dNow = distHead[target];
        if (target != stallTarget)
        {
            stallTarget = target;
            stallBest = dNow;
            stallRound = R;
        }
        else if (dNow < stallBest)
        {
            stallBest = dNow;
            stallRound = R;
        }
        else if (STALL_ROUNDS > 0 && R - stallRound >= STALL_ROUNDS)
        {
            skipPearl[target] = R + STALL_SKIP;
            stallTarget = -1;
        }
        // F4: the timer above restarts whenever the target flips between two
        // pearls; count the rounds each pearl is chased in total instead, and
        // cap a run of "pearl" rounds with nothing eaten.
        if (static_cast<int>(chaseCnt.size()) != N)
        {
            chaseCnt.assign(N, 0);
            chaseLast.assign(N, -100);
        }
        if (R - chaseLast[target] > 15)
            chaseCnt[target] = 0;
        chaseCnt[target]++;
        chaseLast[target] = R;
        if (dryStart < 0 || lastEatR > dryStart)
            dryStart = R;
        if ((chaseCnt[target] >= CHASE_MAX && R - lastEatR >= CHASE_MAX) || (R - dryStart >= DRY_MAX && R - lastEatR >= DRY_MAX))
        {
            skipPearl[target] = R + CHASE_SKIP;
            chaseCnt[target] = 0;
            dryStart = R;
        }
        lastPearl = target;
    }
    else
        lastPearl = -1;
    return false;
}

// Act: feeding sacrifice next to the queen / giant.
bool feed_sacrifice(Controller& ct)
{
    auto& head = tn.head;
    auto& feedId = tn.feedId;
    auto& feedLeader = tn.feedLeader;
    auto& earlyFeed = tn.earlyFeed;
    auto& feedPhase = tn.feedPhase;
    // ---- 3. Feeding sacrifice (section 13) ----------------------------------
    if (feedPhase && feedLeader >= 0 && L >= 3 && units > (earlyFeed ? cons_floor() : 2) && feedId != me)
    {
        // In view: die within 3 cells of her head (the drop lands where she
        // can eat it in a move or two). Out of view: a fresh report within 4.
        bool inView = false, close = false;
        for (auto const& h : heads)
            if (h.mine && h.id == feedId)
            {
                inView = true;
                close = cheb(h.cell, head) <= 3;
                if (FEED_FIX)
                {
                    // P4: the drop must be on her side of the kelp - a short water path to her head.
                    int pd = INF;
                    for (int d = 0; d < 4; d++)
                    {
                        int const n = cross(h.cell, d);
                        if (n >= 0 && distHead[n] < INF)
                            pd = std::min(pd, distHead[n] + 1);
                    }
                    close = pd <= FEED_PATH;
                }
            }
        if (!inView && !FEED_FIX) // P4: out of view we cannot tell which side of the kelp she is on
        {
            auto const ls = fleet.find(feedId);
            close = ls != fleet.end() && R - ls->second.round <= 2 && cheb(ls->second.cell, head) <= 4;
        }
        // Squeeze into the neck: the body turns into pearls next to the leader.
        if (close && body.size() >= 2)
            for (int d = 0; d < 4; d++)
                if (nb(head, d) == body[1])
                    return act_move(ct, d, "feed-leader");
    }
    return false;
}

// Act: splits (bunker trim, queen split, grow, lane, pearl, half, keep).
bool plan_split(Controller& ct)
{
    auto& head = tn.head;
    auto& sealedNoSplit = tn.sealedNoSplit;
    auto& cands = tn.cands;
    auto& anyLegal = tn.anyLegal;
    auto& anySafe = tn.anySafe;
    auto& nextToEnemy = tn.nextToEnemy;
    auto& pearls = tn.pearls;
    auto& pi = tn.pi;
    auto& tieBunker = tn.tieBunker;
    auto& bunkerMode = tn.bunkerMode;
    auto& inBox = tn.inBox;
    // ---- 4. Splits (sections 2, 8, 14) ---------------------------------------
    // v2.14: inside a dead end an optional split leaves the head part to die
    // long; eat to the bottom, then split all but 2.
    // v2.15 (P1): only a real dead end - every legal move continues into a
    // 1-wide corridor that ends. "No safe move" alone is common in corridor
    // mazes and crowds, and blocking splits there stalled the whole fleet.
    if (anySafe)
        miningTrip = false; // back in open water: the trip is over
    bool inDeadEnd = FOUNTAINS && !amQueen && anyLegal && !anySafe;
    // A break-even trip (2 pearls in, a 2-cell stub left) only pays by
    // splitting inside: that is how a lone miner turns into a fleet.
    if (DEADEND_STRICT && !miningTrip)
        inDeadEnd = false;
    if (inDeadEnd && DEADEND_STRICT)
        for (auto const& cd : cands)
            if (cd.legal && (cd.blind || cd.dest < 0 || cd.dest != nb(head, cd.d) || !pocket_ahead(head, cd.dest)))
                inDeadEnd = false; // a way on that is not a dead end (or a portal): splits stay allowed
    bool const calm = !nextToEnemy && !inDeadEnd;
    // Runners: on big maps a share of the fleet stays long enough for two
    // free steps a turn, covers distance, and buds off children of 3+.
    bool const isRunner = (RUNNERS && !sit.openPlan && !amQueen && !my.leader && !my.guard && !my.hunter && my.role == ROLE_EXPLORER && N >= RUNNER_MIN_CELLS &&
                           growth_open()) ||
                          my.defender; // defenders stay medium length too: a 2-long blocker is easily stepped round
    int const growChild = isRunner ? L - RUNNER_KEEP : L / 2;
    if (amQueen && bunkerMode && sit.enemyQueenDead && !tieBunker && !sealedNoSplit && L >= 4 && (inBox || qThreat[head] == INF))
    {
        int const child = L - 2; // keep 2: one stray pearl then still leaves the loop a free cell
        if (split_allowed(ct, child))
            return act_split(ct, child, "bunker-trim");
    }
    if (amQueen && !bunkerMode && !sealedNoSplit)
    {
        if (EXPANSION)
        {
            // Numbers first: the queen splits too (head-ons kill whatever the
            // length, so a short queen is no easier to kill and harder to trap).
            bool crowded = false;
            for (auto const& h : heads)
                if (manhattan(h.cell, head) <= 3)
                    crowded = true;
            // v2.14: the child stays on our tail cells this round, so she needs
            // two ways on that are not dead ends without them.
            int realExits = 0;
            for (int d = 0; d < 4; d++)
            {
                int const n = cross(head, d);
                if (n >= 0 && !(vis[n] && occId[n] >= 0) && !(QUEEN_HOME && n == nb(head, d) && pocket_ahead(head, n)))
                    realExits++;
            }
            if (calm && !sit.consolidate && sit.queenSplits && anySafe && !crowded && L >= 4 && (!QUEEN_HOME || (realExits >= 2 && ((ADAPT_SPLIT && sit.queenOpening) || (OPEN_PLAN && OPEN_QUEEN && sit.openPlan) || (units < QUEEN_SPLIT_UNITS && (L - L / 2 >= QUEEN_MIN_KEEP || (QUEEN_FIRST_SPLIT && units <= 3 && L >= 4)))))) &&
                room_for_more() &&
                split_allowed(ct, L / 2) && child_can_escape(L / 2, true) && split_keeps_loop(L / 2))
                return act_split(ct, L / 2, "queen-split");
        }
        else if (calm && R < EARLY_SPLIT_ROUNDS && units < QUEEN_SEED_UNITS && L >= 5 && split_allowed(ct, 2) && child_can_escape(2, true) &&
                 split_keeps_loop(2))
            return act_split(ct, 2, "queen-seed");
    }
    else if (calm && !sit.consolidate && !my.wall && (!my.hunter || (FAST_OPEN && sit.expansion) || (SPLIT_MORE && R < EARLY_SPLIT_ROUNDS) || (HUNTER_SPLIT_LEN > 0 && L >= HUNTER_SPLIT_LEN)) &&
             L >= (isRunner ? RUNNER_SPLIT_LEN : EARLY_SPLIT_MIN_LEN) && (EXPANSION ? growth_open() && !my.leader : R < EARLY_SPLIT_ROUNDS) &&
             room_for_more() && split_allowed(ct, growChild) && child_can_escape(growChild, true))
        return act_split(ct, growChild, "grow-fleet");

    // Lane split: the tail sits among spawn tiles nobody else holds, so the
    // child is born right where the pearls keep coming.
    if (LANE_SPLIT && !sit.consolidate && calm && !amQueen && !my.leader && !my.guard && (!my.hunter || sit.expansion) && L >= 4 &&
        (!isRunner || L - 2 >= RUNNER_KEEP) && units < std::min(unitLimit - 2, std::max(2, N / LANE_DENSITY)) &&
        static_cast<int>(body.size()) >= L && split_allowed(ct, 2))
    {
        int const tail = body.back(), tx = tail % W, ty = tail / W;
        int tiles = 0;
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3 + std::abs(dy); dx <= 3 - std::abs(dy); dx++)
                if (nextSpawn[cell_of((tx + dx + W) % W, (ty + dy + H) % H)] >= 0)
                    tiles++;
        bool mateNear = false;
        for (auto const& h : heads)
            if (h.mine && h.id != me && manhattan(h.cell, tail) <= 4)
                mateNear = true;
        if (tiles >= LANE_TILES && !mateNear && child_can_escape(2, true))
            return act_split(ct, 2, "lane-split");
    }

    if (calm && !sit.consolidate && !my.leader && !my.hunter && L >= 4 && (!isRunner || L - 2 >= RUNNER_KEEP) && pi >= 0 && room_for_more() && split_allowed(ct, 2) &&
        static_cast<int>(body.size()) >= L)
    {
        int const tail = body.back();
        std::vector<int> child{tail, body[L - 2]};
        {
            WallScope ws; // the front part stays where it is this turn
            for (int i = 0; i < L - 2; i++)
                ws.wall(body[i]);
            BodyScope const sb(child, 2);
            plan_bfs(tail, distTmp);
        }
        int const pc = pearls[pi].cell;
        int const dt = distTmp[pc], dh = distHead[pc];
        if (dt <= PEARL_BEHIND_MAX && dt + PEARL_BEHIND_GAIN <= dh && child_can_escape(2, true))
            return act_split(ct, 2, "pearl-split");
    }

    // Half-split: mid-game a very long dragon is slow to turn and easy to trap
    // on a crowded map; two of half the length hold more ground. On open maps
    // the leaders keep growing (length decides the tiebreaks there).
    if (HALF_SPLIT && calm && !amQueen && !my.heir && L >= HALF_SPLIT_LEN && growth_open() && (!my.leader || !sit.openMap) &&
        room_for_more() && split_allowed(ct, L / 2) && child_can_escape(L / 2, true))
        return act_split(ct, L / 2, "half-split");

    if (calm && !sit.consolidate && sit.transitioned && !my.leader && !my.hunter && sit.mode == MODE_PRESERVE && L >= PRESERVE_SPLIT_LEN && room_for_more() &&
        split_allowed(ct, L / 2) && child_can_escape(L / 2, true))
        return act_split(ct, L / 2, "keep-fleet");

#ifdef FLEET_DEBUG
    {
        std::string dbg = std::string("target ") + intent_name(why) + " " + (target >= 0 ? std::to_string(target % W) + "," + std::to_string(target / W) : "-") +
                          " d" + (target >= 0 ? std::to_string(distHead[target]) : "-") + " pearls";
        for (auto const& p : pearls)
            dbg += " " + std::to_string(p.cell % W) + "," + std::to_string(p.cell / W);
        ct.output_log(dbg);
    }
#endif
    return false;
}

// Act: score the candidate moves (cost terms).
bool score_moves(Controller& ct)
{
    auto& head = tn.head;
    auto& facing = tn.facing;
    auto& queenStarving = tn.queenStarving;
    auto& cands = tn.cands;
    auto& cap = tn.cap;
    auto& anySafe = tn.anySafe;
    auto& diveMin = tn.diveMin;
    auto& allIn = tn.allIn;
    auto& qBox = tn.qBox;
    auto& bunkerFresh = tn.bunkerFresh;
    auto& postDir = tn.postDir;
    auto& pearls = tn.pearls;
    auto& target = tn.target;
    auto& portalDir = tn.portalDir;
    auto& why = tn.why;
    auto& pi = tn.pi;
    auto& guardQ = tn.guardQ;
    auto& bunkerMode = tn.bunkerMode;
    auto& bunkerDir = tn.bunkerDir;
    auto& campWait = tn.campWait;
    auto& bestD = tn.bestD;
    // ---- 5. Best move ------------------------------------------------------
    // Box their queen in: we see her but have no strike this turn. Their
    // queens die when boxed (a body she runs into kills her; theirs even
    // self-destruct). A head-on with her is a trade we always want.
    int eqHead = -1;
    if (BOX_QUEEN && !amQueen && !my.leader && !sit.enemyQueenDead && why != I_FEED &&
        (allIn || !(SAFE_TRADES && (sit.survival || sit.outnumbered))))
        for (auto const& h : heads)
            if (!h.mine && h.id == enemyQueenId)
                eqHead = h.cell;
    int const eqExitsNow = eqHead >= 0 ? queen_exits(eqHead, body) : 0;
    // Camping: the job tile has not spawned yet and we are close. A pearl
    // (almost) never appears under a body, so wait beside the tile and step
    // on in the spawn round.
    campWait = CAMP_JOB && why == I_PEARL && pi >= 0 && pi < static_cast<int>(pearls.size()) &&
                          pearls[pi].cell == target && pearls[pi].appear > R && !pearl_at(target) && distHead[target] <= 3;
    // Our queen in a 1-wide corridor: where is she (vision, else fresh sonar)?
    int qCorr = -1;
    if (CORRIDOR_GUARD && !amQueen && !sit.ourQueenDead)
    {
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qCorr = h.cell;
        auto const q = fleet.find(queenId);
        if (qCorr < 0 && q != fleet.end() && R - q->second.round <= 2)
            qCorr = q->second.cell;
        if (qCorr >= 0 && (open_sides(qCorr) > 2 || manhattan(qCorr, head) > 14))
            qCorr = -1;
    }
    std::vector<int> teamCells;
    for (int c = 0; c < N; c++)
        if (vis[c] && occId[c] >= 0 && occId[c] != me && occMine[c])
            teamCells.push_back(c);

    // Teammates must not wall the queen in: measure her free space with our
    // body where it is now and where each move would put it.
    int qHead = -1, qLen = 0, qNeed = 0, qBase = 0;
    if (QUEEN_ROOM && !amQueen)
        for (auto const& h : heads)
            if (h.mine && h.id == queenId && manhattan(h.cell, head) <= 5)
            {
                qHead = h.cell;
                auto const q = fleet.find(queenId);
                qLen = std::max(h.len, q != fleet.end() && R - q->second.round <= 3 ? q->second.len : 0);
                qNeed = 2 * qLen + 4;
                set_body(body, L);
                ignoreContested = true;
                qBase = flood(qHead, -1, qNeed + 4).area;
                ignoreContested = false;
            }

    // Give the queen room: where is she (vision, else sonar status)?
    int qCell = -1;
    if (QUEEN_CLEAR > 0 && !amQueen && !sit.ourQueenDead && why != I_FEED)
    {
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qCell = h.cell;
        auto const q = fleet.find(queenId);
        if (qCell < 0 && q != fleet.end() && R - q->second.round <= 2)
            qCell = q->second.cell;
        if (qCell >= 0 && manhattan(qCell, head) <= QUEEN_CLEAR + 4)
            queen_dist_bfs(qCell, QUEEN_CLEAR + 2);
        else
            qCell = -1;
    }

    if (BUNKER_GUARDS && amQueen && bunkerMode && bunkerBox >= 0)
    {
        qBunkerTl = bunkerBox;
        qBunkerR = R;
        queue_fact(msg_bunker(bunkerBox), 72, 1); // guards come and post round her box
    }
    if (bunkerDir < 0 && postDir >= 0)
        bunkerDir = postDir; // a guard circling its post: same forced loop step
    if (bunkerDir >= 0 && cands[bunkerDir].legal)
    {
        cands[bunkerDir].ok1 = cands[bunkerDir].ok2 = true; // the box loop never traps a queen of length <= 3
        cands[bunkerDir].surv = true;
    }
    bestD = -1;
    double bestScore = -1e18;
#ifdef BAND_DIAG
    int plainD = -1;
#endif
    for (auto& cd : cands)
    {
        if (!cd.legal)
            continue;
        Cost cost;
        cost.set(BAND_SAFETY, cd.d == bunkerDir ? 900000.0 : 0.0); // the forced bunker / post loop: her safe circle
        if (qCell >= 0 && !cd.blind && cd.dest >= 0)
        {
            // Teammates crowding the queen (her children start right behind
            // her) seal her into corridors; getting out of her way is free.
            int const dq = qDist[cd.dest], dnow = qDist[head];
            if (dq <= QUEEN_CLEAR && dq <= dnow)
                cost.add(BAND_SAFETY, -(QUEEN_CLEAR_PEN * (QUEEN_CLEAR + 1 - dq) * (open_sides(cd.dest) <= 2 ? 2.0 : 1.0)));
        }
        if (guardQ >= 0 && !cd.blind && cd.dest >= 0 && why != I_INTERCEPT)
        {
            int const dg = manhattan(guardQ, cd.dest);
            if (dg < GUARD_MIN && dg <= manhattan(guardQ, head))
                cost.add(BAND_SAFETY, -(GUARD_MIN_PEN * (GUARD_MIN - dg)));
        }
        if (qHead >= 0 && !cd.blind && !cd.nb1.empty() && why != I_FEED)
        {
            set_body(cd.nb1, L + (cd.eat ? 1 : 0));
            ignoreContested = true;
            int const qa = flood(qHead, -1, qNeed + 4).area;
            ignoreContested = false;
            if (qa < qNeed && qa < qBase)
                cost.add(BAND_SAFETY, -(QUEEN_ROOM_PEN * (1.0 + static_cast<double>(qBase - qa) / qNeed)));
        }
        if (cd.blind)
        {
            // Unknown far side: only for small, expendable dragons (section 5).
            bool const wanted = my.role == ROLE_PORTAL || my.role == ROLE_EXPLORER;
            if (my.leader)
                cost.set(BAND_SAFETY, anySafe ? -1e7 : -5e5); // better than a certain trap
            else if (POCKET_CHECK && L > BLIND_PORTAL_MAX_LEN)
                cost.set(BAND_SAFETY, anySafe ? -2e6 : -5e5); // could be a box too small to turn round in
            else if (portalDir == cd.d && target == head)
                cost.set(BAND_GOAL, 5000);
            else
                cost.set(BAND_SAFETY, wanted && L <= 6 ? -500 : -4000); // unknown far side
            if (!anySafe)
                cost.add(BAND_GOAL, 3000);
        }
        else
        {
            // Risk is judged against its reward. A trap with nothing gained is
            // an avoidable death; a trap that pays a pearl is acceptable,
            // cheapest when a survival split can save all but 2 segments.
            int const lenAfter = L + (cd.eat ? 1 : 0);
            bool const canSave = lenAfter >= 4 && units < unitLimit;
            // No pearl compensates for losing the queen.
            if (!amQueen && !cd.ok1 && cd.dive >= diveMin)
                cost.add(BAND_GOAL, 2500.0 * cd.dive - 1500.0); // a dead end whose pearls pay for the split
            else if (QUEEN_SURV_RANK && amQueen && !anySafe && !cd.ok2)
                cost.add(BAND_LIFE, -(cd.surv ? 5e5 : (QUEEN_HARD_THREAT ? QUEEN_TRAP_PEN : 1e6)));
            else if (!cd.ok2)
            {
                // A trap that pays a pearl is a trade (goal); one with nothing gained is a death (life).
                if (cd.eat && !amQueen && !(cd.dive > -1e9 && cd.dive < 0))
                    cost.add(BAND_GOAL, -(canSave ? 2500.0 : 2500.0 + 900.0 * lenAfter));
                else
                    cost.add(BAND_LIFE, -((amQueen && QUEEN_HARD_THREAT) ? QUEEN_TRAP_PEN : 1e6));
            }
            else if (!cd.ok1)
            {
                if (cd.eat && !amQueen)
                    cost.add(BAND_GOAL, -1500.0);
                else
                    cost.add(BAND_SAFETY, -(amQueen ? 2e5 : 2e4));
            }
            cost.add(BAND_GOAL, std::min(cd.area, cap) * (amQueen ? 60.0 : 15.0));
            if (QUEEN_CORRIDOR_MATE && amQueen && cd.dest >= 0 && open_sides(cd.dest) <= 2)
                for (auto const& h : heads)
                    if (h.mine && h.id != me && manhattan(h.cell, cd.dest) <= 10 && corridor_leads_to(cd.dest, h.cell, 10))
                    {
                        cost.add(BAND_LIFE, -QUEEN_EXIT_PEN); // a teammate in the same corridor: neither of us could turn round
                        break;
                    }
            if (amQueen && L >= QUEEN_LONG_LEN && cd.dest >= 0 && open_sides(cd.dest) <= 2)
                cost.add(BAND_SAFETY, -QUEEN_CORRIDOR_PEN); // a long queen cannot turn round in a corridor: one blocker ahead kills her
            if (cd.eat)
                cost.add(BAND_GOAL, my.leader ? 6000 : 3000);
            if (target >= 0)
            {
                set_body(cd.nb1, L + (cd.eat ? 1 : 0));
                plan_bfs(cd.dest, distTmp);
                int dt = dist_to(distTmp, target);
                if (portalDir >= 0 && target != head && cd.dest == target)
                    dt = 0;
                cd.dt = dt;
                if (campWait)
                    cost.add(BAND_GOAL, -(cd.dest == target ? 50000.0 : 150.0 * std::max(0, std::min(dt, 200) - 1)));
                else
                    cost.add(BAND_GOAL, -(150.0 * std::min(dt, 200)));
            }
            for (auto const& h : heads)
            {
                int const reach = (!h.mine && my.leader) ? enemy_reach(h) : 1;
                if (manhattan(h.cell, cd.dest) > reach)
                    continue;
                if (!h.mine && amQueen && QUEEN_THREAT_MAP)
                    continue; // the threat map below covers it
                if (!h.mine && amQueen)
                    cost.add(BAND_LIFE, -3e6);
                else if (!h.mine && my.leader)
                    cost.add(BAND_SAFETY, -(L >= h.len + 2 ? 200000 : 500000));
                else if (!h.mine && SURV_ECON && sit.survival)
                    cost.add(BAND_SAFETY, -SURV_THREAT_PEN);
                else if (!h.mine && sit.mode == MODE_COMBAT && my.role == ROLE_COMBAT)
                    cost.add(BAND_GOAL, -500); // a combat dragon closing in: wanted
                else if (!h.mine)
                    cost.add(BAND_SAFETY, -THREAT_PEN);
                else
                    cost.add(BAND_SAFETY, -(my.leader || amQueen ? 4000 : TEAM_ADJ_PEN)); // two heads side by side can meet
            }
            if (amQueen && QUEEN_THREAT_MAP)
            {
                double const tp = queen_threat_pen(cd.dest), mp = queen_margin_pen(cd.dest);
                cost.add_split(-tp, BAND_LIFE, -(tp - mp), BAND_SAFETY); // in reach: life; just outside: safety
            }
            int esc2 = -1; // P3c: two-turn-safe cells she can reach next turn (-1: not checked)
            if (QUEEN_LOOK2 && amQueen && QUEEN_THREAT_MAP && look2Active && cd.dest >= 0 && !cd.nb1.empty())
            {
                esc2 = look2_safe_cells(cd.dest, cd.nb1, L + (cd.eat ? 1 : 0));
                cost.add(BAND_SAFETY, esc2 == 0 ? -LOOK2_PEN : LOOK2_CELL_BONUS * esc2);
            }
            if (QUEEN_HOME && amQueen && cd.dest >= 0 && cd.dest == nb(head, cd.d) && pocket_ahead(head, cd.dest))
                cost.add(BAND_LIFE, -QUEEN_TRAP_PEN); // a dead end: no pearl in it is worth the queen
            else if (TOPO_TRAP && amQueen && cd.dest >= 0 && topo_trap(head, cd.d, L + (cd.eat ? 1 : 0)))
                cost.add(BAND_LIFE, -QUEEN_TRAP_PEN); // 7b: a dead-end branch of any shape that cannot hold her
            // v2.14 pursuit: enemy heads move too. A cell with one way on is a
            // coffin once one comes; every way on within their next two steps
            // is no better. Near them, open water beats hugging a wall.
            if (PURSUIT_SAFE && amQueen && cd.dest >= 0 && cd.d != bunkerDir)
            {
                int nearE = INF, nearAny = INF;
                for (auto const& h : heads)
                    if (h.id != me)
                    {
                        nearAny = std::min(nearAny, manhattan(h.cell, cd.dest));
                        if (!h.mine)
                            nearE = std::min(nearE, manhattan(h.cell, cd.dest));
                    }
                // Teammates move too: one stepping into her last way on boxes her
                // in, and in a 1-wide corridor one coming the other way does.
                if (open_sides(cd.dest) <= 2 && open_sides(head) >= 3)
                    cost.add(BAND_SAFETY, -(nearAny <= 6 ? CORR_ENTER_PEN : CORR_ENTER_PEN / 5));
                {
                    int ex = 0, exHit = 0;
                    int const nl = L + (cd.eat ? 1 : 0);
                    for (int d2 = 0; d2 < 4; d2++)
                    {
                        int const n = cross(cd.dest, d2);
                        if (n < 0 || n == head)
                            continue;
                        bool blocked = vis[n] && occId[n] >= 0 && occId[n] != me;
                        for (int i = 0; i + 1 < nl && i < static_cast<int>(cd.nb1.size()); i++)
                            if (cd.nb1[i] == n)
                                blocked = true;
                        if (blocked)
                            continue;
                        ex++;
                        for (auto const& h : heads)
                            if (!h.mine && manhattan(h.cell, n) <= 2 * std::max(1, (std::max(h.len, h.estLen) + 3) / 4))
                            {
                                exHit++;
                                break;
                            }
                    }
                    if (LOOK2_SKIP_CORNER && esc2 > 0)
                        ; // P3c: a two-turn-safe way on exists - the corner rules are only proxies for that
                    else if (ex <= 1)
                        cost.add(BAND_SAFETY, -((nearE <= PURSUIT_DIST || nearAny <= 3) ? CORNER_PEN : CORNER_PEN / 5));
                    else if (exHit >= ex && nearE <= PURSUIT_DIST)
                        cost.add(BAND_SAFETY, -ALL_EXITS_PEN);
                    if (nearE <= 3)
                        cost.add(BAND_SAFETY, QUEEN_MOBILITY * ex);
                }
            }
            if (cd.noLoop && cd.d != bunkerDir)
                cost.add(BAND_SAFETY, -(queenStarving ? 50000 : QUEEN_LOOP_PEN));
            if (amQueen && queenSealed && cd.eat && L + 1 > sealedCells - 1)
                cost.add(BAND_LIFE, -2e6); // eating would fill the sealed pocket: no free cell left to move into
            if (amQueen && !vis[cd.dest])
                cost.add(BAND_SAFETY, -(queenStarving ? 1e5 : 5e5)); // a portal whose far side we cannot see: only as a last resort
            if (amQueen && QUEEN_AVOID_PORTAL && !queenStarving)
            {
                std::uint8_t const et = edgeType[edge_code(head, cd.d)];
                if (et != E_OPEN && et != E_KELP && et != E_UNK)
                    cost.add(BAND_SAFETY, -1e6); // crossing a portal: never by choice
                for (int k = 0; k < 4; k++)
                {
                    std::uint8_t const t2 = edgeType[edge_code(cd.dest, k)];
                    if (t2 != E_OPEN && t2 != E_KELP && t2 != E_UNK)
                        cost.add(BAND_SAFETY, -150000); // next to a portal mouth
                }
            }
            if (amQueen && QUEEN_AVOID_UNKNOWN && !queenStarving)
            {
                for (int k = 0; k < 4; k++)
                    if (edgeType[edge_code(cd.dest, k)] == E_UNK)
                        cost.add(BAND_SAFETY, -30000);
                if (seenRound[cd.dest] < 0)
                    cost.add(BAND_SAFETY, -200000);
            }
            if (!amQueen && !vis[cd.dest])
            {
                // Blind portal exit: we cannot see who stands there. Never risk
                // landing on the queen, who moves first and may be right there.
                cost.add(BAND_GOAL, -3000);
                auto const q = fleet.find(queenId);
                if (q != fleet.end() && R - q->second.round <= 3 && manhattan(q->second.cell, cd.dest) <= 2 + (R - q->second.round))
                    cost.add(BAND_LIFE, -2e6); // could land on our queen
            }
            if (amQueen) // stay clear of where the fleet saw enemies recently, even out of view
                for (auto const& e : enemyReps)
                {
                    if (R - e.round > 8)
                        continue;
                    int const dd = manhattan(e.cell, cd.dest);
                    if (dd <= 7)
                        cost.add(BAND_SAFETY, -(4000.0 * (8 - dd)));
                }
            if (amQueen) // keep the queen well away from enemy heads, not just out of reach
                for (auto const& h : heads)
                    if (!h.mine)
                    {
                        int const dd = manhattan(h.cell, cd.dest);
                        if (dd <= 5)
                            cost.add(BAND_SAFETY, -(25000.0 * (6 - dd)));
                    }
            if (my.leader) // an enemy that splits sends a new head out of its tail
                for (int tc : enemyTails)
                    if (manhattan(tc, cd.dest) <= 1)
                        cost.add(BAND_SAFETY, -(amQueen ? 400000 : 100000));
            for (auto const& dg : dangers)
                if (R - dg.round <= 30 && manhattan(dg.cell, cd.dest) <= 3)
                    cost.add(BAND_STYLE, -(my.leader ? 3000 : 300));
            // Confinement: when we are safe, prefer moves that squeeze a nearby
            // enemy's free space (pin it against kelp and bodies).
            if (cd.ok2 && cd.area >= 2 * L + 4)
                for (auto const& h : heads)
                {
                    if (h.mine || manhattan(h.cell, cd.dest) > 4)
                        continue;
                    int const ecap = 2 * h.len + 8;
                    set_body(cd.nb1, L + (cd.eat ? 1 : 0));
                    ignoreContested = true;
                    FloodRes const ef = flood(h.cell, -1, ecap);
                    ignoreContested = false;
                    int const earea = ef.area + (ef.frontier ? ecap : 0);
                    double const mult = h.id == enemyQueenId ? 3.0 : 1.0;
                    if (earea <= h.len + 1)
                        cost.add(BAND_STYLE, 3500 * mult);
                    else
                        cost.add(BAND_STYLE, mult * 1500.0 * std::max(0.0, 1.0 - static_cast<double>(earea) / ecap));
                }
            if (!my.leader && why != I_FEED)
                for (int tc : teamCells)
                    cost.add(BAND_STYLE, -(150.0 / (1 + manhattan(tc, cd.dest))));
            if (PORTAL_ROW && cd.dest >= 0 && cd.dest != nb(head, cd.d))
            {
                // Through a portal: a teammate whose head is at (or next to) the
                // far end may be stepping the other way this very round, and
                // the two heads would meet. The lower id goes first.
                auto clash = [&](int id, int cell) {
                    int const dd = manhattan(cell, cd.dest);
                    return dd == 0 || (dd == 1 && id < me);
                };
                bool hit = false;
                for (auto const& h : heads)
                    if (h.mine && h.id != me && clash(h.id, h.cell))
                        hit = true;
                for (auto const& [id, st] : fleet)
                    if (id != me && R - st.round <= 1 && clash(id, st.cell))
                        hit = true;
                if (hit)
                    cost.add(BAND_LIFE, -PORTAL_ROW_PEN);
            }
            if (eqHead >= 0 && !cd.nb1.empty())
            {
                int const after = queen_exits(eqHead, cd.nb1);
                cost.add(BAND_STYLE, BOX_BONUS * (eqExitsNow - after));
                if (after == 0)
                    cost.add(BAND_STYLE, BOX_BONUS); // no way out: she must split or die
            }
            if (bunkerFresh && cd.dest >= 0 && std::find(qBox.begin(), qBox.end(), cd.dest) != qBox.end())
                cost.add(BAND_LIFE, -QUEEN_EXIT_PEN); // her bunker box is hers alone
            if (qCorr >= 0 && cd.dest >= 0 && open_sides(cd.dest) <= 2 && manhattan(cd.dest, qCorr) <= manhattan(head, qCorr) &&
                corridor_leads_to(cd.dest, qCorr, 12))
                cost.add(BAND_LIFE, -QUEEN_EXIT_PEN); // never walk into her corridor towards her: she cannot turn round there
            bool const onPost = bunkerFresh && why == I_POST; // her way out is her box loop, which nobody enters
            if (QUEEN_EXIT_GUARD && !amQueen && !onPost && !cd.nb1.empty()) // feeders too: they can give their pearls from 4 cells away
                for (auto const& h : heads)
                    if (h.mine && h.id == queenId && manhattan(h.cell, cd.dest) <= 2)
                    {
                        std::vector<int> nowBody(body.begin(), body.begin() + std::min<int>(body.size(), L));
                        std::vector<int> after(cd.nb1.begin(), cd.nb1.begin() + std::min<int>(cd.nb1.size(), L + (cd.eat ? 1 : 0)));
                        int const before = queen_exits(h.cell, nowBody), now = queen_exits(h.cell, after);
                        if (now < before && now < 2)
                            cost.add(BAND_LIFE, -QUEEN_EXIT_PEN); // never take one of the queen's last ways out
                    }
            if (!amQueen && !onPost && (why != I_FEED || sit.consolidate))
                for (auto const& h : heads)
                    if (h.mine && h.id == queenId)
                    {
                        // Do not box the queen in: her own body is mostly out of
                        // our view, so keep clear of her head instead.
                        int const dq = manhattan(h.cell, cd.dest);
                        if (dq <= 2)
                            cost.add(BAND_SAFETY, -QUEEN_SPACE_PEN);
                        else if (dq == 3)
                            cost.add(BAND_SAFETY, -(QUEEN_SPACE_PEN / 3.5));
                    }
            if (cd.d == facing)
                cost.add(BAND_STYLE, 3);
        }
        cd.score = cost.total;
        cd.bands = cost.band;
        // 6a: compare by band so no amount of a lower band can outweigh a higher one. BAND_COMPARE 1: life,
        // then safety, then goal + style. 2: life, then the plain sum of the rest. 0: one plain sum (v2.16).
        bool better = cd.score > bestScore;
        if (BAND_COMPARE == 2 && bestD >= 0)
        {
            auto const& o = cands[bestD].bands;
            auto const& b = cd.bands;
            better = b[BAND_LIFE] != o[BAND_LIFE] ? b[BAND_LIFE] > o[BAND_LIFE] : cd.score - b[BAND_LIFE] > bestScore - o[BAND_LIFE];
        }
        else if (BAND_COMPARE == 1 && bestD >= 0)
        {
            auto const& o = cands[bestD].bands;
            auto const& b = cd.bands;
            better = b[BAND_LIFE] != o[BAND_LIFE]     ? b[BAND_LIFE] > o[BAND_LIFE]
                     : b[BAND_SAFETY] != o[BAND_SAFETY] ? b[BAND_SAFETY] > o[BAND_SAFETY]
                                                        : b[BAND_GOAL] + b[BAND_STYLE] > o[BAND_GOAL] + o[BAND_STYLE];
        }
        if (better)
        {
            bestScore = cd.score;
            bestD = cd.d;
        }
#ifdef BAND_DIAG
        if (plainD < 0 || cd.score > cands[plainD].score)
            plainD = cd.d;
#endif
    }
#ifdef BAND_DIAG
    // Debug: turns where the band comparison chose another move than the plain sum (FLEET_DIAG=<file>).
    if (char const* df = std::getenv("FLEET_DIAG"); df && plainD >= 0 && plainD != bestD)
        if (std::FILE* f = std::fopen(df, "a"))
        {
            auto const& a = cands[bestD].bands;
            auto const& b = cands[plainD].bands;
            std::fprintf(f, "%d %d q%d L%d chose %c A%.0f B%.0f C%.0f D%.0f | sum %c A%.0f B%.0f C%.0f D%.0f | %s\n", R, me, amQueen ? 1 : 0, L,
                         DCH[bestD], a[0], a[1], a[2], a[3], DCH[plainD], b[0], b[1], b[2], b[3], intent_name(why));
            std::fclose(f);
        }
#endif
    set_body(body, L);
    if (bestD < 0)
        bestD = facing;
#ifdef QDEBUG
    if (amQueen)
    {
        int nearE = INF;
        for (auto const& h : heads)
            if (!h.mine)
                nearE = std::min(nearE, manhattan(h.cell, head));
        if (nearE <= 8)
        {
            std::string dbg = std::string("QDBG ") + intent_name(why) + " best" + DCH[bestD];
            for (auto const& cd : cands)
            {
                dbg += std::string(" ") + DCH[cd.d] + (cd.legal ? "" : "x");
                if (!cd.legal || cd.dest < 0)
                    continue;
                dbg += "@" + std::to_string(cd.dest % W) + "," + std::to_string(cd.dest / W) + "s" + std::to_string(static_cast<long long>(cd.score)) + "[A" + std::to_string(static_cast<long long>(cd.bands[BAND_LIFE])) +
                       " B" + std::to_string(static_cast<long long>(cd.bands[BAND_SAFETY])) + "]" +
                       "t" + (qThreat[cd.dest] >= INF ? std::string("-") : std::to_string(qThreat[cd.dest])) + "o" + std::to_string(cd.ok1) +
                       std::to_string(cd.ok2) + "a" + std::to_string(cd.area);
            }
            for (auto const& h : heads)
                if (!h.mine)
                    dbg += " E" + std::to_string(h.id) + "@" + std::to_string(h.cell % W) + "," + std::to_string(h.cell / W) + "l" +
                           std::to_string(h.len) + "/" + std::to_string(h.estLen);
            ct.output_log(dbg);
        }
    }
#endif
    return false;
}

// Act: give up a pearl behind a bad dead end, extend the move with free steps, and move.
bool finish_move(Controller& ct)
{
    auto& head = tn.head;
    auto& cands = tn.cands;
    auto& diveMin = tn.diveMin;
    auto& freeN = tn.freeN;
    auto& pearls = tn.pearls;
    auto& target = tn.target;
    auto& portalDir = tn.portalDir;
    auto& why = tn.why;
    auto& pi = tn.pi;
    auto& campWait = tn.campWait;
    auto& bestD = tn.bestD;
    // The way to the pearl only leads into a dead end that does not pay:
    // stop circling outside and let the matching pick another pearl.
    if (pi >= 0 && target == pearls[pi].cell && distHead[target] < INF)
    {
        bool closer = false, closerOk = false;
        for (auto const& cd : cands)
            if (cd.legal && !cd.blind && cd.dt < distHead[target])
            {
                closer = true;
                closerOk |= cd.ok1 || cd.dive >= diveMin;
            }
        if (closer && !closerOk)
            skipPearl[target] = R + SKIP_PEARL_ROUNDS;
    }

    // Free steps: keep going along the chosen direction while it pays (more
    // pearls, more progress) and the turn still ends somewhere safe.
    Cand const& c1 = cands[bestD];
    if (DEADEND_STRICT && !amQueen && c1.legal && !c1.blind && !c1.ok1 && c1.dive >= std::max(diveMin, 1.0))
        miningTrip = true; // going in for pearls that pay for the stub: no split until the bottom
    // A paid step wins a pearl race an enemy head would otherwise win
    // (theirs do this constantly); never for the queen.
    bool paidRace = false;
    if (PAID_PEARL_RACE && !amQueen && !sit.survival && L >= 3 && why == I_PEARL && target >= 0 && distHead[target] == 2)
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, target) <= 2)
                paidRace = true;
    if ((freeN >= 2 || paidRace) && c1.legal && !c1.blind && c1.ok2 && target >= 0 && portalDir < 0 && !c1.nb1.empty() &&
        !(amQueen && SURVIVAL && sit.survival) && !campWait)
    {
        stamp++; // distances to the goal ignore our own body
        plan_bfs(target, distGoal);
        set_body(body, L);
        int const g0 = distGoal[head];
        bool const useGoal = g0 < INF;
        int const nl = L + (c1.eat ? 1 : 0);
        int const cap1 = std::min(N, 3 * nl + 24);
        double const v1 = (c1.eat ? 3000.0 : 0.0) + std::min(c1.area, cap1) * 15.0 - end_threat(c1.dest) +
                          (useGoal && distGoal[c1.dest] < INF ? 150.0 * (g0 - distGoal[c1.dest]) : 0.0);
        SprintPlan sp;
        std::vector<int> dirs{bestD}, eatenCells;
        if (c1.eat)
            eatenCells.push_back(c1.dest);
        sprint_dfs(c1.nb1, nl, 1, paidRace ? std::max(freeN, 2) : freeN, freeN, c1.eat ? 1 : 0, dirs, eatenCells, useGoal, g0, sp);
        set_body(body, L);
        bool sprintOk = sp.kill || (sp.value > v1 + 300 && sp.dirs.size() >= 2);
        if (paidRace && !sp.kill && sp.dirs.size() >= 2)
        {
            // Only when the sprint really lands on the raced pearl (the paid
            // step costs more than one pearl is worth in the plan's own score).
            int c = head;
            bool hits = false;
            for (int d : sp.dirs)
            {
                c = c >= 0 ? cross(c, d) : -1;
                hits |= c == target;
            }
            sprintOk = sprintOk || (hits && sp.value > v1 - 1500);
        }
        if (sprintOk && amQueen && !sp.kill)
        {
            int el = 0;
            std::vector<int> const endBody = simulate_steps(body, L, sp.dirs, el);
            sprintOk = !endBody.empty() && queen_can_survive(endBody, el) &&
                       !(QUEEN_LOOP_SAFE && !loop_reachable(endBody, el));
            if (sprintOk && PURSUIT_SAFE)
            {
                int const e = endBody[0];
                int nearE = INF;
                for (auto const& h : heads)
                    if (!h.mine)
                        nearE = std::min(nearE, manhattan(h.cell, e));
                if (nearE <= PURSUIT_DIST + 1)
                {
                    int ex = 0;
                    for (int d2 = 0; d2 < 4; d2++)
                    {
                        int const n = cross(e, d2);
                        if (n < 0)
                            continue;
                        bool blocked = vis[n] && occId[n] >= 0 && occId[n] != me;
                        for (int i = 0; i + 1 < el && i < static_cast<int>(endBody.size()); i++)
                            if (endBody[i] == n)
                                blocked = true;
                        ex += !blocked;
                    }
                    if (ex <= 1)
                        sprintOk = false; // a corner with an enemy near: one step and she is boxed
                }
            }
            set_body(body, L);
        }
        if (sprintOk)
            return act_moves(ct, sp.dirs, sp.kill ? "regicide" : intent_name(why));
    }
    act_move(ct, bestD, intent_name(why));
    return true;
}

// One turn: analyse, then the first reflex that fires, else a target, then feed / split / move.
using Stage = bool (*)(Controller&);

void decide(Controller& ct)
{
    tn = Turn{};
    prepare_turn(ct);
    analyse_moves(ct);
    read_contact(ct);
    // Reflexes, in priority order: the first that acts ends the turn.
    static Stage const reflexes[] = {
        rx_regicide_paid,
        rx_escort_hit,
        rx_snipe,
        rx_pack_strike,
        rx_regicide_sprint,
        rx_intercept,
        rx_queen_paid_escape,
        rx_no_legal_move,
        rx_escape_sprint,
        rx_trap_split,
        rx_insure_split,
        rx_recycle,
        rx_attack,
    };
    for (Stage rx : reflexes)
        if (rx(ct))
            return;
    // Target: each step may keep or replace the goal so far (order matters); guards may act.
    static Stage const targetSteps[] = {
        tg_bunker_watch,
        tg_pearl,
        tg_queen_caution,
        tg_hunt,
        tg_hunt_queen,
        tg_feed,
        tg_pack,
        tg_wall,
        tg_guard,
        tg_rescue,
        tg_queen_bunker,
        tg_queen_rest,
        tg_portal,
        tg_push,
        tg_defend,
        tg_camp_open,
        tg_zone,
        tg_outpost,
        tg_explore,
        commit_target,
    };
    for (Stage tg : targetSteps)
        if (tg(ct))
            return;
    if (feed_sacrifice(ct) || plan_split(ct))
        return;
    score_moves(ct);
    finish_move(ct);
}

} // namespace
