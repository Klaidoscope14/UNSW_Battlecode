#define USE_PORTALS 0
#include "helper.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <unordered_map>
#include <vector>

using namespace unswbc;

// ===========================================================================
// v5.0.0: stateful sea-dragon.
//
//   * Persistent world model, rebuilt across turns (the process survives turn
//     to turn, so globals persist). We remember static terrain (kelp) and each
//     tile's pearl countdown with the round we last saw it, so we can predict
//     spawns beyond the 7x7 window.
//   * Exact tracking of our own body as a deque, so we know our whole shape
//     even when it runs off the edge of vision.
//   * Occupancy is stamp-based, which lets us flood-fill against SIMULATED
//     bodies. Survival rests on the tail-reachability invariant (if the head
//     can still reach the tail through open water, we can chase it forever),
//     checked at DEPTH 2 so we reject a move that itself leads to a forced
//     trap the turn after -- the failure mode a 1-ply check is blind to.
//   * Openness (honest flood area, tail as a wall) is the primary objective,
//     so the dragon spreads out instead of coiling; growth is a small tiebreak
//     under a board-relative governor, aggressive on big maps, cautious on
//     small confined ones where a long body inevitably eats itself.
//   * Multi-dragon coordination: implicit ID-based roles, teammate dispersion,
//     and splitting to colonise (the lead dragon keeps growing for tiebreak 1).
//   * Enemy/head-to-head threat map: cells any other head could enter next turn
//     are penalised, since a head-on kills both dragons.
//   * Topology: openness is scored by free area AND mobility (escape routes at
//     depth 1 and 2), so a roomy-but-bottlenecked cell loses to a truly open
//     one, and a single-exit corridor with no safe loop is avoided. (Measured:
//     these together took the win rate 75% -> 84% -> 93% vs the sparring bots,
//     and eliminated kelp-corridor deaths.)
//
// Fog of war: only edges seen to be open are traversable, so flood-fill is a
// safe lower bound and never leaks through undiscovered kelp. Portals are
// modelled but treated as walls (safe). Sonar/sprinting: deferred.
// ===========================================================================

namespace {

// Direction indices 0=N,1=E,2=S,3=W, matching Tile's edge array order.
constexpr int DX[4] = {0, 1, 0, -1};
constexpr int DY[4] = {-1, 0, 1, 0};
constexpr int OPP[4] = {2, 3, 0, 1};

int dir_index(Direction d)
{
    switch (d.value)
    {
    case Direction::NORTH: return 0;
    case Direction::EAST:  return 1;
    case Direction::SOUTH: return 2;
    default:               return 3;
    }
}
Direction dir_from_index(int i)
{
    switch (i)
    {
    case 0:  return Direction(Direction::NORTH);
    case 1:  return Direction(Direction::EAST);
    case 2:  return Direction(Direction::SOUTH);
    default: return Direction(Direction::WEST);
    }
}

enum EdgeMem : std::uint8_t { E_UNKNOWN = 0, E_OPEN = 1, E_KELP = 2, E_PORTAL = 3 };

struct World
{
    int W = 0, H = 0, N = 0;

    std::vector<std::array<std::uint8_t, 4>> edge;
    std::vector<std::array<int, 4>> nbr;

    std::vector<int> pearlTime;
    std::vector<int> pearlRound;
    std::vector<std::uint8_t> pearlNow;

    // Occupancy, kept as generation stamps so several sets can coexist without
    // clearing arrays: a cell is "set" when its stamp equals the live gen.
    std::vector<int> otherStamp; int otherGen = 1; // other dragons' segments
    std::vector<int> bodyStamp;  int bodyGen = 1;  // our body except its tail
    std::vector<int> threatStamp; int threatGen = 1; // cells a non-self head can enter next

    // BFS scratch.
    std::vector<int> visit; int visitGen = 1;
    std::vector<int> dist;
    std::vector<int> first;
    std::vector<int> queue;

    void init(int w, int h)
    {
        W = w; H = h; N = w * h;
        edge.assign(N, {E_UNKNOWN, E_UNKNOWN, E_UNKNOWN, E_UNKNOWN});
        nbr.assign(N, {0, 0, 0, 0});
        pearlTime.assign(N, -2);
        pearlRound.assign(N, -1);
        pearlNow.assign(N, 0);
        otherStamp.assign(N, 0);
        bodyStamp.assign(N, 0);
        threatStamp.assign(N, 0);
        visit.assign(N, 0);
        dist.assign(N, 0);
        first.assign(N, -1);
        queue.reserve(N);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                int const c = y * W + x;
                for (int d = 0; d < 4; d++)
                {
                    int const nx = ((x + DX[d]) % W + W) % W;
                    int const ny = ((y + DY[d]) % H + H) % H;
                    nbr[c][d] = ny * W + nx;
                }
            }
    }

    int idx(int x, int y) const { return y * W + x; }
    int idx(Position p) const { return p.y * W + p.x; }

    // Only edges we have actually seen to be open are traversable. Unknown
    // edges are walls (fog of war): this makes flood-fill a safe LOWER bound on
    // free space, so we never flood through undiscovered kelp into phantom
    // cells. Immediate-move safety uses live vision, so this only costs caution.
    bool passable(int cell, int d) const { return edge[cell][d] == E_OPEN; }
    bool occupied(int c) const
    {
        return otherStamp[c] == otherGen || bodyStamp[c] == bodyGen;
    }
    // For enemy prediction we are pessimistic the other way: assume a head can
    // cross unless we KNOW the edge is a wall (kelp) or a portal (leads away).
    bool enemy_can_cross(int cell, int d) const
    {
        std::uint8_t const e = edge[cell][d];
        return e == E_OPEN || e == E_UNKNOWN;
    }
    bool threatened(int c) const { return threatStamp[c] == threatGen; }

    // --- portals ----------------------------------------------------------
    // Each physical edge has two (cell,dir) spellings; we canonicalise to the
    // WEST or NORTH owner so a portal pair keys consistently. partnerOf maps one
    // canonical portal edge to the other once both ends have been seen.
    std::unordered_map<int, int> partnerOf; // canon edge code -> partner canon code
    std::unordered_map<int, int> portalSeen; // portal id -> first canon code seen

    int canon_code(int cell, int d) const
    {
        switch (d)
        {
        case 0:  return cell * 4 + 0;            // NORTH: (cell, NORTH)
        case 1:  return nbr[cell][1] * 4 + 3;    // EAST : (east nbr, WEST)
        case 2:  return nbr[cell][2] * 4 + 0;    // SOUTH: (south nbr, NORTH)
        default: return cell * 4 + 3;            // WEST : (cell, WEST)
        }
    }

    void register_portal(int cell, int d, int id)
    {
        if (id < 0)
            return;
        int const c = canon_code(cell, d);
        auto const it = portalSeen.find(id);
        if (it == portalSeen.end())
            portalSeen[id] = c;
        else if (it->second != c)
        {
            partnerOf[c] = it->second;
            partnerOf[it->second] = c;
        }
    }

    // The cell a head reaches by crossing edge (cell,d): the wrapped neighbour
    // for open water; the partner's far cell for a KNOWN portal (both ends seen,
    // continuing in the same direction); -1 for kelp, unseen edges, or a portal
    // whose partner we have not found yet. This is the one place movement, flood
    // and lookahead consult, so portals are handled uniformly and safely.
    int cross(int cell, int d) const
    {
        std::uint8_t const e = edge[cell][d];
        if (e == E_OPEN)
            return nbr[cell][d];
#ifndef USE_PORTALS
#define USE_PORTALS 1
#endif
        if (USE_PORTALS && e == E_PORTAL)
        {
            auto const it = partnerOf.find(canon_code(cell, d));
            if (it == partnerOf.end())
                return -1; // partner unknown -> do not use (safe)
            int const pcell = it->second / 4;
            return (d == 1 || d == 2) ? pcell : nbr[pcell][d]; // exit, same facing
        }
        return -1; // kelp, unseen, or (portals disabled) a portal
    }
};

World world;
std::deque<int> body; // front = head cell
bool haveBody = false;

#ifdef SEAD_TELEMETRY
int g_chosenArea = 0; // reachable area of the move we committed to this turn
#endif

// --- multi-dragon coordination state ---------------------------------------
// Dragons share no memory and communicate only by sonar, so coordination is
// implicit: each dragon derives a distinct role from its own ID (IDs are handed
// out deterministically, team A even and team B odd on the symmetric maps), and
// dragons repel each other so the team spreads across the water instead of
// clumping and fighting over the same pearls (or colliding head-to-head).
int g_teamSize = 0;   // living team dragons on our first turn (initial fleet size)
int g_teamIndex = 0;  // our slot within the fleet
bool g_role = false;
int g_lastMove = -1;  // dir index of our last move (-1 = split/none), for portal-aware body tracking

void init_role(Controller& C)
{
    if (g_role)
        return;
    g_teamSize = std::max(1, C.get_unit_count());
    g_teamIndex = (C.get_id() / 2) % g_teamSize;
    g_role = true;
}

int wrap_dist(int a, int b, int size)
{
    int const d = std::abs(a - b);
    return std::min(d, size - d);
}
int cheby(int c1, int c2)
{
    return std::max(wrap_dist(c1 % world.W, c2 % world.W, world.W),
                    wrap_dist(c1 / world.W, c2 / world.W, world.H));
}

void record_edge(int cell, int d, EdgeType type)
{
    std::uint8_t v = E_OPEN;
    if (type == EdgeType::KELP)   v = E_KELP;
    else if (type == EdgeType::PORTAL) v = E_PORTAL;
    world.edge[cell][d] = v;
    world.edge[world.nbr[cell][d]][OPP[d]] = v;
}

void absorb_vision(Controller& C, int round)
{
    for (Tile const& t : C.get_tiles())
    {
        int const c = world.idx(t.get_position());
        world.pearlTime[c] = t.get_pearl_time();
        world.pearlRound[c] = round;
        world.pearlNow[c] = t.has_pearl() ? 1 : 0;
        for (int d = 0; d < 4; d++)
        {
            Edge const& e = t.get_edge(dir_from_index(d));
            record_edge(c, d, e.get_edge_type());
            if (e.get_edge_type() == EdgeType::PORTAL)
                world.register_portal(c, d, e.get_portal_id());
        }
    }
}

void rebuild_body(Controller& C)
{
    body.clear();
    int const id = C.get_id();
    int const headCell = world.idx(C.get_position());
    body.push_back(headCell);
    int prev = -1, cur = headCell;
    int const want = C.get_length();
    while (static_cast<int>(body.size()) < want)
    {
        int nextCell = -1;
        for (int d = 0; d < 4; d++)
        {
            int const nc = world.nbr[cur][d];
            if (nc == prev)
                continue;
            Position const np(nc % world.W, nc / world.W);
            Tile const* t = C.get_tile(np);
            if (!t)
                continue;
            DragonPart const* p = t->get_dragon();
            if (p && p->get_id() == id && !p->is_head())
            {
                nextCell = nc;
                break;
            }
        }
        if (nextCell < 0)
            break;
        body.push_back(nextCell);
        prev = cur;
        cur = nextCell;
    }
    haveBody = true;
}

void update_body(Controller& C)
{
    int const headCell = world.idx(C.get_position());
    int const len = C.get_length();
    if (!haveBody)
    {
        rebuild_body(C);
        return;
    }
    if (body.front() != headCell)
    {
        // The head is where our last move -- possibly a portal jump -- predicts,
        // so push it even though it may be far from the old head (a portal makes
        // the body discontiguous in cell space, which occupancy handles fine).
        // Only a genuine surprise triggers a rebuild.
        bool expected = (g_lastMove >= 0 && world.cross(body.front(), g_lastMove) == headCell);
        if (!expected)
            for (int d = 0; d < 4; d++)
                if (world.nbr[body.front()][d] == headCell)
                {
                    expected = true;
                    break;
                }
        if (!expected)
        {
            rebuild_body(C);
            return;
        }
        body.push_front(headCell);
    }
    while (static_cast<int>(body.size()) > len)
        body.pop_back();
    if (static_cast<int>(body.size()) < len)
        rebuild_body(C);
}

// Mark the cells other dragons occupy this turn (from vision).
void compute_other(Controller& C)
{
    world.otherGen++;
    int const id = C.get_id();
    for (Tile const& t : C.get_tiles())
    {
        DragonPart const* p = t.get_dragon();
        if (p && p->get_id() != id)
            world.otherStamp[world.idx(t.get_position())] = world.otherGen;
    }
}

// Enemy-head threat map: every cell a non-self head (enemy OR teammate) could
// step into next turn is dangerous, because if our head is standing there when
// that head enters, both dragons die in a head-to-head. Marking the reachable
// cells (kelp-aware) is more precise than "is a head adjacent", and it is the
// cheapest attack on the collision/head-to-head deaths the telemetry flagged.
void compute_threat(Controller& C)
{
    world.threatGen++;
    int const id = C.get_id();
    for (Tile const& t : C.get_tiles())
    {
        DragonPart const* p = t.get_dragon();
        if (!p || !p->is_head() || p->get_id() == id)
            continue;
        int const h = world.idx(t.get_position());
        for (int d = 0; d < 4; d++)
            if (world.enemy_can_cross(h, d))
                world.threatStamp[world.nbr[h][d]] = world.threatGen;
    }
}

// Install a body configuration into the occupancy stamps (every segment except
// the tail, which is free to move into during flood reachability).
int set_body(std::vector<int> const& cells)
{
    world.bodyGen++;
    int const n = static_cast<int>(cells.size());
    for (int i = 0; i + 1 < n; i++)
        world.bodyStamp[cells[i]] = world.bodyGen;
    return n ? cells.back() : -1; // tail
}

// Free space reachable from `start`, with the tail treated as a solid wall.
// Blocking the tail keeps the estimate honest: a move that eats leaves the tail
// in place, and even a normal move only frees the tail one cell later. Trusting
// the tail as free is what let earlier versions flood through their own body.
int flood(int start, int tail)
{
    world.visitGen++;
    world.queue.clear();
    world.queue.push_back(start);
    world.visit[start] = world.visitGen;
    int area = 0;
    for (std::size_t qi = 0; qi < world.queue.size(); qi++)
    {
        int const c = world.queue[qi];
        area++;
        for (int d = 0; d < 4; d++)
        {
            int const nc = world.cross(c, d);
            if (nc < 0 || world.visit[nc] == world.visitGen || world.occupied(nc) || nc == tail)
                continue;
            world.visit[nc] = world.visitGen;
            world.queue.push_back(nc);
        }
    }
    return area;
}

// Like flood, but stops once `cap` cells are seen. Bounded cost per call, which
// matters for the many nodes a beam search touches; the exact area beyond a few
// body-lengths never changes a decision.
int flood_cap(int start, int tail, int cap)
{
    world.visitGen++;
    world.queue.clear();
    world.queue.push_back(start);
    world.visit[start] = world.visitGen;
    int area = 0;
    for (std::size_t qi = 0; qi < world.queue.size(); qi++)
    {
        area++;
        if (area >= cap)
            break;
        int const c = world.queue[qi];
        for (int d = 0; d < 4; d++)
        {
            int const nc = world.cross(c, d);
            if (nc < 0 || world.visit[nc] == world.visitGen || world.occupied(nc) || nc == tail)
                continue;
            world.visit[nc] = world.visitGen;
            world.queue.push_back(nc);
        }
    }
    return area;
}

// Can `start` reach `tail` through free water? If so, the head can chase the
// tail indefinitely -- the one provably-safe survival invariant for a snake.
// The tail cell itself is a valid target (it is not in the occupancy stamps).
bool reaches_tail(int start, int tail)
{
    if (tail < 0)
        return false;
    world.visitGen++;
    world.queue.clear();
    world.queue.push_back(start);
    world.visit[start] = world.visitGen;
    for (std::size_t qi = 0; qi < world.queue.size(); qi++)
    {
        int const c = world.queue[qi];
        if (c == tail)
            return true;
        for (int d = 0; d < 4; d++)
        {
            int const nc = world.cross(c, d);
            if (nc < 0 || world.visit[nc] == world.visitGen || world.occupied(nc))
                continue;
            world.visit[nc] = world.visitGen;
            world.queue.push_back(nc);
        }
    }
    return false;
}

constexpr int PEARL_HORIZON = 10;

// First-step direction toward the most attractive pearl (present now, else a
// tile about to spawn one), using current occupancy. -1 if none.
int pearl_direction(Controller& C, int round, int& outDist)
{
    int const headCell = world.idx(C.get_position());
    world.visitGen++;
    world.queue.clear();
    world.visit[headCell] = world.visitGen;
    world.dist[headCell] = 0;
    world.first[headCell] = -1;
    world.queue.push_back(headCell);

    int bestDir = -1;
    long bestScore = -1;
    int bestDist = 1 << 30;
    for (std::size_t qi = 0; qi < world.queue.size(); qi++)
    {
        int const c = world.queue[qi];
        int const cd = world.dist[c];
        int const cf = world.first[c];
        if (cf >= 0)
        {
            long attract = 0;
            if (world.pearlNow[c] && world.pearlRound[c] == round)
                attract = 1000;
            else
            {
                int const pt = world.pearlTime[c];
                if (pt >= 0)
                {
                    int const predicted = pt - (round - world.pearlRound[c]);
                    if (predicted <= PEARL_HORIZON)
                        attract = 500 - std::max(0, predicted) * 20;
                }
            }
            if (attract > 0)
            {
                long const score = attract * 100 - cd;
                if (score > bestScore) { bestScore = score; bestDir = cf; bestDist = cd; }
            }
        }
        for (int d = 0; d < 4; d++)
        {
            int const nc = world.cross(c, d);
            if (nc < 0 || world.visit[nc] == world.visitGen || world.occupied(nc))
                continue;
            world.visit[nc] = world.visitGen;
            world.dist[nc] = cd + 1;
            world.first[nc] = (cf < 0) ? d : cf;
            world.queue.push_back(nc);
        }
    }
    outDist = bestDist;
    return bestDir;
}

// --- immediate-move safety (this turn's authoritative vision) ---------------

// A single immediate move is safe if we can cross the edge (open water, or a
// KNOWN portal) and the destination holds no dragon. For a portal the exit is
// usually beyond our vision; we cannot verify it is empty, so crossing there is
// a calculated risk we accept only because portals otherwise sit unused.
bool step_is_safe(Controller& C, Tile const* /*fromTile*/, Position from, Direction d)
{
    int const nh = world.cross(world.idx(from), dir_index(d));
    if (nh < 0)
        return false; // kelp, unseen edge, or portal with unknown partner
    Position const dest(nh % world.W, nh / world.W);
    Tile const* dt = C.get_tile(dest);
    return dt == nullptr || dt->get_dragon() == nullptr; // unseen (portal far side) = accepted
}
// Cells holding a teammate's segment (from vision), for dispersion.
std::vector<int> friendly_cells(Controller& C)
{
    std::vector<int> out;
    Team const us = C.get_team();
    int const myId = C.get_id();
    for (Tile const& t : C.get_tiles())
    {
        DragonPart const* p = t.get_dragon();
        if (p && p->get_team() == us && p->get_id() != myId)
            out.push_back(world.idx(t.get_position()));
    }
    return out;
}
bool has_pearl_at(Controller& C, int cell)
{
    Position const p(cell % world.W, cell / world.W);
    Tile const* t = C.get_tile(p);
    return t && t->has_pearl();
}

// --- authoritative movement simulator --------------------------------------
// One place that knows how a body moves: cardinal step, wrap, kelp, self- and
// other-collision, pearl eating and tail advance. Everything that looks ahead
// (survival, mobility, and later beam search / sprinting) goes through this so
// the rules live in exactly one function.
struct SimState
{
    std::vector<int> body; // front = head cell
    bool alive = true;
    bool ate = false;
    int head() const { return body.front(); }
    int tail() const { return body.back(); }
    int length() const { return static_cast<int>(body.size()); }
};

// Load a state's occupancy into the stamps so flood / reaches_tail / the next
// step see it (every segment except the tail, which may vacate).
void install(SimState const& s) { set_body(s.body); }

// Is a cardinal step from `from` legal, given its occupancy is installed? A step
// dies on a wall we know, another dragon, or our own body -- the tail included,
// since the engine checks self-collision before the tail advances.
bool can_step(SimState const& from, int dirIdx)
{
    int const nh = world.cross(from.body.front(), dirIdx);
    return nh >= 0 && nh != from.body.back() && !world.occupied(nh);
}

// Apply one cardinal step to `from`, whose occupancy MUST already be installed.
// `alive` reports whether the step is legal; the resulting body is always built
// (from the portal-aware destination), so a caller that already trusts the step
// -- the vision-checked depth-0 move -- can use the state regardless, while
// lookahead filters on `alive`.
SimState simulate_step(SimState const& from, int dirIdx, Controller& C)
{
    SimState out;
    out.alive = can_step(from, dirIdx);
    int nh = world.cross(from.body.front(), dirIdx);
    if (nh < 0)
    {
        out.body = from.body; // no legal destination; keep shape, alive already false
        return out;
    }
    out.ate = has_pearl_at(C, nh);
    out.body.reserve(from.body.size() + 1);
    out.body.push_back(nh);
    for (int c : from.body)
        out.body.push_back(c);
    if (!out.ate)
        out.body.pop_back(); // tail advances unless we grew
    return out;
}

// --- deep survival beam search ---------------------------------------------
// Look several moves past the depth-2 check, to catch multi-step traps (the
// classic snake spiral) that only close a few moves out. From the state after
// the first move we expand every legal move, keep the BEAM_WIDTH roomiest
// states (ranked by a capped flood), and repeat for BEAM_DEPTH plies. If every
// path dies, or none stays roomier than the body, the first move is a deep trap.
//
// Measured: against the local sparring bots the beam produced IDENTICAL results
// (its max-area metric saturates -- there is almost always some roomy escape
// path within the horizon, and the depth-2 + bottleneck checks already catch
// the real traps). So it ships DISABLED (BEAM_DEPTH 0) to avoid ~15% runtime
// for no measured gain; the machinery stays -- enable with -DBEAM_DEPTH=4 to
// revisit (ideally with stronger opponents or a non-saturating metric).
// Parameters are override-able (-D) for tuning.
#ifndef BEAM_DEPTH
#define BEAM_DEPTH 0
#endif
#ifndef BEAM_WIDTH
#define BEAM_WIDTH 6
#endif

struct DeepResult
{
    bool safe = true; // some path stays alive and roomy for the whole horizon
    int bestArea = 0; // best capped free area reached at the horizon
};

struct BeamNode
{
    SimState st;
    int area = 0;
};

DeepResult deep_search(SimState const& s0, Controller& C, int length)
{
    if (BEAM_DEPTH <= 0)
        return {true, 0};
    int const cap = length * 2 + 8;

    std::vector<BeamNode> beam;
    beam.push_back({s0, 0});
    for (int depth = 0; depth < BEAM_DEPTH; depth++)
    {
        std::vector<BeamNode> next;
        for (BeamNode const& bn : beam)
        {
            install(bn.st);
            for (int m = 0; m < 4; m++)
                if (can_step(bn.st, m))
                    next.push_back({simulate_step(bn.st, m, C), 0});
        }
        if (next.empty())
            return {false, 0}; // every surviving path dead-ends within the horizon
        for (BeamNode& n : next)
        {
            install(n.st);
            n.area = flood_cap(n.st.head(), n.st.tail(), cap);
        }
        std::sort(next.begin(), next.end(),
                  [](BeamNode const& a, BeamNode const& b) { return a.area > b.area; });
        if (static_cast<int>(next.size()) > BEAM_WIDTH)
            next.resize(BEAM_WIDTH);
        beam = std::move(next);
    }

    DeepResult r{false, 0};
    for (BeamNode const& bn : beam)
    {
        r.bestArea = std::max(r.bestArea, bn.area);
        if (bn.area > length)
            r.safe = true; // room for the whole body at the horizon
    }
    return r;
}

// Split to colonise more water when we are long, safe and roomy. The lead
// dragon of the fleet (teamIndex 0) never splits so the team always keeps one
// dragon growing toward the longest-dragon tiebreak.
constexpr int SPLIT_MIN = 18; // both halves stay >= 9 segments

bool should_split(Controller& C, int& childOut)
{
    int const len = C.get_length();
    if (g_teamIndex == 0)
        return false;
    if (len < SPLIT_MIN || C.get_unit_count() >= C.unit_limit)
        return false;
    childOut = len / 2;
    if (childOut < 2 || len - childOut < 2 || !C.can_split(childOut))
        return false;
    // Only split with plenty of open water around us, so neither half is boxed.
    std::vector<int> ob(body.begin(), body.end());
    int const tail = ob.back();
    set_body(ob);
    return flood(world.idx(C.get_position()), tail) >= len * 2;
}

Direction choose_move(Controller& C, int round)
{
    Position const head = C.get_position();
    Tile const* headTile = C.get_tile(head);
    int const length = C.get_length();
    auto const dirs = Direction::get_direction_list();

    std::vector<Direction> safe;
    for (auto const d : dirs)
        if (step_is_safe(C, headTile, head, d))
            safe.push_back(d);
    if (safe.empty())
        return C.get_dir();

    std::vector<int> origBody(body.begin(), body.end());
    SimState const base{origBody}; // our current shape, the root of every lookahead

    set_body(origBody);
    int pearlDist = 0;
    int const pearlDir = pearl_direction(C, round, pearlDist);

    std::vector<int> const friends = friendly_cells(C);

    int const healthy = length * 2 + 4;
    // Growth governor: on a small, confined board a long body inevitably coils
    // into itself, so cap how long we grow relative to the board. Big maps
    // (large N) impose no practical limit, so we still win length races there.
    int const growthCap = std::max(9, world.N / 10);

    long bestScore = LONG_MIN;
    Direction best = safe.front();
#ifdef SEAD_DEBUG
    int dbgArea0 = 0, dbgFuture = 0, dbgEat = 0;
#endif
    for (auto const d : safe)
    {
        int const di = dir_index(d);

        // Depth 0: the state after this move, via the one authoritative
        // simulator. d is already vision-safe, so we trust it (the world model
        // and live vision can differ by a stale cell; vision is authoritative).
        install(base);
        SimState const s0 = simulate_step(base, di, C);
        bool const eat0 = s0.ate;
        int const newHead = s0.head();
        int const tail1 = s0.tail();

        // Openness (honest open area, tail as wall) and the reach-to-tail
        // survival invariant, both from the moved state.
        install(s0);
        bool const reach = reaches_tail(newHead, tail1);
        int const area = flood(newHead, tail1);

        // Depth 2: reach-to-tail plus mobility -- how many escape routes we keep.
        // `exits1` is the legal moves out of s0; `exits2` the legal moves beyond
        // those. A big area with only one exit is behind a bottleneck (an
        // articulation point), which mobility exposes where area alone cannot.
        std::array<SimState, 4> kids;
        int exits1 = 0;
        for (int m2 = 0; m2 < 4; m2++)
        {
            SimState child = simulate_step(s0, m2, C); // s0 still installed
            if (child.alive)
                kids[exits1++] = std::move(child);
        }
        bool reach2 = false;
        int exits2 = 0;
        for (int k = 0; k < exits1; k++)
        {
            install(kids[k]);
            if (reaches_tail(kids[k].head(), kids[k].tail()))
                reach2 = true;
            for (int m3 = 0; m3 < 4; m3++)
                if (can_step(kids[k], m3))
                    exits2++;
        }

        // --- score --------------------------------------------------------
        // Openness is PRIMARY, now measured by BOTH free area and mobility so a
        // roomy-but-bottlenecked cell loses to an equally roomy open one. In
        // open water every direction ties on these, so growth/threat below
        // decide; inside a tightening coil they diverge and survival leads.
        // Weights are override-able (-D) for tuning; defaults are the shipped
        // values.
#ifndef W_AREA
#define W_AREA 60
#endif
#ifndef W_MOB
#define W_MOB 30
#endif
#ifndef W_EXIT1
#define W_EXIT1 100
#endif
#ifndef W_EXIT2
#define W_EXIT2 20
#endif
#ifndef W_BOTTLE
#define W_BOTTLE 60000 // tuned: firmer bottleneck guard -> 93% win, fewer self-coils
#endif
        long const mobility = exits1 * (long)W_EXIT1 + exits2 * (long)W_EXIT2;
        long score = static_cast<long>(area) * W_AREA + mobility * W_MOB;

        if (reach2)
            score += 200'000;        // safe two moves deep: strongly preferred
        else if (reach)
            score += 20'000;         // safe next move, but risky the one after
        else if (area < length + 1)
            score -= 2'000'000;      // no loop and no room: near-certain death
        else if (area < healthy)
            score -= static_cast<long>(healthy - area) * 500;

        // Bottleneck guard: a single exit with no safe loop is a corridor that
        // can seal behind us. Cheap topology signal on top of area/reach.
        if (exits1 <= 1 && !reach2)
            score -= W_BOTTLE;

        // Deep beam: penalise a first move that leads to a forced trap a few
        // moves out (a spiral the depth-2 check cannot see). Only a penalty, so
        // in the common case (all moves deep-safe) it changes nothing.
#ifndef W_DEEP_TRAP
#define W_DEEP_TRAP 40000
#endif
#ifndef W_DEEP_ROOM
#define W_DEEP_ROOM 0
#endif
        if (BEAM_DEPTH > 0) // compile-time; the whole block folds away when disabled
        {
            install(s0);
            DeepResult const dr = deep_search(s0, C, length);
            if (!dr.safe)
                score -= W_DEEP_TRAP;
            score += static_cast<long>(dr.bestArea) * W_DEEP_ROOM;
        }

        bool const mayGrow = length < growthCap;
        if (eat0)
            score += (reach && reach2 && mayGrow) ? 800 : -40'000; // eat only when safe and short enough
        else if (di == pearlDir && mayGrow)
            score += (reach2 && area >= healthy) ? (1500L / (pearlDist + 1)) : 0;

        // Head-to-head risk: a cell another head can enter next turn. Precise
        // (kelp-aware reachable cells) replacement for plain head-adjacency.
        if (world.threatened(newHead))
            score -= 6000;

        // Coordination: repel from nearby teammates so the fleet spreads across
        // the water instead of clumping on the same pearls. Weak enough that
        // survival and pearls still lead; it only decides otherwise-close moves.
        if (!friends.empty())
        {
            int md = 99;
            for (int fc : friends)
                md = std::min(md, cheby(newHead, fc));
            score += std::min(md, 4) * 250; // farther from teammates is better
        }

        if (d == C.get_dir())
            score += 1;

        if (score > bestScore)
        {
            bestScore = score;
            best = d;
#ifdef SEAD_DEBUG
            dbgArea0 = area; dbgFuture = (int)reach; dbgEat = eat0;
#endif
#ifdef SEAD_TELEMETRY
            g_chosenArea = area;
#endif
        }
    }
#ifdef SEAD_DEBUG
    {
        static std::FILE* dbg = std::fopen("/tmp/sead_v2.log", "a");
        std::fprintf(dbg, "r=%d len=%d safe=%d dir=%c area0=%d future=%d eat=%d\n",
                     round, length, (int)safe.size(), (char)best.value,
                     dbgArea0, dbgFuture, (int)dbgEat);
        std::fflush(dbg);
    }
#endif
    return best;
}

#ifdef SEAD_TELEMETRY
// The engine kills a dragon's process rather than letting it exit cleanly, so
// atexit never runs. Instead each dragon rewrites its own running summary every
// turn to SEAD_TEL_DIR/<id>.csv; the final overwrite is always on disk however
// the process ends. The harness reads one row per dragon after the game. All of
// this is compile-flag gated, so the submitted bot carries none of it.
struct Telem
{
    char team = '?';
    int id = -1, first = -1, last = -1, maxLen = 0, finalLen = 0;
    int ate = 0, splits = 0, prevLen = -1;
    long areaSum = 0;
    int turns = 0;
};
Telem g_tel;

void tel_note(Controller& C, int round, bool didSplit)
{
    int const len = C.get_length();
    if (g_tel.id < 0)
    {
        g_tel.id = C.get_id();
        g_tel.team = (C.get_team().value == Team::A) ? 'A' : 'B';
        g_tel.first = round;
        g_tel.prevLen = len;
    }
    if (g_tel.prevLen >= 0 && len > g_tel.prevLen)
        g_tel.ate += len - g_tel.prevLen; // a length gain is a pearl eaten
    g_tel.prevLen = len;
    g_tel.last = round;
    g_tel.finalLen = len;
    g_tel.maxLen = std::max(g_tel.maxLen, len);
    if (didSplit)
        g_tel.splits++;
    g_tel.areaSum += g_chosenArea;
    g_tel.turns++;

    char const* dir = std::getenv("SEAD_TEL_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%d.csv", dir ? dir : "/tmp/sead_tel", g_tel.id);
    std::FILE* f = std::fopen(path, "w");
    if (!f)
        return;
    std::fprintf(f, "%c,%d,%d,%d,%d,%d,%d,%d,%ld,%d\n", g_tel.team, g_tel.id,
                 g_tel.first, g_tel.last, g_tel.maxLen, g_tel.finalLen, g_tel.ate,
                 g_tel.splits, g_tel.areaSum, g_tel.turns);
    std::fclose(f);
}
#endif

} // namespace

int main()
{
    auto [C, G] = unswbc::init();
    auto const [w, h] = G.get_map_size();
    world.init(w, h);

    while (unswbc::update(C, G))
    {
        int const round = G.get_round_num();
        init_role(C);
        absorb_vision(C, round);
        update_body(C);
        compute_other(C);
        compute_threat(C);

        int child = 0;
        bool didSplit = false;
        if (should_split(C, child))
        {
            C.do_split(child); // spawns a colonist from our rear segments
            didSplit = true;
            g_lastMove = -1; // no move this turn
        }
        else
        {
            Direction const mv = choose_move(C, round);
            C.make_move(mv);
            g_lastMove = dir_index(mv); // remember for portal-aware body tracking
        }

#ifdef SEAD_TELEMETRY
        tel_note(C, round, didSplit);
#else
        (void)didSplit;
#endif
        unswbc::end_turn();
    }
}
