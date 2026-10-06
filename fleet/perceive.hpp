// L0 PERCEIVE - learning edges and symmetry, reading vision, own body and sonar, death detection.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

// Learning
void check_edge_symmetry(int code)
{
    for (int k = 0; k < 3; k++)
    {
        if (!(symMask & (1 << k)))
            continue;
        int const mc = mirror_edge(k, code);
        if (obsEdge[mc] == E_UNK)
            continue;
        if (obsEdge[mc] != obsEdge[code])
            symMask &= ~(1 << k);
        else
            symConfirm[k]++;
    }
}

void set_partner(int a, int b)
{
    if (a == b || edgePartner[a] == b)
        return;
    edgePartner[a] = b;
    edgePartner[b] = a;
    if (edgeType[a] == E_UNK) edgeType[a] = E_PORTAL;
    if (edgeType[b] == E_UNK) edgeType[b] = E_PORTAL;
    portalPairs.push_back({a, b});
}

void mirror_pair(int a, int b)
{
    if (symKnown < 0)
        return;
    int const ma = mirror_edge(symKnown, a), mb = mirror_edge(symKnown, b);
    if (edgePartner[ma] < 0 && edgePartner[mb] < 0 && ma != mb)
        set_partner(ma, mb);
}

// Record an edge. direct = seen by a dragon (us or a teammate), which is what
// symmetry testing trusts. Returns true if it taught us something new.
bool learn_edge(int code, std::uint8_t type, int pid, bool direct)
{
    bool changed = false;
    if (edgeType[code] != type)
    {
        edgeType[code] = type;
        changed = true;
        if (type == E_KELP)
            kelpList.push_back(code);
    }
    if (direct && obsEdge[code] != type)
    {
        obsEdge[code] = type;
        check_edge_symmetry(code);
    }
    if (type == E_PORTAL && pid >= 0 && edgePid[code] != pid)
    {
        edgePid[code] = pid;
        changed = true;
        auto& v = portalById[pid];
        if (std::find(v.begin(), v.end(), code) == v.end())
            v.push_back(code);
        if (v.size() == 2 && edgePartner[v[0]] != v[1])
        {
            set_partner(v[0], v[1]);
            mirror_pair(v[0], v[1]);
            queue_fact(msg_portal(v[0], v[1]), 85, 4); // section 5: share portal structure
        }
        else if (v.size() == 1)
            queue_fact(msg_portal_edge(code, pid), 40, 2);
    }
    if (symKnown >= 0)
    {
        int const mc = mirror_edge(symKnown, code);
        if (edgeType[mc] == E_UNK)
        {
            edgeType[mc] = type;
            if (type == E_KELP)
                kelpList.push_back(mc);
        }
    }
    return changed;
}

void mirror_tile(int c)
{
    if (symKnown < 0)
        return;
    int const m = mirror_cell(symKnown, c);
    if (m == c || seenRound[m] == R)
        return;
    // Mirrored tiles share one countdown, so the timing carries over exactly.
    if (nextSpawn[c] != -1 && (nextSpawn[m] == -1 || seenRound[m] < seenRound[c]))
    {
        nextSpawn[m] = nextSpawn[c];
        nextEst[m] = nextEst[c];
    }
    if (gapLo[m] == 0 && gapLo[c] > 0)
    {
        gapLo[m] = gapLo[c];
        gapHi[m] = gapHi[c];
    }
}

void note_drop(int c, int round)
{
    if (!FEEDER_TRAIL)
        return;
    for (auto const& e : dropEv)
        if (e.cell == c && std::abs(e.round - round) <= 2)
            return;
    bool nearQ = false;
    auto const q = fleet.find(queenId);
    if (!ourQueenDead && q != fleet.end() && R - q->second.round <= 20 && manhattan(q->second.cell, c) <= 6)
        nearQ = true;
    if (!ourQueenDead && me == queenId && !body.empty() && manhattan(body[0], c) <= 6)
        nearQ = true;
    dropEv.push_back({c, round, nearQ});
    if (dropEv.size() > 64)
        dropEv.erase(dropEv.begin());
}

void observe_tile(int c, bool pearl, int k)
{
    int const ns = k < 0 ? -2 : R + std::max(k, 0);
    // A reset between two sightings reveals one draw of the tile's hidden gap.
    if (obsNext[c] >= 0 && obsNext[c] <= R && ns >= 0 && ns > obsNext[c])
    {
        int const g = ns - obsNext[c];
        gapLo[c] = gapLo[c] == 0 ? g : std::min(gapLo[c], g);
        gapHi[c] = std::max(gapHi[c], g);
    }
    bool const newTiming = obsNext[c] != ns;
    bool const wasPresent = pearlState[c] == 1;
    seenRound[c] = R;
    obsNext[c] = ns;
    nextSpawn[c] = ns;
    nextEst[c] = 0;

    // Symmetry test on shared countdowns.
    for (int kk = 0; kk < 3; kk++)
    {
        if (!(symMask & (1 << kk)))
            continue;
        int const m = mirror_cell(kk, c);
        if (m == c || seenRound[m] < 0 || obsNext[m] == -1)
            continue;
        bool const neverA = ns == -2, neverB = obsNext[m] == -2;
        if (neverA != neverB)
            symMask &= ~(1 << kk);
        else if (!neverA && ns > R && obsNext[m] > R)
        {
            if (ns == obsNext[m])
                symConfirm[kk]++;
            else
                symMask &= ~(1 << kk);
        }
    }

    if (pearl)
    {
        if (!wasPresent && k < 0)
        {
            dropRound[c] = R; // a pearl on a tile that never spawns: a death drop
            note_drop(c, R);
        }
        if (!wasPresent)
        {
            queue_pearl(1, c, R, k < 0 ? 75 : 60); // death drops first: teammates come and collect
            // A fresh pearl here means the mirrored tile fired at the same moment.
            if (symKnown >= 0)
            {
                int const m = mirror_cell(symKnown, c);
                if (m != c && seenRound[m] < R && pearlState[m] == 0)
                {
                    pearlState[m] = 2;
                    pearlRound[m] = R;
                }
            }
        }
        pearlState[c] = 1;
    }
    else
    {
        if (wasPresent)
            queue_pearl(2, c, R, 35);
        pearlState[c] = 0;
    }
    pearlRound[c] = R;
    if (newTiming && ns >= 0 && ns - R <= 25)
        queue_pearl(0, c, ns, 25);
    mirror_tile(c);
}

// Tiles out of view whose countdown ran out: a pearl probably spawned there.
void update_unobserved()
{
    for (int c = 0; c < N; c++)
    {
        int const ns = nextSpawn[c];
        if (ns < 0 || ns > R || seenRound[c] >= ns)
            continue;
        if (pearlState[c] == 0)
        {
            pearlState[c] = 2;
            pearlRound[c] = ns;
        }
        if (gapLo[c] > 0)
        {
            nextSpawn[c] = ns + (gapLo[c] + gapHi[c]) / 2;
            nextEst[c] = 1;
        }
        else
            nextSpawn[c] = -1;
    }
}

void set_symmetry(int k)
{
    if (symKnown >= 0 || k < 0 || k > 2)
        return;
    symKnown = k;
    queue_fact(msg_sym(), 90, 4);
    int const codes = N * 2;
    for (int code = 0; code < codes; code++)
    {
        if (obsEdge[code] == E_UNK)
            continue;
        int const mc = mirror_edge(k, code);
        if (edgeType[mc] == E_UNK)
        {
            edgeType[mc] = obsEdge[code];
            if (obsEdge[code] == E_KELP)
                kelpList.push_back(mc);
        }
    }
    auto const pairs = portalPairs;
    for (auto const& [a, b] : pairs)
        mirror_pair(a, b);
    for (int c = 0; c < N; c++)
        if (seenRound[c] >= 0)
            mirror_tile(c);
}

void resolve_symmetry()
{
    if (symKnown >= 0 || symFailed)
        return;
    if (symMask == 0)
    {
        symFailed = true; // inconsistent observations: stop predicting
        return;
    }
    int bits = 0, first = -1;
    bool allConfirmed = true;
    for (int k = 0; k < 3; k++)
        if (symMask & (1 << k))
        {
            bits++;
            if (first < 0) first = k;
            if (symConfirm[k] < 80) allConfirmed = false;
        }
    // Several survivors with lots of evidence: the map has several symmetries,
    // and any of them predicts correctly.
    if (bits == 1 || allConfirmed)
        set_symmetry(first);
}

// Reading the turn
void read_vision(Controller& ct)
{
    std::fill(vis.begin(), vis.end(), 0);
    std::fill(occId.begin(), occId.end(), -1);
    heads.clear();
    enemyTails.clear();
    eqBodyNow.clear();
    std::unordered_map<int, int> partCount;

    for (Tile const& tile : ct.get_tiles())
    {
        int const c = cell_of(tile.get_position());
        vis[c] = 1;
        observe_tile(c, tile.has_pearl(), tile.get_pearl_time());
        for (int d = 0; d < 4; d++)
        {
            Edge const& e = tile.get_edge(dir_of(d));
            std::uint8_t const t = e.get_edge_type() == EdgeType::KELP ? E_KELP
                                 : e.get_edge_type() == EdgeType::PORTAL ? E_PORTAL
                                                                        : E_OPEN;
            learn_edge(edge_code(c, d), t, e.get_portal_id(), true);
        }
        if (DragonPart const* p = tile.get_dragon())
        {
            occId[c] = p->get_id();
            occMine[c] = p->get_team() == myTeam;
            occHead[c] = p->is_head();
            occDir[c] = dir_index(p->get_dir());
            partCount[p->get_id()]++;
            if (p->get_id() <= 1 && me > 1)
            {
                bool const ours = p->get_team() == myTeam;
                queenSeen = true;
                queenId = ours ? p->get_id() : 1 - p->get_id();
                enemyQueenId = 1 - queenId;
            }
            // A pearl can never sit under a dragon.
            pearlState[c] = 0;
        }
    }
    for (Tile const& tile : ct.get_tiles())
    {
        DragonPart const* p = tile.get_dragon();
        if (!p || !p->is_head() || p->get_id() == me)
            continue;
        heads.push_back({cell_of(tile.get_position()), p->get_id(), partCount[p->get_id()],
                         dir_index(p->get_dir()), p->get_team() == myTeam});
    }

    // Other dragons: solid by default, time-released when the whole body is visible.
    int const myHead = cell_of(ct.get_position());
    for (int c = 0; c < N; c++)
    {
        otherRel[c] = (vis[c] && occId[c] >= 0 && occId[c] != me) ? INF : 0;
        contested[c] = 0;
    }
    for (auto const& h : heads)
        for (int d = 0; d < 4; d++)
        {
            int const n = nb(h.cell, d);
            if (!(vis[n] && occId[n] >= 0))
                contested[n] = 1;
        }
    // The queen also assumes enemies use their free sprint steps.
    if (me == queenId && !QUEEN_THREAT_MAP)
        for (auto const& h : heads)
        {
            if (h.mine)
                continue;
            int const reach = std::max(1, (h.len + 3) / 4);
            if (reach <= 1)
                continue;
            for (int c = 0; c < N; c++)
                if (vis[c] && !(occId[c] >= 0) && manhattan(c, h.cell) <= reach)
                    contested[c] = 1;
        }
    for (auto& h : heads)
    {
        std::vector<int> chain{h.cell};
        for (bool grew = true; grew;)
        {
            grew = false;
            int const cur = chain.back();
            for (int d = 0; d < 4; d++)
            {
                int const n = nb(cur, d);
                if (vis[n] && occId[n] == h.id && !occHead[n] && nb(n, occDir[n]) == cur &&
                    std::find(chain.begin(), chain.end(), n) == chain.end())
                {
                    chain.push_back(n);
                    grew = true;
                    break;
                }
            }
        }
        int const m = static_cast<int>(chain.size());
        bool const partial = m != partCount[h.id] || cheb(chain.back(), myHead) > 2;
        if (!h.mine && h.id == enemyQueenId)
            eqBodyNow = chain;
        if (!h.mine)
        {
            h.partial = partial;
            h.estLen = est_enemy_len(h.id, std::max(m, partCount[h.id]), partial);
            remember_enemy_len(h.id, partial ? std::max(m, partCount[h.id]) : m, !partial);
        }
        if (partial)
            continue; // tail may continue out of view
        if (!h.mine && m >= 4)
            enemyTails.push_back(chain.back());
        for (int i = 0; i < m; i++)
            otherRel[chain[i]] = (m - 1 - i) + 3;
    }
}

// Own body: follow the facing chain from the head inside the window, then
// extend with last turn's body for the part that is out of view.
void update_body(Controller& ct)
{
    int const head = cell_of(ct.get_position());
    std::vector<int> chain{head};
    std::vector<char> inChain(N, 0);
    inChain[head] = 1;
    while (static_cast<int>(chain.size()) < L)
    {
        int const cur = chain.back();
        int found = -1;
        for (int d = 0; d < 4 && found < 0; d++)
        {
            int const n = nb(cur, d);
            if (vis[n] && occId[n] == me && !inChain[n] && !occHead[n] && nb(n, occDir[n]) == cur)
                found = n;
        }
        if (found < 0)
            break;
        chain.push_back(found);
        inChain[found] = 1;
    }
    // Replay last turn's steps from last turn's body: exact even when the
    // body is broken across a portal or the move was a sprint.
    if (!prevBody.empty())
    {
        std::vector<int> path;
        int pos = prevBody[0];
        bool ok = true;
        for (std::size_t i = 0; i < lastDirs.size(); i++)
        {
            pos = cross(pos, lastDirs[i]);
            if (pos < 0)
            {
                if (pos == -3 && i + 1 == lastDirs.size())
                    pos = head; // first trip through this portal: we now stand at its far side
                else
                {
                    ok = false;
                    break;
                }
            }
            path.push_back(pos);
        }
        if (ok && pos == head)
        {
            std::vector<int> pred(path.rbegin(), path.rend());
            pred.insert(pred.end(), prevBody.begin(), prevBody.end());
            if (static_cast<int>(pred.size()) > L)
                pred.resize(L);
            bool consistent = true;
            for (std::size_t i = 0; i < chain.size() && i < pred.size(); i++)
                consistent &= chain[i] == pred[i];
            if (consistent && pred.size() >= chain.size())
            {
                body = pred;
                return;
            }
        }
    }
    if (static_cast<int>(chain.size()) < L && !prevBody.empty())
    {
        auto const it = std::find(prevBody.begin(), prevBody.end(), chain.back());
        if (it != prevBody.end())
            for (auto j = it + 1; j != prevBody.end() && static_cast<int>(chain.size()) < L; ++j)
                if (!inChain[*j])
                {
                    chain.push_back(*j);
                    inChain[*j] = 1;
                }
    }
    body = chain;
}

void prune_knowledge()
{
    std::erase_if(enemyReps, [](EnemyRep const& e) { return R - e.round > 60; });
    std::erase_if(dangers, [](Danger const& d) { return R - d.round > 60; });
    std::erase_if(fleet, [](auto const& kv) { return R - kv.second.round > 60; });
    std::erase_if(claims, [](auto const& kv) { return R - kv.second.round > 6; });
    if (enemyReps.size() > 200)
        enemyReps.erase(enemyReps.begin(), enemyReps.begin() + (enemyReps.size() - 200));
    if (dangers.size() > 100)
        dangers.erase(dangers.begin(), dangers.begin() + (dangers.size() - 100));
}

void add_enemy(int cell, int round, int id, int len, int estLen = 0)
{
    if (id == enemyQueenId)
        lastEqSeenR = std::max(lastEqSeenR, round);
    for (auto& e : enemyReps)
        if (e.id == id && e.round <= round)
        {
            e = {cell, round, id, len, estLen};
            return;
        }
    enemyReps.push_back({cell, round, id, len, estLen});
}

// Returns true if the message taught us something (so it is worth relaying).
bool apply_message(std::uint64_t m)
{
    int type;
    std::uint64_t payload;
    if (!unpack(m, type, payload))
        return false;
    BitR b(payload);
    switch (type)
    {
    case T_STATUS: {
        int const id = b.get(14), len = b.get(10), cell = b.get(12), leader = b.get(1), round = b.get(9);
        if (id == me || cell >= N || round > R)
            return false;
        if (id <= 1 && me > 1 && !queenSeen) // only teammates speak our protocol, so this is our queen
        {
            queenId = id;
            enemyQueenId = 1 - id;
        }
        auto it = fleet.find(id);
        if (it != fleet.end() && it->second.round >= round)
            return false;
        fleet[id] = {len, cell, round, leader == 1};
        if (id == queenId)
            return true; // relay the queen's position across the fleet
        return leader == 1;
    }
    case T_PEARL: {
        bool changed = false;
        int const count = 1 + b.get(1);
        for (int i = 0; i < count; i++)
        {
            int const kind = b.get(2), cell = b.get(12), round = b.get(9);
            if (cell >= N)
                continue;
            if (kind == 0 && round >= R && (obsNext[cell] < R || obsNext[cell] == -1) && obsNext[cell] != -2)
            {
                obsNext[cell] = round;
                nextSpawn[cell] = round;
                nextEst[cell] = 0;
                mirror_tile(cell);
                changed = true;
            }
            else if (kind == 1 && pearlRound[cell] < round)
            {
                if (nextSpawn[cell] == -2 && pearlState[cell] != 1)
                    note_drop(cell, round); // a teammate saw a death drop
                pearlState[cell] = 1;
                pearlRound[cell] = round;
                changed = true;
            }
            else if (kind == 2 && pearlRound[cell] <= round && pearlState[cell] != 0)
            {
                pearlState[cell] = 0;
                pearlRound[cell] = round;
                changed = true;
            }
        }
        return changed;
    }
    case T_PORTAL: {
        int const a = b.get(13), c = b.get(13);
        if (a >= 2 * N || c >= 2 * N || edgePartner[a] == c)
            return false;
        set_partner(a, c);
        mirror_pair(a, c);
        return true;
    }
    case T_PORTAL_EDGE: {
        int const code = b.get(13), pid = b.get(20);
        if (code >= 2 * N || edgePid[code] == pid)
            return false;
        return learn_edge(code, E_PORTAL, pid, true);
    }
    case T_KELP: {
        bool changed = false;
        int const count = b.get(2);
        for (int i = 0; i < count; i++)
        {
            int const code = b.get(13);
            if (code < 2 * N && edgeType[code] != E_KELP)
                changed |= learn_edge(code, E_KELP, -1, true);
        }
        return changed;
    }
    case T_ENEMY: {
        int const cell = b.get(12), round = b.get(9), id = b.get(14), len = b.get(8);
        bool const partial = b.get(1) == 1;
        if (cell >= N || round > R)
            return false;
        if (R - round <= 2)
            remember_enemy_len(id, len, !partial);
        add_enemy(cell, round, id, len, est_enemy_len(id, len, partial));
        return R - round <= 2;
    }
    case T_DANGER: {
        int const cell = b.get(12), round = b.get(9), cause = b.get(3);
        if (cell >= N || round > R)
            return false;
        for (auto const& d : dangers)
            if (d.cell == cell && d.round == round)
                return false;
        dangers.push_back({cell, round, cause});
        return true;
    }
    case T_SYM: {
        int const k = b.get(2);
        if (symKnown >= 0 || k > 2 || !(symMask & (1 << k)))
            return false;
        set_symmetry(k);
        return true;
    }
    case T_ZONE: {
        int const cell = b.get(12), round = b.get(9), tiles = b.get(6);
        if (cell >= N || round > R || R - round > 60)
            return false;
        for (auto& z : zoneReps)
            if (manhattan(z.cell, cell) <= 3)
            {
                bool const news = round > z.round + 5 || tiles > z.tiles;
                if (round >= z.round)
                    z = {z.cell, std::max(z.tiles, tiles), round};
                return news;
            }
        zoneReps.push_back({cell, tiles, round});
        if (zoneReps.size() > 24)
            zoneReps.erase(zoneReps.begin());
        return true;
    }
    case T_BUNKER: {
        int const tl = b.get(12), round = b.get(9);
        if (tl >= N || round > R || round <= qBunkerR)
            return false;
        qBunkerTl = tl;
        qBunkerR = round;
        return R - round <= 3;
    }
    case T_PHASE: {
        int const pm = b.get(6), sw = b.get(1), round = b.get(9);
        if (round > R)
            return false;
        heardPressure = true;
        bool changed = pm > pressureMax || (sw == 1 && !switched);
        pressureMax = std::max(pressureMax, pm);
        if (sw == 1 && R >= SW_LO)
            switched = true;
        return changed;
    }
    case T_QSPAWN: {
        int const cell = static_cast<int>(b.get(12));
        if (ourQueenSpawn < 0 && cell < N)
        {
            ourQueenSpawn = cell;
            return true;
        }
        return false;
    }
    case T_QUEEN_DEAD: {
        bool const enemy = b.get(1) == 1;
        if (enemy && !enemyQueenDead)
        {
            enemyQueenDead = true;
            return true;
        }
        if (!enemy && !ourQueenDead && !amQueen)
        {
            ourQueenDead = true;
            return true;
        }
        return false;
    }
    case T_CLAIM: {
        int const id = b.get(14), cell = b.get(12), eta = b.get(8), round = b.get(9);
        if (id == me || cell >= N || round > R)
            return false;
        auto it = claims.find(cell);
        if (it != claims.end() && it->second.round > round)
            return false;
        claims[cell] = {id, eta, round};
        return false;
    }
    default:
        return false;
    }
}

void read_sonar(Controller& ct)
{
    for (std::uint64_t const m : ct.get_sonar_messages())
        if (apply_message(m))
        {
            int type;
            std::uint64_t payload;
            int prio = 15;
            bool const typed = unpack(m, type, payload);
            if (QUEEN_POS_RELAY && typed && type == T_STATUS && me != queenId)
            {
                // The queen's position: guards and screens need it, so it goes
                // out ahead of pearl gossip while fresh.
                int const id = static_cast<int>(payload & 0x3FFF);
                int const round = static_cast<int>((payload >> 37) & 0x1FF);
                if (id == queenId && R - round <= 4)
                    prio = 45;
                else if (GIANT && ((payload >> 36) & 1) && R - round <= 4)
                    prio = 35; // leaders (the giant among them) are fed too
            }
            if (DYN_ALLOC && typed && type == T_PHASE)
                prio = 40; // the whole fleet must switch together
            if (BUNKER_GUARDS && typed && type == T_BUNKER)
                prio = 45; // guards must find her box
            if (AIM_REPORTS && me != queenId && typed && type == T_ENEMY)
            {
                // An enemy near our queen: pass it on first (and toward her).
                auto const q = fleet.find(queenId);
                int const cell = static_cast<int>(payload & 0xFFF);
                if (q != fleet.end() && R - q->second.round <= 3 && cell < N && manhattan(cell, q->second.cell) <= 14)
                    prio = 85;
            }
            queue_fact(m, prio, prio > 15 ? 1 : 2); // gossip: pass new facts along the chain
        }
    SonarEchoes const e = ct.get_sonar_echoes();
    echoEnemy = e.enemy + e.enemy_head;
}

// Section 17: a teammate that vanished from the middle of our window died.
void detect_teammate_deaths()
{
    int const head = body[0];
    std::unordered_set<int> visibleIds;
    for (int c = 0; c < N; c++)
        if (vis[c] && occId[c] >= 0)
            visibleIds.insert(occId[c]);
    for (auto const& [id, cell] : prevTeamHeads)
    {
        if (visibleIds.count(id) || cheb(cell, head) > 2)
            continue;
        int cause = 2;
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, cell) <= 2)
                cause = 1;
        dangers.push_back({cell, R, cause});
        queue_fact(msg_danger(cell, cause), 75, 3);
        fleet.erase(id);
        if (id == queenId && !ourQueenDead)
        {
            ourQueenDead = true;
            queue_fact(msg_queen_dead(false), 95, 4);
        }
    }
    // The enemy queen vanished from the middle of our window: it died. Or she
    // is out of view and a death-drop pearl (on a tile that never spawns) lies
    // on her last body or within one turn's move of her head: a head-on kills
    // both, and the striker's own sonar dies with it, so watchers must tell.
    if (prevEnemyQueenCell >= 0 && !enemyQueenDead && !visibleIds.count(enemyQueenId))
    {
        int const fq = EQ_DEATH_DETECT ? std::max(1, (prevEnemyQueenLen + 3) / 4) : 1;
        bool dead = cheb(prevEnemyQueenCell, head) <= 3 - fq;
        for (int c = 0; c < N && !dead && EQ_DEATH_DETECT; c++)
        {
            if (!vis[c] || dropRound[c] != R)
                continue;
            if ((!EQ_STRICT && manhattan(c, prevEnemyQueenCell) <= fq) ||
                std::find(prevEnemyQueenBody.begin(), prevEnemyQueenBody.end(), c) != prevEnemyQueenBody.end())
                dead = true;
        }
        if (dead)
        {
            enemyQueenDead = true;
            queue_fact(msg_queen_dead(true), 95, 4);
        }
    }
    // The same from a fleet report of her (at most three rounds old: a watcher
    // that moves before the striker sees the drop only next round): a fresh
    // death drop near where she was, and she is not in our window.
    if (EQ_DEATH_DETECT && !EQ_STRICT && !enemyQueenDead && !visibleIds.count(enemyQueenId))
        for (auto const& e : enemyReps)
        {
            int const age = R - e.round;
            if (e.id != enemyQueenId || age < 1 || age > 3 || cheb(e.cell, head) > 3 + age)
                continue;
            int const rad = std::min(3, std::max(1, (e.len + 3) / 4) * age);
            if (cheb(e.cell, head) + rad <= 3)
            {
                enemyQueenDead = true; // every cell she could have reached is in view, and she is not
                queue_fact(msg_queen_dead(true), 95, 4);
                break;
            }
            for (int c = 0; c < N; c++)
                if (vis[c] && dropRound[c] == R && manhattan(c, e.cell) <= rad)
                {
                    enemyQueenDead = true;
                    queue_fact(msg_queen_dead(true), 95, 4);
                    break;
                }
        }
    prevEnemyQueenCell = -1;
    prevEnemyQueenBody = eqBodyNow;
    for (auto const& h : heads)
        if (!h.mine && h.id == enemyQueenId)
        {
            prevEnemyQueenCell = h.cell;
            prevEnemyQueenLen = std::max(h.len, h.estLen);
        }
    prevTeamHeads.clear();
    for (auto const& h : heads)
        if (h.mine)
            prevTeamHeads[h.id] = h.cell;
}

void record_enemies()
{
    int shared = 0;
    for (auto const& h : heads)
    {
        if (h.mine)
            continue;
        add_enemy(h.cell, R, h.id, h.len, h.estLen);
        if (h.id == enemyQueenId)
            queue_fact(msg_enemy(h.cell, h.id, h.len, h.partial), 92, 2); // "one smells blood"
        else if (shared < 2)
        {
            int prio = 70;
            auto const q = fleet.find(queenId);
            if (AIM_REPORTS && me != queenId && q != fleet.end() && R - q->second.round <= 3 && manhattan(h.cell, q->second.cell) <= 14)
                prio = 88; // a threat to the queen goes out before anything else
            queue_fact(msg_enemy(h.cell, h.id, h.len, h.partial), prio, 1);
            shared++;
        }
    }
}

} // namespace
