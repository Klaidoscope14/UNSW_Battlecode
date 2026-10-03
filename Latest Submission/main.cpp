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
// v12.0.0: Competition replay analysis fixes.
//
//   Based on v11 with targeted changes from match replay analysis:
//   * Leaders sprint to contested pearls when enemy within 4 cells of pearl.
//   * Dynamic leader threat penalty: 200k when ahead by 2+ (was flat 500k).
//   * ENDGAME phase (round >= 470): fodder near the leader seeks death to
//     create pearl drops that the leader can eat — sacrificial feeding.
//     No splits in endgame; death drop attraction 4x for leaders.
// ===========================================================================

namespace {

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

    std::vector<int> otherStamp; int otherGen = 1;
    std::vector<int> bodyStamp;  int bodyGen = 1;
    std::vector<int> threatStamp; int threatGen = 1;

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

    bool passable(int cell, int d) const { return edge[cell][d] == E_OPEN; }
    bool occupied(int c) const
    {
        return otherStamp[c] == otherGen || bodyStamp[c] == bodyGen;
    }
    bool enemy_can_cross(int cell, int d) const
    {
        std::uint8_t const e = edge[cell][d];
        return e == E_OPEN || e == E_UNKNOWN;
    }
    bool threatened(int c) const { return threatStamp[c] == threatGen; }

    std::unordered_map<int, int> partnerOf;
    std::unordered_map<int, int> portalSeen;

    int canon_code(int cell, int d) const
    {
        switch (d)
        {
        case 0:  return cell * 4 + 0;
        case 1:  return nbr[cell][1] * 4 + 3;
        case 2:  return nbr[cell][2] * 4 + 0;
        default: return cell * 4 + 3;
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
                return -1;
            int const pcell = it->second / 4;
            return (d == 1 || d == 2) ? pcell : nbr[pcell][d];
        }
        return -1;
    }
};

World world;
std::deque<int> body;
bool haveBody = false;

// --- Map symmetry detection --------------------------------------------------
enum class Symmetry : int { UNKNOWN = -1, VERTICAL = 0, HORIZONTAL = 1, ROTATIONAL = 2 };
Symmetry g_symmetry = Symmetry::UNKNOWN;
bool g_symElim[3] = {};

struct SymEdge { int cell; int dir; };

SymEdge sym_map(int x, int y, int d, int symType)
{
    int sx, sy, sd;
    switch (symType) {
        case 0:
            sx = world.W - 1 - x; sy = y;
            sd = (d == 1) ? 3 : (d == 3) ? 1 : d;
            break;
        case 1:
            sx = x; sy = world.H - 1 - y;
            sd = (d == 0) ? 2 : (d == 2) ? 0 : d;
            break;
        case 2:
            sx = world.W - 1 - x; sy = world.H - 1 - y;
            sd = (d + 2) % 4;
            break;
        default:
            return {-1, -1};
    }
    return {sy * world.W + sx, sd};
}

void detect_symmetry()
{
    if (g_symmetry != Symmetry::UNKNOWN) return;

    for (int c = 0; c < world.N; c++)
    {
        int const x = c % world.W, y = c / world.W;
        for (int d = 0; d < 4; d++)
        {
            if (world.edge[c][d] == E_UNKNOWN) continue;
            std::uint8_t const val = world.edge[c][d];

            for (int s = 0; s < 3; s++)
            {
                if (g_symElim[s]) continue;
                auto [sc, sd] = sym_map(x, y, d, s);
                if (sc < 0 || sd < 0) continue;
                if (world.edge[sc][sd] == E_UNKNOWN) continue;

                bool match = (val == E_PORTAL && world.edge[sc][sd] == E_PORTAL)
                          || (world.edge[sc][sd] == val);

                if (!match) g_symElim[s] = true;
            }
        }
    }

    int remaining = 0, which = -1;
    for (int s = 0; s < 3; s++)
        if (!g_symElim[s]) { remaining++; which = s; }

    if (remaining == 1)
        g_symmetry = static_cast<Symmetry>(which);
}

void apply_symmetry()
{
    if (g_symmetry == Symmetry::UNKNOWN) return;
    int const symType = static_cast<int>(g_symmetry);

    for (int c = 0; c < world.N; c++)
    {
        int const x = c % world.W, y = c / world.W;
        for (int d = 0; d < 4; d++)
        {
            if (world.edge[c][d] == E_UNKNOWN) continue;
            auto [sc, sd] = sym_map(x, y, d, symType);
            if (sc < 0 || sd < 0) continue;
            if (world.edge[sc][sd] == E_UNKNOWN)
            {
                world.edge[sc][sd] = world.edge[c][d];
                world.edge[world.nbr[sc][sd]][OPP[sd]] = world.edge[c][d];
            }
        }
        if (world.pearlTime[c] >= 0 && world.pearlRound[c] >= 0)
        {
            auto [sc, sd_unused] = sym_map(x, y, 0, symType);
            (void)sd_unused;
            if (sc >= 0 && world.pearlRound[sc] < world.pearlRound[c])
            {
                world.pearlTime[sc] = world.pearlTime[c];
                world.pearlRound[sc] = world.pearlRound[c];
            }
        }
    }
}

// --- v11 Phases: FLOOD then CONSOLIDATE --------------------------------------
enum class Phase { FLOOD, CONSOLIDATE, ENDGAME };
enum class Role  { LEADER, FODDER };

constexpr int ENDGAME_START = 470;

Phase get_phase(int round, int unitCount, int unitLimit)
{
    if (round >= ENDGAME_START)
        return Phase::ENDGAME;

    int const N = world.N;
    int consolidateStart;
    if (N > 800)       consolidateStart = 350;
    else if (N > 400)  consolidateStart = 300;
    else if (N >= 200) consolidateStart = 300;
    else               consolidateStart = 250;

    return (round <= consolidateStart) ? Phase::FLOOD : Phase::CONSOLIDATE;
}

// --- multi-dragon coordination -----------------------------------------------
int g_teamSize = 0;
int g_teamIndex = 0;
bool g_roleInit = false;
int g_lastMove = -1;
int g_maxEnemyLen = 0;
int g_enemyCount = 0;
bool g_echoEnemyDetected = false;
int g_ourMaxLen = 0;

void update_role(Controller& C)
{
    g_teamSize = std::max(1, C.get_unit_count());
    if (!g_roleInit) {
        g_teamIndex = (std::hash<int>{}(C.get_id()) % 100) % std::max(1, g_teamSize);
        g_roleInit = true;
    }
}

Role get_role(int length, int ourMaxLen, Phase phase)
{
    if (phase != Phase::FLOOD && length >= ourMaxLen - 2 && length >= 6)
        return Role::LEADER;
    if (length >= ourMaxLen && ourMaxLen >= 8)
        return Role::LEADER;
    return Role::FODDER;
}

// --- persistent enemy tracking -----------------------------------------------
struct EnemyInfo
{
    int headCell = -1;
    int facing = -1;
    int visibleLen = 0;
    int lastSeen = -1;
    bool isEnemy = true;
};
std::unordered_map<int, EnemyInfo> g_enemies;

// --- death pearl farming -----------------------------------------------------
struct DeathDrop
{
    int cell;
    int round;
    int estimatedPearls;
};
std::vector<DeathDrop> g_deathDrops;

// --- sacrifice feeding -------------------------------------------------------
int g_lastSacrificeRound = -100;

int sacrifice_start_round()
{
    // Map identification by dimensions — only specified maps
    if (world.W == 25 && world.H == 35) return 380;   // Queen of Spades
    if (world.W == 25 && world.H == 25) return 420;   // Trophy
    if (world.W == 48 && world.H == 24) return 420;   // Stronghold / Trauma
    if (world.W == 60 && world.H == 40) return 420;   // Schooltime
    return 9999; // disabled for all other maps
}

// --- map type detection (open vs separated) ----------------------------------
bool g_openMap = false;
bool g_separatedMap = false;
int g_mapTypeRound = -1;

void compute_map_type(int round)
{
    if (round - g_mapTypeRound < 50 && g_mapTypeRound >= 0) return;
    g_mapTypeRound = round;
    int kelp = 0, open = 0;
    for (int c = 0; c < world.N; c++)
        for (int d = 0; d < 4; d++)
        {
            if (world.edge[c][d] == E_KELP) kelp++;
            else if (world.edge[c][d] == E_OPEN) open++;
        }
    int total = kelp + open;
    g_openMap = (world.N > 400 && total > 100 && kelp * 20 < total);

    g_separatedMap = false;
    for (int x = 0; x < world.W - 1 && !g_separatedMap; x++)
    {
        int kc = 0, gaps = 0;
        for (int y = 0; y < world.H; y++)
        {
            std::uint8_t e = world.edge[y * world.W + x][1];
            if (e == E_KELP) kc++;
            else if (e == E_OPEN || e == E_PORTAL) gaps++;
        }
        if (kc >= 4 && gaps <= 2 && kc >= world.H / 2)
            g_separatedMap = true;
    }
    for (int y = 0; y < world.H - 1 && !g_separatedMap; y++)
    {
        int kc = 0, gaps = 0;
        for (int x = 0; x < world.W; x++)
        {
            std::uint8_t e = world.edge[y * world.W + x][2];
            if (e == E_KELP) kc++;
            else if (e == E_OPEN || e == E_PORTAL) gaps++;
        }
        if (kc >= 4 && gaps <= 2 && kc >= world.W / 2)
            g_separatedMap = true;
    }
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

// --- sonar protocol (Protocol 3: 64-bit, directional) -----------------------
std::uint64_t sonar_encode(int x, int y, int facing, Team team, int enemyLen)
{
    std::uint64_t p = 0;
    p |= (static_cast<std::uint64_t>(x) & 0x3F);
    p |= (static_cast<std::uint64_t>(y) & 0x3F) << 6;
    p |= (static_cast<std::uint64_t>(facing) & 0x3) << 12;
    p |= ((team.value == Team::A) ? 0ULL : 1ULL) << 14;
    p |= (static_cast<std::uint64_t>(std::min(enemyLen, 15)) & 0xF) << 15;
    std::uint64_t salt = (team.value == Team::A) ? 0x5AULL : 0xA5ULL;
    std::uint64_t chk = ((p & 0xFF) ^ ((p >> 8) & 0xFF) ^ ((p >> 16) & 0xFF) ^ salt) & 0xFF;
    return p | (chk << 24);
}
bool sonar_decode(std::uint64_t raw, Team team, int& ox, int& oy, int& oface, int& oLen)
{
    std::uint64_t p = raw & 0x00FFFFFFULL;
    std::uint64_t salt = (team.value == Team::A) ? 0x5AULL : 0xA5ULL;
    std::uint64_t expected = ((p & 0xFF) ^ ((p >> 8) & 0xFF) ^ ((p >> 16) & 0xFF) ^ salt) & 0xFF;
    if (((raw >> 24) & 0xFF) != expected)
        return false;
    if (((p >> 14) & 1) != ((team.value == Team::A) ? 0ULL : 1ULL))
        return false;
    ox = static_cast<int>(p & 0x3F);
    oy = static_cast<int>((p >> 6) & 0x3F);
    oface = static_cast<int>((p >> 12) & 0x3);
    oLen = static_cast<int>((p >> 15) & 0xF);
    return true;
}

struct SonarReport { int cell; int round; int enemyLen; };
std::vector<SonarReport> g_sonarReports;
int g_sonarTargetCell = -1;
int g_sonarTargetFacing = 0;
int g_sonarTargetLen = 0;

void process_sonar(Controller& C, int round)
{
    Team const myTeam = C.get_team();
    for (std::uint64_t raw : C.get_sonar_messages())
    {
        int ex, ey, ef, elen;
        if (!sonar_decode(raw, myTeam, ex, ey, ef, elen))
            continue;
        if (ex >= world.W || ey >= world.H)
            continue;
        int const ec = ey * world.W + ex;
        Position const ep(ex, ey);
        if (C.get_tile(ep) == nullptr)
            g_sonarReports.push_back({ec, round, elen});
    }
    g_sonarReports.erase(
        std::remove_if(g_sonarReports.begin(), g_sonarReports.end(),
                        [&](SonarReport const& r) { return round - r.round > 5; }),
        g_sonarReports.end());
}

void pick_sonar_target(Controller& C, int round)
{
    g_sonarTargetCell = -1;
    g_sonarTargetLen = 0;
    Team const us = C.get_team();
    int const headCell = world.idx(C.get_position());

    int bestPriority = -1;
    for (Tile const& t : C.get_tiles())
    {
        DragonPart const* p = t.get_dragon();
        if (!p || !p->is_head() || p->get_team() == us)
            continue;
        int const ec = world.idx(t.get_position());
        int const d = cheby(headCell, ec);

        int visLen = 0;
        int eid = p->get_id();
        auto it = g_enemies.find(eid);
        if (it != g_enemies.end())
            visLen = it->second.visibleLen;

        int priority = 1000 - d;
        if (visLen >= 8)
            priority += 5000;

        if (priority > bestPriority)
        {
            bestPriority = priority;
            g_sonarTargetCell = ec;
            g_sonarTargetFacing = dir_index(p->get_dir());
            g_sonarTargetLen = visLen;
        }
    }

    if (g_sonarTargetCell < 0)
    {
        int bestDist = 999;
        for (auto const& dd : g_deathDrops)
        {
            int const d = cheby(headCell, dd.cell);
            if (d < bestDist && dd.estimatedPearls >= 3)
            {
                bestDist = d;
                g_sonarTargetCell = dd.cell;
                g_sonarTargetFacing = 0;
                g_sonarTargetLen = 0;
            }
        }
    }

    if (g_sonarTargetCell < 0)
    {
        int bestDist = 999;
        for (Tile const& t : C.get_tiles())
        {
            if (!t.has_pearl())
                continue;
            int const pc = world.idx(t.get_position());
            int const d = cheby(headCell, pc);
            if (d < bestDist)
            {
                bestDist = d;
                g_sonarTargetCell = pc;
                g_sonarTargetFacing = 0;
                g_sonarTargetLen = 0;
            }
        }
    }
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
                int dir_val = dir_index(p->get_dir());
                if (world.nbr[nc][dir_val] == cur) {
                    nextCell = nc;
                    break;
                }
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

void update_enemies(Controller& C, int round)
{
    int const myId = C.get_id();
    Team const myTeam = C.get_team();
    std::unordered_map<int, int> segCount;
    for (Tile const& t : C.get_tiles())
    {
        DragonPart const* p = t.get_dragon();
        if (!p || p->get_id() == myId)
            continue;
        segCount[p->get_id()]++;
        if (p->is_head())
        {
            EnemyInfo& e = g_enemies[p->get_id()];
            e.headCell = world.idx(t.get_position());
            e.facing = dir_index(p->get_dir());
            e.lastSeen = round;
            e.isEnemy = !(p->get_team() == myTeam);
        }
    }
    for (auto& [id, info] : g_enemies)
    {
        auto it = segCount.find(id);
        if (it != segCount.end())
            info.visibleLen = it->second;
    }

    for (auto& [eid, ei] : g_enemies)
    {
        if (!ei.isEnemy || ei.headCell < 0) continue;
        if (ei.lastSeen >= round - 2 && ei.lastSeen < round)
        {
            Position lp(ei.headCell % world.W, ei.headCell / world.W);
            Tile const* t = C.get_tile(lp);
            if (t && t->get_dragon() == nullptr && t->has_pearl())
            {
                g_deathDrops.push_back({ei.headCell, round,
                    std::max(1, ei.visibleLen / 2)});
            }
        }
    }
    g_deathDrops.erase(
        std::remove_if(g_deathDrops.begin(), g_deathDrops.end(),
            [&](DeathDrop const& d) { return round - d.round > 10; }),
        g_deathDrops.end());

    for (auto it = g_enemies.begin(); it != g_enemies.end(); )
    {
        if (round - it->second.lastSeen > 50)
            it = g_enemies.erase(it);
        else
            ++it;
    }
    g_maxEnemyLen = 0;
    g_enemyCount = 0;
    for (auto const& [id, ei] : g_enemies)
    {
        if (!ei.isEnemy)
            continue;
        g_enemyCount++;
        if (ei.visibleLen > g_maxEnemyLen)
            g_maxEnemyLen = ei.visibleLen;
    }

    g_ourMaxLen = C.get_length();
    for (auto const& [id, ei] : g_enemies)
    {
        if (ei.isEnemy) continue;
        if (ei.visibleLen > g_ourMaxLen)
            g_ourMaxLen = ei.visibleLen;
    }
}

int set_body(std::vector<int> const& cells)
{
    world.bodyGen++;
    int const n = static_cast<int>(cells.size());
    for (int i = 0; i + 1 < n; i++)
        world.bodyStamp[cells[i]] = world.bodyGen;
    return n ? cells.back() : -1;
}

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

int flood_optimistic(int start, int tail, int cap)
{
    world.visitGen++;
    world.queue.clear();
    world.queue.push_back(start);
    world.visit[start] = world.visitGen;
    int area = 0;
    for (std::size_t qi = 0; qi < world.queue.size(); qi++)
    {
        area++;
        if (area >= cap) break;
        int const c = world.queue[qi];
        for (int d = 0; d < 4; d++)
        {
            std::uint8_t const e = world.edge[c][d];
            if (e == E_KELP) continue;
            int nc;
            if (e == E_OPEN || e == E_UNKNOWN)
                nc = world.nbr[c][d];
            else if (e == E_PORTAL) {
                nc = world.cross(c, d);
                if (nc < 0) continue;
            } else continue;

            if (world.visit[nc] == world.visitGen || world.occupied(nc) || nc == tail)
                continue;
            world.visit[nc] = world.visitGen;
            world.queue.push_back(nc);
        }
    }
    return area;
}

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

int pearl_direction(Controller& C, int round, int& outDist, bool isLongest)
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
                bool enemyCloser = false;
                for (auto const& [eid, ei] : g_enemies)
                {
                    if (!ei.isEnemy || ei.lastSeen < round - 3 || ei.headCell < 0)
                        continue;
                    int const eDist = cheby(ei.headCell, c);
                    if (eDist < cd) { enemyCloser = true; break; }
                }
                if (enemyCloser)
                    attract /= 3;

                if (isLongest)
                    attract = attract * 3 / 2;

                for (auto const& dd : g_deathDrops)
                {
                    if (cheby(c, dd.cell) <= 2)
                        attract = attract * 2;
                }

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

// --- immediate-move safety ---------------------------------------------------
bool step_is_safe(Controller& C, Tile const*, Position from, Direction d)
{
    int const nh = world.cross(world.idx(from), dir_index(d));
    if (nh < 0)
        return false;
    Position const dest(nh % world.W, nh / world.W);
    Tile const* dt = C.get_tile(dest);
    return dt == nullptr || dt->get_dragon() == nullptr;
}

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

// --- movement simulator ------------------------------------------------------
struct SimState
{
    std::vector<int> body;
    bool alive = true;
    bool ate = false;
    int head() const { return body.front(); }
    int tail() const { return body.back(); }
    int length() const { return static_cast<int>(body.size()); }
};

void install(SimState const& s) { set_body(s.body); }

bool can_step(SimState const& from, int dirIdx)
{
    int const nh = world.cross(from.body.front(), dirIdx);
    return nh >= 0 && nh != from.body.back() && !world.occupied(nh);
}

SimState simulate_step(SimState const& from, int dirIdx, Controller& C)
{
    SimState out;
    out.alive = can_step(from, dirIdx);
    int nh = world.cross(from.body.front(), dirIdx);
    if (nh < 0)
    {
        out.body = from.body;
        return out;
    }
    out.ate = has_pearl_at(C, nh);
    out.body.reserve(from.body.size() + 1);
    out.body.push_back(nh);
    for (int c : from.body)
        out.body.push_back(c);
    if (!out.ate)
        out.body.pop_back();
    return out;
}

// --- sprint escape (checks mid-step collisions) ------------------------------
bool try_sprint_escape(Controller& C)
{
    int const length = C.get_length();
    if (length <= 2)
        return false;

    Position const head = C.get_position();
    auto const dirs = Direction::get_direction_list();

    bool anySingleSafe = false;
    for (auto const d : dirs)
        if (step_is_safe(C, C.get_tile(head), head, d))
            anySingleSafe = true;
    if (anySingleSafe)
        return false;

    for (auto const d1 : dirs)
    {
        int const di1 = dir_index(d1);
        int const mid = world.cross(world.idx(head), di1);
        if (mid < 0)
            continue;

        Position const mp(mid % world.W, mid / world.W);
        Tile const* mt = C.get_tile(mp);
        if (mt && mt->get_dragon() != nullptr)
            continue;

        bool midHitsSelf = false;
        for (int c : body)
            if (c == mid) { midHitsSelf = true; break; }
        if (midHitsSelf) continue;

        for (auto const d2 : dirs)
        {
            int const dest = world.cross(mid, dir_index(d2));
            if (dest < 0)
                continue;

            Position const dp(dest % world.W, dest / world.W);
            Tile const* dt = C.get_tile(dp);
            if (dt && dt->get_dragon() != nullptr)
                continue;

            bool destHitsSelf = false;
            for (std::size_t i = 1; i + 1 < body.size(); i++)
                if (body[i] == dest) { destHitsSelf = true; break; }
            if (destHitsSelf) continue;

            std::vector<int> origBody(body.begin(), body.end());
            SimState const base{origBody};
            install(base);
            int const newArea = flood(dest, -1);
            if (newArea >= length - 1)
            {
                C.make_moves({d1, d2});
                return true;
            }
        }
    }
    return false;
}

// --- v12 split: leaders protected in CONSOLIDATE only -----------------------
bool should_split(Controller& C, int& childOut, Phase phase, Role role, int round)
{
    int const len = C.get_length();
    int const unitCount = C.get_unit_count();

    if (unitCount >= C.unit_limit)
        return false;

    if (phase != Phase::FLOOD && role == Role::LEADER)
        return false;

    if (phase != Phase::FLOOD && unitCount >= 10)
        return false;

    int const estimatedOccupied = 3 * unitCount;
    if (estimatedOccupied * 10 >= world.N * 9)
        return false;

    int minSplitLen = 4;
    if (g_openMap && unitCount >= 50)
        minSplitLen = 6;
    if (len < minSplitLen)
        return false;

    childOut = 2;
    if (!C.can_split(childOut))
        return false;

    if (g_enemyCount > 0 && unitCount * 2 < g_enemyCount)
        return true;

    if (phase == Phase::FLOOD)
    {
        std::vector<int> ob(body.begin(), body.end());
        if (!ob.empty()) {
            set_body(ob);
            int const currentArea = flood(world.idx(C.get_position()), ob.back());
            return currentArea > len;
        }
        return true;
    }

    std::vector<int> ob(body.begin(), body.end());
    if (!ob.empty()) {
        set_body(ob);
        return flood(world.idx(C.get_position()), ob.back()) >= len + 2;
    }
    return true;
}

// --- v11 emergency split (restored from v9) ----------------------------------
bool emergency_split(Controller& C)
{
    int const len = C.get_length();
    if (len < 4) return false;
    if (C.get_unit_count() >= C.unit_limit) return false;

    int const headCell = world.idx(C.get_position());
    std::vector<int> ob(body.begin(), body.end());
    if (ob.empty()) return false;

    set_body(ob);
    int const currentArea = flood(headCell, ob.back());
    if (currentArea < len * 2 + 4)
    {
        if (C.can_split(2))
        {
            C.do_split(2);
            return true;
        }
    }
    return false;
}

// --- v12 sprint to pearl (leaders + FLOOD on big maps) ----------------------
bool try_sprint_to_pearl(Controller& C, int round, Phase phase, Role role)
{
    int const length = C.get_length();
    bool const isLeader = (role == Role::LEADER && length >= 5);
    bool const isFloodBig = false;
    if (!isLeader && !isFloodBig)
        return false;

    int const headCell = world.idx(C.get_position());
    auto const dirs = Direction::get_direction_list();

    for (auto const d : dirs)
    {
        int const nc = world.cross(headCell, dir_index(d));
        if (nc >= 0 && has_pearl_at(C, nc))
            return false;
    }

    int bestScore = 0;
    Direction bestD1 = dirs[0], bestD2 = dirs[0];
    bool found = false;

    for (auto const d1 : dirs)
    {
        int const di1 = dir_index(d1);
        int const mid = world.cross(headCell, di1);
        if (mid < 0) continue;

        Position const mp(mid % world.W, mid / world.W);
        Tile const* mt = C.get_tile(mp);
        if (!mt || mt->get_dragon() != nullptr) continue;

        bool midBad = false;
        for (std::size_t i = 0; i + 2 < body.size(); i++)
            if (body[i] == mid) { midBad = true; break; }
        if (midBad) continue;

        for (auto const d2 : dirs)
        {
            int const dest = world.cross(mid, dir_index(d2));
            if (dest < 0) continue;

            Position const dp(dest % world.W, dest / world.W);
            Tile const* dt = C.get_tile(dp);
            if (!dt || dt->get_dragon() != nullptr) continue;

            bool destBad = false;
            for (std::size_t i = 0; i + 3 < body.size(); i++)
                if (body[i] == dest) { destBad = true; break; }
            if (destBad) continue;

            if (!dt->has_pearl()) continue;
            if (world.threatened(dest)) continue;

            int exits = 0;
            for (int dd = 0; dd < 4; dd++)
            {
                int const nc = world.cross(dest, dd);
                if (nc < 0) continue;
                Position const np(nc % world.W, nc / world.W);
                Tile const* nt = C.get_tile(np);
                if (!nt || nt->get_dragon() == nullptr) exits++;
            }
            if (exits < 2) continue;

            int score = 1000;

            if (isLeader)
            {
                bool enemyCompeting = false;
                for (auto const& [eid, ei] : g_enemies)
                {
                    if (!ei.isEnemy || ei.headCell < 0 || ei.lastSeen < round - 3)
                        continue;
                    if (cheby(ei.headCell, dest) <= 4)
                    {
                        enemyCompeting = true;
                        break;
                    }
                }
                if (!enemyCompeting) continue;
                score = 5000;
            }

            if (score > bestScore)
            {
                bestScore = score;
                bestD1 = d1;
                bestD2 = d2;
                found = true;
            }
        }
    }

    if (found)
    {
        C.make_moves({bestD1, bestD2});
        return true;
    }
    return false;
}

std::pair<int, int> find_nearest_enemy_head(Controller& C)
{
    int bestCell = -1, bestDist = 999;
    Team const us = C.get_team();
    int const headCell = world.idx(C.get_position());
    for (Tile const& t : C.get_tiles())
    {
        DragonPart const* p = t.get_dragon();
        if (!p || !p->is_head() || p->get_team() == us)
            continue;
        int const ec = world.idx(t.get_position());
        int const d = cheby(headCell, ec);
        if (d < bestDist) { bestDist = d; bestCell = ec; }
    }
    return {bestCell, bestDist};
}

struct FriendlyInfo { int nearCell; int nearSegs; int bigCell; int bigSegs; };

FriendlyInfo find_friendly_heads(Controller& C)
{
    FriendlyInfo info{-1, 0, -1, 0};
    int nearDist = 999;
    Team const us = C.get_team();
    int const myId = C.get_id();
    int const headCell = world.idx(C.get_position());

    struct FHead { int cell; int id; int dist; };
    std::vector<FHead> heads;

    for (Tile const& t : C.get_tiles())
    {
        DragonPart const* p = t.get_dragon();
        if (!p || !p->is_head() || p->get_team() != us || p->get_id() == myId)
            continue;
        int const fc = world.idx(t.get_position());
        int const d = cheby(headCell, fc);
        heads.push_back({fc, p->get_id(), d});
    }

    for (auto& fh : heads)
    {
        int segs = 0;
        for (Tile const& t : C.get_tiles())
        {
            DragonPart const* p = t.get_dragon();
            if (p && p->get_id() == fh.id) segs++;
        }
        if (fh.dist < nearDist) { nearDist = fh.dist; info.nearCell = fh.cell; info.nearSegs = segs; }
        if (segs > info.bigSegs) { info.bigCell = fh.cell; info.bigSegs = segs; }
    }
    return info;
}

bool try_sacrifice_kill(Controller& C, int round, Role role)
{
    if (role == Role::LEADER) return false;
    int const length = C.get_length();
    if (length >= g_ourMaxLen) return false;
    if (length < 3) return false;
    if (C.get_unit_count() <= 2) return false;
    int const sacStart = sacrifice_start_round();
    if (round < sacStart) return false;
    if (round - g_lastSacrificeRound < 10) return false;

    FriendlyInfo fi = find_friendly_heads(C);
    if (fi.bigCell < 0) return false;

    int const headCell = world.idx(C.get_position());
    int const distToLeader = cheby(headCell, fi.bigCell);
    if (distToLeader > 3) return false;

    auto const dirs = Direction::get_direction_list();
    for (auto d : dirs)
    {
        int nc = world.cross(headCell, dir_index(d));
        if (nc < 0) continue;
        for (size_t i = 1; i < body.size(); i++)
        {
            if (body[i] == nc)
            {
                C.make_move(d);
                g_lastSacrificeRound = round;
                return true;
            }
        }
    }
    return false;
}

Direction choose_move(Controller& C, int round, Phase phase, Role role)
{
    Position const head = C.get_position();
    Tile const* headTile = C.get_tile(head);
    int const length = C.get_length();
    auto const dirs = Direction::get_direction_list();

    bool const isTrauma = [&]{
        if (world.W != 48 || world.H != 24) return false;
        int kc = 0;
        for (int c = 0; c < world.N; c++)
            for (int dd = 0; dd < 4; dd++)
                if (world.edge[c][dd] == E_KELP) kc++;
        return kc > 610;
    }();

    std::vector<Direction> safe;
    for (auto const d : dirs)
        if (step_is_safe(C, headTile, head, d))
            safe.push_back(d);
    if (safe.empty())
        return C.get_dir();

    std::vector<int> origBody(body.begin(), body.end());
    SimState const base{origBody};

    bool const isLongest = (length >= g_ourMaxLen);

    set_body(origBody);
    int pearlDist = 0;
    int const pearlDir = pearl_direction(C, round, pearlDist, isLongest);

    std::vector<int> const friends = friendly_cells(C);

    int const healthy = length * 2 + 4;

    int growthCap;
    if (role == Role::LEADER)
        growthCap = 9999;
    else if (phase != Phase::FLOOD)
        growthCap = 9999;
    else if (g_openMap && C.get_unit_count() >= 50)
        growthCap = 12;
    else
        growthCap = 6;

    auto const [enemyHead, enemyDist] = find_nearest_enemy_head(C);
    auto const fi = find_friendly_heads(C);
    int const friendlyHead = fi.nearCell;
    int const friendlySegs = fi.nearSegs;

    int const leaderCell = fi.bigCell;
    int const leaderSegs = fi.bigSegs;
    int const unitCount = C.get_unit_count();
    bool const endgameSacrifice = (round >= 430 && g_separatedMap
        && role != Role::LEADER && length <= 4);

    long bestScore = LONG_MIN;
    Direction best = safe.front();
    for (auto const d : safe)
    {
        int const di = dir_index(d);

        install(base);
        SimState const s0 = simulate_step(base, di, C);
        bool const eat0 = s0.ate;
        int const newHead = s0.head();
        int const tail1 = s0.tail();

        install(s0);
        bool const reach = reaches_tail(newHead, tail1);

        int area;
        int unknownAround = 0;
        for (int dd = 0; dd < 4; dd++)
            if (world.edge[newHead][dd] == E_UNKNOWN) unknownAround++;

        if (unknownAround >= 2)
        {
            install(s0);
            int const conservativeArea = flood(newHead, tail1);
            install(s0);
            int const optimisticArea = flood_optimistic(newHead, tail1, length * 4);
            area = std::min(optimisticArea, std::max(conservativeArea * 2, conservativeArea + 20));
        }
        else
        {
            area = flood(newHead, tail1);
        }

        // Depth 2 lookahead
        std::array<SimState, 4> kids;
        int exits1 = 0;
        for (int m2 = 0; m2 < 4; m2++)
        {
            SimState child = simulate_step(s0, m2, C);
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

        long const mobility = exits1 * 100L + exits2 * 20L;
        long score = static_cast<long>(area) * 60 + mobility * 30;

        if (reach2)
            score += 200'000;
        else if (reach)
            score += 20'000;
        else if (area < length + 1)
            score -= 2'000'000;
        else if (area < healthy)
            score -= static_cast<long>(healthy - area) * 500;

        if (exits1 <= 1 && !reach2)
        {
            bool const justPortaled = (g_lastMove >= 0 &&
                world.edge[world.idx(head)][g_lastMove] == E_PORTAL);
            if (isTrauma && justPortaled)
                score -= 5000;
            else if (phase != Phase::FLOOD || justPortaled)
                score -= 60000;
            else if (area >= healthy)
                score -= 2000;
            else
                score -= 60000;
        }

        bool const mayGrow = length < growthCap;

        // v11: Higher pearl bonuses across the board
        int pearlBonus, pearlSeekBonus;
        if (length <= 2) {
            pearlBonus = 30000;
            pearlSeekBonus = 15000;
        } else if (length == 3) {
            pearlBonus = 35000;
            pearlSeekBonus = 18000;
        } else if (phase != Phase::FLOOD) {
            pearlBonus = 8000;
            pearlSeekBonus = 6000;
        } else {
            pearlBonus = 5000;
            pearlSeekBonus = 3000;
        }

        if (isLongest || role == Role::LEADER) {
            pearlBonus = pearlBonus * 2;
            pearlSeekBonus = pearlSeekBonus * 2;
        }

        if (eat0)
        {
            if (length <= 3 && reach && area >= length + 2)
                score += pearlBonus;
            else if (isLongest && reach && mayGrow && area >= length + 2)
                score += pearlBonus;
            else if (reach && reach2 && mayGrow && area >= healthy)
                score += pearlBonus;
            else
                score -= 40'000;
        }
        else if (di == pearlDir && mayGrow)
            score += (reach2 && area >= healthy) ? (static_cast<long>(pearlSeekBonus) / (pearlDist + 1)) :
                     (length <= 3 && reach ? (static_cast<long>(pearlSeekBonus) / (pearlDist + 1)) : 0);

        // Death drop attraction
        for (auto const& dd : g_deathDrops)
        {
            int const ddDist = cheby(newHead, dd.cell);
            if (ddDist <= 8)
            {
                if (g_separatedMap && !isLongest && role != Role::LEADER)
                    continue;
                int dropMult = (round >= 400 && g_separatedMap) ? 4 : 1;
                int dropBonus = dd.estimatedPearls * 500 * dropMult / (ddDist + 1);
                if (isLongest || role == Role::LEADER)
                    dropBonus *= 2;
                score += dropBonus;
            }
        }

        if (endgameSacrifice)
        {
            // Fodder sacrifice: seek danger, die to drop pearls
            if (world.threatened(newHead))
                score += 10'000;
            score -= area * 50;
        }
        else if (world.threatened(newHead))
        {
            if (role == Role::LEADER)
            {
                bool const comfortable = phase != Phase::FLOOD
                    && length >= g_maxEnemyLen + 2;
                score -= comfortable ? 200'000 : 500'000;
            }
            else if (phase != Phase::FLOOD && length >= 6)
                score -= 200'000;
            else if (length <= 3)
                score -= 5'000;
            else
                score -= 30'000;
        }

        // Echo-informed caution for leaders
        if (g_echoEnemyDetected && role == Role::LEADER)
            score -= 5000;

        // Sonar-informed navigation
        if (!endgameSacrifice)
        {
            for (auto const& sr : g_sonarReports)
            {
                int const sdist = cheby(newHead, sr.cell);
                if (sr.enemyLen == 0 && mayGrow)
                {
                    if (g_separatedMap && !isLongest && role != Role::LEADER)
                        continue;
                    score += 3000 / (sdist + 1);
                }
                else if (role == Role::LEADER)
                {
                    if (sdist <= 3) score -= 10000;
                    else if (sdist <= 5) score -= 4000;
                }
                else
                {
                    if (sdist <= 2) score -= 3000;
                }
            }
        }

        // Chase enemies (all dragons help confine enemies)
        if (enemyHead >= 0 && reach2 && length >= 3)
        {
            int const distToEnemy = cheby(newHead, enemyHead);
            if (enemyDist <= 5)
                score += 2000L / (distToEnemy + 1);
            if (distToEnemy <= 2)
                score += 3000;
        }

        // Small map aggression: fodder hunts big enemies for elimination
        if (world.N <= 400 && world.N >= 200
            && role != Role::LEADER && length <= 5
            && C.get_unit_count() > g_enemyCount + 3)
        {
            for (auto const& [eid, ei] : g_enemies)
            {
                if (!ei.isEnemy || ei.lastSeen != round || ei.visibleLen < 6)
                    continue;
                int const eDist = cheby(newHead, ei.headCell);
                if (eDist <= 4)
                    score += 5000L / (eDist + 1);
                else if (eDist <= 8)
                    score += 2000L / (eDist + 1);
            }
        }

        // Predatory confinement
        if (reach2 && area >= healthy)
        {
            install(s0);
            for (auto const& [eid, ei] : g_enemies)
            {
                if (!ei.isEnemy || ei.lastSeen != round)
                    continue;
                int const eDist = cheby(newHead, ei.headCell);

                int eSafe = 0;
                for (int ed = 0; ed < 4; ed++)
                {
                    int const enc = world.cross(ei.headCell, ed);
                    if (enc >= 0 && !world.occupied(enc))
                        eSafe++;
                }
                int const cap = std::max(ei.visibleLen * 3, 30);
                int const eArea = flood_cap(ei.headCell, -1, cap);

                if (eDist <= 3)
                {
                    if (eSafe == 0)
                        score += 5000;
                    else if (eSafe == 1 && eArea <= ei.visibleLen + 2)
                        score += 3500;
                }

                if (ei.visibleLen < length && eDist <= 5)
                {
                    if (eArea <= ei.visibleLen * 2)
                        score += 2000;
                    score += std::max(0, cap - eArea) * 60 / std::max(1, cap);
                }
            }
        }

        // Friendly collision avoidance (reversed in endgame sacrifice)
        if (friendlyHead >= 0 && !endgameSacrifice)
        {
            int const distToFriend = cheby(newHead, friendlyHead);
            if (distToFriend <= 2 && length < friendlySegs)
                score -= 200'000;
        }

        // Portal blind crossing penalty
        if (world.edge[world.idx(head)][di] == E_PORTAL &&
            C.get_tile(Position(newHead % world.W, newHead / world.W)) == nullptr)
        {
            if (isTrauma)
                score += 2000;
            else if (exits1 <= 1)
                score -= 3000;
            else
                score -= 15000;
        }

        // v11: Lighter repulsion to allow crowding (top players crowd the board)
        if (!friends.empty() && !endgameSacrifice)
        {
            int md = 99;
            for (int fc : friends)
                md = std::min(md, cheby(newHead, fc));
            score += std::min(md, 5) * 500;
        }

        // Trauma pearl cluster attraction (permanent spawners at two clusters)
        if (isTrauma && pearlDir < 0 && mayGrow)
        {
            int const nx = newHead % world.W, ny = newHead / world.W;
            int const dL = wrap_dist(nx, 11, world.W) + wrap_dist(ny, 12, world.H);
            int const dR = wrap_dist(nx, 36, world.W) + wrap_dist(ny, 11, world.H);
            int const dMin = std::min(dL, dR);
            score += 3000 / (dMin + 1);
        }

        // Exploration
        if (pearlDir < 0 || pearlDist > 5)
        {
            int unknownEdges = 0;
            for (int ed = 0; ed < 4; ed++)
                if (world.edge[newHead][ed] == E_UNKNOWN) unknownEdges++;
            score += unknownEdges * 600;
        }

        // Leader mutual repulsion (keep leaders spread)
        if (role == Role::LEADER && !friends.empty())
        {
            int md = 99;
            for (int fc : friends)
                md = std::min(md, cheby(newHead, fc));
            score += std::min(md, 8) * 1500;
        }

        if (d == C.get_dir())
            score += 1;

        if (score > bestScore)
        {
            bestScore = score;
            best = d;
        }
    }

    // Trauma: cross undiscovered portals (step_is_safe blocks these from scoring)
    if (isTrauma)
    {
        int const headCell = world.idx(head);
        for (auto const d : dirs)
        {
            int const di = dir_index(d);
            if (world.edge[headCell][di] == E_PORTAL && world.cross(headCell, di) < 0)
            {
                long const portalScore = 50000;
                if (portalScore > bestScore)
                {
                    bestScore = portalScore;
                    best = d;
                }
            }
        }
    }

    return best;
}

} // namespace

int main()
{
    auto [C, G] = unswbc::init();
    auto const [w, h] = G.get_map_size();
    world.init(w, h);

    while (unswbc::update(C, G))
    {
        int const round = G.get_round_num();
        update_role(C);
        Phase const phase = get_phase(round, C.get_unit_count(), C.unit_limit);

        absorb_vision(C, round);

        detect_symmetry();
        apply_symmetry();

        update_body(C);
        compute_other(C);
        compute_threat(C);
        compute_map_type(round);
        update_enemies(C, round);
        process_sonar(C, round);

        for (auto const& sr : g_sonarReports)
            if (sr.enemyLen > g_maxEnemyLen)
                g_maxEnemyLen = sr.enemyLen;

        if (C.has_echoes) {
            auto const& ech = C.echoes;
            g_echoEnemyDetected = (ech.enemy_head > 0 || ech.enemy > 0);
        }

        Role const role = get_role(C.get_length(), g_ourMaxLen, phase);

        // --- Action: split, sprint-to-pearl, move, emergency split as fallback ---
        int child = 0;
        bool didSplit = false;

        if (should_split(C, child, phase, role, round))
        {
            C.do_split(child);
            didSplit = true;
            g_lastMove = -1;
        }
        else if (try_sacrifice_kill(C, round, role))
        {
            g_lastMove = -1;
        }
        else
        {
            bool anySafe = false;
            for (auto d : Direction::get_direction_list())
                if (step_is_safe(C, C.get_tile(C.get_position()), C.get_position(), d))
                    anySafe = true;

            if (!anySafe)
            {
                if (try_sprint_escape(C))
                {
                    g_lastMove = -1;
                }
                else if (C.get_length() >= 8
                         && C.get_unit_count() < C.unit_limit
                         && C.can_split(2))
                {
                    C.do_split(2);
                    didSplit = true;
                    g_lastMove = -1;
                }
                else if (emergency_split(C))
                {
                    didSplit = true;
                    g_lastMove = -1;
                }
                else
                {
                    Direction const mv = choose_move(C, round, phase, role);
                    C.make_move(mv);
                    g_lastMove = dir_index(mv);
                }
            }
            else
            {
                Direction const mv = choose_move(C, round, phase, role);
                int const mvCell = world.cross(world.idx(C.get_position()), dir_index(mv));
                bool const wouldEat = mvCell >= 0 && has_pearl_at(C, mvCell);

                if (!wouldEat && try_sprint_to_pearl(C, round, phase, role))
                {
                    g_lastMove = -1;
                }
                else
                {
                    C.make_move(mv);
                    g_lastMove = dir_index(mv);
                }
            }
        }

        // Send sonar in ALL 4 directions
        pick_sonar_target(C, round);
        if (g_sonarTargetCell >= 0)
        {
            auto const msg = sonar_encode(
                g_sonarTargetCell % world.W,
                g_sonarTargetCell / world.W,
                g_sonarTargetFacing, C.get_team(), g_sonarTargetLen);
            for (auto d : Direction::get_direction_list())
                C.send_sonar(d, msg);
        }

        (void)didSplit;
        unswbc::end_turn();
    }
}
