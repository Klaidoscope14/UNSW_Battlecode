#include "helper.hpp"
#include <array>
#include <climits>
#include <queue>
#include <unordered_map>
#include <vector>
using namespace unswbc;
static constexpr int PEARL_TIME_HORIZON=8; static constexpr int INF=INT_MAX/4;
static bool edge_ok(Tile const*t,Direction d){if(!t)return false;Edge const&e=t->get_edge(d);return e.is_passable()&&!e.is_portal();}
static bool step_is_safe(Controller&C,Tile const*f,Position from,Direction d){if(!edge_ok(f,d))return false;Tile const*dt=C.get_tile(from.add_dir(d));return dt!=nullptr&&dt->get_dragon()==nullptr;}
static bool enemy_head_adjacent(Controller&C,Position dest){Team const us=C.get_team();for(auto const d:Direction::get_direction_list()){Tile const*t=C.get_tile(dest.add_dir(d));if(!t)continue;DragonPart const*p=t->get_dragon();if(p&&p->is_head()&&!(p->get_team()==us))return true;}return false;}
struct Eval{int area=0;int pearlDist=INF;int soonDist=INF;};
static Eval evaluate_from(Controller&C,Position start){Eval e;std::unordered_map<Position,int,PositionHash>dist;dist.reserve(64);std::queue<Position>q;dist[start]=0;q.push(start);while(!q.empty()){Position const p=q.front();q.pop();int const pd=dist[p];Tile const*t=C.get_tile(p);if(!t)continue;e.area++;if(t->has_pearl()&&pd<e.pearlDist)e.pearlDist=pd;int const pt=t->get_pearl_time();if(pt>=0&&pt<=PEARL_TIME_HORIZON&&pd<e.soonDist)e.soonDist=pd;for(auto const d:Direction::get_direction_list()){if(!edge_ok(t,d))continue;Position const np=p.add_dir(d);Tile const*nt=C.get_tile(np);if(!nt||nt->get_dragon()!=nullptr||dist.count(np))continue;dist[np]=pd+1;q.push(np);}}return e;}
static Direction choose_move(Controller&C){Position const head=C.get_position();Tile const*ht=C.get_tile(head);int const length=C.get_length();auto const dirs=Direction::get_direction_list();std::vector<Direction>safe;for(auto const d:dirs)if(step_is_safe(C,ht,head,d))safe.push_back(d);if(safe.empty())return C.get_dir();long bestScore=LONG_MIN;Direction best=safe.front();for(auto const d:safe){Position const dest=head.add_dir(d);Tile const*dt=C.get_tile(dest);Eval const e=evaluate_from(C,dest);long score=(long)e.area*100;if(e.area<=length)score-=1000000;if(dt&&dt->has_pearl())score+=500;else if(e.pearlDist<INF)score-=(long)e.pearlDist*12;else if(e.soonDist<INF)score-=(long)e.soonDist*6;if(enemy_head_adjacent(C,dest))score-=3000;if(d==C.get_dir())score+=1;if(score>bestScore){bestScore=score;best=d;}}return best;}
int main(){auto[C,G]=unswbc::init();while(unswbc::update(C,G)){C.make_move(choose_move(C));unswbc::end_turn();}}
