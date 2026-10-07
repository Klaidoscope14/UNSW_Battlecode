// L0 WORLD - what this dragon knows: edges, portals, symmetry, tiles and pearls, the dragons in
// view, fleet reports, enemy reports and lengths, dangers, pearl claims.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

// World model: edges and portals
// Every edge is stored once. code = cell*2 + o, where o = 0 is the horizontal
// edge on the NORTH side of the cell and o = 1 the vertical edge on its WEST
// side. The south/east edges of a cell are the north/west edges of its
// southern/eastern neighbour (the map wraps).
enum : std::uint8_t { E_UNK = 0, E_OPEN = 1, E_KELP = 2, E_PORTAL = 3 };

std::vector<std::uint8_t> edgeType; // best knowledge (observed, told or mirrored)
int edgeVersion = 0;                // bumped whenever edgeType / edgePartner change (topology rebuilds)
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

// GraphView (Architecture_v2 2.3): how a search treats an edge nobody has seen. Every search uses one of:
//   step_open(c, d)  unexplored edge = open water (optimistic): planning, Voronoi, enemy reach, survival, exits
//   cross(c, d)      unexplored edge = not crossable (< 0; -2 tells it apart): dives, pockets, sprints, strikes
//   edgeType == E_OPEN only (known open water, no portals): free loops, bunker boxes
// flood() is the one FRONTIER search: it does not enter unexplored edges but reports that the water may go on.
int step_open(int c, int d)
{
    int const n = cross(c, d);
    return n == -2 ? nb(c, d) : n;
}

// Symmetry (section 10)
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

// World model: tiles and pearls (sections 6, 18)
std::vector<int> seenRound;  // last round the tile was in view, -1 never
std::vector<int> obsNext;    // observed next spawn round, -1 unknown, -2 never spawns
std::vector<int> nextSpawn;  // best estimate incl. mirrored/predicted, same encoding
std::vector<char> nextEst;   // nextSpawn is an estimate from observed gaps
std::vector<int> pearlState; // 0 none, 1 present (seen), 2 predicted
std::vector<int> dropRound;  // round a death-drop pearl (non-spawn tile) was first seen
std::vector<int> pearlRound; // round the pearlState info refers to
struct DropEv
{
    int cell, round;
    bool nearOurQueen; // likely one of our own feeders
};
std::vector<DropEv> dropEv; // death drops seen or heard of (non-spawn tiles), newest last
std::vector<int> gapLo, gapHi; // observed respawn gaps (0 = unknown)

// This turn's view of dragons
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
    int estLen = 0;       // enemies: best length estimate for strike reach (len counts only what is in view)
    bool partial = false; // body runs out of view
};
std::vector<HeadInfo> heads; // other dragons' heads in view
std::vector<int> enemyTails; // visible enemy tails that could split: a child's head appears there this round

std::vector<int> body; // my body, head first (may be shorter than L if unseen)
std::vector<int> prevBody;
std::vector<int> lastDirs; // steps of last turn's move (empty after a split)
std::unordered_map<int, int> prevTeamHeads; // teammate id -> head cell last turn

// Fleet knowledge (sonar)
struct Status
{
    int len, cell, round;
    bool leader;
};
std::unordered_map<int, Status> fleet;

struct EnemyRep
{
    int cell, round, id, len;
    int estLen = 0; // queen threat only: length guess when the body was not seen whole
};
std::vector<EnemyRep> enemyReps;
// Q2c: the latest strike news per striker (perceive.hpp strike_news_check).
struct StrikeWatch
{
    int striker, cell, round;
    bool off;
};
std::vector<StrikeWatch> strikeWatch;
int strikeAnnR = -1000; // the last round this dragon said "she is within my reach"
struct LenMem
{
    int len, round;
    bool exact;
};
std::unordered_map<int, LenMem> enemyLenMem; // enemy id -> last observed length (exact = whole body seen)
extern bool amQueen;

// Length of an enemy for its strike reach: exact when the whole body is in
// view, otherwise the last exact sighting, and at least a guess when we never
// saw it whole (a long hunter shows only its head at the edge of our window).
int est_enemy_len(int id, int seen, bool partial)
{
    if (!ENEMY_LEN_MEMORY || !partial)
        return seen;
    auto const it = enemyLenMem.find(id);
    // D: for the queen a long enemy stays long - it only shrinks by splitting
    // or paying, and losing track of it cost us a queen.
    if (it != enemyLenMem.end() && R - it->second.round <= (amQueen ? QUEEN_LEN_MEM : LEN_MEM_ROUNDS))
        return std::max({seen, it->second.len, it->second.exact ? 0 : UNSEEN_LEN_GUESS});
    return std::max(seen, UNSEEN_LEN_GUESS);
}
void remember_enemy_len(int id, int len, bool exact)
{
    auto it = enemyLenMem.find(id);
    if (exact || it == enemyLenMem.end() || R - it->second.round > (amQueen ? QUEEN_LEN_MEM : LEN_MEM_ROUNDS))
        enemyLenMem[id] = {len, R, exact};
    else if (!it->second.exact || len > it->second.len)
        it->second = {std::max(it->second.len, len), R, it->second.exact && len <= it->second.len};
}

// 6d: role claims heard (giant / heir / leader): claimant id -> its length and the round of the claim. A dragon keeps
// a one-of-a-kind role only while fewer than the role's slots of fresh claims rank above it.
enum ClaimRole { CR_LEADER = 0, CR_GIANT = 1, CR_HEIR = 2 };
struct RoleClaim
{
    int len, round;
};
std::unordered_map<int, RoleClaim> roleClaims[3];
std::unordered_map<int, int> claimRelayed[3]; // claimant id -> round we last passed its claim on

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

} // namespace
