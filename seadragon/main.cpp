#include "helper.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <vector>

using namespace unswbc;

// ===========================================================================
// v2: stateful sea-dragon.
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
//     are penalised, since a head-on kills both dragons. (Measured: this raised
//     the win rate 75% -> 84% against the local sparring bots.)
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
            record_edge(c, d, t.get_edge(dir_from_index(d)).get_edge_type());
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
        bool adjacent = false;
        for (int d = 0; d < 4; d++)
            if (world.nbr[body.front()][d] == headCell)
            {
                adjacent = true;
                break;
            }
        if (!adjacent)
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
            if (!world.passable(c, d))
                continue;
            int const nc = world.nbr[c][d];
            if (world.visit[nc] == world.visitGen || world.occupied(nc) || nc == tail)
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
            if (!world.passable(c, d))
                continue;
            int const nc = world.nbr[c][d];
            if (world.visit[nc] == world.visitGen || world.occupied(nc))
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
            if (!world.passable(c, d))
                continue;
            int const nc = world.nbr[c][d];
            if (world.visit[nc] == world.visitGen || world.occupied(nc))
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

bool edge_ok(Tile const* tile, Direction d)
{
    if (!tile)
        return false;
    Edge const& e = tile->get_edge(d);
    return e.is_passable() && !e.is_portal();
}
bool step_is_safe(Controller& C, Tile const* fromTile, Position from, Direction d)
{
    if (!edge_ok(fromTile, d))
        return false;
    Tile const* dt = C.get_tile(from.add_dir(d));
    return dt != nullptr && dt->get_dragon() == nullptr;
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
    int const headCell = world.idx(head);
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
        int const newHead = world.nbr[headCell][di];
        bool const eat0 = has_pearl_at(C, newHead);

        // Simulate the body after this move.
        std::vector<int> sim;
        sim.reserve(origBody.size() + 1);
        sim.push_back(newHead);
        for (int c : origBody)
            sim.push_back(c);
        if (!eat0)
            sim.pop_back(); // tail vacates on a non-eating move
        int const tail1 = sim.back();
        set_body(sim);

        // Openness (honest open area, tail as wall) ranks how much room a move
        // keeps, and reach-to-tail is the survival invariant. But 1-ply reach is
        // blind to a move that itself leads to a forced trap next turn, so we
        // also demand DEPTH-2 safety: after this move, at least one follow-up
        // move must still keep the tail reachable.
        bool const reach = reaches_tail(newHead, tail1);
        int const area = flood(newHead, tail1);

        bool reach2 = false;
        for (int m2 = 0; m2 < 4 && !reach2; m2++)
        {
            if (!world.passable(newHead, m2))
                continue;
            int const nh2 = world.nbr[newHead][m2];
            if (nh2 == tail1 || world.occupied(nh2)) // occupancy is still `sim`
                continue;
            std::vector<int> sim2;
            sim2.reserve(sim.size() + 1);
            sim2.push_back(nh2);
            for (int c : sim)
                sim2.push_back(c);
            if (!has_pearl_at(C, nh2))
                sim2.pop_back();
            int const tail2 = sim2.back();
            set_body(sim2);
            reach2 = reaches_tail(nh2, tail2);
        }
        set_body(sim); // restore this move's occupancy

        // --- score --------------------------------------------------------
        // Openness is PRIMARY (raw area, always discriminating). Growth is a
        // small tiebreak, so a pearl never lures us into a materially tighter
        // position: in open water directions differ by a cell or two and eating
        // wins; inside a tightening coil the uncoiling direction has far more
        // room and wins.
        long score = static_cast<long>(area) * 100;

        if (reach2)
            score += 200'000;        // safe two moves deep: strongly preferred
        else if (reach)
            score += 20'000;         // safe next move, but risky the one after
        else if (area < length + 1)
            score -= 2'000'000;      // no loop and no room: near-certain death
        else if (area < healthy)
            score -= static_cast<long>(healthy - area) * 500;

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
        }
        else
            C.make_move(choose_move(C, round));

#ifdef SEAD_TELEMETRY
        tel_note(C, round, didSplit);
#else
        (void)didSplit;
#endif
        unswbc::end_turn();
    }
}
