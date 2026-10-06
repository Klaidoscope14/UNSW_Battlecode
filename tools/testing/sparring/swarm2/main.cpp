// Sparring bot "swarm": modelled on the user's description of ladder opponents.
//  - multiply fast: every dragon (queen included, early) splits at length >= 4
//  - hunters: small dragons go for the enemy queen and kill her head-on
//  - guards: small dragons stay 2-3 tiles from their own queen and intercept
// Built only from the toolkit API; testing use only.
#include "helper.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <random>
#include <vector>
using namespace unswbc;

#ifndef HUNT_START
#define HUNT_START 60     // rounds of pure farming/splitting before roles
#endif
#ifndef QUEEN_SPLIT_UNTIL
#define QUEEN_SPLIT_UNTIL 150
#endif

namespace {
int W, H, R, me, L;
Team myTeam = Team::A;
std::mt19937 rng;
Position myQ{-1, -1}, enemyQ{-1, -1}, home{-1, -1};
int myQRound = -100, enemyQRound = -100;
constexpr std::uint64_t MAGIC = 0x5A;

int wrapd(int a, int b, int n) { int d = std::abs(a - b); return std::min(d, n - d); }
int mdist(Position a, Position b) { return wrapd(a.x, b.x, W) + wrapd(a.y, b.y, H); }
int cdist(Position a, Position b) { return std::max(wrapd(a.x, b.x, W), wrapd(a.y, b.y, H)); }

std::uint64_t pack(int type, Position p, int round)
{
    return (MAGIC << 56) | (std::uint64_t(type) << 48) | (std::uint64_t(p.x & 0xFF) << 40) | (std::uint64_t(p.y & 0xFF) << 32) |
           std::uint64_t(round & 0xFFFF) << 16 | std::uint64_t(myTeam == Team::A ? 1 : 2);
}
void unpack(std::uint64_t m)
{
    if ((m >> 56) != MAGIC || (m & 0xFFFF) != std::uint64_t(myTeam == Team::A ? 1 : 2))
        return;
    int const type = (m >> 48) & 0xFF, x = (m >> 40) & 0xFF, y = (m >> 32) & 0xFF, r = (m >> 16) & 0xFFFF;
    if (type == 1 && r > myQRound) { myQ = {x, y}; myQRound = r; if (home.x < 0) home = myQ; }
    if (type == 2 && r > enemyQRound) { enemyQ = {x, y}; enemyQRound = r; }
    if (type == 3 && home.x < 0) home = {x, y};
}

struct Seen { Position p; int id, len; bool mine, head; };
std::map<std::pair<int, int>, int> pearlMem; // tile -> round it should hold a pearl (seen or timed)
Position explore{-1, -1};
int exploreRound = -100;
std::vector<Seen> heads;

bool occupied(Controller& ct, Position p)
{
    auto const* t = ct.get_tile(p);
    return t && t->get_dragon();
}

// Free tiles reachable inside the window from p (bodies and kelp block).
int free_space(Controller& ct, Position p, int cap)
{
    std::deque<Position> q{p};
    std::vector<Position> seen{p};
    int n = 0;
    while (!q.empty() && n < cap)
    {
        Position c = q.front(); q.pop_front(); n++;
        auto const* t = ct.get_tile(c);
        if (!t) continue;
        for (Direction d : Direction::get_direction_list())
        {
            if (!t->get_edge(d).is_passable() || t->get_edge(d).is_portal()) continue;
            Position nx = c.add_dir(d);
            if (!nx.is_in_vision() || occupied(ct, nx) || std::find(seen.begin(), seen.end(), nx) != seen.end()) continue;
            seen.push_back(nx); q.push_back(nx);
        }
    }
    return n;
}

// Path distance inside the window (kelp-aware), falling back to wrapped Manhattan.
int path_dist(Controller& ct, Position from, Position to)
{
    if (!to.is_in_vision()) return mdist(from, to);
    std::map<std::pair<int, int>, int> dist{{{from.x, from.y}, 0}};
    std::deque<Position> q{from};
    while (!q.empty())
    {
        Position c = q.front(); q.pop_front();
        int const dc = dist[{c.x, c.y}];
        if (c == to) return dc;
        auto const* t = ct.get_tile(c);
        if (!t) continue;
        for (Direction d : Direction::get_direction_list())
        {
            if (!t->get_edge(d).is_passable() || t->get_edge(d).is_portal()) continue;
            Position nx = c.add_dir(d);
            if (!nx.is_in_vision() || dist.count({nx.x, nx.y})) continue;
            if (occupied(ct, nx) && !(nx == to)) continue;
            dist[{nx.x, nx.y}] = dc + 1; q.push_back(nx);
        }
    }
    return mdist(from, to) + 6; // walled off inside the window
}

void act(Controller& ct)
{
    Position const pos = ct.get_position();
    bool const queen = me <= 1;
    if (queen) { myQ = pos; myQRound = R; if (home.x < 0) home = pos; }

    heads.clear();
    Seen const* enemyQueenSeen = nullptr;
    for (auto const& t : ct.get_tiles())
    {
        auto const* d = t.get_dragon();
        if (!d || !d->is_head() || d->get_id() == me) continue;
        heads.push_back({t.get_position(), d->get_id(), 0, d->get_team() == myTeam, true});
    }
    for (auto const& h : heads)
    {
        if (!h.mine && h.id <= 1) { enemyQ = h.p; enemyQRound = R; enemyQueenSeen = &h; }
        if (h.mine && h.id <= 1) { myQ = h.p; myQRound = R; }
    }

    // Pearl memory: pearls in view, and timers that run out soon.
    for (auto const& t : ct.get_tiles())
    {
        auto const key = std::make_pair(t.get_position().x, t.get_position().y);
        if (t.has_pearl()) pearlMem[key] = R;
        else if (t.get_pearl_time() >= 0 && t.get_pearl_time() <= 12) pearlMem[key] = R + t.get_pearl_time();
        else pearlMem.erase(key);
    }
    // Roles.
    enum { FARM, HUNT, GUARD, QUEEN } role = queen ? QUEEN : FARM;
    if (!queen && R >= 5 && (me % 5 == 1 || me % 5 == 3))
        role = GUARD; // a ring of small guards around the queen from early on
    else if (!queen && R >= HUNT_START)
        role = (me % 3 == 2 && L <= 3) ? HUNT : FARM; // small ones hunt, long ones breed
    if (role == GUARD && (myQ.x < 0 || R - myQRound > 15)) role = HUNT;

    // Multiply: split whenever there is room (queen only early on).
    bool const splitOk = queen ? R < QUEEN_SPLIT_UNTIL : role != GUARD;
    if (splitOk && L >= 4 && ct.can_split(L / 2) && free_space(ct, pos, 12) >= 4)
    {
        ct.do_split(L / 2);
        ct.set_indicator_string("split");
        return;
    }

    // Target.
    Position target{-1, -1};
    Position attack{-1, -1};
    if (role == HUNT)
    {
        if (enemyQueenSeen) target = attack = enemyQueenSeen->p;
        else if (enemyQ.x >= 0 && R - enemyQRound <= 20) target = enemyQ;
        else if (home.x >= 0)
        {
            // Guess the enemy queen's start from our queen's start (one guess per symmetry).
            int const g = me % 3;
            target = g == 0 ? Position{W - 1 - home.x, H - 1 - home.y} : g == 1 ? Position{W - 1 - home.x, home.y} : Position{home.x, H - 1 - home.y};
        }
    }
    else if (role == GUARD)
    {
        Seen const* threat = nullptr;
        for (auto const& h : heads)
            if (!h.mine && cdist(h.p, myQ) <= 5 && (!threat || cdist(h.p, myQ) < cdist(threat->p, myQ))) threat = &h;
        if (threat) target = attack = threat->p;
        else if (cdist(pos, myQ) > 2) target = myQ; // a tight ring around the queen
    }
    if (target.x < 0)
    {
        // Pearls: nearest remembered one that will be there when we arrive.
        int best = 1 << 20;
        for (auto const& [k, when] : pearlMem)
        {
            Position const p{k.first, k.second};
            if (p.is_in_vision() && occupied(ct, p)) continue;
            int const d = path_dist(ct, pos, p);
            if (when > R + d || R - when > 40 || (queen && d > 3)) continue;
            if (d < best) { best = d; target = p; }
        }
        if (target.x < 0 && queen && home.x >= 0 && mdist(pos, home) > 4) target = home;
        if (target.x < 0 && !queen)
        {
            if (explore.x < 0 || R - exploreRound > 30 || mdist(pos, explore) <= 2)
            {
                explore = {int(rng() % W), int(rng() % H)};
                exploreRound = R;
            }
            target = explore;
        }
    }

    // Choose a step.
    double bestS = -1e18;
    Direction bestD = ct.get_dir();
    bool anyStep = false;
    auto const* here = ct.get_tile(pos);
    for (Direction d : Direction::get_direction_list())
    {
        if (!here->get_edge(d).is_passable()) continue;
        bool const portal = here->get_edge(d).is_portal();
        Position nx = pos.add_dir(d);
        double s = 0;
        if (portal) s -= (queen || role == GUARD) ? 3000 : (target.x < 0 || role == HUNT ? -200 : 300);
        if (!portal && occupied(ct, nx))
        {
            auto const* od = ct.get_tile(nx)->get_dragon();
            if (attack.x >= 0 && nx == attack && od->is_head() && od->get_team() != myTeam)
                s += 1e6; // head-on: we trade a small dragon for their queen / an intruder
            else
                continue;
        }
        if (!portal)
        {
            int const fs = free_space(ct, nx, 2 * L + 8);
            if (fs < L + 1) s -= queen ? 200000 : 2000; // dead end
            s += fs * (queen ? 60 : 5);
            for (auto const& h : heads)
                if (!h.mine && mdist(h.p, nx) == 1 && !(attack.x >= 0 && h.p == attack))
                    s -= queen ? 50000 : 800; // a head next to ours can kill us
            if (queen)
                for (auto const& h : heads)
                    if (!h.mine && mdist(h.p, nx) == 2) s -= 100000;
            if (queen)
                for (auto const& h : heads)
                    if (!h.mine) s += 300 * std::min(mdist(h.p, nx), 6);
        }
        if (target.x >= 0 && !portal) s -= 40.0 * path_dist(ct, nx, target);
        if (d == ct.get_dir()) s += 5;
        s += std::uniform_real_distribution<double>(0, 3)(rng);
        if (s > bestS) { bestS = s; bestD = d; anyStep = true; }
    }
    if (!anyStep && L >= 4 && ct.can_split(L - 2)) { ct.do_split(L - 2); return; } // boxed in: save most of the body
    ct.make_move(bestD);
    ct.set_indicator_string(role == HUNT ? "hunt" : role == GUARD ? "guard" : role == QUEEN ? "queen" : "farm");
}
} // namespace

int main()
{
    auto [ct, g] = init();
    auto [w, h] = g.get_map_size();
    W = w; H = h;
    me = ct.get_id();
    myTeam = ct.get_team();
    rng.seed(me * 7919 + 13);
    while (update(ct, g))
    {
        R = g.get_round_num();
        L = ct.get_length();
        for (auto m : ct.get_sonar_messages()) unpack(m);
        try { act(ct); } catch (...) { ct.make_move(ct.get_dir()); }
        // Share our queen, the enemy queen and home.
        auto const dirs = Direction::get_direction_list();
        if (myQ.x >= 0 && R - myQRound <= 3) { ct.send_sonar(dirs[0], pack(1, myQ, myQRound)); ct.send_sonar(dirs[2], pack(1, myQ, myQRound)); }
        if (enemyQ.x >= 0 && R - enemyQRound <= 10) ct.send_sonar(dirs[1], pack(2, enemyQ, enemyQRound));
        if (home.x >= 0) ct.send_sonar(dirs[3], pack(3, home, 0));
        end_turn();
    }
}
