// L7 COMMS (wire format) - message layout, checksum, encoders, the fact and pearl outboxes.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

// Sonar messages (7f): 64 bits = [63..12] the 52-bit block (type 4 bits << 48 | payload 48 bits) under a keyed
// 4-round Feistel cipher, [11..0] a 12-bit check code of the plain block. All battles and replays are public and
// replays show sonar values: without the key nobody can read our messages (the queen's position is in her
// STATUS) or forge them. The key is salted by team, so the other side's copy of this bot cannot read them either.
// A message is accepted only if the code matches, the type is one we use and every payload bit past that type's
// layout is zero - so a stray enemy value almost never passes (a 3-bit QUEEN_DEAD has 45 bits that must be 0).
enum MsgType
{
    T_STRIKE = 0, // Q2c: "their queen is within my strike reach" / "off: she got away"
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
    T_ROLE = 15, // 6d: a one-of-a-kind role claim (giant, heir, leader)
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

#ifndef MSG_KEY
#define MSG_KEY 0x6A09E667F3BCC908ULL // secret: change it to change every message
#endif
std::uint64_t mix64(std::uint64_t x)
{
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}
std::uint64_t team_key() { return MSG_KEY ^ (myTeam == Team::A ? 0x3C6EF372FE94F82BULL : 0xA54FF53A5F1D36F1ULL); }

constexpr int HALF_BITS = 26;
constexpr std::uint64_t HALF_MASK = (1ULL << HALF_BITS) - 1;
std::uint64_t feistel_f(std::uint64_t half, int round) { return mix64(half ^ team_key() ^ (0x9E3779B97F4A7C15ULL * (round + 1))) & HALF_MASK; }
std::uint64_t encrypt52(std::uint64_t block)
{
    std::uint64_t l = block >> HALF_BITS, r = block & HALF_MASK;
    for (int k = 0; k < 4; k++)
    {
        std::uint64_t const t = r;
        r = l ^ feistel_f(r, k);
        l = t;
    }
    return (l << HALF_BITS) | r;
}
std::uint64_t decrypt52(std::uint64_t block)
{
    std::uint64_t l = block >> HALF_BITS, r = block & HALF_MASK;
    for (int k = 3; k >= 0; k--)
    {
        std::uint64_t const t = l;
        l = r ^ feistel_f(l, k);
        r = t;
    }
    return (l << HALF_BITS) | r;
}
std::uint64_t check_code(std::uint64_t block) { return mix64(block ^ ~team_key()) >> 52; }

// Payload bits each type uses (the rest must be zero). PEARL and KELP carry a count in their lowest bits.
int layout_bits(int type, std::uint64_t payload)
{
    switch (type)
    {
    case T_STATUS: return 48;
    case T_PEARL: return 1 + (static_cast<int>(payload & 1) + 1) * 23;
    case T_PORTAL: return 26;
    case T_KELP: return 2 + static_cast<int>(payload & 3) * 13;
    case T_ENEMY: return 44;
    case T_DANGER: return 24;
    case T_SYM: return 2;
    case T_CLAIM: return 43;
    case T_PORTAL_EDGE: return 33;
    case T_QUEEN_DEAD: return 3;
    case T_QSPAWN: return 12;
    case T_PHASE: return 16;
    case T_BUNKER: return 21;
    case T_ZONE: return 27;
    case T_ROLE: return 35;
    case T_STRIKE: return 36;
    default: return -1; // not a type we send
    }
}

std::uint64_t pack(int type, std::uint64_t payload)
{
    payload &= (1ULL << 48) - 1;
    std::uint64_t const block = (static_cast<std::uint64_t>(type) << 48) | payload;
    return (encrypt52(block) << 12) | check_code(block);
}

bool unpack(std::uint64_t m, int& type, std::uint64_t& payload)
{
    std::uint64_t const block = decrypt52(m >> 12);
    if ((m & 0xFFF) != check_code(block))
        return false;
    type = static_cast<int>(block >> 48);
    payload = block & ((1ULL << 48) - 1);
    int const bits = layout_bits(type, payload);
    return bits >= 0 && (payload >> bits) == 0;
}

#ifdef MSG_SELFTEST
// Debug: round trips of valid messages, and how many random / small foreign values pass as ours.
void msg_selftest(std::FILE* f)
{
    std::mt19937_64 g(12345);
    int bad = 0, randomPass = 0, smallPass = 0, qdeadPass = 0;
    int const types[] = {T_STATUS, T_PEARL, T_PORTAL, T_KELP, T_ENEMY, T_DANGER, T_SYM, T_CLAIM, T_PORTAL_EDGE, T_QUEEN_DEAD, T_QSPAWN, T_PHASE, T_BUNKER, T_ZONE, T_STRIKE};
    for (int i = 0; i < 200000; i++)
    {
        int const t = types[i % 15];
        std::uint64_t p = g();
        int const bits = layout_bits(t, p);
        p &= (1ULL << bits) - 1;
        int t2;
        std::uint64_t p2;
        if (!unpack(pack(t, p), t2, p2) || t2 != t || p2 != p)
            bad++;
    }
    int t2;
    std::uint64_t p2;
    for (int i = 0; i < 1000000; i++)
    {
        if (unpack(g(), t2, p2))
        {
            randomPass++;
            qdeadPass += t2 == T_QUEEN_DEAD || t2 == T_SYM;
        }
        smallPass += unpack(static_cast<std::uint64_t>(i), t2, p2);
    }
    std::fprintf(f, "selftest team %c: round-trip failures %d / 200000; random 64-bit accepted %d / 1000000 (queen-dead or sym %d); "
                    "values 0..999999 accepted %d\n", myTeam == Team::A ? 'A' : 'B', bad, randomPass, qdeadPass, smallPass);
}
#endif

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

// 6d'': giant / heir claims gossiped on every STATUS (GIANT_GOSSIP). Length 5..35+ -> 1..31 (0 = none).
int gossip_len(int len) { return std::clamp(len - 4, 1, 31); }
int gossip_hash(int id) { return id & 3; }
struct GossipClaim
{
    int gl = 0, gh = 0, born = -1000; // born: the round its holder last made it
};
GossipClaim gBest;      // the best fresh claim heard (any holder, our own echo included)
bool better_claim(int gl, int gh, GossipClaim const& c) { return gl > c.gl || (gl == c.gl && gh < c.gh); }
bool gossip_fresh(GossipClaim const& c) { return c.gl > 0 && R - c.born <= GOSSIP_MAX_AGE; }
// A claim from another holder (id hash differs) that beats a dragon of length len and id me.
// Same hash: it may be our own echo - it counts only if 2+ longer than we are now.
bool gossip_beats_me(int len)
{
    if (!GIANT_GOSSIP || !gossip_fresh(gBest))
        return false;
    if (gBest.gh == gossip_hash(me))
        return gBest.gl >= gossip_len(len) + 2;
    return better_claim(gBest.gl, gBest.gh, {gossip_len(len), gossip_hash(me), R});
}
static_assert(!(GIANT_GOSSIP && SWITCH_STATUS_BIT), "GIANT_GOSSIP repacks STATUS: no room for the 6e switch bit");
struct StatusMsg
{
    int id, len, cell, leader, round, eq, sw, gl, gh, ga;
};
StatusMsg read_status(std::uint64_t payload)
{
    BitR b(payload);
    StatusMsg s{};
    if (GIANT_GOSSIP)
    {
        s.id = b.get(13), s.len = b.get(7), s.cell = b.get(12), s.leader = b.get(1);
        int const r4 = b.get(4);
        s.round = R - ((R - r4) & 15);
        s.eq = b.get(1), s.gl = b.get(5), s.gh = b.get(2), s.ga = b.get(3);
        return s;
    }
    s.id = b.get(14), s.len = b.get(10), s.cell = b.get(12), s.leader = b.get(1), s.round = b.get(9), s.eq = b.get(1), s.sw = b.get(1);
    return s;
}

std::uint64_t msg_status()
{
    BitW b;
    if (GIANT_GOSSIP)
    {
        GossipClaim own;
        if (my.giant || my.heir)
            own = {gossip_len(L), gossip_hash(me), R};
        GossipClaim const& c = gossip_fresh(gBest) && (own.gl == 0 || better_claim(gBest.gl, gBest.gh, own)) ? gBest : own;
        b.put(me, 13);
        b.put(std::min(L, 127), 7);
        b.put(body.empty() ? 0 : body[0], 12);
        b.put(my.leader ? 1 : 0, 1);
        b.put(R & 15, 4);
        b.put(EQ_STATUS_BIT && sit.enemyQueenDead ? 1 : 0, 1);
        b.put(c.gl, 5);
        b.put(c.gh, 2);
        b.put(c.gl > 0 ? std::min((R - c.born + GOSSIP_AGE_UNIT - 1) / GOSSIP_AGE_UNIT, 7) : 0, 3);
        return pack(T_STATUS, b.v);
    }
    b.put(me, 14);
    b.put(std::min(L, 1023), 10);
    b.put(body.empty() ? 0 : body[0], 12);
    b.put(my.leader ? 1 : 0, 1);
    b.put(R, 9);
    b.put(EQ_STATUS_BIT && sit.enemyQueenDead ? 1 : 0, 1); // Q: every status carries "their queen is dead"
    b.put(SWITCH_STATUS_BIT && sit.switched ? 1 : 0, 1);   // 6e: ...and "we switched from numbers to length"
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
// 6d: "I hold this role at this length" (relayed so the whole fleet hears it; the best-ranked claims win).
std::uint64_t msg_role(int role, int id, int len, int round)
{
    BitW b;
    b.put(role, 2);
    b.put(id, 14);
    b.put(std::min(len, 1023), 10);
    b.put(round, 9);
    return pack(T_ROLE, b.v);
}
// Q2c: striker id, her head cell, round, and whether the strike is off (she got away, the striker is alive).
std::uint64_t msg_strike(int cell, bool off)
{
    BitW b;
    b.put(me, 14);
    b.put(cell, 12);
    b.put(R, 9);
    b.put(off ? 1 : 0, 1);
    return pack(T_STRIKE, b.v);
}
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
    b.put(std::min(sit.pressureMax, 63), 6);
    b.put(sit.switched ? 1 : 0, 1);
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
