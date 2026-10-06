// L7 COMMS (wire format) - message layout, checksum, encoders, the fact and pearl outboxes.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

// Sonar messages
// [63..56] magic  [55..52] type  [51..48] checksum  [47..0] payload
constexpr std::uint64_t MAGIC = 0xB7;
enum MsgType
{
    T_STATUS = 1,
    T_PEARL = 2,
    T_PORTAL = 3,
    T_KELP = 4,
    T_ENEMY = 5,
    T_DANGER = 6,
    T_SYM = 7,
    T_CLAIM = 8,
    T_PORTAL_EDGE = 9,
    T_QUEEN_DEAD = 10,
    T_QSPAWN = 11,
    T_PHASE = 12,
    T_BUNKER = 13,
    T_ZONE = 14,
};

struct BitW
{
    std::uint64_t v = 0;
    int pos = 0;
    void put(std::uint64_t x, int bits)
    {
        v |= (x & ((1ULL << bits) - 1)) << pos;
        pos += bits;
    }
};
struct BitR
{
    std::uint64_t v;
    int pos = 0;
    explicit BitR(std::uint64_t v) : v(v) {}
    int get(int bits)
    {
        int const x = static_cast<int>((v >> pos) & ((1ULL << bits) - 1));
        pos += bits;
        return x;
    }
};

std::uint64_t checksum(int type, std::uint64_t payload)
{
    // Salted by team, so the other side's copy of this bot fails validation.
    std::uint64_t cs = static_cast<std::uint64_t>(type) ^ (myTeam == Team::A ? 0x5 : 0xA);
    for (int i = 0; i < 12; i++)
        cs ^= (payload >> (4 * i)) & 15;
    return cs;
}

std::uint64_t pack(int type, std::uint64_t payload)
{
    payload &= (1ULL << 48) - 1;
    return (MAGIC << 56) | (static_cast<std::uint64_t>(type) << 52) | (checksum(type, payload) << 48) | payload;
}

bool unpack(std::uint64_t m, int& type, std::uint64_t& payload)
{
    if ((m >> 56) != MAGIC)
        return false;
    type = static_cast<int>((m >> 52) & 15);
    payload = m & ((1ULL << 48) - 1);
    return ((m >> 48) & 15) == checksum(type, payload);
}

struct Fact
{
    std::uint64_t msg;
    int prio, sends;
};
std::vector<Fact> facts;

void queue_fact(std::uint64_t msg, int prio, int sends)
{
    for (auto& f : facts)
        if (f.msg == msg)
        {
            f.prio = std::max(f.prio, prio);
            f.sends = std::max(f.sends, sends);
            return;
        }
    facts.push_back({msg, prio, sends});
    if (facts.size() > 64)
    {
        auto it = std::min_element(facts.begin(), facts.end(), [](Fact const& a, Fact const& b) { return a.prio < b.prio; });
        facts.erase(it);
    }
}

struct PearlEntry
{
    int kind, cell, round, prio; // kind 0 next spawn at round, 1 present at round, 2 gone at round
};
std::vector<PearlEntry> pearlQ;

void queue_pearl(int kind, int cell, int round, int prio)
{
    for (auto& e : pearlQ)
        if (e.cell == cell && e.kind == kind)
        {
            e.round = round;
            e.prio = std::max(e.prio, prio);
            return;
        }
    pearlQ.push_back({kind, cell, round, prio});
    if (pearlQ.size() > 40)
    {
        auto it = std::min_element(pearlQ.begin(), pearlQ.end(), [](PearlEntry const& a, PearlEntry const& b) { return a.prio < b.prio; });
        pearlQ.erase(it);
    }
}

std::uint64_t msg_status()
{
    BitW b;
    b.put(me, 14);
    b.put(std::min(L, 1023), 10);
    b.put(body.empty() ? 0 : body[0], 12);
    b.put(isLeader ? 1 : 0, 1);
    b.put(R, 9);
    return pack(T_STATUS, b.v);
}
std::uint64_t msg_portal(int a, int c)
{
    BitW b;
    b.put(a, 13);
    b.put(c, 13);
    return pack(T_PORTAL, b.v);
}
std::uint64_t msg_portal_edge(int code, int pid)
{
    BitW b;
    b.put(code, 13);
    b.put(pid, 20);
    return pack(T_PORTAL_EDGE, b.v);
}
std::uint64_t msg_enemy(int cell, int id, int len, bool partial = false)
{
    BitW b;
    b.put(cell, 12);
    b.put(R, 9);
    b.put(id, 14);
    b.put(std::min(len, 255), 8);
    b.put(partial ? 1 : 0, 1); // len is only what the sender saw
    return pack(T_ENEMY, b.v);
}
std::uint64_t msg_danger(int cell, int cause)
{
    BitW b;
    b.put(cell, 12);
    b.put(R, 9);
    b.put(cause, 3);
    return pack(T_DANGER, b.v);
}
std::uint64_t msg_sym()
{
    BitW b;
    b.put(symKnown, 2);
    return pack(T_SYM, b.v);
}
std::uint64_t msg_queen_dead(bool enemy)
{
    BitW b;
    b.put(enemy ? 1 : 0, 1);
    b.put(enemy ? enemyQueenId : queenId, 2);
    return pack(T_QUEEN_DEAD, b.v);
}
std::uint64_t msg_qspawn(int cell)
{
    BitW b;
    b.put(cell, 12);
    return pack(T_QSPAWN, b.v);
}
// OPEN_PLAN zones: a pearl-rich spot and how many spawn tiles it has.
std::uint64_t msg_zone(int cell, int tiles)
{
    BitW b;
    b.put(cell, 12);
    b.put(R, 9);
    b.put(std::min(tiles, 63), 6);
    return pack(T_ZONE, b.v);
}
// The queen's bunker box (its top-left cell): guards post round it.
std::uint64_t msg_bunker(int tl)
{
    BitW b;
    b.put(tl, 12);
    b.put(R, 9);
    return pack(T_BUNKER, b.v);
}
// Phase: the highest enemy count anyone has seen, and whether we switched
// from numbers to length (newborn dragons know nothing until told).
std::uint64_t msg_phase()
{
    BitW b;
    b.put(std::min(pressureMax, 63), 6);
    b.put(switched ? 1 : 0, 1);
    b.put(R, 9);
    return pack(T_PHASE, b.v);
}
std::uint64_t msg_claim(int cell, int eta)
{
    BitW b;
    b.put(me, 14);
    b.put(cell, 12);
    b.put(std::min(eta, 255), 8);
    b.put(R, 9);
    return pack(T_CLAIM, b.v);
}

} // namespace
