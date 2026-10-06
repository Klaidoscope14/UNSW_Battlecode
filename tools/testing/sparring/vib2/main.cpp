// vib2 (vib1 with long hunters): sparring bot modelled on the tactics observed in the ladder leader's games
// (written from scratch for testing; never submitted).
//  - strike: any dragon that can reach the enemy queen's head this turn with free +
//    paid steps does so (reach = ceil(L/4) + L - 2)
//  - hunters (a third of the fleet) stalk the enemy queen 2-4 cells away
//  - factory: split 2 off at length 4 (the queen too, until round 150)
//  - trapped or pocketed small dragons self-destruct (illegal split) to recycle
//  - spawn camping: wait next to a pearl tile that is about to spawn
//  - queen keeps out of the visible enemies' strike range
//  - enemy queen sightings are shared by sonar
#include "helper.hpp"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <random>
#include <unordered_map>
#include <vector>
using namespace unswbc;

int W, H, N, R, me, L;
char myTeam;
std::vector<std::uint8_t> edgeT; // per cell*4+dir: 0 unknown, 1 open, 2 kelp, 3 portal
std::vector<int> occ;            // visible occupant id (-1 none)
std::vector<char> occMine, occHead, vis;
std::vector<int> pearlSeen;      // round a pearl was last seen (-1 none)
std::vector<int> spawnAt;        // predicted spawn round from countdowns (-1 unknown)
int eqCell = -1, eqRound = -1;   // enemy queen last sighting
int exploreTarget = -1, exploreSince = 0;
std::mt19937 rng;
int const DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
char const DC[4] = {'N', 'E', 'S', 'W'};

int nbc(int c, int d) { return ((c / W + DY[d] + H) % H) * W + (c % W + DX[d] + W) % W; }
int mdist(int a, int b)
{
    int dx = std::abs(a % W - b % W), dy = std::abs(a / W - b / W);
    return std::min(dx, W - dx) + std::min(dy, H - dy);
}
int reachOf(int len) { return (len + 3) / 4 + std::max(0, len - 2); }
bool edgeOpen(int c, int d) { int t = edgeT[c * 4 + d]; return t == 0 || t == 1; }

// BFS over open edges, avoiding visible bodies (target cell allowed).
std::vector<int> bfs(int src, int maxD, int allow = -1, std::vector<int>* par = nullptr)
{
    std::vector<int> dist(N, 1 << 29);
    std::deque<int> q{src};
    dist[src] = 0;
    while (!q.empty())
    {
        int c = q.front();
        q.pop_front();
        if (dist[c] >= maxD)
            continue;
        for (int d = 0; d < 4; d++)
        {
            if (!edgeOpen(c, d))
                continue;
            int n = nbc(c, d);
            if (dist[n] <= dist[c] + 1)
                continue;
            if (vis[n] && occ[n] >= 0 && n != allow)
                continue;
            dist[n] = dist[c] + 1;
            if (par)
                (*par)[n] = c * 4 + d;
            if (n != allow)
                q.push_back(n);
        }
    }
    return dist;
}
std::vector<int> pathTo(int src, int dst, int maxD)
{
    std::vector<int> par(N, -1);
    auto dist = bfs(src, maxD, dst, &par);
    if (dist[dst] > maxD)
        return {};
    std::vector<int> dirs;
    for (int c = dst; c != src; c = par[c] / 4)
        dirs.push_back(par[c] % 4);
    std::reverse(dirs.begin(), dirs.end());
    return dirs;
}
int floodArea(int start, int cap)
{
    std::vector<char> seen(N, 0);
    std::deque<int> q{start};
    seen[start] = 1;
    int a = 0;
    while (!q.empty() && a < cap)
    {
        int c = q.front();
        q.pop_front();
        a++;
        for (int d = 0; d < 4; d++)
        {
            if (!edgeOpen(c, d))
                continue;
            int n = nbc(c, d);
            if (seen[n] || (vis[n] && occ[n] >= 0))
                continue;
            seen[n] = 1;
            q.push_back(n);
        }
    }
    return a;
}

int main()
{
    auto [ct, game] = init();
    bool ready = false;
    while (update(ct, game))
    {
        if (!ready)
        {
            auto [w, h] = game.get_map_size();
            W = w; H = h; N = W * H;
            edgeT.assign(N * 4, 0);
            pearlSeen.assign(N, -1);
            spawnAt.assign(N, -1);
            me = ct.get_id();
            myTeam = ct.get_team().value;
            rng.seed(me * 7919 + 17);
            ready = true;
        }
        R = game.get_round_num();
        L = ct.get_length();
        Position hp = ct.get_position();
        int head = hp.y * W + hp.x;
        bool amQueen = me <= 1;
        occ.assign(N, -1);
        occMine.assign(N, 0);
        occHead.assign(N, 0);
        vis.assign(N, 0);
        std::unordered_map<int, int> partCount;
        for (Tile const& t : ct.get_tiles())
        {
            Position p = t.get_position();
            int c = p.y * W + p.x;
            vis[c] = 1;
            for (int d = 0; d < 4; d++)
            {
                Edge const& e = t.get_edge(Direction(DC[d]));
                int ty = e.get_edge_type() == EdgeType::KELP ? 2 : e.is_portal() ? 3 : 1;
                edgeT[c * 4 + d] = ty;
                edgeT[nbc(c, d) * 4 + (d + 2) % 4] = ty;
            }
            if (t.has_pearl())
                pearlSeen[c] = R;
            else
            {
                pearlSeen[c] = -1;
                if (t.get_pearl_time() > 0)
                    spawnAt[c] = R + t.get_pearl_time();
            }
            if (DragonPart const* dp = t.get_dragon())
            {
                occ[c] = dp->get_id();
                occMine[c] = dp->get_team().value == myTeam;
                occHead[c] = dp->is_head();
                partCount[dp->get_id()]++;
                if (dp->get_id() <= 1 && dp->is_head() && !occMine[c])
                {
                    eqCell = c;
                    eqRound = R;
                }
            }
        }
        for (std::uint64_t m : ct.get_sonar_messages())
            if ((m >> 48) == 0x7A11)
            {
                int c = static_cast<int>(m & 0xFFFFF), r = static_cast<int>((m >> 20) & 0xFFF);
                if (c < N && r > eqRound)
                {
                    eqCell = c;
                    eqRound = r;
                }
            }
        if (eqRound >= 0 && R - eqRound <= 3)
            for (char d : {'N', 'E', 'S', 'W'})
                ct.send_sonar(Direction(d), (std::uint64_t(0x7A11) << 48) | (std::uint64_t(eqRound) << 20) | std::uint64_t(eqCell));

        // 1. Strike the enemy queen if her head is within reach this turn.
        if (!amQueen && eqRound == R)
        {
            auto path = pathTo(head, eqCell, reachOf(L));
            if (!path.empty() && static_cast<int>(path.size()) <= reachOf(L))
            {
                std::vector<Direction> ds;
                for (int d : path)
                    ds.push_back(Direction(DC[d]));
                ct.make_moves(ds);
                end_turn();
                continue;
            }
        }

        // Enemy heads in view (and their visible length) for safety.
        std::vector<std::pair<int, int>> enemyHeads;
        for (int c = 0; c < N; c++)
            if (vis[c] && occ[c] >= 0 && occHead[c] && !occMine[c])
                enemyHeads.push_back({c, partCount[occ[c]]});

        // 2. Candidate single steps.
        struct Cand { int d, dest, area; bool ok; double s; };
        std::vector<Cand> cands;
        for (int d = 0; d < 4; d++)
        {
            if (!edgeOpen(head, d) || edgeT[head * 4 + d] == 3)
                continue;
            int n = nbc(head, d);
            if (vis[n] && occ[n] >= 0)
                continue;
            int a = floodArea(n, 2 * L + 6);
            cands.push_back({d, n, a, a >= L + 2, 0});
        }
        bool anyOk = false;
        for (auto& c : cands)
            anyOk |= c.ok;
        // 3. Trapped: self-destruct (recycle) rather than wander into a wall.
        if (cands.empty() || (!anyOk && !amQueen && L <= 3))
        {
            std::cout << "SPLIT 1\n";
            end_turn();
            continue;
        }

        // 4. Factory splitting.
        int unitCap = std::min(game.get_unit_limit() - 2, std::max(4, N / 14));
        bool queenGrows = amQueen && R >= 150;
        bool nearEnemy = false;
        for (auto& e : enemyHeads)
            if (mdist(e.first, head) <= 3)
                nearEnemy = true;
        bool const longHunter = !amQueen && me % 3 == 0 && L < 9; // hunters grow to 6-9 before splitting
        if (L >= 4 && !queenGrows && !nearEnemy && !longHunter && ct.get_unit_count() < unitCap && ct.can_split(2))
        {
            ct.do_split(2);
            end_turn();
            continue;
        }

        // 5. Target.
        int target = -1;
        bool hunter = !amQueen && me % 3 == 0 && L >= 6;
        bool stalk = false;
        auto distH = bfs(head, 200);
        if (hunter && eqRound >= 0 && R - eqRound <= 30)
        {
            target = eqCell;
            stalk = true;
        }
        if (target < 0)
        {
            int best = -1, bd = 1 << 29;
            for (int c = 0; c < N; c++)
            {
                int dd = distH[c];
                if (dd >= (1 << 29))
                    continue;
                bool pearl = pearlSeen[c] >= 0 && R - pearlSeen[c] <= 40;
                bool soon = spawnAt[c] >= R && spawnAt[c] - R <= dd + 3; // camp a spawn that is due
                if (!(pearl || soon))
                    continue;
                if (amQueen && dd > 12)
                    continue;
                if (dd < bd)
                {
                    bd = dd;
                    best = c;
                }
            }
            target = best;
        }
        if (target < 0)
        {
            if (exploreTarget < 0 || head == exploreTarget || R - exploreSince > 40 || distH[exploreTarget] >= (1 << 29))
            {
                exploreTarget = std::uniform_int_distribution<int>(0, N - 1)(rng);
                exploreSince = R;
            }
            target = amQueen ? -1 : exploreTarget;
        }
        std::vector<int> distT;
        if (target >= 0)
            distT = bfs(target, 200, head);

        // 6. Score moves.
        int best = -1;
        double bestS = -1e18;
        for (auto& c : cands)
        {
            double s = c.ok ? 0 : -1e6;
            s += std::min(c.area, 2 * L + 6) * 10.0;
            if (pearlSeen[c.dest] == R)
                s += 300;
            if (target >= 0 && distT[c.dest] < (1 << 29))
            {
                int dt = distT[c.dest];
                if (stalk && dt <= 2)
                    s -= 400; // hold at a distance and wait for her to come within reach
                else
                    s -= 100.0 * dt;
            }
            for (auto& e : enemyHeads)
            {
                int dd = mdist(e.first, c.dest);
                if (amQueen)
                {
                    if (dd <= reachOf(e.second) + 1)
                        s -= 1e5 * (reachOf(e.second) + 2 - dd);
                }
                else if (dd <= 1)
                    s -= 2000;
            }
            if (s > bestS)
            {
                bestS = s;
                best = c.d;
            }
        }
        // Free steps: follow the path to the target while it stays free.
        int freeN = (L + 3) / 4;
        if (freeN >= 2 && target >= 0 && !amQueen && !stalk)
        {
            auto path = pathTo(head, target, 60);
            if (path.size() >= 2 && path[0] == best)
            {
                std::vector<Direction> ds;
                for (int k = 0; k < freeN && k < static_cast<int>(path.size()); k++)
                    ds.push_back(Direction(DC[path[k]]));
                ct.make_moves(ds);
                end_turn();
                continue;
            }
        }
        ct.make_move(Direction(DC[best]));
        end_turn();
    }
}
