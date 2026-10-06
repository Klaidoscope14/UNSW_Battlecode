// Fleet bot: numbers and information first, then size and control.
//
// Every dragon runs its own copy of this program and shares nothing but sonar,
// so the "fleet" behaviour is emergent: each dragon keeps a persistent model of
// the map, tells teammates what it learns, and makes decisions that stay
// consistent with what the others are likely to decide from the same facts.
//
// Per turn (strategy section 20):
//   1. read the 7x7 window into the world model (kelp, portals, pearls, timers)
//   2. read sonar: teammate status, pearls, portals, kelp, enemies, dangers
//   3. infer map symmetry and mirror everything learned onto the other half
//   4. estimate opponent density, pick phase / mode / leaders
//   5. choose one action: survival split, attack, feed, split, or best move
//   6. send four sonars (N, E, S, W) with the most useful facts

#include "helper.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace unswbc;

// ---------------------------------------------------------------------------
// Tunables (override with -D for local experiments)
// ---------------------------------------------------------------------------
#ifndef EARLY_SPLIT_ROUNDS
#define EARLY_SPLIT_ROUNDS 150 // "X": rounds of aggressive splitting (section 2)
#endif
#ifndef EARLY_SPLIT_MIN_LEN
#define EARLY_SPLIT_MIN_LEN 4 // split as soon as length reaches this (section 2)
#endif
#ifndef LEADER_COUNT
#define LEADER_COUNT 3 // leader dragons in the growth phase (section 11)
#endif
#ifndef PRESERVE_SPLIT_LEN
#define PRESERVE_SPLIT_LEN 8 // non-leaders split at this length to keep numbers
#endif
#ifndef FEED_WINDOW
#define FEED_WINDOW 50 // last rounds in which farming may sacrifice feeders
#endif
#ifndef SPLIT_DENSITY
#define SPLIT_DENSITY 32 // at most one dragon per this many tiles (small maps get crowded)
#endif
#ifndef TRANS_SHIFT
#define TRANS_SHIFT 100 // leader phase starts this much earlier than the section-12 table (tuned)
#endif
#ifndef THREAT_PEN
#define THREAT_PEN 9000 // non-leader stepping next to an enemy head
#endif
#ifndef PEARL_BEHIND_GAIN
#define PEARL_BEHIND_GAIN 4 // tail must be this much closer to use a pearl split
#endif
#ifndef PEARL_BEHIND_MAX
#define PEARL_BEHIND_MAX 10 // ...and the pearl at most this far from the tail
#endif

namespace {

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
constexpr int INF = 1 << 29;
constexpr int DX[4] = {0, 1, 0, -1};
constexpr int DY[4] = {-1, 0, 1, 0};
constexpr char DCH[4] = {'N', 'E', 'S', 'W'};

int W = 0, H = 0, N = 0;
int me = 0;
Team::Value myTeam = Team::A;
int R = 0;       // current round
int L = 3;       // my length
int units = 1;   // living dragons on my team
int unitLimit = 64;

int dir_index(Direction d)
{
    switch (d.value)
    {
    case Direction::NORTH: return 0;
    case Direction::EAST: return 1;
    case Direction::SOUTH: return 2;
    default: return 3;
    }
}
Direction dir_of(int i) { return Direction(DCH[i]); }

int cell_of(int x, int y) { return y * W + x; }
int cell_of(Position p) { return cell_of(p.x, p.y); }

int nb(int c, int d)
{
    int x = c % W + DX[d], y = c / W + DY[d];
    if (x < 0) x += W;
    else if (x >= W) x -= W;
    if (y < 0) y += H;
    else if (y >= H) y -= H;
    return y * W + x;
}

int wrap_dist(int a, int b, int n)
{
    int const d = std::abs(a - b);
    return std::min(d, n - d);
}
int manhattan(int a, int b) { return wrap_dist(a % W, b % W, W) + wrap_dist(a / W, b / W, H); }
int cheb(int a, int b) { return std::max(wrap_dist(a % W, b % W, W), wrap_dist(a / W, b / W, H)); }

std::mt19937 rng;

// ---------------------------------------------------------------------------
// World model: edges and portals
// ---------------------------------------------------------------------------
// Every edge is stored once. code = cell*2 + o, where o = 0 is the horizontal
// edge on the NORTH side of the cell and o = 1 the vertical edge on its WEST
// side. The south/east edges of a cell are the north/west edges of its
// southern/eastern neighbour (the map wraps).
enum : std::uint8_t { E_UNK = 0, E_OPEN = 1, E_KELP = 2, E_PORTAL = 3 };

std::vector<std::uint8_t> edgeType; // best knowledge (observed, told or mirrored)
std::vector<std::uint8_t> obsEdge;  // observed by us or a teammate (symmetry test)
std::vector<int> edgePid;           // portal id, -1 unknown
std::vector<int> edgePartner;       // partner edge code, -1 unknown
std::unordered_map<int, std::vector<int>> portalById;
std::vector<int> kelpList;                      // known kelp codes (gossip)
std::vector<std::pair<int, int>> portalPairs;   // known pairs (gossip)

int edge_code(int c, int d)
{
    switch (d)
    {
    case 0: return c * 2;
    case 1: return nb(c, 1) * 2 + 1;
    case 2: return nb(c, 2) * 2;
    default: return c * 2 + 1;
    }
}

// Where a head standing on c ends up after stepping in direction d.
//   >= 0 : the destination cell
//     -1 : kelp (death)
//     -2 : edge never seen
//     -3 : portal whose partner we have not located
// A portal keeps the heading: the head leaves the partner edge on its d side
// (verified in the engine with a test map).
int cross(int c, int d)
{
    int const code = edge_code(c, d);
    switch (edgeType[code])
    {
    case E_OPEN: return nb(c, d);
    case E_KELP: return -1;
    case E_UNK: return -2;
    default: break;
    }
    int const p = edgePartner[code];
    if (p < 0)
        return -3;
    int const pc = p >> 1;
    if ((p & 1) == 0) // partner is the north edge of pc
        return d == 2 ? pc : nb(pc, 0);
    return d == 1 ? pc : nb(pc, 3); // partner is the west edge of pc
}

// ---------------------------------------------------------------------------
// Symmetry (section 10)
// ---------------------------------------------------------------------------
// 0: mirror x -> W-1-x   1: mirror y -> H-1-y   2: rotate 180 degrees.
// These formulas were checked against every bundled map file.
int symMask = 7;
int symKnown = -1;
int symConfirm[3] = {0, 0, 0};
bool symFailed = false;

int mirror_cell(int k, int c)
{
    int x = c % W, y = c / W;
    if (k != 1) x = W - 1 - x;
    if (k != 0) y = H - 1 - y;
    return cell_of(x, y);
}

int mirror_edge(int k, int code)
{
    int const m = mirror_cell(k, code >> 1);
    if ((code & 1) == 0)
        return (k != 0 ? nb(m, 2) : m) * 2; // north edge maps to the mirrored south edge when y flips
    return (k != 1 ? nb(m, 1) : m) * 2 + 1; // west edge maps to the mirrored east edge when x flips
}

// ---------------------------------------------------------------------------
// World model: tiles and pearls (sections 6, 18)
// ---------------------------------------------------------------------------
std::vector<int> seenRound;  // last round the tile was in view, -1 never
std::vector<int> obsNext;    // observed next spawn round, -1 unknown, -2 never spawns
std::vector<int> nextSpawn;  // best estimate incl. mirrored/predicted, same encoding
std::vector<char> nextEst;   // nextSpawn is an estimate from observed gaps
std::vector<int> pearlState; // 0 none, 1 present (seen), 2 predicted
std::vector<int> pearlRound; // round the pearlState info refers to
std::vector<int> gapLo, gapHi; // observed respawn gaps (0 = unknown)

// ---------------------------------------------------------------------------
// This turn's view of dragons
// ---------------------------------------------------------------------------
std::vector<char> vis;       // in view this turn
std::vector<int> occId;      // dragon id on the tile (in view), -1 none
std::vector<char> occMine;   // that dragon is on my team
std::vector<char> occHead;
std::vector<int> occDir;
std::vector<int> otherRel; // 0 free, else the earliest step at which that segment is gone
std::vector<char> contested; // free now, but another head could step here before our next move

struct HeadInfo
{
    int cell, id, len, dir;
    bool mine;
};
std::vector<HeadInfo> heads; // other dragons' heads in view

std::vector<int> body; // my body, head first (may be shorter than L if unseen)
std::vector<int> prevBody;
std::unordered_map<int, int> prevTeamHeads; // teammate id -> head cell last turn

// ---------------------------------------------------------------------------
// Fleet knowledge (sonar)
// ---------------------------------------------------------------------------
struct Status
{
    int len, cell, round;
    bool leader;
};
std::unordered_map<int, Status> fleet;

struct EnemyRep
{
    int cell, round, id, len;
};
std::vector<EnemyRep> enemyReps;

struct Danger
{
    int cell, round, cause; // cause 1 = enemy involved, 2 = hazard / trap
};
std::vector<Danger> dangers;

struct Claim
{
    int id, eta, round;
};
std::unordered_map<int, Claim> claims; // pearl cell -> claim

// ---------------------------------------------------------------------------
// Strategy state
// ---------------------------------------------------------------------------
enum Role { ROLE_COLLECTOR, ROLE_EXPLORER, ROLE_PORTAL, ROLE_COMBAT };
enum Mode { MODE_NORMAL, MODE_FARM, MODE_PRESERVE, MODE_COMBAT };

Role role = ROLE_COLLECTOR;
Mode mode = MODE_NORMAL;
bool isLeader = false;
bool transitioned = false;
int transitionRound = 300;
int localThreat = 0;
int distinctEnemies = 0;
int echoEnemy = 0;
int exploreGoal = -1, exploreSetRound = -1000;
int claimTarget = -1, claimEta = 0;
std::string note;

// ---------------------------------------------------------------------------
// Sonar messages
// ---------------------------------------------------------------------------
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
    std::uint64_t cs = static_cast<std::uint64_t>(type) ^ 0x5;
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
std::uint64_t msg_enemy(int cell, int id, int len)
{
    BitW b;
    b.put(cell, 12);
    b.put(R, 9);
    b.put(id, 14);
    b.put(std::min(len, 255), 8);
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
std::uint64_t msg_claim(int cell, int eta)
{
    BitW b;
    b.put(me, 14);
    b.put(cell, 12);
    b.put(std::min(eta, 255), 8);
    b.put(R, 9);
    return pack(T_CLAIM, b.v);
}

// ---------------------------------------------------------------------------
// Learning
// ---------------------------------------------------------------------------
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
        if (!wasPresent)
        {
            queue_pearl(1, c, R, 60);
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

// ---------------------------------------------------------------------------
// Reading the turn
// ---------------------------------------------------------------------------
void read_vision(Controller& ct)
{
    std::fill(vis.begin(), vis.end(), 0);
    std::fill(occId.begin(), occId.end(), -1);
    heads.clear();
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
    for (auto const& h : heads)
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
        if (m != partCount[h.id] || cheb(chain.back(), myHead) > 2)
            continue; // tail may continue out of view
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

void add_enemy(int cell, int round, int id, int len)
{
    for (auto& e : enemyReps)
        if (e.id == id && e.round <= round)
        {
            e = {cell, round, id, len};
            return;
        }
    enemyReps.push_back({cell, round, id, len});
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
        auto it = fleet.find(id);
        if (it != fleet.end() && it->second.round >= round)
            return false;
        fleet[id] = {len, cell, round, leader == 1};
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
        if (cell >= N || round > R)
            return false;
        add_enemy(cell, round, id, len);
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
            queue_fact(m, 15, 2); // gossip: pass new facts along the chain
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
        add_enemy(h.cell, R, h.id, h.len);
        if (shared < 2)
        {
            queue_fact(msg_enemy(h.cell, h.id, h.len), 70, 1);
            shared++;
        }
    }
}

// ---------------------------------------------------------------------------
// Occupancy, flood fill and path finding
// ---------------------------------------------------------------------------
// Bodies are time-aware: the segment i places from the tail (tail = 0) is
// still there when the head tries to enter at step t < i + 2, because
// collision is checked before the tail moves. Other dragons get the same rule
// (plus one step of margin in case they eat) when their whole body is in view,
// and are solid otherwise.
std::vector<int> ownStamp, ownRel, wallStamp;
int stamp = 1, wstamp = 1;

void set_body(std::vector<int> const& b, int len)
{
    stamp++;
    for (int i = 0; i < static_cast<int>(b.size()); i++)
    {
        ownStamp[b[i]] = stamp;
        ownRel[b[i]] = (len - 1 - i) + 2;
    }
}

bool passable(int c, int t)
{
    if ((otherRel[c] && t < otherRel[c]) || wallStamp[c] == wstamp)
        return false;
    return ownStamp[c] != stamp || t >= ownRel[c];
}

struct FloodRes
{
    int area = 0;
    bool tail = false;
    bool frontier = false;
};

std::vector<int> bfsQ, bfsT, visStamp;
int vstamp = 1;
bool ignoreContested = false;

// Cells reachable from `start` using only edges we know are crossable.
FloodRes flood(int start, int tailCell, int cap)
{
    FloodRes r;
    vstamp++;
    bfsQ.clear();
    bfsT.clear();
    bfsQ.push_back(start);
    bfsT.push_back(0);
    visStamp[start] = vstamp;
    for (std::size_t i = 0; i < bfsQ.size() && r.area < cap; i++)
    {
        int const c = bfsQ[i], t = bfsT[i];
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n == -2)
            {
                r.frontier = true; // unexplored water beyond: count as open
                continue;
            }
            if (n < 0 || visStamp[n] == vstamp)
                continue;
            if (!passable(n, t + 1) || (t == 0 && contested[n] && !ignoreContested))
                continue;
            visStamp[n] = vstamp;
            r.area++;
            if (n == tailCell)
                r.tail = true;
            bfsQ.push_back(n);
            bfsT.push_back(t + 1);
        }
    }
    return r;
}

// Optimistic shortest paths for planning: unknown edges count as open, kelp and
// unlocated portals as closed, known portals as shortcuts.
void plan_bfs(int start, std::vector<int>& dist)
{
    std::fill(dist.begin(), dist.end(), INF);
    bfsQ.clear();
    bfsQ.push_back(start);
    dist[start] = 0;
    for (std::size_t i = 0; i < bfsQ.size(); i++)
    {
        int const c = bfsQ[i];
        int const t = dist[c] + 1;
        for (int d = 0; d < 4; d++)
        {
            int n = cross(c, d);
            if (n == -2)
                n = nb(c, d);
            if (n < 0 || dist[n] != INF || !passable(n, t))
                continue;
            dist[n] = t;
            bfsQ.push_back(n);
        }
    }
}

// Distance to a cell, or to its nearest neighbour if the cell itself is
// occupied (used for enemy heads and teammates).
int dist_to(std::vector<int> const& dist, int target)
{
    if (dist[target] < INF)
        return dist[target];
    int best = INF;
    for (int d = 0; d < 4; d++)
        best = std::min(best, dist[nb(target, d)] + (dist[nb(target, d)] < INF ? 1 : 0));
    return best;
}

bool pearl_at(int c)
{
    return pearlState[c] == 1 && (vis[c] || R - pearlRound[c] <= 3);
}

// ---------------------------------------------------------------------------
// Fleet state: danger, transition, leaders, mode (sections 11-16)
// ---------------------------------------------------------------------------
Role role_for(int id)
{
    std::uint32_t const h = static_cast<std::uint32_t>(id) * 2654435761u;
    int const r = static_cast<int>((h >> 16) % 10);
    if (r < 3) return ROLE_COMBAT;
    if (r < 4) return ROLE_PORTAL;
    if (r < 6) return ROLE_EXPLORER;
    return ROLE_COLLECTOR;
}

void compute_fleet_state()
{
    int const head = body[0];
    std::unordered_set<int> recent;
    for (auto const& e : enemyReps)
        if (R - e.round <= 60)
            recent.insert(e.id);
    distinctEnemies = static_cast<int>(recent.size());

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

    // Too many leaders (they could not hear each other at election time):
    // step down if enough longer leaders are known.
    if (isLeader)
    {
        int longer = 0;
        for (auto const& [id, s] : fleet)
            if (id != me && s.leader && R - s.round <= 30 && (s.len > L || (s.len == L && id < me)))
                longer++;
        if (longer >= LEADER_COUNT)
            isLeader = false;
    }

    // Leader election (section 11): the longest dragons we know of.
    if (transitioned && !isLeader && L >= 3)
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

// ---------------------------------------------------------------------------
// Target selection (sections 6-10, 18)
// ---------------------------------------------------------------------------
struct PearlCand
{
    int cell, appear;
    double conf;
};

std::vector<int> distHead, distTmp;

int est_enemy_units()
{
    return std::max(distinctEnemies, 1);
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
    std::vector<Pair> pairs;
    for (int pi = 0; pi < static_cast<int>(pearls.size()); pi++)
    {
        auto const& p = pearls[pi];
        auto const cl = claims.find(p.cell);
        for (int ai = 0; ai < static_cast<int>(agents.size()); ai++)
        {
            auto const& a = agents[ai];
            int eta;
            if (a.id == me)
                eta = distHead[p.cell];
            else if (cl != claims.end() && cl->second.id == a.id)
                eta = std::max(0, cl->second.eta - (R - cl->second.round));
            else
                eta = manhattan(a.cell, p.cell);
            if (eta >= INF)
                continue;
            double cost = std::max(eta, p.appear - R) + 1.0;
            cost /= p.conf;
            if (a.leader)
                cost *= 0.6; // feed the leaders first (section 11)
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

std::vector<PearlCand> gather_pearls()
{
    std::vector<PearlCand> out;
    for (int c = 0; c < N; c++)
    {
        if (vis[c] && occId[c] >= 0)
            continue;
        double conf = 0;
        int appear = R;
        if (pearlState[c] == 1)
            conf = vis[c] ? 1.0 : std::max(0.2, 0.9 - 0.02 * (R - pearlRound[c]));
        else if (pearlState[c] == 2)
            conf = std::max(0.15, 0.55 - 0.01 * (R - pearlRound[c]));
        else if (nextSpawn[c] >= R && nextSpawn[c] - R <= 25)
        {
            conf = nextEst[c] ? 0.35 : 0.6;
            appear = nextSpawn[c];
        }
        if (conf <= 0 || distHead[c] >= INF)
            continue;
        int const eta = distHead[c];
        if (eta > 60)
            continue;
        // Contested: an enemy head is clearly closer.
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, c) + 1 < eta)
                conf *= 0.35;
        // Dangerous area reported by the fleet.
        for (auto const& dg : dangers)
            if (dg.cause == 1 && R - dg.round <= 20 && manhattan(dg.cell, c) <= 3)
                conf *= isLeader ? 0.3 : 0.7;
        out.push_back({c, appear, conf});
    }
    if (out.size() > 60)
    {
        std::nth_element(out.begin(), out.begin() + 60, out.end(), [](PearlCand const& a, PearlCand const& b) {
            return std::max(distHead[a.cell], a.appear - R) / a.conf < std::max(distHead[b.cell], b.appear - R) / b.conf;
        });
        out.resize(60);
    }
    return out;
}

// Section 9: spread out. Unseen cells first, then stale ones, away from teammates.
int choose_explore_goal()
{
    bool const valid = exploreGoal >= 0 && R - exploreSetRound < 40 && distHead[exploreGoal] < INF &&
                       exploreGoal != body[0] && (seenRound[exploreGoal] < exploreSetRound);
    if (valid)
        return exploreGoal;
    std::vector<int> teamCells;
    for (auto const& h : heads)
        if (h.mine)
            teamCells.push_back(h.cell);
    for (auto const& [id, s] : fleet)
        if (R - s.round <= 10)
            teamCells.push_back(s.cell);

    int best = -1;
    double bestScore = 1e18;
    for (int pass = 0; pass < 2 && best < 0; pass++)
        for (int i = 0; i < 48; i++)
        {
            int const c = static_cast<int>(rng() % static_cast<unsigned>(N));
            bool const unseen = seenRound[c] < 0;
            if (pass == 0 && !unseen)
                continue;
            if (pass == 1 && R - seenRound[c] < 25)
                continue;
            int const dh = distHead[c];
            if (dh >= INF || dh < 4)
                continue;
            int crowd = 0;
            for (int tc : teamCells)
                if (manhattan(tc, c) < 8)
                    crowd++;
            double score = dh + 25.0 * crowd;
            if (role == ROLE_EXPLORER)
                score -= 0.3 * dh; // explorers push further out
            for (auto const& dg : dangers)
                if (dg.cause == 1 && R - dg.round <= 30 && manhattan(dg.cell, c) <= 4)
                    score += isLeader ? 200 : 40;
            if (score < bestScore)
            {
                bestScore = score;
                best = c;
            }
        }
    exploreGoal = best;
    exploreSetRound = R;
    return best;
}

// Section 5: nearest portal edge whose far side is still unknown.
int choose_portal_goal(int& portalDir)
{
    int best = -1, bestD = 30;
    portalDir = -1;
    for (int c = 0; c < N; c++)
    {
        if (distHead[c] >= bestD)
            continue;
        for (int d = 0; d < 4; d++)
            if (cross(c, d) == -3)
            {
                best = c;
                bestD = distHead[c];
                portalDir = d;
                break;
            }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Decision
// ---------------------------------------------------------------------------
struct Cand
{
    int d = 0, dest = -1;
    bool legal = false, blind = false, attack = false, eat = false;
    bool ok1 = false, ok2 = false;
    int area = 0;
    double score = -1e18;
    std::vector<int> nb1;
};

bool is_adjacent_enemy_head(int cell)
{
    for (auto const& h : heads)
        if (!h.mine && manhattan(h.cell, cell) <= 1)
            return true;
    return false;
}

// Can a child made of the rear `childLen` segments get out? Its head is my
// tail and it moves later this round; my front part stays where it is.
bool child_can_escape(int childLen)
{
    if (static_cast<int>(body.size()) < L)
        return true; // tail is out of view, well away from whatever traps the head
    std::vector<int> child(body.rbegin(), body.rbegin() + childLen);
    wstamp++;
    for (int i = 0; i < L - childLen; i++)
        wallStamp[body[i]] = wstamp;
    set_body(child, childLen);
    FloodRes const f = flood(child[0], child.back(), childLen + 8);
    wstamp++;
    set_body(body, L);
    return f.tail || f.area >= std::min(childLen + 1, 6);
}

// Numbers are the goal, but a crowded map kills small dragons for nothing.
bool room_for_more()
{
    return units < std::min(unitLimit, std::max(2, N / SPLIT_DENSITY));
}

void act_move(Controller& ct, int d, char const* why)
{
    ct.make_move(dir_of(d));
    note = why;
    prevBody = body;
}

void act_split(Controller& ct, int child, char const* why)
{
    ct.do_split(child);
    note = why;
    prevBody = body;
}

void decide(Controller& ct)
{
    int const head = body[0];
    int const facing = dir_index(ct.get_dir());
    claimTarget = -1;

    set_body(body, L);
    plan_bfs(head, distHead);

    // ---- Candidate moves ------------------------------------------------
    std::array<Cand, 4> cands;
    int const cap = std::min(N, 3 * L + 24);
    bool anyLegal = false, anySafe = false, attackAvail = false;
    int attackDir = -1, attackLen = 0;
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
        if (!passable(dest, 1) || (vis[dest] && occId[dest] == me))
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
        cd.area = f1.area + (f1.frontier ? nl : 0);
        // A snake cannot turn round in a corridor, so raw area is only trusted
        // when there is room to circle; reaching our own tail is always an exit.
        cd.ok1 = f1.tail || cd.area >= 2 * nl + 4;

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
            if (f2.tail || f2.area + (f2.frontier ? nl2 : 0) >= 2 * nl2 + 2)
                cd.ok2 = true;
        }
        if (cd.ok2)
            anySafe = true;
        cd.nb1 = std::move(nb1);
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
        }
        ct.output_log(dbg);
    }
#endif

    bool const nextToEnemy = is_adjacent_enemy_head(head);
    bool const wantAttack = attackAvail && !isLeader &&
                            (role == ROLE_COMBAT || mode == MODE_COMBAT) &&
                            (L <= attackLen + 1 || units > est_enemy_units() + 1 || L <= 4);

    // ---- 1. Death is certain: survival split (sections 3, 12) --------------
    if (!anyLegal)
    {
        if (attackAvail && !isLeader && L < 6)
            return act_move(ct, attackDir, "last-attack");
        if (L >= 4 && ct.can_split(L - 2))
        {
            queue_fact(msg_danger(head, nextToEnemy ? 1 : 2), 80, 3);
            return act_split(ct, L - 2, "save-split");
        }
        if (attackAvail)
            return act_move(ct, attackDir, "last-attack");
        return act_move(ct, facing, "doomed");
    }
    if (!anySafe && L >= 4 && ct.can_split(L - 2) && child_can_escape(L - 2))
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

    // ---- 2. Head-on attack (sections 15, 16) ------------------------------
    if (wantAttack)
    {
        queue_fact(msg_danger(head, 1), 70, 2);
        return act_move(ct, attackDir, "attack");
    }

    // ---- Target ----------------------------------------------------------
    std::vector<PearlCand> const pearls = gather_pearls();
    int target = -1;
    int portalDir = -1;
    char const* why = "explore";
    int const pi = choose_pearl(pearls);
    if (pi >= 0)
    {
        target = pearls[pi].cell;
        claimTarget = target;
        claimEta = distHead[target];
        why = "pearl";
    }

    // Combat dragons hunt enemies, preferring the ones near our leaders. When
    // we clearly outnumber them, small dragons also hunt the long enemies.
    bool const outnumber = units > distinctEnemies + 3 && distinctEnemies > 0;
    bool const hunter = !isLeader && ((mode == MODE_COMBAT && role == ROLE_COMBAT) || (outnumber && L <= 5));
    if (hunter && (target < 0 || distHead[target] > 4))
    {
        int best = -1, bestD = 16;
        for (auto const& e : enemyReps)
        {
            if (R - e.round > 3)
                continue;
            if (!(mode == MODE_COMBAT && role == ROLE_COMBAT) && e.len < 6)
                continue;
            int dd = dist_to(distHead, e.cell);
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
            why = "hunt";
        }
    }

    // Farming endgame (section 13): feeders walk to the nearest leader.
    int feedLeader = -1;
    if (mode == MODE_FARM && !isLeader && R >= 500 - FEED_WINDOW)
    {
        int bestD = INF;
        for (auto const& [id, s] : fleet)
            if (s.leader && R - s.round <= 5 && manhattan(s.cell, head) < bestD)
            {
                bestD = manhattan(s.cell, head);
                feedLeader = s.cell;
            }
        for (auto const& h : heads)
            if (h.mine)
            {
                auto it = fleet.find(h.id);
                if (it != fleet.end() && it->second.leader)
                    feedLeader = h.cell;
            }
        if (feedLeader >= 0 && (target < 0 || distHead[target] > 6))
        {
            target = feedLeader;
            why = "feed";
        }
    }

    if (target < 0 && role == ROLE_PORTAL && !isLeader && L <= 12)
    {
        int const pg = choose_portal_goal(portalDir);
        if (pg >= 0)
        {
            target = pg;
            why = "portal";
        }
    }
    if (target < 0)
        target = choose_explore_goal();

    // ---- 3. Feeding sacrifice (section 13) ----------------------------------
    if (mode == MODE_FARM && !isLeader && feedLeader >= 0 && R >= 500 - FEED_WINDOW && L >= 3 &&
        units > LEADER_COUNT + 1 && localThreat == 0)
    {
        for (auto const& [lid, ls] : fleet)
        {
            if (lid == me || !ls.leader || ls.len < L || R - ls.round > 2 || cheb(ls.cell, head) > 4)
                continue;
            // Squeeze into the neck: the body turns into pearls next to the leader.
            if (body.size() >= 2)
                for (int d = 0; d < 4; d++)
                    if (nb(head, d) == body[1])
                        return act_move(ct, d, "feed-leader");
        }
    }

    // ---- 4. Splits (sections 2, 8, 14) ---------------------------------------
    bool const calm = !nextToEnemy;
    if (calm && L >= EARLY_SPLIT_MIN_LEN && R < EARLY_SPLIT_ROUNDS && room_for_more() && ct.can_split(L / 2) &&
        child_can_escape(L / 2))
        return act_split(ct, L / 2, "grow-fleet");

    if (calm && !isLeader && L >= 4 && pi >= 0 && room_for_more() && ct.can_split(2) && static_cast<int>(body.size()) >= L)
    {
        int const tail = body.back();
        std::vector<int> child{tail, body[L - 2]};
        wstamp++;
        for (int i = 0; i < L - 2; i++)
            wallStamp[body[i]] = wstamp;
        set_body(child, 2);
        plan_bfs(tail, distTmp);
        wstamp++;
        set_body(body, L);
        int const pc = pearls[pi].cell;
        int const dt = distTmp[pc], dh = distHead[pc];
        if (dt <= PEARL_BEHIND_MAX && dt + PEARL_BEHIND_GAIN <= dh && child_can_escape(2))
            return act_split(ct, 2, "pearl-split");
    }

    if (calm && transitioned && !isLeader && mode == MODE_PRESERVE && L >= PRESERVE_SPLIT_LEN && room_for_more() &&
        ct.can_split(L / 2) && child_can_escape(L / 2))
        return act_split(ct, L / 2, "keep-fleet");

    // ---- 5. Best move ------------------------------------------------------
    std::vector<int> teamCells;
    for (int c = 0; c < N; c++)
        if (vis[c] && occId[c] >= 0 && occId[c] != me && occMine[c])
            teamCells.push_back(c);

    int bestD = -1;
    double bestScore = -1e18;
    for (auto& cd : cands)
    {
        if (!cd.legal)
            continue;
        double s = 0;
        if (cd.blind)
        {
            // Unknown far side: only for small, expendable dragons (section 5).
            bool const wanted = role == ROLE_PORTAL || role == ROLE_EXPLORER;
            if (isLeader)
                s = anySafe ? -1e7 : -5e5; // better than a certain trap
            else if (portalDir == cd.d && target == head)
                s = 5000;
            else
                s = wanted && L <= 6 ? -500 : -4000;
            if (!anySafe)
                s += 3000;
        }
        else
        {
            // Risk is judged against its reward. A trap with nothing gained is
            // an avoidable death; a trap that pays a pearl is acceptable,
            // cheapest when a survival split can save all but 2 segments.
            int const lenAfter = L + (cd.eat ? 1 : 0);
            bool const canSave = lenAfter >= 4 && units < unitLimit;
            if (!cd.ok2)
                s -= cd.eat ? (canSave ? 2500.0 : 2500.0 + 900.0 * lenAfter) : 1e6;
            else if (!cd.ok1)
                s -= cd.eat ? 1500.0 : 2e4;
            s += std::min(cd.area, cap) * 15.0;
            if (cd.eat)
                s += isLeader ? 6000 : 3000;
            if (target >= 0)
            {
                set_body(cd.nb1, L + (cd.eat ? 1 : 0));
                plan_bfs(cd.dest, distTmp);
                int dt = dist_to(distTmp, target);
                if (portalDir >= 0 && target != head && cd.dest == target)
                    dt = 0;
                s -= 150.0 * std::min(dt, 200);
            }
            for (auto const& h : heads)
            {
                if (manhattan(h.cell, cd.dest) != 1)
                    continue;
                if (!h.mine)
                    s -= isLeader ? (L >= h.len + 2 ? 200000 : 500000)
                                  : (mode == MODE_COMBAT && role == ROLE_COMBAT ? 500 : THREAT_PEN);
                else
                    s -= 4000;
            }
            for (auto const& dg : dangers)
                if (R - dg.round <= 30 && manhattan(dg.cell, cd.dest) <= 3)
                    s -= isLeader ? 3000 : 300;
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
                    if (earea <= h.len + 1)
                        s += 3500;
                    else
                        s += 1500.0 * std::max(0.0, 1.0 - static_cast<double>(earea) / ecap);
                }
            if (!isLeader && std::string(why) != "feed")
                for (int tc : teamCells)
                    s -= 150.0 / (1 + manhattan(tc, cd.dest));
            if (cd.d == facing)
                s += 3;
        }
        cd.score = s;
        if (s > bestScore)
        {
            bestScore = s;
            bestD = cd.d;
        }
    }
    set_body(body, L);
    if (bestD < 0)
        bestD = facing;
    act_move(ct, bestD, why);
}

// ---------------------------------------------------------------------------
// Sonar output (section 4): four messages a turn, N/E/S/W.
// ---------------------------------------------------------------------------
void send_sonars(Controller& ct)
{
    std::vector<std::uint64_t> out;
    out.push_back(msg_status());
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
    for (int i = 0; i < static_cast<int>(out.size()) && i < 4; i++)
        ct.send_sonar(dir_of((i + offset) & 3), out[i]);
}

// ---------------------------------------------------------------------------
// Fallback: any move that is not immediately fatal.
// ---------------------------------------------------------------------------
void fallback_move(Controller& ct)
{
    Position const here = ct.get_position();
    Tile const* t = ct.get_tile(here);
    for (int d = 0; d < 4; d++)
    {
        if (t && !t->get_edge(dir_of(d)).is_passable())
            continue;
        Tile const* ahead = ct.get_tile(here.add_dir(dir_of(d)));
        if (ahead && ahead->get_dragon())
            continue;
        ct.make_move(dir_of(d));
        return;
    }
    ct.make_move(ct.get_dir());
}

void init_world(Controller& ct, Game& game)
{
    auto const [w, h] = game.get_map_size();
    W = w;
    H = h;
    N = W * H;
    me = ct.get_id();
    myTeam = ct.get_team().value;
    unitLimit = game.get_unit_limit();
    rng.seed(static_cast<std::uint32_t>(me) * 7919u + 17u);
    role = role_for(me);

    edgeType.assign(2 * N, E_UNK);
    obsEdge.assign(2 * N, E_UNK);
    edgePid.assign(2 * N, -1);
    edgePartner.assign(2 * N, -1);
    seenRound.assign(N, -1);
    obsNext.assign(N, -1);
    nextSpawn.assign(N, -1);
    nextEst.assign(N, 0);
    pearlState.assign(N, 0);
    pearlRound.assign(N, -1);
    gapLo.assign(N, 0);
    gapHi.assign(N, 0);
    vis.assign(N, 0);
    occId.assign(N, -1);
    occMine.assign(N, 0);
    occHead.assign(N, 0);
    occDir.assign(N, 0);
    otherRel.assign(N, 0);
    contested.assign(N, 0);
    ownStamp.assign(N, 0);
    ownRel.assign(N, 0);
    wallStamp.assign(N, 0);
    visStamp.assign(N, 0);
    distHead.assign(N, INF);
    distTmp.assign(N, INF);
    bfsQ.reserve(N);
    bfsT.reserve(N);
}

} // namespace

int main()
{
    auto [ct, game] = unswbc::init();
    init_world(ct, game);

    while (unswbc::update(ct, game))
    {
        bool acted = false;
        try
        {
            R = game.get_round_num();
            L = ct.get_length();
            units = ct.get_unit_count();

            read_vision(ct);
            update_body(ct);
            read_sonar(ct);
            update_unobserved();
            resolve_symmetry();
            prune_knowledge();
            detect_teammate_deaths();
            record_enemies();
            fleet[me] = {L, body[0], R, isLeader};
            compute_fleet_state();

            decide(ct);
            acted = true;
            send_sonars(ct);

            static char const* const roleName[] = {"col", "exp", "prt", "cmb"};
            static char const* const modeName[] = {"nrm", "farm", "keep", "war"};
            std::string ind = std::string(isLeader ? "LEAD " : "") + roleName[role] + " " + modeName[mode] + " " + note +
                              " L" + std::to_string(L);
            if (symKnown >= 0)
                ind += " s" + std::to_string(symKnown);
            ct.set_indicator_string(ind);
        }
        catch (...)
        {
            if (!acted)
                fallback_move(ct);
        }
        unswbc::end_turn();
    }
}
