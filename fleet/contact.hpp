// L4b CONTACT - the queen's contact search (R1) and its trigger (R8).
// When an enemy head is close and the move is hard, the queen looks two of her own moves ahead with the
// replies of the nearest enemies in between (max-min, Tron style) and judges the result by the ground she
// reaches before them, whether a free loop is still reachable, and her exits. The value is blended into
// her ordinary move score (score_moves), it does not replace it.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

// Distance field with a stamp (no clearing between uses).
struct DField
{
    std::vector<int> d, st;
    int s = 0;
    void reset()
    {
        if (static_cast<int>(d.size()) != N)
        {
            d.assign(N, INF);
            st.assign(N, 0);
        }
        s++;
    }
    int get(int c) const { return st[c] == s ? d[c] : INF; }
    void set(int c, int v)
    {
        st[c] = s;
        d[c] = v;
    }
};

int csWork = 0;     // BFS cells popped in this search (R8 budget guard)
bool csAbort = false;
std::vector<int> csQ, restReach;

bool cs_budget_ok()
{
    if (csWork > CS_WORK_CAP)
        csAbort = true;
    return !csAbort;
}

// Enemy movement field from `src`: unexplored edges open, every dragon body in view blocks.
void cs_enemy_bfs(int src, int maxD, DField& f)
{
    f.reset();
    csQ.clear();
    csQ.push_back(src);
    f.set(src, 0);
    for (std::size_t i = 0; i < csQ.size(); i++)
    {
        int const c = csQ[i], dc = f.get(c);
        csWork++;
        if (dc >= maxD)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int const n = step_open(c, k);
            if (n < 0 || f.get(n) < INF || (vis[n] && occId[n] >= 0))
                continue;
            f.set(n, dc + 1);
            csQ.push_back(n);
        }
    }
}

// Queen movement field from her head after a move (body b, length len): known water only, timed bodies.
void cs_queen_bfs(std::vector<int> const& b, int len, int maxD, DField& f)
{
    BodyScope const sb(b, len);
    f.reset();
    csQ.clear();
    csQ.push_back(b[0]);
    f.set(b[0], 0);
    for (std::size_t i = 0; i < csQ.size(); i++)
    {
        int const c = csQ[i], dc = f.get(c);
        csWork++;
        if (dc >= maxD)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int const n = cross(c, k);
            if (n < 0 || f.get(n) < INF || !passable(n, dc + 1))
                continue;
            f.set(n, dc + 1);
            csQ.push_back(n);
        }
    }
}

// Body after one step (b head first, len); eats a pearl if there is one.
std::vector<int> cs_step(std::vector<int> const& b, int len, int n, int& outLen)
{
    bool const eat = pearl_at(n);
    std::vector<int> nb;
    nb.reserve(b.size() + 1);
    nb.push_back(n);
    nb.insert(nb.end(), b.begin(), b.end());
    outLen = len + (eat ? 1 : 0);
    if (!eat && static_cast<int>(b.size()) >= len)
        nb.pop_back();
    return nb;
}

struct CsEnemy
{
    int id, cell, len, freeR, fullR;
    std::vector<int> replies; // cells it can put its head on with free steps (no paid steps: it keeps its length)
};

struct CsResult
{
    bool ran = false, aborted = false;
    std::array<double, 4> val{}; // per direction, -1 (certain loss) .. 1
    std::array<bool, 4> has{};
};

// The search. cands: the analysed moves (legal / blind / dest / nb1); head: her head.
CsResult contact_search(std::array<Cand, 4> const& cands, int head)
{
    CsResult res;
    csWork = 0;
    csAbort = false;
    // Near enemies: the CS_ENEMIES closest visible heads within CS_NEAR.
    std::vector<CsEnemy> near;
    {
        std::vector<std::pair<int, HeadInfo const*>> byDist;
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, head) <= CS_NEAR)
                byDist.push_back({manhattan(h.cell, head), &h});
        std::sort(byDist.begin(), byDist.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        for (std::size_t i = 0; i < byDist.size() && static_cast<int>(near.size()) < CS_ENEMIES; i++)
        {
            HeadInfo const& h = *byDist[i].second;
            int const len = std::max(h.len, h.estLen);
            int const f = std::max(1, (len + 3) / 4);
            int const m = std::min(PAID_REACH_CAP, f + (PAID_REACH ? std::max(0, len - 2) : 0));
            near.push_back({h.id, h.cell, len, f, std::max(f, m), {}});
        }
    }
    if (near.empty())
        return res;
    std::unordered_set<int> nearIds;
    for (auto const& e : near)
        nearIds.insert(e.id);
    // Everyone else: where they can strike within two turns (as the look-2 map, without the near enemies).
    restReach.assign(N, INF);
    for (auto const& h : heads)
    {
        if (h.mine || nearIds.count(h.id))
            continue;
        int const len = std::max(h.len, h.estLen);
        int const f = std::max(1, (len + 3) / 4);
        int const m = std::min(PAID_REACH_CAP, f + (PAID_REACH ? std::max(0, len - 2) : 0));
        reach_bfs(h.cell, 2 * f, std::min(PAID_REACH_CAP, m + f), restReach);
    }
    {
        std::unordered_set<int> seen;
        for (auto const& h : heads)
            seen.insert(h.id);
        for (auto const& e : enemyReps)
        {
            int const age = R - e.round;
            if (age < 1 || age > REP_MAX_AGE || seen.count(e.id) || nearIds.count(e.id))
                continue;
            int const el = std::max(e.len, e.estLen);
            int const f1 = std::max(1, (el + 3) / 4);
            int const f = f1 * (age + 1);
            int const m = std::min(PAID_REACH_CAP, f + (PAID_REACH ? std::max(0, el - 2) : 0));
            reach_bfs(e.cell, f + f1, std::min(PAID_REACH_CAP, m + f1), restReach);
        }
    }
    // Each near enemy's reply cells (free steps only), and a distance field from every reply cell we use.
    static DField tmp;
    for (auto& e : near)
    {
        cs_enemy_bfs(e.cell, e.freeR, tmp);
        for (int c : csQ)
            if (c != e.cell && tmp.get(c) >= 1)
                e.replies.push_back(c);
    }
    static std::vector<DField> slots;
    if (slots.size() < 32)
        slots.resize(32);
    std::unordered_map<int, int> slotOf; // (enemy index * N + cell) -> slot
    int nSlots = 0;
    int const RT = CS_TERR_RADIUS;
    auto field_for = [&](int ei, int cell) -> DField const* {
        long long const key = static_cast<long long>(ei) * N + cell;
        auto const it = slotOf.find(static_cast<int>(key));
        if (it != slotOf.end())
            return &slots[it->second];
        if (nSlots >= static_cast<int>(slots.size()) || !cs_budget_ok())
            return nullptr;
        int const s = nSlots++;
        slotOf[static_cast<int>(key)] = s;
        CsEnemy const& e = near[ei];
        cs_enemy_bfs(cell, std::min(40, std::max(e.fullR, RT * e.freeR)), slots[s]);
        return &slots[s];
    };

    static DField qf;
    for (auto const& cd : cands)
    {
        if (!cd.legal || cd.blind || cd.dest < 0 || cd.nb1.empty())
            continue;
        int const d = cd.d;
        res.has[d] = true;
        // Inside some enemy's reach right now: their reply ends her (exact, every enemy moves after the queen).
        if (QUEEN_THREAT_MAP && qThreat[cd.dest] < INF)
        {
            res.val[d] = -1;
            continue;
        }
        int const len1 = L + (cd.eat ? 1 : 0);
        std::vector<int> const& b1 = cd.nb1;
        int const n1 = b1[0];
        // Her second moves (known water, legal with her body after the first move).
        struct Second
        {
            std::vector<int> b;
            int len;
            double base; // loop + exits part of the value (does not depend on the replies)
        };
        std::vector<Second> seconds;
        {
            BodyScope const sb(b1, len1);
            for (int k = 0; k < 4; k++)
            {
                int const n2 = cross(n1, k);
                if (n2 < 0 || !passable(n2, 1) || (vis[n2] && occId[n2] >= 0 && occId[n2] != me))
                    continue;
                int l2 = 0;
                std::vector<int> b2 = cs_step(b1, len1, n2, l2);
                seconds.push_back({std::move(b2), l2, 0.0});
            }
        }
        if (seconds.empty())
        {
            res.val[d] = -1; // no way on after this move
            continue;
        }
        for (auto& s : seconds)
        {
            bool const loop = loop_reachable(s.b, s.len);
            int ex = 0;
            {
                BodyScope const sb(s.b, s.len);
                for (int k = 0; k < 4; k++)
                {
                    int const n = cross(s.b[0], k);
                    ex += n >= 0 && passable(n, 1) && !(vis[n] && occId[n] >= 0 && occId[n] != me);
                }
            }
            s.base = CS_W_LOOP * (loop ? 1.0 : 0.0) + CS_W_EXITS * std::min(ex, 3) / 3.0;
        }
        // Replies to rank: the cells closest to her new head (the most threatening), top CS_REPLIES per enemy.
        std::vector<std::vector<int>> rep(near.size());
        for (std::size_t ei = 0; ei < near.size(); ei++)
        {
            rep[ei] = near[ei].replies;
            std::sort(rep[ei].begin(), rep[ei].end(), [&](int a, int b) { return manhattan(a, n1) < manhattan(b, n1) || (manhattan(a, n1) == manhattan(b, n1) && a < b); });
            if (static_cast<int>(rep[ei].size()) > CS_REPLIES)
                rep[ei].resize(CS_REPLIES);
            if (rep[ei].empty())
                rep[ei].push_back(near[ei].cell); // boxed in: assume it stays a threat where it is
        }
        // Queen fields for each second move (independent of the replies).
        std::vector<std::vector<std::pair<int, int>>> qCells(seconds.size()); // (cell, her distance)
        for (std::size_t si = 0; si < seconds.size() && cs_budget_ok(); si++)
        {
            cs_queen_bfs(seconds[si].b, seconds[si].len, RT, qf);
            for (int c : csQ)
                qCells[si].push_back({c, qf.get(c)});
        }
        if (csAbort)
            break;
        // max-min: the replies minimise, her second move maximises.
        double worst = 2.0;
        std::vector<int> idx(near.size(), 0);
        for (bool more = true; more && cs_budget_ok();)
        {
            std::vector<DField const*> ef(near.size());
            for (std::size_t ei = 0; ei < near.size(); ei++)
                ef[ei] = field_for(static_cast<int>(ei), rep[ei][idx[ei]]);
            if (csAbort)
                break;
            double best = -1.0;
            for (std::size_t si = 0; si < seconds.size(); si++)
            {
                auto const& s = seconds[si];
                int const n2 = s.b[0];
                bool dead = restReach[n2] < INF;
                for (std::size_t ei = 0; ei < near.size() && !dead; ei++)
                    dead = ef[ei] && ef[ei]->get(n2) <= near[ei].fullR;
                if (dead)
                    continue;
                int const fq = std::max(1, (s.len + 3) / 4);
                int terr = 0;
                for (auto const& [c, dq] : qCells[si])
                {
                    int const tq = (dq + fq - 1) / fq;
                    bool first = true;
                    for (std::size_t ei = 0; ei < near.size() && first; ei++)
                    {
                        int const de = ef[ei] ? ef[ei]->get(c) : INF;
                        if (de < INF && tq >= (de + near[ei].freeR - 1) / near[ei].freeR)
                            first = false;
                    }
                    terr += first;
                }
                csWork += static_cast<int>(qCells[si].size());
                // Ground saturates gently: 1 - e^(-cells / (2L + 8)) (a short queen with 14 cells is at 0.63, with 60 at 0.99).
                double const v = CS_W_TERR * (1.0 - std::exp(-static_cast<double>(terr) / (2 * s.len + 8))) + s.base;
                best = std::max(best, v);
            }
            worst = std::min(worst, best);
            // next joint reply
            more = false;
            for (std::size_t ei = 0; ei < near.size(); ei++)
            {
                if (++idx[ei] < static_cast<int>(rep[ei].size()))
                {
                    more = true;
                    break;
                }
                idx[ei] = 0;
            }
        }
        if (csAbort)
            break;
        res.val[d] = worst > 1.5 ? 0.0 : worst;
    }
    res.aborted = csAbort;
    res.ran = !csAbort;
    return res;
}

} // namespace
