// L7 SPEAK - the four sonar rays each turn: what to send and in which direction.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

// Sonar output (section 4): four messages a turn, N/E/S/W.
// Direction of a sonar ray that runs straight to our queen (same row or
// column, the shorter way round), or -1.
int queen_sonar_dir()
{
    if (me == queenId || ourQueenDead || body.empty())
        return -1;
    auto const q = fleet.find(queenId);
    if (q == fleet.end() || R - q->second.round > 3)
        return -1;
    int const h = body[0], qc = q->second.cell;
    int const hx = h % W, hy = h / W, qx = qc % W, qy = qc / W;
    if (hy == qy && hx != qx)
        return ((qx - hx + W) % W) <= ((hx - qx + W) % W) ? 1 : 3;
    if (hx == qx && hy != qy)
        return ((qy - hy + H) % H) <= ((hy - qy + H) % H) ? 2 : 0;
    return -1;
}

void send_sonars(Controller& ct)
{
    std::vector<std::uint64_t> out;
    out.push_back(msg_status());
    if (QUEEN_POS_RELAY && amQueen)
        out.push_back(out[0]); // two directions a turn: every teammate in line hears where she is
    if (claimTarget >= 0)
        out.push_back(msg_claim(claimTarget, claimEta));

    std::sort(facts.begin(), facts.end(), [](Fact const& a, Fact const& b) { return a.prio > b.prio; });
    for (auto& f : facts)
    {
        if (out.size() >= 4)
            break;
        if (f.prio < 30 && !pearlQ.empty() && out.size() == 3)
            break; // leave room for pearl news
        out.push_back(f.msg);
        f.sends--;
    }
    std::erase_if(facts, [](Fact const& f) { return f.sends <= 0; });

    if (out.size() < 4 && !pearlQ.empty())
    {
        std::sort(pearlQ.begin(), pearlQ.end(), [](PearlEntry const& a, PearlEntry const& b) { return a.prio > b.prio; });
        BitW b;
        int const take = std::min<int>(2, static_cast<int>(pearlQ.size()));
        b.put(take - 1, 1);
        for (int i = 0; i < take; i++)
        {
            b.put(pearlQ[i].kind, 2);
            b.put(pearlQ[i].cell, 12);
            b.put(pearlQ[i].round, 9);
        }
        pearlQ.erase(pearlQ.begin(), pearlQ.begin() + take);
        out.push_back(pack(T_PEARL, b.v));
    }

    // Gossip what we already know, so late arrivals catch up.
    for (int attempt = 0; attempt < 12 && out.size() < 4; attempt++)
    {
        int const pick = static_cast<int>(rng() % 3);
        if (pick == 0 && !kelpList.empty())
        {
            BitW b;
            int const n = std::min<int>(3, static_cast<int>(kelpList.size()));
            b.put(n, 2);
            for (int i = 0; i < n; i++)
                b.put(kelpList[rng() % kelpList.size()], 13);
            out.push_back(pack(T_KELP, b.v));
        }
        else if (pick == 1 && !portalPairs.empty())
        {
            auto const& pp = portalPairs[rng() % portalPairs.size()];
            out.push_back(msg_portal(pp.first, pp.second));
        }
        else if (pick == 2)
        {
            // a known upcoming or present pearl
            int found = -1;
            for (int tries = 0; tries < 24 && found < 0; tries++)
            {
                int const c = static_cast<int>(rng() % static_cast<unsigned>(N));
                if (pearlState[c] == 1 || (nextSpawn[c] >= R && !nextEst[c] && nextSpawn[c] - R <= 30))
                    found = c;
            }
            if (found < 0)
                continue;
            BitW b;
            b.put(0, 1);
            bool const present = pearlState[found] == 1;
            b.put(present ? 1 : 0, 2);
            b.put(found, 12);
            b.put(present ? pearlRound[found] : nextSpawn[found], 9);
            out.push_back(pack(T_PEARL, b.v));
        }
    }

    int const offset = (R + me) & 3;
    std::array<int, 4> dirOf{};
    for (int i = 0; i < 4; i++)
        dirOf[i] = (i + offset) & 3;
    // Aim the most urgent enemy news at the queen when she is on our row or
    // column (a ping stops at the first dragon it meets, so nearer is better).
    int const qd = AIM_REPORTS ? queen_sonar_dir() : -1;
    if (qd >= 0)
    {
        int best = -1;
        for (int i = 0; i < static_cast<int>(out.size()); i++)
        {
            int type;
            std::uint64_t payload;
            if (unpack(out[i], type, payload) && (type == T_ENEMY || type == T_DANGER))
            {
                best = i;
                break; // facts are already in priority order
            }
        }
        if (best >= 0)
            for (int i = 0; i < 4; i++)
                if (dirOf[i] == qd)
                {
                    if (i >= static_cast<int>(out.size()))
                        out.resize(i + 1, out[0]);
                    std::swap(out[i], out[best]);
                    break;
                }
    }
    for (int i = 0; i < static_cast<int>(out.size()) && i < 4; i++)
    {
        ct.send_sonar(dir_of(dirOf[i]), out[i]);
        if (traceF)
            traceAct += std::string(" ") + DCH[dirOf[i]] + std::to_string(out[i]);
    }
}

} // namespace
