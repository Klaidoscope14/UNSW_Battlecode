// Fleet bot: numbers and information first, then size and control.
//
// Every dragon runs its own copy of this program and shares nothing but sonar,
// so the "fleet" behaviour is emergent: each dragon keeps a persistent model of
// the map, tells teammates what it learns, and makes decisions that stay
// consistent with what the others are likely to decide from the same facts.
// Dragons 0 and 1 are the queens; after round 500 the longer living queen wins.
//
// Layout (Architecture_v2.txt): one translation unit, the modules below are
// included in dependency order.
//   config.hpp     every switch, grouped by module
//   core.hpp       grid geometry, sizes, rng, decision trace
//   world.hpp      L0 what we know (edges, portals, symmetry, pearls, dragons, reports)
//   state.hpp      this dragon's strategy state and phase flags
//   comms.hpp      L7 wire format and outboxes
//   perceive.hpp   L0 reading vision / body / sonar, learning, death detection
//   graph.hpp      L1 occupancy over time, flood fill, BFS, survival search, dives
//   fields.hpp     L1 per-turn maps: distances, richness, Voronoi, enemy reach
//   situation.hpp  L2 phases, roles by id, leaders / giant / heir, tiebreak guess
//   topology.hpp   L1 corridors, pockets, dead ends, free loops
//   commander.hpp  L3 team assignment: pearls, hunter / wall slots, packs, prey
//   intent.hpp     L5 goal choosers (economy, queen rest / flee / bunker, screens)
//   tactics.hpp    L4 move candidates, threat terms, lookahead, strike paths
//   act.hpp        L6 split gates, moves / sprints / splits, sprint planner
//   decide.hpp     the per-turn decision
//   speak.hpp      L7 the four sonar rays
// Per turn: perceive -> fleet state -> decide -> speak (see main() below).

// ===== helper.hpp =====

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace unswbc {

// Limits the engine holds every game to.
struct Constants {
    static constexpr int MAX_ROUNDS = 500;
    static constexpr int VISION_RADIUS = 3;
    static constexpr int VISION_SIZE = 2 * VISION_RADIUS + 1;
    static constexpr int INITIAL_LENGTH = 3;
    static constexpr int MIN_SIZE = 2;
    static constexpr int PROTOCOL_MAJOR = 3;
};

// The protocol's direction letters.
class Direction {
  public:
    enum Value : char {
        NORTH = 'N',
        EAST = 'E',
        SOUTH = 'S',
        WEST = 'W'
    };
    Value value;

    // Throws if the letter is not one of N, E, S or W.
    Direction(Value value) : value(value) {
        if (value != NORTH && value != EAST && value != SOUTH && value != WEST)
            throw std::invalid_argument("invalid direction");
    }
    Direction(char value) : Direction(static_cast<Value>(value)) {}
    constexpr bool operator==(Direction const&) const = default;

    // North, east, south and west, in that order.
    static std::array<Direction, 4> get_direction_list() {
        static std::array<Direction, 4> const direction_list{NORTH, EAST, SOUTH, WEST};
        return direction_list;
    }

    // The step this direction makes, as dx east and dy south.
    std::pair<int, int> get_offset() const {
        switch (value) {
        case NORTH: return {0, -1};
        case EAST:  return {1, 0};
        case SOUTH: return {0, 1};
        default:    return {-1, 0};
        }
    }

    Direction get_opposite() const {
        return value == NORTH ? Direction(SOUTH)
             : value == EAST  ? Direction(WEST)
             : value == SOUTH ? Direction(NORTH)
                              : Direction(EAST);
    }
    Direction get_left() const {
        return value == NORTH ? Direction(WEST)
             : value == EAST  ? Direction(NORTH)
             : value == SOUTH ? Direction(EAST)
                              : Direction(SOUTH);
    }
    Direction get_right() const {
        return value == NORTH ? Direction(EAST)
             : value == EAST  ? Direction(SOUTH)
             : value == SOUTH ? Direction(WEST)
                              : Direction(NORTH);
    }
};

inline std::ostream& operator<<(std::ostream& stream, Direction direction) {
    return stream << direction.value;
}

// The protocol's team letters.
class Team {
  public:
    enum Value : char {
        A = 'A',
        B = 'B'
    };
    Value value;

    // Throws if the letter is not A or B.
    Team(Value value) : value(value) {
        if (value != A && value != B)
            throw std::invalid_argument("invalid team");
    }
    Team(char value) : Team(static_cast<Value>(value)) {}
    constexpr bool operator==(Team const&) const = default;

    Team get_enemy_team() const {
        return value == B ? A : B;
    }
};

class Game;
class Controller;

inline Game* game = nullptr;
inline Controller* ct = nullptr;

// Board coordinates, with y growing south from 0 at the top.
struct Position {
    int x = 0, y = 0;

    Position() = default;
    Position(int x, int y) : x(x), y(y) {}
    constexpr bool operator==(Position const&) const = default;

    // The tile one step away, wrapped around the map.
    Position add_dir(Direction direction) const;
    bool is_in_map() const;
    // True if the position is inside the dragon's window this turn.
    bool is_in_vision() const;
};

struct PositionHash {
    std::size_t operator()(Position const& position) const {
        auto const x = static_cast<std::uint32_t>(position.x);
        auto const y = static_cast<std::uint32_t>(position.y);
        return (static_cast<std::uint64_t>(x) << 32U) | y;
    }
};

// The board, and the round being played.
class Game {
  public:
    int round_num = 1;
    int width, height;
    int unit_limit;

    Game(int width, int height, int unit_limit) : width(width), height(height), unit_limit(unit_limit) {}

    int get_round_num() const { return round_num; }
    // Width then height.
    std::pair<int, int> get_map_size() const { return {width, height}; }
    int get_unit_limit() const { return unit_limit; }
};

inline Position Position::add_dir(Direction direction) const {
    if (!game)
        throw std::runtime_error("game is not initialised");
    auto const [dx, dy] = direction.get_offset();
    auto const wrap = [](int value, int size) { return (value % size + size) % size; };
    return {wrap(x + dx, game->width), wrap(y + dy, game->height)};
}

inline bool Position::is_in_map() const {
    if (!game)
        throw std::runtime_error("game is not initialised");
    return x >= 0 && y >= 0 && x < game->width && y < game->height;
}

// A segment of a dragon, standing on a tile.
class DragonPart {
  public:
    Position position;
    int dragon_id;
    Team team;
    Direction dir;
    bool is_dragon_head;

    DragonPart(Position position, int dragon_id, Team team, Direction direction, bool is_dragon_head)
        : position(position), dragon_id(dragon_id), team(team), dir(direction), is_dragon_head(is_dragon_head) {}

    Position get_position() const { return position; }
    int get_id() const { return dragon_id; }
    Team get_team() const { return team; }
    // The way this segment faces.
    // A head's facing is its heading, a body segment's points towards the head.
    Direction get_dir() const { return dir; }
    bool is_head() const { return is_dragon_head; }
};

// What lies on the edge between two tiles.
enum class EdgeType {
    EMPTY,
    KELP,
    PORTAL,
};

// One side of a tile, which a dragon crosses to leave it.
class Edge {
  public:
    bool is_horizontal = false;
    EdgeType edge_type = EdgeType::EMPTY;
    std::optional<int> portal_id;

    Edge() = default;
    Edge(bool is_horizontal, EdgeType edge_type, std::optional<int> portal_id = std::nullopt)
        : is_horizontal(is_horizontal), edge_type(edge_type), portal_id(portal_id) {}

    // False for kelp.
    bool is_passable() const { return edge_type != EdgeType::KELP; }
    // True if crossing the edge teleports the dragon to the partner edge.
    bool is_portal() const { return edge_type == EdgeType::PORTAL; }
    EdgeType get_edge_type() const { return edge_type; }
    // -1 when the edge is not a portal.
    int get_portal_id() const { return portal_id.value_or(-1); }
};

// One of the 49 tiles the dragon can see this turn.
class Tile {
  public:
    Position position;
    int pearl_time = 0;
    bool pearl = false;
    std::optional<DragonPart> dragon_part;
    std::array<Edge, 4> edges{}; // {NORTH, EAST, SOUTH, WEST}

    Tile(Position position, std::optional<DragonPart> dragon_part = std::nullopt, int pearl_time = 0, bool has_pearl = false,
         std::array<Edge, 4> edges = {})
        : position(position), pearl_time(pearl_time), pearl(has_pearl), dragon_part(std::move(dragon_part)),
          edges(std::move(edges)) {}

    Edge const& get_edge(Direction direction) const {
        return edges[get_direction_index(direction)];
    }
    Edge& get_edge(Direction direction) {
        return edges[get_direction_index(direction)];
    }
    // Null if no dragon segment is on the tile.
    DragonPart const* get_dragon() const {
        return dragon_part.has_value() ? &*dragon_part : nullptr;
    }
    DragonPart* get_dragon() {
        return dragon_part.has_value() ? &*dragon_part : nullptr;
    }
    bool has_pearl() const { return pearl; }
    // Rounds until a pearl tries to spawn; -1 if it never does.
    int get_pearl_time() const { return pearl_time; }
    Position get_position() const { return position; }

  private:
    static std::size_t get_direction_index(Direction direction) {
        switch (direction.value) {
        case Direction::NORTH: return 0;
        case Direction::EAST:  return 1;
        case Direction::SOUTH: return 2;
        default:               return 3;
        }
    }
};

// The 7x7 window of tiles the dragon can see.
class Vision {
  public:
    std::vector<Tile> tiles;
    std::unordered_map<Position, std::size_t, PositionHash> tiles_by_position;

    Vision(std::vector<Tile> tiles = {},
           std::unordered_map<Position, std::size_t, PositionHash> tiles_by_position = {})
        : tiles(std::move(tiles)), tiles_by_position(std::move(tiles_by_position)) {
        if (this->tiles_by_position.empty()) {
            for (std::size_t index = 0; index < this->tiles.size(); index++) {
                auto const [_, inserted] = this->tiles_by_position.emplace(this->tiles[index].position, index);
                if (!inserted)
                    throw std::runtime_error("duplicate tile position");
            }
        }
    }

    // Row by row, starting three tiles north and three west of the head.
    std::vector<Tile> const& get_tiles() const { return tiles; }
    
    // Null outside the 7x7 window.
    Tile const* get_tile(Position position) const {
        auto const tile_index = tiles_by_position.find(position);
        return tile_index == tiles_by_position.end() ? nullptr : &tiles[tile_index->second];
    }
    Tile* get_tile(Position position) {
        auto const tile_index = tiles_by_position.find(position);
        return tile_index == tiles_by_position.end() ? nullptr : &tiles[tile_index->second];
    }
};

struct SonarEchoes {
    int kelp = 0;
    int ally = 0;
    int ally_head = 0;
    int enemy = 0;
    int enemy_head = 0;
};

// What the dragon knows about itself and what it can see this turn.
class Controller {
  public:
    int length = Constants::INITIAL_LENGTH;
    int unit_count = 1;
    int unit_limit;
    DragonPart head;
    Vision vision;
    std::vector<std::uint64_t> sonar_messages;
    SonarEchoes sonar_echoes;

    Controller(int dragon_id, Team team, Direction direction, Vision vision, int unit_limit,
               std::vector<std::uint64_t> sonar_messages = {})
        : unit_limit(unit_limit), head(Position(0, 0), dragon_id, team, direction, true),
          vision(std::move(vision)), sonar_messages(std::move(sonar_messages)) {}

    // Segments, head included.
    int get_length() const { return length; }
    // Living dragons on this dragon's team.
    int get_unit_count() const { return unit_count; }
    DragonPart& get_head() { return head; }
    DragonPart const& get_head() const { return head; }
    // Unique within the game, and a split child gets the next one.
    int get_id() const { return head.dragon_id; }
    Team get_team() const { return head.team; }
    Direction get_dir() const { return head.dir; }
    // Where the dragon's head is.
    Position get_position() const { return head.get_position(); }

    Vision& get_vision() { return vision; }
    Vision const& get_vision() const { return vision; }

    std::vector<Tile> const& get_tiles() const { return vision.get_tiles(); }
    // Null outside the 7x7 window.
    Tile const* get_tile(Position position) const { return vision.get_tile(position); }
    Tile* get_tile(Position position) { return vision.get_tile(position); }

    // The last action printed in a turn is the one the engine applies.
    void make_move(Direction direction) {
        std::cout << "MOVE " << direction.value << "\n";
    }
    // One tile per direction, all in this turn.
    // The helper sends whatever you pass. A dragon of length L takes its first
    // ceil(L / 4) steps free and pays a segment for each step after them, and it
    // must keep at least 2 segments.
    void make_moves(std::vector<Direction> const& directions) {
        std::cout << "MOVE ";
        for (Direction direction : directions)
            std::cout << direction.value;
        std::cout << "\n";
    }
    // True if child_size segments can be split off this turn.
    bool can_split(int child_size) const {
        return Constants::MIN_SIZE <= child_size && length - child_size >= Constants::MIN_SIZE && unit_count < unit_limit;
    }
    // Splits child_size segments off the tail as a new dragon.
    // The child is those segments reversed, and takes its own turn later in
    // the same round.
    void do_split(int child_size) {
        std::cout << "SPLIT " << child_size << "\n";
    }
    // Attaches a message to this turn, which the replay shows.
    // Each line costs points, see the Timeouts page.
    template <typename... Messages> void output_log(Messages const&... messages) {
        std::cout << "LOG";
        ((std::cout << " " << messages), ...);
        std::cout << "\n";
    }
    // Draws a dot on the board in the replay.
    void draw_indicator_dot(Position position, int r, int g, int b) {
        std::cout << "DOT " << position.x << " " << position.y << " " << r << " " << g << " " << b << "\n";
    }
    // Draws a line on the board in the replay.
    void draw_indicator_line(Position start, Position end, int r, int g, int b) {
        std::cout << "LINE " << start.x << " " << start.y << " " << end.x << " " << end.y << " " << r << " " << g << " " << b << "\n";
    }
    // Labels the dragon for this turn in the replay.
    void set_indicator_string(std::string_view message) {
        std::cout << "INDICATOR " << message << "\n";
    }
    // This turn's messages, in the order they were sent.
    std::vector<std::uint64_t> get_sonar_messages() const { return sonar_messages; }
    SonarEchoes get_sonar_echoes() const { return sonar_echoes; }
    void send_sonar(Direction direction, std::uint64_t message) {
        std::cout << "SONAR " << static_cast<char>(direction.value) << " " << message << "\n";
    }
    bool send_sonar(std::uint64_t message) {
        if (message > UINT32_MAX)
            return false;
        std::cout << "SONAR " << message << "\n";
        return true;
    }
};

inline bool Position::is_in_vision() const {
    if (!game || !ct)
        throw std::runtime_error("game and controller are not initialised");
    if (!is_in_map())
        return false;
    Position const centre = ct->get_position();
    auto const wrap = [](int value, int size) { return (value % size + size) % size; };
    int const column = wrap(x - (centre.x - Constants::VISION_RADIUS), game->width);
    int const row = wrap(y - (centre.y - Constants::VISION_RADIUS), game->height);
    return column < Constants::VISION_SIZE && row < Constants::VISION_SIZE;
}

namespace parse_util {

inline std::unique_ptr<Controller> controller_storage;
inline std::unique_ptr<Game> game_storage;

inline std::vector<std::string> read_data_line() {
    for (std::string line; std::getline(std::cin, line);) {
        if (auto const comment = line.find('#'); comment != std::string::npos)
            line.erase(comment);
        std::istringstream stream(line);
        std::vector<std::string> parts;
        for (std::string part; stream >> part;)
            parts.push_back(std::move(part));

        if (!parts.empty())
            return parts;
    }
    throw std::runtime_error("unexpected end of controller input");
}

inline std::vector<std::string> read_labelled(std::string_view label, std::size_t value_count = 1) {
    auto parts = read_data_line();
    if (parts.front() != label)
        throw std::runtime_error("expected " + std::string(label) + ", got " + parts.front());
    if (parts.size() != value_count + 1)
        throw std::runtime_error(std::string(label) + " requires " + std::to_string(value_count) + " value(s)");
    parts.erase(parts.begin());
    return parts;
}

template <typename T> T parse_integer(std::string_view text) {
    T value{};
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("invalid integer: " + std::string(text));
    return value;
}

inline Edge parse_edge(std::string_view value, bool is_horizontal) {
    if (value == ".")
        return Edge(is_horizontal, EdgeType::EMPTY);
    if (value == "w")
        return Edge(is_horizontal, EdgeType::KELP);
    int const portal_id = parse_integer<int>(value);
    if (portal_id < 0)
        throw std::runtime_error("portal IDs cannot be negative");
    return Edge(is_horizontal, EdgeType::PORTAL, portal_id);
}

inline char read_char_safe(std::string_view str) {
    if (str.empty())
        throw std::runtime_error("expected non-empty character token");
    return str.front();
}

} // end namespace parse_util

// Reads the spawn block. The full buffer it sets is what makes a turn's
// output cost one write.
inline std::tuple<Controller&, Game&> init() {
    // The judge's stdout looks like a terminal, so it is line buffered unless
    // we ask otherwise, and every line costs a write. One buffer, one write a
    // turn, flushed by end_turn. Leave sync_with_stdio alone: it is what puts
    // std::cout through this buffer.
    std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    int const dragon_id = parse_util::parse_integer<int>(parse_util::read_labelled("ID").front());
    Team const team(parse_util::read_char_safe(parse_util::read_labelled("TEAM").front()));

    auto const dimensions = parse_util::read_labelled("MAP", 2);
    int const width = parse_util::parse_integer<int>(dimensions[0]);
    int const height = parse_util::parse_integer<int>(dimensions[1]);
    if (width <= 0 || height <= 0)
        throw std::runtime_error("map dimensions must be positive");
    int const unit_limit = parse_util::parse_integer<int>(parse_util::read_labelled("UNIT_LIMIT").front());
    if (unit_limit <= 0)
        throw std::runtime_error("UNIT_LIMIT must be positive");

    parse_util::game_storage = std::make_unique<Game>(width, height, unit_limit);
    parse_util::controller_storage = std::make_unique<Controller>(dragon_id, team, Direction::NORTH, Vision{}, unit_limit);
    ct = parse_util::controller_storage.get();
    game = parse_util::game_storage.get();
    return {*ct, *game};
}

// Reads the next turn into the controller. False once the game is over or the
// dragon has died.
inline bool update(Controller& controller, Game& game_state) {
    auto const continue_flag = parse_util::read_data_line();
    if (continue_flag.front() == "ENDGAME")
        return false;
    if (continue_flag.size() != 2)
        throw std::runtime_error("malformed: no round number");

    game_state.round_num = parse_util::parse_integer<int>(continue_flag[1]);

    Direction const current_direction(parse_util::read_char_safe(parse_util::read_labelled("DIR").front()));
    controller.head.dir = current_direction;
    controller.length = parse_util::parse_integer<int>(parse_util::read_labelled("LENGTH").front());
    controller.unit_count = parse_util::parse_integer<int>(parse_util::read_labelled("UNIT_COUNT").front());
    if (controller.unit_count <= 0 || controller.unit_count > controller.unit_limit)
        throw std::runtime_error("UNIT_COUNT must be between 1 and UNIT_LIMIT");

    int const num_sonar_msgs = parse_util::parse_integer<int>(parse_util::read_labelled("NUM_MSGS").front());
    if (num_sonar_msgs < 0)
        throw std::runtime_error("NUM_MSGS cannot be negative");
    controller.sonar_messages.clear();
    controller.sonar_messages.reserve(static_cast<std::size_t>(num_sonar_msgs));
    for (int index = 0; index < num_sonar_msgs; index++) {
        auto const line = parse_util::read_data_line();
        if (line.size() != 1)
            throw std::runtime_error("missing sonar message value");
        controller.sonar_messages.push_back(parse_util::parse_integer<std::uint64_t>(line.front()));
    }

    auto first_tile_values = parse_util::read_data_line();
    controller.sonar_echoes = {};
    if (first_tile_values.front() == "ECHOES") {
        if (first_tile_values.size() != 6)
            throw std::runtime_error("ECHOES requires 5 values");
        auto const echo = [&](std::size_t index) { return parse_util::parse_integer<int>(first_tile_values[index]); };
        controller.sonar_echoes = {echo(1), echo(2), echo(3), echo(4), echo(5)};
        first_tile_values = parse_util::read_data_line();
    }

    std::vector<Tile> tiles;
    std::unordered_map<Position, std::size_t, PositionHash> tile_indices;
    tiles.reserve(Constants::VISION_SIZE * Constants::VISION_SIZE);
    for (int index = 0; index < Constants::VISION_SIZE * Constants::VISION_SIZE; index++) {
        auto const values = index == 0 ? first_tile_values : parse_util::read_data_line();
        if (values.size() != 4)
            throw std::runtime_error("each vision tile requires: x y hasPearl pearlIn");

        int const x = parse_util::parse_integer<int>(values[0]);
        int const y = parse_util::parse_integer<int>(values[1]);
        int const has_pearl = parse_util::parse_integer<int>(values[2]);
        int const pearl_time = parse_util::parse_integer<int>(values[3]);
        if (has_pearl != 0 && has_pearl != 1)
            throw std::runtime_error("hasPearl must be 0 or 1");

        Position const position(x, y);
        if (tile_indices.contains(position))
            throw std::runtime_error("duplicate vision tile");

        tile_indices.emplace(position, tiles.size());
        tiles.emplace_back(position, std::nullopt, pearl_time, has_pearl == 1);
    }

    int const num_dragon_parts = parse_util::parse_integer<int>(parse_util::read_labelled("DRAGON_BODIES").front());
    if (num_dragon_parts < 0)
        throw std::runtime_error("DRAGON_BODIES cannot be negative");

    bool found_controller_head = false;
    for (int index = 0; index < num_dragon_parts; index++) {
        auto const values = parse_util::read_data_line();
        if (values.size() != 6)
            throw std::runtime_error("each dragon part requires: team dragonId x y facing isHead");

        Team const team(parse_util::read_char_safe(values[0]));
        int const dragon_id = parse_util::parse_integer<int>(values[1]);
        Position const position(parse_util::parse_integer<int>(values[2]), parse_util::parse_integer<int>(values[3]));
        Direction const facing(parse_util::read_char_safe(values[4]));

        int const head_flag = parse_util::parse_integer<int>(values[5]);
        if (head_flag != 0 && head_flag != 1)
            throw std::runtime_error("isHead must be 0 or 1");

        auto const tile = tile_indices.find(position);
        if (tile == tile_indices.end())
            throw std::runtime_error("dragon part is outside the vision tiles");

        DragonPart part(position, dragon_id, team, facing, head_flag == 1);
        if (dragon_id == controller.get_id() && head_flag == 1) {
            controller.head = part;
            controller.head.dir = current_direction;
            found_controller_head = true;
        }
        tiles[tile->second].dragon_part = part;
    }
    if (!found_controller_head)
        throw std::runtime_error("controller head is missing from DRAGON_BODIES");

    Position const head_position = controller.get_position();
    auto const wrap = [](int value, int size) { return (value % size + size) % size; };
    auto const tile_at = [&](int local_x, int local_y) -> Tile& {
        Position const position(wrap(head_position.x + local_x - Constants::VISION_RADIUS, game_state.width),
                                wrap(head_position.y + local_y - Constants::VISION_RADIUS, game_state.height));
        auto const found = tile_indices.find(position);
        if (found == tile_indices.end())
            throw std::runtime_error("vision is missing expected tile");
        return tiles[found->second];
    };

    for (int y = 0; y < Constants::VISION_SIZE + 1; y++) {
        auto const line = parse_util::read_data_line();
        if (line.size() != Constants::VISION_SIZE)
            throw std::runtime_error("horizontal edge row requires 7 values");

        for (int x = 0; x < Constants::VISION_SIZE; x++) {
            Edge edge = parse_util::parse_edge(line[x], true);
            if (y > 0)
                tile_at(x, y - 1).edges[2] = edge;
            if (y < Constants::VISION_SIZE)
                tile_at(x, y).edges[0] = edge;
        }
    }

    for (int y = 0; y < Constants::VISION_SIZE; y++) {
        auto const line = parse_util::read_data_line();
        if (line.size() != Constants::VISION_SIZE + 1)
            throw std::runtime_error("vertical edge row requires 8 values");

        for (int x = 0; x < Constants::VISION_SIZE + 1; x++) {
            Edge edge = parse_util::parse_edge(line[x], false);
            if (x > 0)
                tile_at(x - 1, y).edges[1] = edge;
            if (x < Constants::VISION_SIZE)
                tile_at(x, y).edges[3] = edge;
        }
    }

    controller.vision = Vision(std::move(tiles), std::move(tile_indices));
    return true;
}

// Ends the turn and flushes, which is the one write a turn costs.
inline void end_turn() {
    std::cout << "PROTOCOL " << Constants::PROTOCOL_MAJOR << "\nENDTURN" << std::endl;
}

} // end namespace unswbc

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace unswbc;

// ===== config.hpp =====
// CONFIG - every tunable switch, grouped by module. Override with -D or a #define before this
// header (tools/testing/suite.sh and trace.py prepend #defines to main.cpp). Every name must be unique:
// run tools/testing/check_switches.py. Switches marked OFF were tested and are kept for experiments.

// ====================================================================================================
// OPENING, FLEET SIZE AND SPLITS
// ====================================================================================================
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
#ifndef SPLIT_DENSITY
#define SPLIT_DENSITY 32 // at most one dragon per this many tiles (small maps get crowded)
#endif
#ifndef TRANS_SHIFT
#define TRANS_SHIFT 100 // leader phase starts this much earlier than the section-12 table (tuned)
#endif
#ifndef EXPANSION
#define EXPANSION 1 // numbers first: everyone (queen included) splits from round 0 up to a denser cap
#endif
#ifndef EXP_DENSITY
#define EXP_DENSITY 12 // expansion cap: one dragon per this many tiles (64 at most)
#endif
#ifndef EXP_END
#define EXP_END 350 // after this round the normal SPLIT_DENSITY cap applies again
#endif
#ifndef FAST_OPEN
#define FAST_OPEN 1 // 2.1: hunters split at EARLY_SPLIT_MIN_LEN too while the fleet is still expanding
#endif
#ifndef LANE_SPLIT
#define LANE_SPLIT 1 // 2.2: a dragon whose tail sits among spawn tiles splits there, the child inherits the lane
#endif
#ifndef LANE_TILES
#define LANE_TILES 3 // spawn tiles within 3 of the tail that make a lane
#endif
#ifndef LANE_DENSITY
#define LANE_DENSITY 8 // lane splits allowed up to one dragon per this many tiles
#endif
#ifndef RUNNERS
#define RUNNERS 1 // 2.5: explorer-role dragons on big maps stay length >= 5 (2 free steps a turn)
#endif
#ifndef RUNNER_MIN_CELLS
#define RUNNER_MIN_CELLS 1200
#endif
#ifndef RUNNER_KEEP
#define RUNNER_KEEP 5
#endif
#ifndef RUNNER_SPLIT_LEN
#define RUNNER_SPLIT_LEN 8
#endif
#ifndef SPLIT_ROOM
#define SPLIT_ROOM 0 // (v2.14.4: off - faster splitting) growth splits only where the child has room (no children born into pockets)
#endif
#ifndef DYN_ALLOC
#define DYN_ALLOC 1 // numbers first on every map until a switch round set by how many enemies we see, then length
#endif
#ifndef SW_LO
#define SW_LO 260 // switch round when we see (almost) no enemies...
#endif
#ifndef SW_HI
#define SW_HI 400 // ...and when we see SW_E_HI or more (linear in between); tuned by backtest
#endif
#ifndef SW_E_HI
#define SW_E_HI 30
#endif
#ifndef SWITCH_FIXED
#define SWITCH_FIXED 0 // test only: force one switch round
#endif
#ifndef DYN_DENSITY
#define DYN_DENSITY 6 // fleet cap before the switch: one dragon per this many cells (64-2 at most)
#endif
#ifndef QUEEN_SLOTS
#define QUEEN_SLOTS 3 // unit slots only the queen may fill (her survival split must never be refused)
#endif
#ifndef HALF_SPLIT
#define HALF_SPLIT 1 // 26: mid-game, a dragon this long splits in half (leaders too on congested maps)
#endif
#ifndef HALF_SPLIT_LEN
#define HALF_SPLIT_LEN 20
#endif
#ifndef PEARL_BEHIND_GAIN
#define PEARL_BEHIND_GAIN 4 // tail must be this much closer to use a pearl split
#endif
#ifndef PEARL_BEHIND_MAX
#define PEARL_BEHIND_MAX 10 // ...and the pearl at most this far from the tail
#endif
#ifndef SPLIT_MORE
#define SPLIT_MORE 1 // hunters keep splitting at EARLY_SPLIT_MIN_LEN for the first EARLY_SPLIT_ROUNDS (numbers first)
#endif
#ifndef LEADER_MIN_LEN
#define LEADER_MIN_LEN 6 // F1: a dragon elects itself leader only from this length (big maps had ~30 leaders, some L2)
#endif
#ifndef OPEN_PLAN
#define OPEN_PLAN 1 // until we have the majority: no hunters / wall / runners (all collect and split at 4), the queen
                    // splits down to 2 and explores when no teammate is near her; hunting starts with the majority
#endif
#ifndef OPEN_MAJ_UNITS
#define OPEN_MAJ_UNITS 8 // OPEN_PLAN majority needs at least this many dragons
#endif
#ifndef MAJ_ON
#define MAJ_ON 1.4 // majority from this many x their estimated dragons...
#endif
#ifndef MAJ_OFF
#define MAJ_OFF 1.1 // ...lost again below this
#endif
#ifndef QUEEN_START_SPLIT
#define QUEEN_START_SPLIT 0 // (OFF: over 408 games -7 wins, +14 queen deaths) user (2026-10-07): the queen splits at the beginning. Before QSS_END she splits in half
                            // (4 -> 2 + 2) whenever she can; only ENEMY heads within 3 stop it (the start crowd of our own
                            // dragons blocked every queen split before: she sat at length 4 to r150 on Australia). Her
                            // other split checks stay (calm, safe move, two real exits, room, child escape, loop, sealed start)
#endif
#ifndef QSS_END
#define QSS_END 30
#endif
#ifndef OPEN_QUEEN_SPLIT
#define OPEN_QUEEN_SPLIT 0 // (OFF: 194-10, Qd 40 - few extra splits, more queen deaths) O1 (user: multiply fast): during the opening plan (no majority yet) and before OQS_END the
                           // queen splits at length 4 like everyone else (2 + 2); all her split safety checks stay
#endif
#ifndef OQS_END
#define OQS_END 120
#endif
#ifndef OPEN_PUSH
#define OPEN_PUSH 0 // (neutral over 408 games: 194-10 / 191-13, Qd 35 / 36) O2 (user: occupy space): before OPEN_PUSH_END idle explorers are not kept out of their side and the
                    // border band (they arrive up to 3 sooner .. we arrive 4 sooner) pulls twice as hard
#endif
#ifndef OPEN_PUSH_END
#define OPEN_PUSH_END 100
#endif
#ifndef OPEN_HOLD
#define OPEN_HOLD 0 // (OFF: Around UNSW pearls r100 293 -> 199, 193-11) O3 (user: multiply fast; ladder #1 sits by spawn tiles: heads within 1 of a tile 60% vs our 38%):
                    // before OPEN_HOLD_END a spawn tile within HOLD_DIST steps is a pearl job if it spawns within
                    // HOLD_WAIT rounds (not only within distance + CAMP_LEAD) - waiting beats walking to a far pearl
#endif
#ifndef OPEN_HOLD_END
#define OPEN_HOLD_END 150
#endif
#ifndef HOLD_DIST
#define HOLD_DIST 3
#endif
#ifndef HOLD_WAIT
#define HOLD_WAIT 12
#endif
#ifndef OPEN_PLAN_END
#define OPEN_PLAN_END 300 // the opening plan ends here at the latest (or at the numbers -> length switch)
#endif

// ====================================================================================================
// SITUATION: PHASES, STRENGTH, MAP CLASS
// ====================================================================================================
#ifndef CONSOLIDATE
#define CONSOLIDATE 1 // big open maps: from CONSOL_ROUND no optional splits, spare dragons feed the queen (length wins there)
#endif
#ifndef CONSOL_ROUND
#define CONSOL_ROUND 350
#endif
#ifndef OPEN_MIN_CELLS
#define OPEN_MIN_CELLS 1000 // smaller maps are congested: numbers matter more than length
#endif
#ifndef OPEN_KELP_MAX
#define OPEN_KELP_MAX 0.20 // share of known edges that are kelp above which a map counts as congested
#endif
#ifndef CONSOL_MIN_UNITS
#define CONSOL_MIN_UNITS 10 // never feed the fleet below this (elimination)
#endif
#ifndef TIE_TRACK
#define TIE_TRACK 1 // 27: running guess of the round-500 tiebreaks steers feeding
#endif
#ifndef TIE_MARGIN
#define TIE_MARGIN 5 // lead that counts as safe on queen / longest length
#endif
#ifndef SAFE_TRADES
#define SAFE_TRADES 1 // no snipes / packs / pushes / even trades while outnumbered or in survival mode
#endif
#ifndef SURVIVAL
#define SURVIVAL 1 // survival mode: tiny fleet, heavily outnumbered or the last rounds
#endif
#ifndef SURV_UNITS
#define SURV_UNITS 3
#endif
#ifndef SURV_RATIO
#define SURV_RATIO 3 // survival when they have more than this many times our dragons...
#endif
#ifndef SURV_RATIO_MIN
#define SURV_RATIO_MIN 6 // ...and we know of at least this many of theirs
#endif
#ifndef LATE_SAFE
#define LATE_SAFE 1 // last rounds: the queen stops chasing pearls and keeps to safe ground
#endif
#ifndef LATE_SAFE_ROUNDS
#define LATE_SAFE_ROUNDS 40
#endif
#ifndef SMALL_MAP_CELLS
#define SMALL_MAP_CELLS 1200
#endif
#ifndef LOG_PHASE
#define LOG_PHASE 0 // test only: append the enemy pressure (P) to the status text
#endif
#ifndef ENEMY_EST
#define ENEMY_EST 1 // trade gates count enemies scaled by how much of the map our fleet sees (heard ids undercount a swarm)
#endif
#ifndef EST_VIEW_CELLS
#define EST_VIEW_CELLS 40 // cells one dragon effectively watches
#endif
#ifndef EST_MIN_COVER_X100
#define EST_MIN_COVER_X100 25 // coverage never assumed below this (percent)
#endif
#ifndef OPEN_FARM_ROUND
#define OPEN_FARM_ROUND 100 // before this round: no guards, screens, hunters or long queen chases - everyone farms
#endif

// ====================================================================================================
// ECONOMY: PEARLS, CAMPING, ZONES, TERRITORY
// ====================================================================================================
#ifndef UNKNOWN_EDGE_COST
#define UNKNOWN_EDGE_COST 1 // >1: route cost of an edge nobody has seen (benchmarked at 2: about 8 wins worse)
#endif
#ifndef TARGET_COMMIT
#define TARGET_COMMIT 1 // keep the current pearl unless another is clearly closer
#endif
#ifndef INTENT_MEMORY
#define INTENT_MEMORY 0 // 6c: zone / outpost / camp / explore / wall goals are kept unless a new one is clearly closer;
                        // (all OFF: every version loses ~3 Portals games) 1 = always (193-11, Qd 33, prot 81-3: slow to
                        // react), 2 = only while no enemy head is in view (195-9, Qd 35, prot 81-3);
                        // 3 = no override: the zone / outpost / camp choosers give last round's goal a bonus of
                        //     max(COMMIT_MARGIN, 30%) steps, so a goal they no longer allow (skipped, taken, crowded) drops out
                        //     (194-10, Qd 38, prot 81-3)
#endif
#ifndef COMMIT_MARGIN
#define COMMIT_MARGIN 4 // ... by this many steps and by 30%
#endif
#ifndef STALL_ROUNDS
#define STALL_ROUNDS 12 // no progress toward a pearl for this long: give it up (0 = off)
#endif
#ifndef STALL_SKIP
#define STALL_SKIP 30 // ... for this many rounds
#endif
#ifndef PREPOS_SLACK
#define PREPOS_SLACK 15 // head for a future pearl when its spawn is within travel time + this (not just 25 rounds)
#endif
#ifndef SKIP_PEARL_ROUNDS
#define SKIP_PEARL_ROUNDS 0 // >0: ignore a pearl this long once it is only reachable at a loss (benchmarked: hurts)
#endif
#ifndef CAMP_CONF
#define CAMP_CONF 0.85 // 2.3: confidence of an exactly known upcoming spawn (was 0.6)
#endif
#ifndef IDLE_CAMP
#define IDLE_CAMP 1 // 2.4: nothing to do and nothing unseen: camp the nearest free spawn tile
#endif
#ifndef OWN_HALF
#define OWN_HALF 1 // 17: pearls on our half count more; defenders (medium length) camp our half near the middle
#endif
#ifndef OWN_HALF_BONUS
#define OWN_HALF_BONUS 1.25
#endif
#ifndef DEFENDER_SLOT
#define DEFENDER_SLOT 5 // id-hash slot (0-9) of the defenders
#endif
#ifndef OPEN_CAMP_ROUNDS
#define OPEN_CAMP_ROUNDS 60 // opening: an idle dragon heads for a known spawn tile before exploring
#endif
#ifndef PAID_PEARL_RACE
#define PAID_PEARL_RACE 1 // a non-queen pays one segment to win a pearl race it would otherwise lose
#endif
#ifndef CAMP_JOB
#define CAMP_JOB 1 // 12: future spawns are jobs timed to arrive CAMP_LEAD rounds early; campers wait beside the tile
#endif
#ifndef CAMP_LEAD
#define CAMP_LEAD 3
#endif
#ifndef REGION_VALUE
#define REGION_VALUE 1 // pearls / camp spots in pearl-rich areas are worth more; the opening explores toward the centre
#endif
#ifndef RV_RAD
#define RV_RAD 4 // spawn tiles within this many cells make an area rich
#endif
#ifndef RV_PEARL_W
#define RV_PEARL_W 0.0 // (off: hurt Portals; 1.0 tested) a pearl in the richest area counts up to (1 + this) x a lone one
#endif
#ifndef RV_CAMP_W
#define RV_CAMP_W 0.0 // (off with RV_PEARL_W; 10 tested) camp spot: cells of travel the richest area is worth
#endif
#ifndef RV_EXPLORE_W
#define RV_EXPLORE_W 12.0 // opening exploration: cells of travel the map centre is worth
#endif
#ifndef RV_EXPLORE_END
#define RV_EXPLORE_END 120
#endif
#ifndef RV_FULL_CELLS
#define RV_FULL_CELLS 1600 // weights at full strength from this map area (40% at least on small maps)
#endif
#ifndef OUTPOSTS
#define OUTPOSTS 1 // idle collectors hold the pearl-richest spots (outposts) instead of drifting to the nearest tile
#endif
#ifndef OUTPOST_SEP
#define OUTPOST_SEP 8 // outposts at least this far apart
#endif
#ifndef OUTPOST_MIN_RV
#define OUTPOST_MIN_RV 0.35 // richness (0-1) an outpost needs
#endif
#ifndef OUTPOST_CROWD
#define OUTPOST_CROWD 6.0 // cells of travel each teammate already at an outpost costs
#endif
#ifndef OUTPOST_MIN_MARGIN
#define OUTPOST_MIN_MARGIN -2 // outposts only on our half or the middle (halfMargin >= this); -99 = anywhere
#endif
#ifndef CHASE_MAX
#define CHASE_MAX 20 // F4: rounds chasing one pearl (in total, target switches included) without eating: drop it
#endif
#ifndef CHASE_SKIP
#define CHASE_SKIP 40
#endif
#ifndef DRY_MAX
#define DRY_MAX 30 // F4: rounds on "pearl" without eating anything: drop the current target
#endif
#ifndef CENTRE_KNOWN
#define CENTRE_KNOWN 0 // (off: with the camp fix it cost Weakhold/Australia openings) F6: the opening centre pull stops once the centre is mostly seen and holds no spawn tiles
#endif
#ifndef OUTPOST_MIN_TILES
#define OUTPOST_MIN_TILES 0 // (off, tested 3: Weakhold opening 111 -> 70) F6: an outpost needs this many known spawn tiles within RV_RAD
#endif
#ifndef CAMP_MAX_WAIT
#define CAMP_MAX_WAIT 25 // F6: no camping on a tile whose next pearl is further off than this (explore instead)
#endif
#ifndef CAMP_GIVEUP
#define CAMP_GIVEUP 15 // F6: rounds by a camped tile with no pearl: leave it for 40 rounds
#endif
#ifndef VORONOI
#define VORONOI 1 // who reaches each cell first, by path: pearl contests, camping, idle exploring, the queen's pearls
#endif
#ifndef VOR_TURNS
#define VOR_TURNS 0 // 7h: territory by arrival TURN (cells / free steps per turn), not by cells
#endif
#ifndef VOR_REP_AGE
#define VOR_REP_AGE 3 // position reports this fresh (ours and theirs) seed the fields
#endif
#ifndef VOR_FRONT_BONUS
#define VOR_FRONT_BONUS 8.0 // idle exploring: cells of travel a cell on our side near the border is worth
#endif
#ifndef VOR_DEEP_PEN
#define VOR_DEEP_PEN 20.0 // idle exploring: cost of a cell deep in their territory (they arrive 4+ sooner)
#endif
#ifndef MATCH_SWAP
#define MATCH_SWAP 0 // 7i: after the greedy matching, swap pearls with a nearby teammate when the pair's total cost drops
#endif
#ifndef MATCH_SWAP_RANGE
#define MATCH_SWAP_RANGE 10
#endif
#ifndef MATCH_PATHS
#define MATCH_PATHS 0 // (tested, OFF: opening pearls fell - UNSW 555 -> 426/478, Islands 358 -> 332/292) teammates' cost to a pearl = path distance (bounded BFS), not manhattan
#endif
#ifndef MATCH_SELF_SAME
#define MATCH_SELF_SAME 1 // C: our own cost in the matching uses the same BFS as the teammates' (consistent metric)
#endif
#ifndef MATCH_DEPTH
#define MATCH_DEPTH 16 // BFS depth per teammate; beyond it the manhattan guess (at least MATCH_DEPTH + 1)
#endif
#ifndef ZONES
#define ZONES 1 // pearl-rich zones (own spawn knowledge + T_ZONE sonar reports); collectors spread over them in
                // proportion to richness: value = tiles / (teammates there + 1) - ZONE_DIST_W * travel
#endif
#ifndef DENSITY_BOX
#define DENSITY_BOX 0 // (OFF for v3.2.0: alone 195-9 Portals -3; in the combined build 194-10, prot 80-4) 7a: spawn-tile counts by 2D prefix sums over a box instead of the radius-r diamond loop
#endif
#ifndef ZONE_TILES
#define ZONE_TILES 4 // spawn tiles within RV_RAD that make a zone worth reporting
#endif
#ifndef ZONE_RAD
#define ZONE_RAD 5 // teammates this close to a zone are working it
#endif
#ifndef ZONE_DIST_W
#define ZONE_DIST_W 0.15 // tiles of richness one cell of travel costs
#endif
#ifndef ZONE_OVERRIDE
#define ZONE_OVERRIDE 8 // a pearl target further than this gives way to a better zone
#endif
#ifndef ZONE_MSG_EVERY
#define ZONE_MSG_EVERY 15
#endif
#ifndef LOOP_STALL
#define LOOP_STALL 1 // circling one 2x2 for LOOP_STALL_ROUNDS without eating while after a far pearl: drop that pearl
#endif
#ifndef LOOP_STALL_ROUNDS
#define LOOP_STALL_ROUNDS 8
#endif

// ====================================================================================================
// DEAD ENDS, DIVES, RECYCLING
// ====================================================================================================
#ifndef DIVE_MIN_GAIN
#define DIVE_MIN_GAIN 1 // net segments a dead-end dive must win (pearls eaten minus the 2 left behind)
#endif
#ifndef DIVE_DEPTH
#define DIVE_DEPTH 12 // moves the dive search looks ahead
#endif
#ifndef DIVE_NODE_CAP
#define DIVE_NODE_CAP 4000 // node budget per dive search
#endif
#ifndef DIVE_ENABLE
#define DIVE_ENABLE 1 // non-queens value dead ends by the dive search
#endif
#ifndef FRONTIER_OPEN
#define FRONTIER_OPEN 0 // 1: non-leaders treat a way into unexplored water as an exit (benchmarked: no gain)
#endif
#ifndef RECYCLE
#define RECYCLE 1 // 19: trapped with a teammate close by: self-destruct now so it eats the drop
#endif
#ifndef RECYCLE_MATE_DIST
#define RECYCLE_MATE_DIST 4
#endif
#ifndef DIVE_MATE_GAIN
#define DIVE_MATE_GAIN 0 // dead-end dive threshold when a teammate is near to eat the recycle drop
#endif
#ifndef POCKET_CHECK
#define POCKET_CHECK 1 // 21: through a portal or into a small enclosed area, the body must be able to keep moving
#endif
#ifndef DIVE_MAX_LEN
#define DIVE_MAX_LEN 12 // longest dragon that considers dead-end dives
#endif
#ifndef POCKET_NODE_CAP
#define POCKET_NODE_CAP 3000 // search budget of the pocket check (out of budget = assume fine)
#endif
#ifndef FOUNTAINS
#define FOUNTAINS 1 // tiles that refill within FOUNT_GAP rounds count as pearls in dives; no optional split inside a dead end
#endif
#ifndef SLOT_CHECK
#define SLOT_CHECK 1 // F7: stepping into a fully seen 1-wide dead end is a trap (non-queens) unless its pearls pay (dive)
#endif
#ifndef DEADEND_STRICT
#define DEADEND_STRICT 1 // P1: the no-split-in-a-dead-end rule only where every legal move leads into a 1-wide dead end
#endif
#ifndef FOUNT_GAP
#define FOUNT_GAP 3
#endif
#ifndef SURV_ECON
#define SURV_ECON 1 // survival mode: never step next to an enemy head for a pearl; no recycling in a small fleet
#endif
#ifndef SURV_THREAT_PEN
#define SURV_THREAT_PEN 200000.0
#endif
#ifndef RECYCLE_MIN_UNITS
#define RECYCLE_MIN_UNITS 6
#endif

#ifndef BODY_TRUST_VISION
#define BODY_TRUST_VISION 0 // B1 (tested, OFF): our body model never keeps a cell vision shows is not ours - 195-9,
                            // Portals 81-3: the queen forgot her real segments beyond a portal and hit herself
#endif

#ifndef LEGAL_BY_VISION
#define LEGAL_BY_VISION 1 // B1b: a visible cell with no dragon on it is always a legal step (the body model can hold
                          // stale segments after a portal crossing and made such moves look like hitting our tail)
#endif

#ifndef TOPO_TRAP
#define TOPO_TRAP 1 // 7b: a step across a bridge into a fully known branch with no loop longer than us is a trap
                    // (workers: unsafe unless a dive pays; queen: like a dead-end pocket). Any shape, any size.
#endif

// ====================================================================================================
// PORTALS
// ====================================================================================================
#ifndef PORTAL_SCOUTS
#define PORTAL_SCOUTS 1 // the hunter group (and portal role) opens unknown portals from round 0
#endif
#ifndef SCOUT_REACH_PCT
#define SCOUT_REACH_PCT 50 // scout portals only while we can reach less than this % of the map without them
#endif
#ifndef SCOUT_PEARL_MARGIN
#define SCOUT_PEARL_MARGIN 4 // a scout keeps its pearl only if it is this much closer than the portal
#endif
#ifndef PORTAL_ALWAYS
#define PORTAL_ALWAYS 1 // 2.6: scouts probe unknown portals whatever share of the map we can reach
#endif
#ifndef PORTAL_ROW
#define PORTAL_ROW 1 // 20: never step through a portal onto/next to a teammate's head (lower id has right of way)
#endif
#ifndef PORTAL_ROW_PEN
#define PORTAL_ROW_PEN 2000000
#endif
#ifndef BLIND_PORTAL_MAX_LEN
#define BLIND_PORTAL_MAX_LEN 3 // longest dragon that may cross a portal whose far side is unknown (can turn in a 2x2)
#endif
#ifndef PORTAL_RUSH
#define PORTAL_RUSH 0 // (off: v2.15.2 = v2.15.0 + fixes; 1 = v2.15.1) a short dragon that sees an unexplored portal close by goes through it at once (portals lead to
                      // regions nobody of ours has seen; one crossing teaches the whole fleet the pair)
#endif
#ifndef PORTAL_RUSH_PEARL
#define PORTAL_RUSH_PEARL 5 // a dragon with a pearl target this close collects it instead
#endif
#ifndef PORTAL_RUSH_DIST
#define PORTAL_RUSH_DIST 4 // water steps to the portal mouth
#endif

// ====================================================================================================
// QUEEN SAFETY: THREAT, TRAPS, LOOKAHEAD
// ====================================================================================================
#ifndef MAX_SPRINT
#define MAX_SPRINT 3 // free steps planned per turn (vision radius is 3)
#endif
#ifndef SURV_NODE_CAP
#define SURV_NODE_CAP 20000 // node budget of the queen's survival search
#endif
#ifndef SURV_UNKNOWN_SAFE
#define SURV_UNKNOWN_SAFE 0 // 1: queen survival search stops at an unexplored edge and calls it safe (old)
#endif
#ifndef QUEEN_THREAT_MAP
#define QUEEN_THREAT_MAP 1 // queen judges enemy reach by path distance (kelp, bodies), sonar reports included
#endif
#ifndef PAID_REACH
#define PAID_REACH 1 // enemy reach includes paid sprint steps (one segment each, down to length 2)
#endif
#ifndef PAID_REACH_CAP
#define PAID_REACH_CAP 24 // longest enemy sprint considered
#endif
#ifndef PAID_THREAT_PEN
#define PAID_THREAT_PEN 200000 // (QUEEN_HARD_THREAT 0) queen ending where an enemy needs k paid steps: PEN / k
#endif
#ifndef QUEEN_HARD_THREAT
#define QUEEN_HARD_THREAT 1 // any cell an enemy can reach with free + paid steps is near-forbidden for the queen
#endif
#ifndef HARD_THREAT_PEN
#define HARD_THREAT_PEN 1200000 // queen ending within an enemy's paid sprint reach (a known dead end costs 1e6)
#endif
#ifndef QUEEN_TRAP_PEN
#define QUEEN_TRAP_PEN 2500000 // queen move with no way out (certain death) must cost more than a mere threat
#endif
#ifndef THREAT_EXTRA_ATTACKER
#define THREAT_EXTRA_ATTACKER 300000 // per additional enemy that can reach the cell
#endif
#ifndef QUEEN_MARGIN
#define QUEEN_MARGIN 4 // steps beyond an enemy's reach the queen still prefers to widen
#endif
#ifndef QUEEN_MARGIN_PEN
#define QUEEN_MARGIN_PEN 20000 // per step of margin missing
#endif
#ifndef ENEMY_LEN_MEMORY
#define ENEMY_LEN_MEMORY 1 // enemy lengths: remember full sightings, guess when the body runs out of view
#endif
#ifndef UNSEEN_LEN_GUESS
#define UNSEEN_LEN_GUESS 4 // assumed length of an enemy whose body leaves our view (never seen whole; 6 boxed the queen in)
#endif
#ifndef QUEEN_LEN_MEM
#define QUEEN_LEN_MEM 100 // D: the queen's threat estimate trusts a remembered enemy length this long (others: LEN_MEM_ROUNDS)
#endif
#ifndef LEN_MEM_ROUNDS
#define LEN_MEM_ROUNDS 25 // a remembered length is trusted this long
#endif
#ifndef QUEEN_PAID_ESCAPE
#define QUEEN_PAID_ESCAPE 0 // every single step is inside some enemy's reach: sprint out, paying segments (off: with EVADE and
                            // STALKER_INTERCEPT it cost ~7 wins vs fleet11)
#endif
#ifndef QUEEN_EVADE
#define QUEEN_EVADE 0 // the queen drops pearls that lie in or next to an enemy's reach (off: see QUEEN_PAID_ESCAPE)
#endif
#ifndef QUEEN_SURV_RANK
#define QUEEN_SURV_RANK 1 // queen without a safe move prefers ways on over known dead ends
#endif
#ifndef QUEEN_AVOID_PORTAL
#define QUEEN_AVOID_PORTAL 1 // the queen never crosses portals and keeps off cells with portal edges
#endif
#ifndef QUEEN_AVOID_UNKNOWN
#define QUEEN_AVOID_UNKNOWN 1 // the queen keeps to explored water and does not explore
#endif
#ifndef QUEEN_LOOP_SAFE
#define QUEEN_LOOP_SAFE 1 // the queen only moves (and splits) where she can still reach a free loop longer than she is
#endif
#ifndef QUEEN_LOOP_DEPTH
#define QUEEN_LOOP_DEPTH 15 // steps searched for that 2x2 (through known open water only)
#endif
#ifndef QUEEN_LOOP_PEN
#define QUEEN_LOOP_PEN 1000000 // a move after which no such 2x2 is reachable (pockets and long corridors trap short queens)
#endif
#ifndef QUEEN_SEALED_NOSPLIT
#define QUEEN_SEALED_NOSPLIT 1 // queen starts in a kelp pocket closed within her first view (no portals): no optional splits
#endif
#ifndef QUEEN_LONG_LEN
#define QUEEN_LONG_LEN 12 // queens this long (feeding makes them) stay out of corridors
#endif
#ifndef QUEEN_CORRIDOR_PEN
#define QUEEN_CORRIDOR_PEN 300000
#endif
#ifndef QUEEN_INSURE
#define QUEEN_INSURE 1 // the queen about to be struck splits off all but 2: the length lives on in the child
#endif
#ifndef QUEEN_INSURE_LEN
#define QUEEN_INSURE_LEN 5
#endif
#ifndef QUEEN_CORRIDOR_MATE
#define QUEEN_CORRIDOR_MATE 1 // the queen does not enter a 1-wide corridor that leads to a teammate's head
#endif
#ifndef PURSUIT_SAFE
#define PURSUIT_SAFE 1 // queen: enemy heads move too - no corner cell with an enemy near, not every exit within their reach
#endif
#ifndef PURSUIT_DIST
#define PURSUIT_DIST 4
#endif
#ifndef CORNER_PEN
#define CORNER_PEN 1500000.0
#endif
#ifndef CORR_ENTER_PEN
#define CORR_ENTER_PEN 150000.0 // queen stepping into a 1-wide corridor with another dragon within 6 (a fifth of it otherwise)
#endif
#ifndef ALL_EXITS_PEN
#define ALL_EXITS_PEN 900000.0
#endif
#ifndef QUEEN_MOBILITY
#define QUEEN_MOBILITY 30000.0 // per free cell next to her destination while an enemy head is within 3 (open water over walls)
#endif
#ifndef THREAT_TAIL_FREE
#define THREAT_TAIL_FREE 1 // P3a: enemy reach passes through the queen's own tail cell (it moves away when she steps)
#endif
#ifndef QUEEN_NO_SUICIDE
#define QUEEN_NO_SUICIDE 0 // P3b (tested, OFF - no fewer queen deaths): no insurance split down to 2; boxed with 4+,
                           // she splits to stall rather than strike head-on
#endif
#ifndef QUEEN_LOOK2
#define QUEEN_LOOK2 1 // P3c: two-move lookahead - after this move, can she still reach next turn a cell no enemy can
                      // reach in two turns? If not, a big penalty; if so, the corner / all-exits rules stand down
#endif
#ifndef LOOK2_PEN
#define LOOK2_PEN 800000.0 // below HARD_THREAT_PEN: being inside a reach now is still worse
#endif
#ifndef LOOK2_CELL_BONUS
#define LOOK2_CELL_BONUS 15000.0 // per two-turn-safe cell she can reach next turn (up to 8)
#endif
#ifndef LOOK2_PAID
#define LOOK2_PAID 0 // her next-turn escape may use paid steps (0: free steps only, what she normally walks)
#endif
#ifndef LOOK2_SKIP_CORNER
#define LOOK2_SKIP_CORNER 1 // with a two-turn-safe way on, the corner / all-exits penalties are not applied
#endif
#ifndef LOOK2_QUEEN_REACH
#define LOOK2_QUEEN_REACH 6 // her own next-turn reach considered (free + paid steps)
#endif
#ifndef CONTACT_SEARCH
#define CONTACT_SEARCH 0 // R1 (contact.hpp): when an enemy head is close and the move is hard (R8 trigger), the queen
                         // searches two of her moves ahead with the nearest enemies' replies in between (max-min) and
                         // judges ground reached first + free loop + exits; blended into her score. 1 = on,
                         // 2 = compute and log only (QDIAG "C" lines), 0 = off
#endif
#ifndef CS_ALPHA
#define CS_ALPHA 0.3 // R1 blend weight (implicit-minimax style): score += CS_ALPHA * CS_SCALE * value (value -1 .. 1)
#endif
#ifndef CS_SCALE
#define CS_SCALE 2000000.0 // R1: size of the queen's safety terms (hard threat 1.2e6, corner 1.5e6)
#endif
#ifndef CS_ENEMIES
#define CS_ENEMIES 2 // R1: nearest enemy heads whose replies are searched (the rest: two-turn reach, as look-2)
#endif
#ifndef CS_REPLIES
#define CS_REPLIES 5 // R1: reply cells per enemy (closest to her head = most threatening)
#endif
#ifndef CS_NEAR
#define CS_NEAR 10 // R1: enemy heads this close (manhattan) can be searched enemies
#endif
#ifndef CS_TERR_RADIUS
#define CS_TERR_RADIUS 10 // R1: ground counted within this many of her steps
#endif
#ifndef CS_W_TERR
#define CS_W_TERR 0.55 // R1 leaf weights: ground she reaches first...
#endif
#ifndef CS_W_LOOP
#define CS_W_LOOP 0.30 // ...a free loop longer than her within reach...
#endif
#ifndef CS_W_EXITS
#define CS_W_EXITS 0.15 // ...her free exits (up to 3)
#endif
#ifndef CS_WORK_CAP
#define CS_WORK_CAP 400000 // R8: BFS cells one search may pop (~15M points); over it the search is dropped
#endif
#ifndef CS_TRIG_EXTRA
#define CS_TRIG_EXTRA 3 // R8 trigger: an enemy head within its full strike reach + this many of her head...
#endif
#ifndef CS_MARGIN
#define CS_MARGIN 200000.0 // R8 trigger: ...and (<= 1 fully safe move, or 2+ heads within 6, or best - second < this)
#endif
#ifndef QUEEN_CROWD
#define QUEEN_CROWD 2 // Q4: was 3 (fled too late, cornered: 196-8, Qd 38 -> 36, head-on 19 -> 17) - F5: this many enemy heads within CROWD_RAD of the queen: no pearls, no rest - get to open water (0 = off)
#endif
#ifndef CROWD_RAD
#define CROWD_RAD 6
#endif
#ifndef THREAT_RADIUS
#define THREAT_RADIUS 8 // an enemy head seen (or reported within 3 rounds) this close to her is a threat
#endif

#ifndef BAND_COMPARE
#define BAND_COMPARE 0 // 6a (tested, OFF): 2 = life band first, then the sum of the rest - changed 3 queen moves in 291k
                       // dragon-turns, suite 194-10 Qd 48 (noise); 1 = life > safety > goal + style - 194-10, Portals
                       // 81-3 (the queen gave up room for tiny safety gains); 0 = one plain sum (v2.16)
#endif

#ifndef TOPO_EVERY
#define TOPO_EVERY 1 // 7b/3c: topology rebuilt at most every this many rounds when new edges are known (CPU)
#endif
#ifndef QUEEN_TOPO_LEASH
#define QUEEN_TOPO_LEASH 0 // 7d: the queen may take pearls up to 2 x her leash from home where the pearl's loop region
                           // can hold her (topology) and we arrive first by 2+ (Voronoi)
#endif
#ifndef TOPO_LOOP
#define TOPO_LOOP 1 // 7b: queens too long for the k x k ring test (L >= 16) need a loop region longer than them nearby
#endif

// ====================================================================================================
// QUEEN: HOME, GROWTH, BUNKER
// ====================================================================================================
#ifndef QUEEN_SEED_UNITS
#define QUEEN_SEED_UNITS 3 // the queen may split off 2 only while the team is this small
#endif
#ifndef QUEEN_GROW_ROUND
#define QUEEN_GROW_ROUND 225 // enemy queen still alive by now: our queen stops splitting and grows
#endif
#ifndef BUNKER
#define BUNKER 1 // enemy queen dead: ours trims to length 2 and circles a sheltered 2x2 with no pearls
#endif
#ifndef EQ_DEATH_DETECT
#define EQ_DEATH_DETECT 1 // infer the enemy queen's death from death drops and fleet reports, and keep gossiping it
#endif
#ifndef EQ_STRICT
#define EQ_STRICT 1 // death inference only from a death drop on her own last-seen body (no area or report guesses: 2 false alarms in 176 games)
#endif
#ifndef BUNKER_SEARCH
#define BUNKER_SEARCH 25 // path distance searched for a bunker box
#endif
#ifndef QUEEN_COMMIT
#define QUEEN_COMMIT 0 // the queen keeps her pearl too (target commitment), instead of switching every turn
#endif
#ifndef TIE_BUNKER
#define TIE_BUNKER 1 // late, both queens short and theirs no longer than ours: bunker at our length (no trim)
#endif
#ifndef TIE_BUNKER_ROUND
#define TIE_BUNKER_ROUND 350
#endif
#ifndef TIE_FORCE
#define TIE_FORCE 0 // test only: skip the enemy queen length check
#endif
#ifndef TIE_SEEN_ROUNDS
#define TIE_SEEN_ROUNDS 150 // how old the enemy queen's last exact length may be
#endif
#ifndef STARVE_RELAX
#define STARVE_RELAX 1 // the queen as (almost) the only eater, nothing eaten for a while: portal/unknown/loop rules relax
#endif
#ifndef STARVE_ROUNDS
#define STARVE_ROUNDS 20
#endif
#ifndef QUEEN_HOME
#define QUEEN_HOME 1 // the queen keeps to a leash round her spawn; pearls only inside it, away from enemy heads, never in pockets
#endif
#ifndef QUEEN_LEASH
#define QUEEN_LEASH 7
#endif
#ifndef QUEEN_SPLIT_UNITS
#define QUEEN_SPLIT_UNITS 8 // at home the queen splits only while the fleet is this small (her children crowd her)
#endif
#ifndef QUEEN_MIN_KEEP
#define QUEEN_MIN_KEEP 3 // (v2.14.4: was 4 - faster opening) a home queen never splits below this (their hidden queens are 2-3 long: the tiebreak stays ours)
#endif
#ifndef QUEEN_PEARL_CLEAR
#define QUEEN_PEARL_CLEAR 5 // no visible enemy head this close to a pearl she goes for
#endif
#ifndef LEAD_SAFE
#define LEAD_SAFE 1 // from LEAD_ROUND a queen longer than theirs (or >= LEAD_MIN_LEN, theirs unknown) makes no pearl trips
#endif
#ifndef LEAD_ROUND
#define LEAD_ROUND 300
#endif
#ifndef LEAD_MIN_LEN
#define LEAD_MIN_LEN 5
#endif
#ifndef EQ_INFER
#define EQ_INFER 1 // F2: their queen seen once and then not for EQ_GONE_ROUNDS = probably dead -> ours bunkers (no trim)
#endif
#ifndef EQ_GONE_ROUNDS
#define EQ_GONE_ROUNDS 100
#endif
#ifndef EQ_INFER_ROUND
#define EQ_INFER_ROUND 150
#endif
#ifndef QUEEN_FIRST_SPLIT
#define QUEEN_FIRST_SPLIT 1 // F3: with <= 3 dragons the queen may split at length 4 (keeps 2)
#endif
#ifndef OPEN_QUEEN
#define OPEN_QUEEN 0 // (tested OFF: 191-13 vs 197-7 without it, same queen deaths) OPEN_PLAN queen part: splits down to 2 and explores when no teammate is near
#endif
#ifndef QUEEN_ISO_RAD
#define QUEEN_ISO_RAD 8 // no teammate this close: the queen is on her own and explores
#endif
#ifndef QUEEN_ADAPT
#define QUEEN_ADAPT 0 // P2 (tested, OFF): the home queen's leash follows the threat around her; in the opening she splits and
                      // eats freely. Lifts the opening but raises queen deaths (Islands, Trophy, protected maps) and leaves the
                      // queen at length 2 on pocket maps - revisit once P3 (queen two-move safety) is in.
#endif
#ifndef ADAPT_SPLIT
#define ADAPT_SPLIT 1 // in the opening the queen splits at length 4 (no QUEEN_MIN_KEEP / QUEEN_SPLIT_UNITS)
#endif
#ifndef ADAPT_FLEET
#define ADAPT_FLEET 6 // the opening lasts until the fleet first has this many dragons...
#endif
#ifndef ADAPT_OPEN_END
#define ADAPT_OPEN_END 150 // ...or until this round
#endif
#ifndef QUIET_ROUNDS
#define QUIET_ROUNDS 12 // rounds without a threat before her leash opens
#endif
#ifndef LEASH_MAX
#define LEASH_MAX 24 // after the opening the leash grows one cell per 2 quiet rounds up to this
#endif
#ifndef ADAPT_END_ROUND
#define ADAPT_END_ROUND 300 // after the opening the leash only grows before this round
#endif

// ====================================================================================================
// TEAMMATES AROUND THE QUEEN
// ====================================================================================================
#ifndef QUEEN_ROOM
#define QUEEN_ROOM 0 // 1: teammates near the queen measure her free space (they rarely see her whole body; no gain)
#endif
#ifndef QUEEN_SPACE_PEN
#define QUEEN_SPACE_PEN 40000 // teammate head ending within 2 of the queen's head (PEN / 3.5 at distance 3)
#endif
#ifndef QUEEN_CLEAR
#define QUEEN_CLEAR 0 // >0: teammates keep this path distance from the queen's head (benchmarked at 4: fewer wall deaths, more head-ons)
#endif
#ifndef QUEEN_CLEAR_PEN
#define QUEEN_CLEAR_PEN 30000 // per step closer than QUEEN_CLEAR; doubled in corridors
#endif
#ifndef QUEEN_ROOM_PEN
#define QUEEN_ROOM_PEN 150000
#endif
#ifndef TEAM_ADJ_PEN
#define TEAM_ADJ_PEN 4000 // non-leader ending next to a teammate's head (later movers see us anyway; dithers in corridors)
#endif
#ifndef QUEEN_EXIT_GUARD
#define QUEEN_EXIT_GUARD 1 // teammates never leave the queen fewer than 2 free exits (she moves first next round)
#endif
#ifndef QUEEN_EXIT_PEN
#define QUEEN_EXIT_PEN 2000000
#endif
#ifndef CORRIDOR_GUARD
#define CORRIDOR_GUARD 1 // teammates do not advance into a 1-wide corridor the queen is in
#endif

// ====================================================================================================
// OFFENCE: TRADES, HUNTERS, PACKS, WALL
// ====================================================================================================
#ifndef HUNT_QUEEN_RANGE
#define HUNT_QUEEN_RANGE 14 // non-queens within this path distance chase the enemy queen
#endif
#ifndef THREAT_PEN
#define THREAT_PEN 9000 // non-leader stepping next to an enemy head
#endif
#ifndef STALKER_INTERCEPT
#define STALKER_INTERCEPT 0 // any short teammate strikes an enemy head that threatens our queen (off: see QUEEN_PAID_ESCAPE)
#endif
#ifndef HUNTERS
#define HUNTERS 1 // part of the fleet hunts in coordinated packs (enemy queen first, then long dragons)
#endif
#ifndef HUNT_START
#define HUNT_START 60 // hunters start by this round (or as soon as expansion ends)
#endif
#ifndef HUNTER_SHARE
#define HUNTER_SHARE 3 // tenths of the non-leader fleet that hunt
#endif
#ifndef HUNTER_LEN
#define HUNTER_LEN 6 // hunters grow to this before going in (strike reach = free + L - 2 steps)
#endif
#ifndef HUNTER_SPLIT_LEN
#define HUNTER_SPLIT_LEN 12 // hunters still split from this length (both halves can strike); 0 = never
#endif
#ifndef PACK_SIZE
#define PACK_SIZE 3 // hunters sent at one long enemy dragon
#endif
#ifndef PREY_MIN_LEN
#define PREY_MIN_LEN 8 // enemy dragons this long are worth a small dragon in a head-on
#endif
#ifndef PACK_REGROUP
#define PACK_REGROUP 5 // a pack member this close to its prey waits for a mate (0 = off)
#endif
#ifndef ENDGAME_PACKS
#define ENDGAME_PACKS 1 // our queen dead: small dragons hunt in packs, long ones keep growing
#endif
#ifndef AGGRO_EXPANSION
#define AGGRO_EXPANSION 1 // while expanding: contest pearls nearer the enemy, push idle hunters toward its side
#endif
#ifndef PAID_REGICIDE
#define PAID_REGICIDE 1 // strike the visible enemy queen from as far as free + paid steps allow
#endif
#ifndef REGICIDE_MAX
#define REGICIDE_MAX 12
#endif
#ifndef SNIPE
#define SNIPE 1 // short dragons sprint-strike any visible enemy head at least as long as themselves
#endif
#ifndef SNIPE_MAX_LEN
#define SNIPE_MAX_LEN 6 // only dragons this short strike (cheap trades)
#endif
#ifndef HUNT_GATE
#define HUNT_GATE 1 // hunters only hunt while we are not outnumbered and not in survival mode
#endif
#ifndef MAJ_HUNT
#define MAJ_HUNT 1 // smaller maps: with a clear majority every spare dragon hunts enemy dragons (win by elimination)
#endif
#ifndef MAJ_RATIO_X10
#define MAJ_RATIO_X10 20 // majority = our dragons >= 2.0x the enemies we know of
#endif
#ifndef MAJ_MIN_UNITS
#define MAJ_MIN_UNITS 12
#endif
#ifndef LURK
#define LURK 1 // 30: pack members wait just outside her view, on different sides, and strike when she comes close
#endif
#ifndef LURK_DIST
#define LURK_DIST 4 // Chebyshev distance from her (her view reaches 3)
#endif
#ifndef LURK_FRESH
#define LURK_FRESH 8 // how old her reported position may be (sonar reports are rare)
#endif
#ifndef GIANT_HUNT
#define GIANT_HUNT 1 // 32: our queen dead, from GIANT_HUNT_ROUND, theirs long unseen: their longest dragons first
#endif
#ifndef GIANT_HUNT_ROUND
#define GIANT_HUNT_ROUND 350
#endif
#ifndef GIANT_PREY_MIN
#define GIANT_PREY_MIN 10
#endif
#ifndef CORK
#define CORK 1 // (locally identical: their queen is rarely seen; kept for the ladder) 7g: their queen in a corridor with one way out: take a corridor cell before she can leave
#endif
#ifndef CORK_RANGE
#define CORK_RANGE 12
#endif
#ifndef BOX_QUEEN
#define BOX_QUEEN 1 // 33: a dragon that sees their queen but cannot strike takes her free cells
#endif
#ifndef BOX_BONUS
#define BOX_BONUS 3000.0
#endif
#ifndef TRADE_UP
#define TRADE_UP 1 // head-on trades only for their queen, a dragon TRADE_UP_RATIO x our length, or a stalker of our queen
#endif
#ifndef TRADE_UP_RATIO
#define TRADE_UP_RATIO 2
#endif
#ifndef ANTI_ESCORT
#define ANTI_ESCORT 1 // an enemy head within 2 of our queen for ESCORT_ROUNDS is a stalker: reported, struck by short teammates
#endif
#ifndef ESCORT_ROUNDS
#define ESCORT_ROUNDS 2
#endif
#ifndef RESCUE_RANGE
#define RESCUE_RANGE 12 // teammates this far (path) from a stalker report come to strike it
#endif
#ifndef WALL_MIN_CELLS
#define WALL_MIN_CELLS 1500 // small / cramped maps: no wall (the line has nowhere to go and long members box our queen in)
#endif
#ifndef WALL_FORM_RATIO
#define WALL_FORM_RATIO 1.2 // the wall forms only while we have this many x their (estimated) dragons
#endif
#ifndef WALL_PUSH_RATIO
#define WALL_PUSH_RATIO 1.5 // the wall holds the middle line; it pushes on to their home only with this many x their dragons
#endif
#ifndef WALL
#define WALL 1 // from wall_round() collector slots WALL_SLOT_LO-HI join the hunter packs; with no prey they advance
               // side by side on the enemy's home (slot 9, guards and the defender stay back)
#endif
#ifndef WALL_SLOT_LO
#define WALL_SLOT_LO 6
#endif
#ifndef WALL_SLOT_HI
#define WALL_SLOT_HI 8
#endif
#ifndef WALL_MIN_LEN
#define WALL_MIN_LEN 3
#endif
#ifndef WALL_GAP
#define WALL_GAP 3 // cells between wall members across the line of advance
#endif
#ifndef WALL_LEAD
#define WALL_LEAD 4 // a member this much closer to the target than the others' average waits (collects nearby)
#endif
#ifndef HUNT_FAR_SMALL
#define HUNT_FAR_SMALL 1 // on big maps packs only go for their queen when she was seen in the last HUNT_FRESH rounds
#endif
#ifndef HUNT_FRESH
#define HUNT_FRESH 30
#endif

// ====================================================================================================
// DEFENCE: GUARDS, SCREENS, QUEEN NEWS
// ====================================================================================================
#ifndef GUARDS
#define GUARDS 1 // part of the fleet stays near our queen and intercepts enemies
#endif
#ifndef GUARD_SHARE
#define GUARD_SHARE 2 // tenths of the fleet that guard (hunters join once the enemy queen is dead)
#endif
#ifndef GUARD_RING
#define GUARD_RING 7 // guards come back when farther than this from the queen
#endif
#ifndef GUARD_MIN
#define GUARD_MIN 4 // guards never end a move this close to the queen's head (they screen, not crowd)
#endif
#ifndef GUARD_MIN_PEN
#define GUARD_MIN_PEN 60000 // per step inside GUARD_MIN
#endif
#ifndef CHOKE_GUARDS
#define CHOKE_GUARDS 1 // (user: keep; path search fixed to plan through unexplored water) 7j: idle guards hold the first bridge (topology) on the queen's way toward the enemy side
#endif
#ifndef CHOKE_RANGE
#define CHOKE_RANGE 10
#endif
#ifndef GUARD_INTERCEPT
#define GUARD_INTERCEPT 6 // enemy heads this close to the queen are intercepted
#endif
#ifndef GUARD_MIN_UNITS
#define GUARD_MIN_UNITS 0 // >0: no guard duty while the fleet is smaller than this or still expanding (0 = always)
#endif
#ifndef SCREENS
#define SCREENS 1 // the first guard slot screens: posted 6-9 cells from the queen toward the enemy, on her row or column
#endif
#ifndef SCREEN_MIN
#define SCREEN_MIN 6
#endif
#ifndef SCREEN_MAX
#define SCREEN_MAX 9
#endif
#ifndef AIM_REPORTS
#define AIM_REPORTS 1 // enemy news goes out in the queen's direction when she is on our row or column
#endif
#ifndef REP_MAX_AGE
#define REP_MAX_AGE 3 // teammates' enemy reports the queen's threat map still uses (rounds)
#endif
#ifndef QUEEN_POS_RELAY
#define QUEEN_POS_RELAY 1 // relay the queen's fresh status ahead of gossip (guards and screens need her position)
#endif
#ifndef GUARD_FLEET_MIN
#define GUARD_FLEET_MIN 5 // no guards or screens with this few dragons: everyone collects
#endif
#ifndef EQ_INFER_UNSEEN
#define EQ_INFER_UNSEEN 0 // Q2a: EQ_INFER also when their queen was never seen at all (she died unseen in 5 of 7 test games)
#endif
#ifndef GIANT_GOSSIP
#define GIANT_GOSSIP 0 // (OFF: still 8-14 giants - the sonar graph is in pieces) 6d'': one giant fleet-wide. STATUS is repacked (id 13, length 7, round mod 16 bits) to carry
                       // the best giant / heir claim its sender knows (length 5 bits, id hash 2, age 3); a holder that
                       // hears a better claim steps down. Spreads hop by hop like Q1; an age past GOSSIP_MAX_AGE ends a claim.
#endif
#ifndef GOSSIP_MAX_AGE
#define GOSSIP_MAX_AGE 28 // rounds a giant claim lives without its holder repeating it (one hop costs ~1 round: at 7 the
                          // top giant's own claim came back 9 rounds old and far giants never heard it)
#endif
#ifndef GOSSIP_AGE_UNIT
#define GOSSIP_AGE_UNIT 4 // the 3-bit age counts in units of this many rounds (rounded up: a claim never looks younger)
#endif
#ifndef SWITCH_STATUS_BIT
#define SWITCH_STATUS_BIT 0 // 6e: every STATUS carries the numbers -> length switch (today only T_PHASE does)
#endif
#ifndef STRIKE_NEWS
#define STRIKE_NEWS 0 // (OFF: 2.6% of its detections were right - lost relays look like silence) Q2c: a dragon within next-turn strike reach of their queen says so each turn (T_STRIKE), and
                      // "off" the turn after she got away; a striker dies in its strike and casts no sonar, so when the
                      // news stops with no "off" and nobody has seen her for STRIKE_CONFIRM rounds, she is dead.
                      // 1 = on, 2 = measure only (QDIAG log "N Q2C ...", no effect)
#endif
#ifndef STRIKE_CONFIRM
#define STRIKE_CONFIRM 6 // rounds of silence (no "off", no newer strike news, no sighting of her) before Q2c counts her dead
#endif
#ifndef EQ_STATUS_BIT
#define EQ_STATUS_BIT 1 // Q: every STATUS message carries "their queen is dead" (in 7 test games 47-90 of our dragons
                        // knew, our queen never did - she kept chasing pearls and died)
#endif
#ifndef BUNKER_GUARDS
#define BUNKER_GUARDS 1 // 6: the queen announces her bunker box; guards circle 2x2 posts on its open sides
#endif
#ifndef SCREENS_MORE
#define SCREENS_MORE 1 // 3: 2-3 screens in small fleets; screens face the nearest recent enemy; guards line up with the queen
#endif

// ====================================================================================================
// ENDGAME: FEEDING, GIANT, HEIR
// ====================================================================================================
#ifndef FEED_WINDOW
#define FEED_WINDOW 50 // last rounds in which farming may sacrifice feeders
#endif
#ifndef FEED_ONLY_IF_CONTEST
#define FEED_ONLY_IF_CONTEST 1 // no endgame feeding once the enemy queen is dead (ours just has to live)
#endif
#ifndef HEIR
#define HEIR 1 // enemy queen dead: our longest other dragon becomes a leader (no growth splits, pearl priority)
#endif
#ifndef LATE_LONGEST_ONLY
#define LATE_LONGEST_ONLY 30 // last rounds: without our queen, feed only the longest leader (total length is the last tiebreak)
#endif
#ifndef FEEDER_TRAIL
#define FEEDER_TRAIL 1 // 31: clusters of enemy death drops (their feeders) point at their queen
#endif
#ifndef GIANT
#define GIANT 1 // 25: open maps from CONSOL_ROUND: the longest non-queen leader is fed too (insurance)
#endif
#ifndef GIANT_SHARE
#define GIANT_SHARE 3 // one feeder in this many feeds the giant while the queen lives
#endif
#ifndef FEED_BEHIND_OPEN
#define FEED_BEHIND_OPEN 300 // behind on queen length: feeding starts here on open maps...
#endif
#ifndef FEED_BEHIND_CONGESTED
#define FEED_BEHIND_CONGESTED 400 // ...and here on congested ones
#endif
#ifndef FEED_CROWD
#define FEED_CROWD 3 // teammates already within 5 of the fed dragon: the rest wait
#endif
#ifndef CAP_FEED
#define CAP_FEED 1 // 24: fleet at its cap from CAP_FEED_ROUND and the queen safe: spare dragons feed her
#endif
#ifndef CAP_FEED_ROUND
#define CAP_FEED_ROUND 150
#endif
#ifndef QUEEN_FEED_CAP
#define QUEEN_FEED_CAP 16 // stop feeding her past max(this, their queen + QUEEN_FEED_MARGIN): a huge queen is hard to keep alive
#endif
#ifndef QUEEN_FEED_MARGIN
#define QUEEN_FEED_MARGIN 6
#endif
#ifndef GIANT_ALL
#define GIANT_ALL 1 // from GIANT_ROUND our longest non-queen dragon becomes a leader on every map (no growth splits,
                    // pearl priority, leader safety) and feeders feed it too: the longest-dragon tiebreak stays ours
#endif
#ifndef GIANT_ROUND
#define GIANT_ROUND 200
#endif
#ifndef GIANT_MIN_LEN
#define GIANT_MIN_LEN 5 // (10 tested: Portals 5 losses)
#endif
#ifndef ROLE_CLAIMS
#define ROLE_CLAIMS 0 // 6d (tested, OFF): giant / heir / leader holders announce their role (T_ROLE, relayed). Claims did
                      // not spread: on Around UNSW most giants heard 0 claims in 100 rounds (sonar stops at kelp and at the
                      // first dragon), still up to 16 giants and ~11 non-queen leaders at once
#endif
#ifndef ROLES_AFTER_SWITCH
#define ROLES_AFTER_SWITCH 0 // 6d' (tested, OFF: 196-8, Qd 40, Portals 81-3): giants and elected (non-queen) leaders only after the numbers -> length switch: before
                             // it they stop long dragons from splitting while the fleet should still grow (up to 16
                             // giants / 26 leaders at once on big maps; the switch is fleet-wide via T_PHASE)
#endif
#ifndef CLAIM_TTL
#define CLAIM_TTL 20 // a claim not renewed for this long is forgotten
#endif
#ifndef CLAIM_RELAY
#define CLAIM_RELAY 10 // a dragon passes one claimant's claim on at most once per this many rounds
#endif
#ifndef GIANT_ONE
#define GIANT_ONE 1 // F1: one giant/heir, only with GIANT_MIN_UNITS dragons; leaving the role drops the leader flag
#endif
#ifndef GIANT_MIN_UNITS
#define GIANT_MIN_UNITS 8
#endif
#ifndef FEED_FIX
#define FEED_FIX 1 // P4 (ladder trial; locally more queen deaths, 0 = v2.14.2): feeders die only where she can walk to the drop; drops near her are always hers; short queens fed earlier
#endif
#ifndef FEED_DELIVERY
#define FEED_DELIVERY 1 // (passed: 198-6, Qd 38, prot 84-0) 7e: a feeder dies only when 2/3 of its drop cells lie within FEED_DROP_DIST water steps of her head
#endif
#ifndef FEED_DROP_DIST
#define FEED_DROP_DIST 5
#endif
#ifndef FEED_PATH
#define FEED_PATH 3 // feeder head at most this many water steps from her head (straight-line distance put drops behind kelp)
#endif
#ifndef FEED_EAT_DIST
#define FEED_EAT_DIST 5 // pearls this close to her (path) that no enemy can reach are always hers (leash, late-safe aside)
#endif
#ifndef FEED_KEEP_RADIUS
#define FEED_KEEP_RADIUS 5 // from FEED_KEEP_ROUND teammates leave pearls this close to her head
#endif
#ifndef FEED_KEEP_ROUND
#define FEED_KEEP_ROUND 300
#endif
#ifndef FEED_UNKNOWN_GOAL
#define FEED_UNKNOWN_GOAL 8 // their queen unseen: ours counts as behind (earlier feeding) below this length
#endif


// ===== core.hpp =====
// CORE - geometry on the wrapping grid, global sizes, rng, decision trace.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

namespace {


// Geometry
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

// Decision trace (local testing only): with FLEET_TRACE=<dir> set, every turn
// appends "round id length head action sonars | note" to <dir>/<team><id>.txt.
// tools/testing/trace.py compares two builds turn by turn. Off in the judge.
std::FILE* traceF = nullptr;
#ifdef QDIAG
void diag_note(char const* what) // debug: one line per event (FLEET_DIAG)
{
    if (char const* df = std::getenv("FLEET_DIAG"))
        if (std::FILE* f = std::fopen(df, "a"))
        {
            std::fprintf(f, "N %s\n", what);
            std::fclose(f);
        }
}
#define DIAG(x) diag_note(x)
#else
#define DIAG(x) ((void)0)
#endif
std::string traceAct = "fallback";

} // namespace

// ===== world.hpp =====
// L0 WORLD - what this dragon knows: edges, portals, symmetry, tiles and pearls, the dragons in
// view, fleet reports, enemy reports and lengths, dangers, pearl claims.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

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

// ===== state.hpp =====
// STATE - this dragon's strategy state:
//   Situation sit  team-level facts (phase, strength, map class, queens), computed in compute_fleet_state
//   Duty my        this dragon's roles, decided in compute_fleet_state
//   then identity, queen status, and the commitments a dragon carries from one turn to the next.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

namespace {

enum Role { ROLE_COLLECTOR, ROLE_EXPLORER, ROLE_PORTAL, ROLE_COMBAT };
enum Mode { MODE_NORMAL, MODE_FARM, MODE_PRESERVE, MODE_COMBAT };

// L2: where we stand. Clock, strength, map and queens are team-level; local threat is this dragon's.
struct Situation
{
    // Clock / phase
    bool expansion = true;    // numbers first, until the fleet first reaches its expansion target
    bool openPlan = false;    // OPEN_PLAN: multiply until we have the majority
    bool majority = false;    // OPEN_PLAN majority (with hysteresis)
    bool switched = false;    // DYN_ALLOC: numbers -> length switch has happened (synced by T_PHASE)
    int switchRound = SW_HI;  // DYN_ALLOC: when it happens, from the enemy pressure
    int pressureMax = 0;      // DYN_ALLOC: most enemies anyone has seen
    bool heardPressure = false;
    int firstR = -1;          // first round this dragon played
    bool transitioned = false; // leader phase (section 12 table)
    int transitionRound = 300;
    bool consolidate = false; // late: length over numbers
    bool lateSafe = false;    // v2.12: last rounds - the queen keeps to safe ground
    bool queenSplits = true;  // the queen splits rather than grows (see QUEEN_GROW_ROUND)
    bool fleetEstablished = false; // P2: the fleet once had ADAPT_FLEET dragons
    bool queenOpening = false;     // P2: the queen splits and roams as before the home rule
    // Strength
    bool survival = false;    // v2.12: tiny fleet, heavily outnumbered or the last rounds
    bool outnumbered = false;
    bool majorityHunt = false; // smaller map with a clear majority: every spare dragon hunts
    int enemyEst = 0;          // v2.14: enemy count for the trade gates (scaled by our coverage)
    int distinctEnemies = 0;   // enemy ids heard of in the last 60 rounds
    // Map
    bool openMap = false;      // big map with little kelp: long dragons do not trap themselves
    // Queens
    bool ourQueenDead = false, enemyQueenDead = false;
    int lastEqSeenR = -1000;   // last round their queen was seen or reported
    // Local (this dragon)
    int localThreat = 0;
    int echoEnemy = 0;
    Mode mode = MODE_NORMAL;
};
Situation sit;

// 7c: the map class from its size - known exactly and the same on every dragon, so team decisions can use it
// (learned features such as the kelp share stay in sit.openMap). One place instead of six size thresholds.
struct MapProfile
{
    bool small = false;     // N <= SMALL_MAP_CELLS: majority hunting, hunters may go after their queen
    bool runners = false;   // N >= RUNNER_MIN_CELLS: explorer-role runners
    bool wall = false;      // N >= WALL_MIN_CELLS: the moving wall
    bool openSized = false; // N >= OPEN_MIN_CELLS: big enough to count as open when kelp is scarce
    double rvFactor = 1.0;  // weight of the richness / centre pulls (40% at least on small maps)
    int outposts = 2;       // how many outposts (2-8)
};
MapProfile mapProf;
void set_map_profile()
{
    mapProf.small = N <= SMALL_MAP_CELLS;
    mapProf.runners = N >= RUNNER_MIN_CELLS;
    mapProf.wall = N >= WALL_MIN_CELLS;
    mapProf.openSized = N >= OPEN_MIN_CELLS;
    mapProf.rvFactor = std::clamp(static_cast<double>(N) / RV_FULL_CELLS, 0.4, 1.0);
    mapProf.outposts = std::clamp(N / 250, 2, 8);
}

// L3: this dragon's roles.
struct Duty
{
    Role role = ROLE_COLLECTOR; // id-hash temperament (collector / explorer / portal / combat)
    bool leader = false;     // the queen, elected leaders, the giant and the heir
    bool roleLeader = false; // F1: leader only through the giant / heir role (cleared when the role ends)
    bool hunterSlot = false; // in the hunter group (scouts portals before the hunt starts)
    bool hunter = false;
    bool guard = false;
    bool screen = false;     // guard posted out toward the enemy to see attackers early
    bool defender = false;   // holds our half (medium length, camps near the middle)
    bool wall = false;       // P8: in the moving wall (joins the packs)
    bool giant = false;      // P5: our longest non-queen dragon grows (longest-dragon tiebreak)
    bool heir = false;       // enemy queen dead: the dragon that grows instead of our queen
};
Duty my;

// 6f: the id-slot plan - every dragon has a slot 0-9 from its id (hunter_slot); this table says which slots take which
// share role (the same on every dragon, no messages needed). Same shares as before, now in one place.
struct SlotPlan
{
    bool hunter(int s) const { return s < HUNTER_SHARE; }
    bool guard(int s) const { return s >= HUNTER_SHARE && s < HUNTER_SHARE + GUARD_SHARE; }
    bool screen(int s) const { return s == HUNTER_SHARE; }       // the first guard slot screens
    bool screen2(int s) const { return s == HUNTER_SHARE + 1; }  // a second screen in small fleets
    bool defender(int s) const { return s == DEFENDER_SLOT; }
    bool wall(int s) const { return s >= WALL_SLOT_LO && s <= WALL_SLOT_HI; }
};
constexpr SlotPlan kSlots{};

bool wall_now();
bool wall_slot(int id);
int est_enemy_units();

// Identity: ids 0 and 1 are the queens; which one is ours is learned.
int queenId = 0, enemyQueenId = 1;
bool amQueen = false;
bool queenSeen = false; // queen identity confirmed by vision (team is shown there)
int ourQueenSpawn = -1; // where our queen started (the enemy queen started at its mirror image)

// Reports kept beside the world model
struct ZoneRep
{
    int cell, tiles, round;
};
std::vector<ZoneRep> zoneReps; // pearl-rich zones heard over sonar

// The queens
int qBunkerTl = -1, qBunkerR = -1, inferTl = -1, inferR = -1; // the queen's bunker box as she last announced it
bool sealedChecked = false, queenSealed = false;
int sealedCells = 0; // size of the queen's sealed starting pocket
bool queenStarvingG = false; // v2.14: the queen's starving flag, for pearl gathering
std::unordered_map<int, std::pair<int, int>> escortSeen; // enemy id -> (last round within 2 of our queen, rounds in a row)
int restCell = -1;          // v2.14: the queen's resting spot (open water inside her leash)
int queenLeash = QUEEN_LEASH; // P2: her leash this turn
int lastThreatR = -1000;      // last round an enemy head was near her
int prevEnemyQueenCell = -1;
int prevEnemyQueenLen = 0;
std::vector<int> prevEnemyQueenBody, eqBodyNow; // her body cells we saw last turn / this turn

// Commitments: what this dragon carries from one turn to the next
int lastEatR = 0, lastLenSeen = 0;
std::vector<int> chaseCnt, chaseLast; // F4: rounds each pearl was chased, last round chased
int dryStart = -1;                    // F4: first round of the current run of "pearl" without eating
int campTile = -1, campArriveR = -1;  // F6: the tile we camp by, and since when we are next to it
std::vector<int> headHist;  // our head, last rounds (loop stall)
bool miningTrip = false;    // v2.15: on a dead-end trip that paid >= 1 length when we went in
int lastPearl = -1;         // the pearl this dragon went for last turn
int lastGoal = -1, lastGoalR = -1; // 6c: last non-pearl goal and when
int lastGoalIntent = -1; // 6c: its Intent
int stallTarget = -1, stallBest = INF, stallRound = 0; // progress toward the current pearl
int exploreGoal = -1, exploreSetRound = -1000;
int claimTarget = -1, claimEta = 0;
std::string note;

} // namespace

// ===== comms.hpp =====
// L7 COMMS (wire format) - message layout, checksum, encoders, the fact and pearl outboxes.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

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

// ===== perceive.hpp =====
// L0 PERCEIVE - learning edges and symmetry, reading vision, own body and sonar, death detection.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

namespace {

// Learning
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
    edgeVersion++;
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
        edgeVersion++;
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
            edgeVersion++;
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

void note_drop(int c, int round)
{
    if (!FEEDER_TRAIL)
        return;
    for (auto const& e : dropEv)
        if (e.cell == c && std::abs(e.round - round) <= 2)
            return;
    bool nearQ = false;
    auto const q = fleet.find(queenId);
    if (!sit.ourQueenDead && q != fleet.end() && R - q->second.round <= 20 && manhattan(q->second.cell, c) <= 6)
        nearQ = true;
    if (!sit.ourQueenDead && me == queenId && !body.empty() && manhattan(body[0], c) <= 6)
        nearQ = true;
    dropEv.push_back({c, round, nearQ});
    if (dropEv.size() > 64)
        dropEv.erase(dropEv.begin());
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
        if (!wasPresent && k < 0)
        {
            dropRound[c] = R; // a pearl on a tile that never spawns: a death drop
            note_drop(c, R);
        }
        if (!wasPresent)
        {
            queue_pearl(1, c, R, k < 0 ? 75 : 60); // death drops first: teammates come and collect
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
            edgeVersion++;
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

// Reading the turn
void read_vision(Controller& ct)
{
    std::fill(vis.begin(), vis.end(), 0);
    std::fill(occId.begin(), occId.end(), -1);
    heads.clear();
    enemyTails.clear();
    eqBodyNow.clear();
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
            if (p->get_id() <= 1 && me > 1)
            {
                bool const ours = p->get_team() == myTeam;
                queenSeen = true;
                queenId = ours ? p->get_id() : 1 - p->get_id();
                enemyQueenId = 1 - queenId;
            }
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
    // The queen also assumes enemies use their free sprint steps.
    if (me == queenId && !QUEEN_THREAT_MAP)
        for (auto const& h : heads)
        {
            if (h.mine)
                continue;
            int const reach = std::max(1, (h.len + 3) / 4);
            if (reach <= 1)
                continue;
            for (int c = 0; c < N; c++)
                if (vis[c] && !(occId[c] >= 0) && manhattan(c, h.cell) <= reach)
                    contested[c] = 1;
        }
    for (auto& h : heads)
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
        bool const partial = m != partCount[h.id] || cheb(chain.back(), myHead) > 2;
        if (!h.mine && h.id == enemyQueenId)
            eqBodyNow = chain;
        if (!h.mine)
        {
            h.partial = partial;
            h.estLen = est_enemy_len(h.id, std::max(m, partCount[h.id]), partial);
            remember_enemy_len(h.id, partial ? std::max(m, partCount[h.id]) : m, !partial);
        }
        if (partial)
            continue; // tail may continue out of view
        if (!h.mine && m >= 4)
            enemyTails.push_back(chain.back());
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
    // Replay last turn's steps from last turn's body: exact even when the
    // body is broken across a portal or the move was a sprint.
    if (!prevBody.empty())
    {
        std::vector<int> path;
        int pos = prevBody[0];
        bool ok = true;
        for (std::size_t i = 0; i < lastDirs.size(); i++)
        {
            pos = cross(pos, lastDirs[i]);
            if (pos < 0)
            {
                if (pos == -3 && i + 1 == lastDirs.size())
                    pos = head; // first trip through this portal: we now stand at its far side
                else
                {
                    ok = false;
                    break;
                }
            }
            path.push_back(pos);
        }
        if (ok && pos == head)
        {
            std::vector<int> pred(path.rbegin(), path.rend());
            pred.insert(pred.end(), prevBody.begin(), prevBody.end());
            if (static_cast<int>(pred.size()) > L)
                pred.resize(L);
            bool consistent = true;
            for (std::size_t i = 0; i < chain.size() && i < pred.size(); i++)
                consistent &= chain[i] == pred[i];
            // B1: a cell we can see that is not ours cannot be part of our body (the rest is out of view).
            for (std::size_t i = chain.size(); i < pred.size() && BODY_TRUST_VISION; i++)
                consistent &= !(vis[pred[i]] && occId[pred[i]] != me);
            if (consistent && pred.size() >= chain.size())
            {
                body = pred;
                return;
            }
        }
    }
    if (static_cast<int>(chain.size()) < L && !prevBody.empty())
    {
        auto const it = std::find(prevBody.begin(), prevBody.end(), chain.back());
        if (it != prevBody.end())
            for (auto j = it + 1; j != prevBody.end() && static_cast<int>(chain.size()) < L; ++j)
            {
                if (BODY_TRUST_VISION && vis[*j] && occId[*j] != me)
                    break; // B1: visibly not ours - the rest of the body is out of view
                if (!inChain[*j])
                {
                    chain.push_back(*j);
                    inChain[*j] = 1;
                }
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

void add_enemy(int cell, int round, int id, int len, int estLen = 0)
{
    if (id == enemyQueenId)
        sit.lastEqSeenR = std::max(sit.lastEqSeenR, round);
    for (auto& e : enemyReps)
        if (e.id == id && e.round <= round)
        {
            e = {cell, round, id, len, estLen};
            return;
        }
    enemyReps.push_back({cell, round, id, len, estLen});
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
        StatusMsg const st = read_status(payload);
        int const id = st.id, len = st.len, cell = st.cell, leader = st.leader, round = st.round, eqDead = st.eq, switchedBit = st.sw;
        if (id == me || cell >= N || round > R)
            return false;
        // 6d'': the best giant / heir claim the sender knows; keep the best fresh one.
        if (GIANT_GOSSIP && st.gl > 0)
        {
            GossipClaim const c{st.gl, st.gh, round - st.ga * GOSSIP_AGE_UNIT};
            if (gossip_fresh(c) && (!gossip_fresh(gBest) || better_claim(c.gl, c.gh, gBest) ||
                                    (c.gl == gBest.gl && c.gh == gBest.gh && c.born > gBest.born)))
                gBest = c;
        }
        // Q: the news that their queen is dead rides on every status (the dedicated message rarely reached our
        // queen: sonar runs along rows and columns and stops at the first dragon).
        if (EQ_STATUS_BIT && eqDead && !sit.enemyQueenDead)
            sit.enemyQueenDead = true;
        if (SWITCH_STATUS_BIT && switchedBit && !sit.switched && R >= SW_LO)
            sit.switched = true; // 6e: the fleet switches together (same rule as T_PHASE)
        if (id <= 1 && me > 1 && !queenSeen) // only teammates speak our protocol, so this is our queen
        {
            queenId = id;
            enemyQueenId = 1 - id;
        }
        auto it = fleet.find(id);
        if (it != fleet.end() && it->second.round >= round)
            return false;
        fleet[id] = {len, cell, round, leader == 1};
        if (id == queenId)
            return true; // relay the queen's position across the fleet
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
                if (nextSpawn[cell] == -2 && pearlState[cell] != 1)
                    note_drop(cell, round); // a teammate saw a death drop
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
        bool const partial = b.get(1) == 1;
        if (cell >= N || round > R)
            return false;
        if (R - round <= 2)
            remember_enemy_len(id, len, !partial);
        add_enemy(cell, round, id, len, est_enemy_len(id, len, partial));
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
    case T_ROLE: {
        int const role = b.get(2), id = b.get(14), len = b.get(10), round = b.get(9);
        if (role > CR_HEIR || id == me || round > R || R - round > CLAIM_TTL)
            return false;
        auto& book = roleClaims[role];
        auto const it = book.find(id);
        if (it != book.end() && it->second.round >= round)
            return false;
        book[id] = {len, round};
        // Pass it on, at most once per CLAIM_RELAY rounds per claimant (the fleet must hear it, not drown in it).
        auto& rel = claimRelayed[role][id];
        if (rel != 0 && R - rel < CLAIM_RELAY)
            return false;
        rel = R;
        return true;
    }
    case T_STRIKE: {
        int const id = b.get(14), cell = b.get(12), round = b.get(9), off = b.get(1);
        if (!STRIKE_NEWS || id == me || cell >= N || round > R || R - round > STRIKE_CONFIRM)
            return false;
        for (auto& w : strikeWatch)
            if (w.striker == id)
            {
                if (w.round >= round)
                    return false;
                w = {id, cell, round, off == 1};
                return true;
            }
        strikeWatch.push_back({id, cell, round, off == 1});
        return true;
    }
    case T_ZONE: {
        int const cell = b.get(12), round = b.get(9), tiles = b.get(6);
        if (cell >= N || round > R || R - round > 60)
            return false;
        for (auto& z : zoneReps)
            if (manhattan(z.cell, cell) <= 3)
            {
                bool const news = round > z.round + 5 || tiles > z.tiles;
                if (round >= z.round)
                    z = {z.cell, std::max(z.tiles, tiles), round};
                return news;
            }
        zoneReps.push_back({cell, tiles, round});
        if (zoneReps.size() > 24)
            zoneReps.erase(zoneReps.begin());
        return true;
    }
    case T_BUNKER: {
        int const tl = b.get(12), round = b.get(9);
        if (tl >= N || round > R || round <= qBunkerR)
            return false;
        qBunkerTl = tl;
        qBunkerR = round;
        return R - round <= 3;
    }
    case T_PHASE: {
        int const pm = b.get(6), sw = b.get(1), round = b.get(9);
        if (round > R)
            return false;
        sit.heardPressure = true;
        bool changed = pm > sit.pressureMax || (sw == 1 && !sit.switched);
        sit.pressureMax = std::max(sit.pressureMax, pm);
        if (sw == 1 && R >= SW_LO)
            sit.switched = true;
        return changed;
    }
    case T_QSPAWN: {
        int const cell = static_cast<int>(b.get(12));
        if (ourQueenSpawn < 0 && cell < N)
        {
            ourQueenSpawn = cell;
            return true;
        }
        return false;
    }
    case T_QUEEN_DEAD: {
        bool const enemy = b.get(1) == 1;
        if (enemy && !sit.enemyQueenDead)
        {
            sit.enemyQueenDead = true;
            return true;
        }
        if (!enemy && !sit.ourQueenDead && !amQueen)
        {
            sit.ourQueenDead = true;
            return true;
        }
        return false;
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
        {
            int type;
            std::uint64_t payload;
            int prio = 15;
            bool const typed = unpack(m, type, payload);
            if (QUEEN_POS_RELAY && typed && type == T_STATUS && me != queenId)
            {
                // The queen's position: guards and screens need it, so it goes
                // out ahead of pearl gossip while fresh.
                StatusMsg const st = read_status(payload);
                int const id = st.id;
                int const round = st.round;
                if (id == queenId && R - round <= 4)
                    prio = 45;
                else if (GIANT && st.leader && R - round <= 4)
                    prio = 35; // leaders (the giant among them) are fed too
            }
            if (ROLE_CLAIMS && typed && type == T_ROLE)
                prio = 50; // one-of-a-kind roles: the claim has to cross the fleet to work
            if (DYN_ALLOC && typed && type == T_PHASE)
                prio = 40; // the whole fleet must switch together
            if (BUNKER_GUARDS && typed && type == T_BUNKER)
                prio = 45; // guards must find her box
            if (AIM_REPORTS && me != queenId && typed && type == T_ENEMY)
            {
                // An enemy near our queen: pass it on first (and toward her).
                auto const q = fleet.find(queenId);
                int const cell = static_cast<int>(payload & 0xFFF);
                if (q != fleet.end() && R - q->second.round <= 3 && cell < N && manhattan(cell, q->second.cell) <= 14)
                    prio = 85;
            }
            queue_fact(m, prio, prio > 15 ? 1 : 2); // gossip: pass new facts along the chain
        }
    SonarEchoes const e = ct.get_sonar_echoes();
    sit.echoEnemy = e.enemy + e.enemy_head;
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
        if (id == queenId && !sit.ourQueenDead)
        {
            sit.ourQueenDead = true;
            queue_fact(msg_queen_dead(false), 95, 4);
        }
    }
    // The enemy queen vanished from the middle of our window: it died. Or she
    // is out of view and a death-drop pearl (on a tile that never spawns) lies
    // on her last body or within one turn's move of her head: a head-on kills
    // both, and the striker's own sonar dies with it, so watchers must tell.
    if (prevEnemyQueenCell >= 0 && !sit.enemyQueenDead && !visibleIds.count(enemyQueenId))
    {
        int const fq = EQ_DEATH_DETECT ? std::max(1, (prevEnemyQueenLen + 3) / 4) : 1;
        bool dead = cheb(prevEnemyQueenCell, head) <= 3 - fq;
        for (int c = 0; c < N && !dead && EQ_DEATH_DETECT; c++)
        {
            if (!vis[c] || dropRound[c] != R)
                continue;
            if ((!EQ_STRICT && manhattan(c, prevEnemyQueenCell) <= fq) ||
                std::find(prevEnemyQueenBody.begin(), prevEnemyQueenBody.end(), c) != prevEnemyQueenBody.end())
                dead = true;
        }
        if (dead)
        {
            sit.enemyQueenDead = true;
            queue_fact(msg_queen_dead(true), 95, 4);
        }
    }
    // The same from a fleet report of her (at most three rounds old: a watcher
    // that moves before the striker sees the drop only next round): a fresh
    // death drop near where she was, and she is not in our window.
    if (EQ_DEATH_DETECT && !EQ_STRICT && !sit.enemyQueenDead && !visibleIds.count(enemyQueenId))
        for (auto const& e : enemyReps)
        {
            int const age = R - e.round;
            if (e.id != enemyQueenId || age < 1 || age > 3 || cheb(e.cell, head) > 3 + age)
                continue;
            int const rad = std::min(3, std::max(1, (e.len + 3) / 4) * age);
            if (cheb(e.cell, head) + rad <= 3)
            {
                sit.enemyQueenDead = true; // every cell she could have reached is in view, and she is not
                queue_fact(msg_queen_dead(true), 95, 4);
                break;
            }
            for (int c = 0; c < N; c++)
                if (vis[c] && dropRound[c] == R && manhattan(c, e.cell) <= rad)
                {
                    sit.enemyQueenDead = true;
                    queue_fact(msg_queen_dead(true), 95, 4);
                    break;
                }
        }
    prevEnemyQueenCell = -1;
    prevEnemyQueenBody = eqBodyNow;
    for (auto const& h : heads)
        if (!h.mine && h.id == enemyQueenId)
        {
            prevEnemyQueenCell = h.cell;
            prevEnemyQueenLen = std::max(h.len, h.estLen);
        }
    prevTeamHeads.clear();
    for (auto const& h : heads)
        if (h.mine)
            prevTeamHeads[h.id] = h.cell;
}

// Q2c: strike news that stopped. A striker dies in its strike and casts no sonar; alive, it either says "within
// reach" again or "off". So news with neither for STRIKE_CONFIRM rounds, while nobody saw her, means she died.
void strike_news_check()
{
    if (!STRIKE_NEWS)
        return;
    std::erase_if(strikeWatch, [](StrikeWatch const& w) { return w.off || sit.lastEqSeenR > w.round || R - w.round > STRIKE_CONFIRM + 2; });
    if (sit.enemyQueenDead)
    {
        strikeWatch.clear();
        return;
    }
    for (auto const& w : strikeWatch)
        if (R - w.round >= STRIKE_CONFIRM)
        {
#ifdef QDIAG
            char buf[96];
            std::snprintf(buf, sizeof buf, "Q2C %c %d id%d striker%d sround%d", myTeam == Team::A ? 'A' : 'B', R, me, w.striker, w.round);
            DIAG(buf);
#endif
            if (STRIKE_NEWS == 1)
            {
                sit.enemyQueenDead = true;
                queue_fact(msg_queen_dead(true), 95, 4);
            }
            strikeWatch.clear();
            return;
        }
}

void record_enemies()
{
    int shared = 0;
    for (auto const& h : heads)
    {
        if (h.mine)
            continue;
        add_enemy(h.cell, R, h.id, h.len, h.estLen);
        if (h.id == enemyQueenId)
            queue_fact(msg_enemy(h.cell, h.id, h.len, h.partial), 92, 2); // "one smells blood"
        else if (shared < 2)
        {
            int prio = 70;
            auto const q = fleet.find(queenId);
            if (AIM_REPORTS && me != queenId && q != fleet.end() && R - q->second.round <= 3 && manhattan(h.cell, q->second.cell) <= 14)
                prio = 88; // a threat to the queen goes out before anything else
            queue_fact(msg_enemy(h.cell, h.id, h.len, h.partial), prio, 1);
            shared++;
        }
    }
}

} // namespace

// ===== graph.hpp =====
// L1 GRAPH - body occupancy over time, flood fill, planning BFS, the queen survival search,
// step simulation and dead-end dives.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

namespace {

// Occupancy, flood fill and path finding
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

// Scoped overlays (Architecture_v2 3b). A helper that searches with a simulated body puts it on the board for
// its own scope only: BodyScope restores our real body (body, L) when it ends, WallScope removes the temporary
// walls (e.g. the parent's front segments while checking a split child). Outside every scope the board shows
// our real body and no extra walls.
struct BodyScope
{
    BodyScope(std::vector<int> const& b, int len) { set_body(b, len); }
    ~BodyScope() { set_body(body, L); }
    BodyScope(BodyScope const&) = delete;
    BodyScope& operator=(BodyScope const&) = delete;
};
struct WallScope
{
    WallScope() { wstamp++; }
    void wall(int c) { wallStamp[c] = wstamp; }
    ~WallScope() { wstamp++; }
    WallScope(WallScope const&) = delete;
    WallScope& operator=(WallScope const&) = delete;
};

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
            if (n == -2) // GraphView: the FRONTIER search - flag it, do not enter
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
std::vector<int> planSteps;
std::vector<std::vector<int>> planRing;

void plan_bfs(int start, std::vector<int>& dist)
{
    std::fill(dist.begin(), dist.end(), INF);
    if (UNKNOWN_EDGE_COST > 1)
    {
        // Dial's algorithm: known edges cost 1, edges nobody has seen cost
        // more (in a maze most of them turn out to be kelp). Bodies are still
        // timed by the number of steps, not by the cost.
        int const K = UNKNOWN_EDGE_COST + 1;
        planRing.resize(K);
        for (auto& b : planRing)
            b.clear();
        dist[start] = 0;
        planSteps[start] = 0;
        planRing[0].push_back(start);
        int pending = 1;
        for (int cost = 0; pending > 0; cost++)
        {
            auto& bk = planRing[cost % K];
            for (std::size_t i = 0; i < bk.size(); i++)
            {
                int const c = bk[i];
                if (dist[c] != cost)
                    continue;
                for (int d = 0; d < 4; d++)
                {
                    int n = cross(c, d);
                    int w = 1;
                    if (n == -2) // GraphView: open, at a cost (Dial planner)
                    {
                        n = nb(c, d);
                        w = UNKNOWN_EDGE_COST;
                    }
                    int const nt = planSteps[c] + 1;
                    if (n < 0 || cost + w >= dist[n] || !passable(n, nt))
                        continue;
                    dist[n] = cost + w;
                    planSteps[n] = nt;
                    planRing[(cost + w) % K].push_back(n);
                    pending++;
                }
            }
            pending -= static_cast<int>(bk.size());
            bk.clear();
        }
        return;
    }
    bfsQ.clear();
    bfsQ.push_back(start);
    dist[start] = 0;
    for (std::size_t i = 0; i < bfsQ.size(); i++)
    {
        int const c = bfsQ[i];
        int const t = dist[c] + 1;
        for (int d = 0; d < 4; d++)
        {
            int const n = step_open(c, d);
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

bool fountain(int c)
{
    return FOUNTAINS && gapHi[c] > 0 && gapHi[c] <= FOUNT_GAP; // refills (almost) at once: a pocket worth mining
}
bool in_pocket(int c);

bool pearl_at(int c)
{
    return pearlState[c] == 1 && (vis[c] || R - pearlRound[c] <= 3);
}

// Queen survival search
// Area alone cannot tell a dead-end corridor from open water, and a snake
// cannot turn round in a corridor. So for the queen we look for any path of
// `need` further moves through known water. Body cells (set_body) free up
// over time; cells the head passes through stay occupied for `len` moves.
std::vector<int> pathStamp, pathTime;
int pstamp = 1, survNodes = 0;

int survCap = SURV_NODE_CAP; // node budget of the current survival search
bool survive_rec(int h, int t, int need, int len)
{
    if (t >= need || ++survNodes > survCap)
        return true; // long enough (or out of budget: assume fine)
    for (int d = 0; d < 4; d++)
    {
        int n = cross(h, d);
        if (n == -2)
        {
            if (SURV_UNKNOWN_SAFE)
                return true;
            // Unexplored edge: assume open water, but our own body (often out
            // of view on a long queen) and other dragons still block.
            n = nb(h, d);
        }
        if (n < 0)
            continue;
        int const nt = t + 1;
        if (!passable(n, nt) || (nt == 1 && contested[n]))
            continue;
        if (pathStamp[n] == pstamp && nt < pathTime[n] + len + 1)
            continue;
        int const saveS = pathStamp[n], saveT = pathTime[n];
        pathStamp[n] = pstamp;
        pathTime[n] = nt;
        bool const ok = survive_rec(n, nt, need, len);
        pathStamp[n] = saveS;
        pathTime[n] = saveT;
        if (ok)
            return true;
    }
    return false;
}

// b is the body after the move (head first), with its length.
bool queen_can_survive(std::vector<int> const& b, int len, int nodeCap = SURV_NODE_CAP)
{
    BodyScope const sb(b, len);
    pstamp++;
    survNodes = 0;
    survCap = nodeCap;
    pathStamp[b[0]] = pstamp;
    pathTime[b[0]] = 0;
    return survive_rec(b[0], 0, std::min(2 * len + 6, 50), len);
}

// Body after a sequence of steps (free steps only), or empty if a step is illegal.
std::vector<int> simulate_steps(std::vector<int> b, int len, std::vector<int> const& dirs, int& outLen)
{
    std::vector<int> eaten;
    for (int d : dirs)
    {
        int const n = cross(b[0], d);
        if (n < 0)
            return {};
        bool const eat = pearl_at(n) && std::find(eaten.begin(), eaten.end(), n) == eaten.end();
        if (eat)
            eaten.push_back(n);
        b.insert(b.begin(), n);
        if (eat)
            len++;
        else if (static_cast<int>(b.size()) > len)
            b.pop_back();
    }
    outLen = len;
    return b;
}

// Dead-end dives
// A dead end that holds pearls can still be worth entering when the pearls pay
// for the way out. At the bottom the survival split leaves the front two
// segments behind and the rest leaves as a child, whose head is our old tail
// outside the dead end. This searches our own moves through known water;
// other dragons keep their bodies. Value = net segments won.
int diveNodes = 0, diveCap = DIVE_NODE_CAP;

// After a split of all but the front 2 segments, can the child get away?
bool split_child_escapes(std::vector<int> const& b, int len)
{
    if (static_cast<int>(b.size()) < len)
        return true; // tail out of view, well away from the dead end
    int const childLen = len - 2;
    std::vector<int> child(b.rbegin(), b.rbegin() + childLen);
    WallScope ws;
    for (int i = 0; i < len - childLen; i++)
        ws.wall(b[i]);
    BodyScope const sb(child, childLen);
    FloodRes const f = flood(child[0], child.back(), childLen + 8);
    return f.tail || f.area >= std::min(childLen + 1, 6);
}

double dive_rec(std::vector<int> const& b, int len, int depth, int eaten, std::vector<int>& eatenCells)
{
    int const h = b[0];
    set_body(b, len);
    int const tailCell = static_cast<int>(b.size()) >= len ? b.back() : -1;
    FloodRes const f = flood(h, tailCell, 2 * len + 4);
    if (f.tail || f.area + (f.frontier ? len : 0) >= 2 * len + 4 || (FRONTIER_OPEN && !my.leader && f.frontier))
        return eaten + 0.5; // back in open water: everything eaten is profit, nobody lost
    if (depth >= DIVE_DEPTH || ++diveNodes > diveCap)
        return eaten - 3; // unresolved: assume the split and a bit more
    double best = -1e9;
    for (int d = 0; d < 4; d++)
    {
        int const n = cross(h, d);
        if (n < 0)
            continue;
        set_body(b, len);
        if (!passable(n, 1))
            continue;
        bool const fount = fountain(n) && !(vis[n] && occId[n] >= 0 && occId[n] != me); // refills before we get there
        bool const eat = (pearl_at(n) || fount) && std::find(eatenCells.begin(), eatenCells.end(), n) == eatenCells.end();
        std::vector<int> nb2;
        nb2.reserve(b.size() + 1);
        nb2.push_back(n);
        nb2.insert(nb2.end(), b.begin(), b.end());
        int const nl = len + (eat ? 1 : 0);
        if (!eat && static_cast<int>(b.size()) >= len)
            nb2.pop_back();
        if (eat)
            eatenCells.push_back(n);
        best = std::max(best, dive_rec(nb2, nl, depth + 1, eaten + (eat ? 1 : 0), eatenCells));
        if (eat)
            eatenCells.pop_back();
    }
    if (best > -1e9)
        return best;
    // Stuck: the survival split saves all but the front two segments.
    if (len >= 4 && units < unitLimit && split_child_escapes(b, len))
        return eaten - 2;
    return eaten - len;
}

} // namespace

// ===== fields.hpp =====
// L1 FIELDS - per-turn maps: distances from my head, pearl richness, Voronoi territory, enemy
// strike reach (queen threat map, one and two turns), distances from our queen.
// Field cache (Architecture_v2 3c): each field is computed at most once per turn (or less often), on first use:
//   distHead            plan_bfs(head) in prepare_turn, every turn
//   vorOurs / vorEnemy  compute_voronoi(), once per round (vorRound)
//   regionVal           update_region_value(), every 5 rounds
//   qThreat* / qMargin  compute_queen_threat(), once per turn, queen only
//   qDist               queen_dist_bfs(), only when QUEEN_CLEAR is on
//   topology            topo_refresh() (topology.hpp), when an edge was learned (at most every TOPO_EVERY rounds)
// Per-candidate searches (flood, survival, dives, plan_bfs from a candidate cell) are not fields: they depend on
// the simulated body and run inside a BodyScope.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

namespace {

std::vector<int> distHead, distTmp;
std::vector<int> skipPearl; // round until which this dragon ignores the pearl (only reachable at a loss)

// P7: how pearl-rich the area round each cell is (expected pearls per round
// from known spawn tiles within RV_RAD, fountains most), normalised to [0, 1].
// Recomputed every 5 rounds.
std::vector<float> regionVal;
int regionValRound = -100;
double rv_size_factor() { return mapProf.rvFactor; }
void update_region_value()
{
    if (R - regionValRound < 5 && static_cast<int>(regionVal.size()) == N)
        return;
    regionValRound = R;
    regionVal.assign(N, 0.0f);
    for (int t = 0; t < N; t++)
    {
        if (!(nextSpawn[t] >= 0 || pearlState[t] != 0 || gapHi[t] > 0))
            continue;
        double const rate = gapHi[t] > 0 ? 1.0 / std::max(3, gapHi[t]) : 1.0 / 25.0;
        int const tx = t % W, ty = t / W;
        for (int dy = -RV_RAD; dy <= RV_RAD; dy++)
            for (int dx = -RV_RAD + std::abs(dy); dx <= RV_RAD - std::abs(dy); dx++)
            {
                int const x = tx + dx, y = ty + dy;
                if (x < 0 || y < 0 || x >= W || y >= H)
                    continue;
                regionVal[y * W + x] += static_cast<float>(rate * (RV_RAD + 1 - std::abs(dx) - std::abs(dy)) / (RV_RAD + 1));
            }
    }
    float mx = 0;
    for (float v : regionVal)
        mx = std::max(mx, v);
    if (mx > 0)
        for (float& v : regionVal)
            v /= mx;
}
double rv_norm(int c) { return REGION_VALUE && static_cast<int>(regionVal.size()) == N ? regionVal[c] : 0.0; }

// A: Voronoi fields - path distance from the nearest of our heads (vorOurs)
// and of theirs (vorEnemy) to every cell, over known water (unexplored
// edges open, bodies passable: they move). Seeds: heads in view and
// position reports up to VOR_REP_AGE rounds old.
std::vector<int> vorOurs, vorEnemy, vorQ;
int vorRound = -1;
void vor_bfs(std::vector<int> const& seeds, std::vector<int>& dist)
{
    dist.assign(N, INF);
    vorQ.clear();
    for (int c : seeds)
        if (c >= 0 && c < N && dist[c] != 0)
        {
            dist[c] = 0;
            vorQ.push_back(c);
        }
    for (std::size_t i = 0; i < vorQ.size(); i++)
    {
        int const c = vorQ[i];
        for (int k = 0; k < 4; k++)
        {
            int const n = step_open(c, k); // unexplored edge: assume open
            if (n < 0 || dist[n] <= dist[c] + 1)
                continue;
            dist[n] = dist[c] + 1;
            vorQ.push_back(n);
        }
    }
}
void compute_voronoi()
{
    if (!VORONOI || vorRound == R)
        return;
    vorRound = R;
    std::vector<int> ours, theirs;
    std::unordered_set<int> seen;
    for (auto const& h : heads)
    {
        (h.mine ? ours : theirs).push_back(h.cell);
        seen.insert(h.id);
    }
    if (!body.empty())
        ours.push_back(body[0]);
    for (auto const& [id, st] : fleet)
        if (!seen.count(id) && R - st.round <= VOR_REP_AGE)
            ours.push_back(st.cell);
    for (auto const& e : enemyReps)
        if (!seen.count(e.id) && R - e.round <= VOR_REP_AGE)
            theirs.push_back(e.cell);
    if (!VOR_TURNS)
    {
        vor_bfs(ours, vorOurs);
        vor_bfs(theirs, vorEnemy);
        return;
    }
    // 7h: arrival in TURNS - a dragon of length L covers ceil(L/4) cells a turn for free (speed classes 1, 2, 3+).
    // Stored as "cells at my speed" so the rest of the code keeps comparing them with its own path lengths.
    auto speed = [](int len) { return std::clamp((len + 3) / 4, 1, 3); };
    std::vector<int> oursS[4], theirsS[4];
    for (auto const& h : heads)
        (h.mine ? oursS : theirsS)[speed(h.mine ? h.len : std::max(h.len, h.estLen))].push_back(h.cell);
    if (!body.empty())
        oursS[speed(L)].push_back(body[0]);
    for (auto const& [id, st] : fleet)
        if (!seen.count(id) && R - st.round <= VOR_REP_AGE)
            oursS[speed(st.len)].push_back(st.cell);
    for (auto const& e : enemyReps)
        if (!seen.count(e.id) && R - e.round <= VOR_REP_AGE)
            theirsS[speed(std::max(e.len, e.estLen))].push_back(e.cell);
    int const mine = speed(L);
    static std::vector<int> tmp;
    auto build = [&](std::vector<int>* bySpeed, std::vector<int>& out) {
        out.assign(N, INF);
        for (int s = 1; s <= 3; s++)
        {
            if (bySpeed[s].empty())
                continue;
            vor_bfs(bySpeed[s], tmp);
            for (int c = 0; c < N; c++)
                if (tmp[c] < INF)
                    out[c] = std::min(out[c], (tmp[c] + s - 1) / s * mine);
        }
    };
    build(oursS, vorOurs);
    build(theirsS, vorEnemy);
}
// Margin of a cell: > 0 we get there first by that many steps, < 0 they do.
int vor_margin(int c)
{
    if (!VORONOI || static_cast<int>(vorEnemy.size()) != N || vorEnemy[c] >= INF)
        return 99;
    return vorEnemy[c] - vorOurs[c];
}

// Sprints: the first ceil(L/4) steps of a turn are free.
int free_steps() { return (L + 3) / 4; }
int enemy_reach(HeadInfo const& h) { return std::max(1, (h.len + 3) / 4); }

// Danger of ending a turn on cell n: next to a teammate head, or where an
// enemy head can reach us (leaders assume the enemy sprints its free steps).
// Queen threat map: the fewest paid sprint steps an enemy needs to put its
// head on a cell (0 = within its free steps, INF = out of reach). A head-on
// kills both dragons whatever their lengths, and a dragon may sprint past its
// free steps by paying one segment per step down to length 2.
std::vector<int> qThreat, qThreatVis, thStamp, thDist, thQ;
std::vector<int> qThreatCnt, qMargin; // enemies able to reach a cell; steps beyond the nearest reach
std::vector<int> qThreat2;            // P3c: < INF where some enemy can put its head within two turns
std::vector<int> l2Stamp, l2Dist, l2Q;
int l2S = 1;
bool look2Active = false;             // some enemy is close enough for the lookahead to matter
int thS = 1;

// Cells an enemy at src can put its head on this turn (freeR free steps, up to
// maxR with paid ones). `cnt`/`margin` (optional) also count attackers and
// record how far beyond the reach a cell lies, up to QUEEN_MARGIN more steps.
void reach_bfs(int src, int freeR, int maxR, std::vector<int>& out, std::vector<int>* cnt = nullptr, std::vector<int>* margin = nullptr)
{
    thS++;
    thQ.clear();
    thQ.push_back(src);
    thStamp[src] = thS;
    thDist[src] = 0;
    int const lim = maxR + (margin ? QUEEN_MARGIN : 0);
    for (std::size_t i = 0; i < thQ.size(); i++)
    {
        int const c = thQ[i], dc = thDist[c];
        if (dc >= lim)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int const n = step_open(c, k); // unexplored edge: assume the enemy can pass
            if (n < 0 || thStamp[n] == thS)
                continue;
            thStamp[n] = thS;
            thDist[n] = dc + 1;
            if (dc + 1 <= maxR)
            {
                out[n] = std::min(out[n], std::max(0, dc + 1 - freeR));
                if (cnt)
                    (*cnt)[n]++;
            }
            else if (margin)
                (*margin)[n] = std::min((*margin)[n], dc + 1 - maxR);
            if (vis[n] && occId[n] >= 0)
                continue; // a body: the enemy cannot go through it
            thQ.push_back(n);
        }
    }
}

void compute_queen_threat()
{
    std::fill(qThreat.begin(), qThreat.end(), INF);
    std::fill(qThreatVis.begin(), qThreatVis.end(), INF);
    std::fill(qThreatCnt.begin(), qThreatCnt.end(), 0);
    std::fill(qMargin.begin(), qMargin.end(), INF);
    std::vector<int>* cnt = QUEEN_HARD_THREAT ? &qThreatCnt : nullptr;
    std::vector<int>* mar = QUEEN_HARD_THREAT ? &qMargin : nullptr;
    // P3a: her last segment leaves its cell when she steps, so an enemy can
    // come through it (a U-turn is no shield).
    int tailCell = -1, tailOcc = -1;
    if (THREAT_TAIL_FREE && amQueen && static_cast<int>(body.size()) >= L && L >= 2)
    {
        tailCell = body[L - 1];
        tailOcc = occId[tailCell];
        if (tailOcc == me)
            occId[tailCell] = -1;
        else
            tailCell = -1;
    }
    std::unordered_set<int> seenIds;
    if (QUEEN_LOOK2)
        std::fill(qThreat2.begin(), qThreat2.end(), INF);
    look2Active = false;
    for (auto const& h : heads)
    {
        if (h.mine)
            continue;
        seenIds.insert(h.id);
        int const len = std::max(h.len, h.estLen);
        int const f = std::max(1, (len + 3) / 4);
        int const m = std::min(PAID_REACH_CAP, f + (PAID_REACH ? std::max(0, len - 2) : 0));
        reach_bfs(h.cell, f, f, qThreatVis);
        reach_bfs(h.cell, f, std::max(f, m), qThreat, cnt, mar);
        if (QUEEN_LOOK2)
        {
            int const m2 = std::min(PAID_REACH_CAP, m + f); // two turns: free steps twice, the paid budget once
            reach_bfs(h.cell, 2 * f, m2, qThreat2);
            if (!body.empty() && manhattan(h.cell, body[0]) <= m2 + LOOK2_QUEEN_REACH + 1)
                look2Active = true;
        }
    }
    // Enemies the fleet reported in the last two rounds: they may have moved
    // a free step or two since.
    for (auto const& e : enemyReps)
    {
        int const age = R - e.round;
        if (age < 1 || age > REP_MAX_AGE || seenIds.count(e.id))
            continue;
        int const el = std::max(e.len, e.estLen);
        int const f = std::max(1, (el + 3) / 4) * (age + 1);
        int const m = std::min(PAID_REACH_CAP, f + (PAID_REACH ? std::max(0, el - 2) : 0));
        reach_bfs(e.cell, f, std::max(f, m), qThreat, cnt, mar);
        if (QUEEN_LOOK2)
        {
            int const f1 = std::max(1, (el + 3) / 4);
            int const m2 = std::min(PAID_REACH_CAP, m + f1);
            reach_bfs(e.cell, f + f1, m2, qThreat2);
            if (!body.empty() && manhattan(e.cell, body[0]) <= m2 + LOOK2_QUEEN_REACH + 1)
                look2Active = true;
        }
    }
    if (tailCell >= 0)
        occId[tailCell] = tailOcc;
}

// Path distances from the queen's head out to `maxR` steps (known water;
// unexplored edges count as open). Used by teammates to give her room.
std::vector<int> qDist;
void queen_dist_bfs(int src, int maxR)
{
    std::fill(qDist.begin(), qDist.end(), INF);
    thQ.clear();
    thQ.push_back(src);
    qDist[src] = 0;
    for (std::size_t i = 0; i < thQ.size(); i++)
    {
        int const c = thQ[i];
        if (qDist[c] >= maxR)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int const n = step_open(c, k);
            if (n < 0 || qDist[n] != INF)
                continue;
            qDist[n] = qDist[c] + 1;
            thQ.push_back(n);
        }
    }
}

} // namespace

// ===== situation.hpp =====
// L2 SITUATION - roles by id, fleet size targets, phases, majority, leaders / giant / heir,
// our half vs theirs, enemy count, the round-500 tiebreak guess, where the enemy queen is.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

namespace {

// Fleet state: danger, transition, leaders, mode (sections 11-16)
Role role_for(int id)
{
    std::uint32_t const h = static_cast<std::uint32_t>(id) * 2654435761u;
    int const r = static_cast<int>((h >> 16) % 10);
    if (r < 3) return ROLE_COMBAT;
    if (r < 4) return ROLE_PORTAL;
    if (r < 6) return ROLE_EXPLORER;
    return ROLE_COLLECTOR;
}

// Fleet size the expansion phase aims for.
int expansion_target() { return std::min(unitLimit - QUEEN_SLOTS - 1, std::max(4, N / (DYN_ALLOC ? DYN_DENSITY : EXP_DENSITY))); }
// Growth splits allowed: before the switch round (dynamic) or EXP_END.
bool growth_open() { return DYN_ALLOC ? !sit.switched : R < EXP_END; }
// Fleet floor once dragons feed the queen: more enemies, more dragons kept.
int cons_floor() { return DYN_ALLOC ? 10 + std::min(10, sit.pressureMax / 3) : CONSOL_MIN_UNITS; }

// 6d: fresh claims for a role that rank above a dragon of length len (longer, or as long with a lower id).
int better_claims(int role, int len)
{
    if (!ROLE_CLAIMS)
        return 0;
    int k = 0;
    for (auto const& [id, c] : roleClaims[role])
        if (id != me && id != queenId && R - c.round <= CLAIM_TTL && (c.len > len || (c.len == len && id < me)))
            k++;
    return k;
}

void compute_fleet_state()
{
    int const head = body[0];
    std::unordered_set<int> recent;
    for (auto const& e : enemyReps)
        if (R - e.round <= 60)
            recent.insert(e.id);
    sit.distinctEnemies = static_cast<int>(recent.size());
    // Dynamic allocation: the more enemies we meet, the longer numbers matter
    // and the later we turn them into length. Sticky once switched.
    if (DYN_ALLOC)
    {
        if (sit.firstR < 0)
            sit.firstR = R;
        sit.pressureMax = std::max(sit.pressureMax, sit.distinctEnemies);
        sit.switchRound = SWITCH_FIXED > 0 ? SWITCH_FIXED : SW_LO + (SW_HI - SW_LO) * std::min(sit.pressureMax, SW_E_HI) / SW_E_HI;
        bool const informed = sit.heardPressure || R - sit.firstR >= 8 || sit.firstR <= 1;
        if (!sit.switched && R >= sit.switchRound && informed)
        {
            sit.switched = true;
            queue_fact(msg_phase(), 70, 3);
        }
        if (R % 3 == me % 3)
            queue_fact(msg_phase(), 50, 1); // often: newborns and the queen hear little
    }
    sit.enemyEst = sit.distinctEnemies;
    if (ENEMY_EST && sit.distinctEnemies > 0)
    {
        // We hear of the enemies our fleet happens to see; a swarm is mostly
        // out of view. Scale by the share of the map our dragons watch.
        double const cover = std::max(EST_MIN_COVER_X100 / 100.0, std::min(1.0, units * static_cast<double>(EST_VIEW_CELLS) / N));
        sit.enemyEst = std::max(sit.distinctEnemies, static_cast<int>(sit.distinctEnemies / cover + 0.5));
    }
    sit.outnumbered = sit.enemyEst > units;
    sit.lateSafe = LATE_SAFE && R >= 500 - LATE_SAFE_ROUNDS;
    sit.survival = SURVIVAL && (units <= SURV_UNITS || (sit.distinctEnemies >= SURV_RATIO_MIN && units * SURV_RATIO < sit.distinctEnemies) || sit.lateSafe);

    // Section 12: more opponents around -> keep the big fleet for longer.
    int const d = sit.distinctEnemies;
    int const thr = (d >= 12 ? 400 : d >= 7 ? 360 : d >= 3 ? 310 : d >= 1 ? 250 : 200) - TRANS_SHIFT;
    if (!sit.transitioned)
        sit.transitionRound = thr;
    if (!sit.transitioned && R >= sit.transitionRound)
        sit.transitioned = true;

    sit.localThreat = 0;
    std::unordered_set<int> counted;
    for (auto const& h : heads)
        if (!h.mine)
        {
            sit.localThreat++;
            counted.insert(h.id);
        }
    for (auto const& e : enemyReps)
        if (R - e.round <= 6 && !counted.count(e.id) && manhattan(e.cell, head) <= 8)
        {
            sit.localThreat++;
            counted.insert(e.id);
        }
    if (sit.echoEnemy >= 2)
        sit.localThreat++;

    // P2: the home queen's leash follows the threat around her. Contact pulls
    // her home; quiet lets her eat further out (unlimited in the opening).
    if (units >= ADAPT_FLEET)
        sit.fleetEstablished = true;
    sit.queenOpening = QUEEN_ADAPT && !sit.fleetEstablished && R < ADAPT_OPEN_END;
    queenLeash = QUEEN_LEASH;
    if (QUEEN_ADAPT && amQueen)
    {
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, head) <= THREAT_RADIUS)
                lastThreatR = R;
        for (auto const& e : enemyReps)
            if (R - e.round <= 3 && manhattan(e.cell, head) <= THREAT_RADIUS)
                lastThreatR = R;
        int const quiet = R - lastThreatR - QUIET_ROUNDS;
        if (quiet >= 0 && sit.queenOpening)
            queenLeash = INF;
        else if (quiet >= 0 && R < ADAPT_END_ROUND)
            queenLeash = std::min(LEASH_MAX, QUEEN_LEASH + quiet / 2);
    }

    // Expansion (numbers first): lasts until the fleet first reaches its
    // target. The queen splits too until QUEEN_GROW_ROUND; if the enemy queen
    // is still alive by then she grows instead (once it is dead, staying alive
    // is all that counts, so she keeps splitting while there is room).
    if (EXPANSION)
    {
        if (sit.expansion && (!growth_open() || units >= expansion_target()))
            sit.expansion = false;
        sit.queenSplits = (DYN_ALLOC ? !sit.switched : R < QUEEN_GROW_ROUND) || sit.enemyQueenDead;
    }
    else
        sit.expansion = sit.queenSplits = false;

    // OPEN_PLAN: majority (with hysteresis) ends the opening plan.
    {
        double const ratio = static_cast<double>(units) / std::max(1, est_enemy_units());
        if (!sit.majority && units >= OPEN_MAJ_UNITS && ratio >= MAJ_ON)
            sit.majority = true;
        else if (sit.majority && ratio < MAJ_OFF)
            sit.majority = false;
        sit.openPlan = OPEN_PLAN && !sit.majority && !sit.switched && R < OPEN_PLAN_END;
    }
    // Hunters: a fixed share of the fleet (by id) goes after the enemy queen
    // once the opening is over. The queen and the leaders never hunt.
    {
        std::uint32_t const hh = static_cast<std::uint32_t>(me) * 2246822519u + 3266489917u;
        bool const active = HUNTERS && !(sit.enemyQueenDead && !sit.ourQueenDead) && (R >= HUNT_START || !sit.expansion) && R >= OPEN_FARM_ROUND &&
                            !(HUNT_GATE && (sit.survival || sit.outnumbered) && !sit.ourQueenDead);
        int const slot = static_cast<int>((hh >> 13) % 10);
        my.hunterSlot = !amQueen && kSlots.hunter(slot);
        my.hunter = active && !amQueen && !my.leader && kSlots.hunter(slot) && !sit.openPlan; // OPEN_PLAN: multiply first
        // Guards: their own slice of the fleet; once the enemy queen is dead
        // the hunters come home too.
        my.guard = GUARDS && !amQueen && !my.leader && !sit.ourQueenDead && !my.hunter &&
                  (kSlots.guard(slot) || (sit.enemyQueenDead && kSlots.hunter(slot))) &&
                  (GUARD_MIN_UNITS == 0 || sit.enemyQueenDead || (units >= GUARD_MIN_UNITS && !sit.expansion)) && units > GUARD_FLEET_MIN &&
                  (R >= OPEN_FARM_ROUND || sit.enemyQueenDead);
        my.screen = SCREENS && my.guard &&
                   (kSlots.screen(slot) || (SCREENS_MORE && units < 20 && kSlots.screen2(slot) && (me & 1)));
        my.defender = OWN_HALF && !amQueen && !my.leader && !my.guard && !my.hunter && kSlots.defender(slot) && R >= HUNT_START;
    }
    my.wall = false; // set below once the leader election is done

    // Too many leaders (they could not hear each other at election time):
    // step down if enough longer leaders are known.
    amQueen = me == queenId;
    if (amQueen)
        my.leader = true; // the queen leads for the whole game
    else if (my.leader)
    {
        // F1: any longer teammate counts (not just leaders): with patchy sonar
        // few leaders are ever heard of, and nobody stepped down.
        int longer = 0;
        for (auto const& [id, s] : fleet)
            if (id != me && id != queenId && (s.leader || GIANT_ONE) && R - s.round <= 30 && (s.len > L || (s.len == L && id < me)))
                longer++;
        if (longer >= LEADER_COUNT || better_claims(CR_LEADER, L) >= LEADER_COUNT || (GIANT_ONE && !my.roleLeader && L < LEADER_MIN_LEN - 1))
            my.leader = false;
    }

    // Leader election (section 11): the longest dragons we know of.
    if (sit.transitioned && !sit.expansion && !my.leader && L >= (GIANT_ONE ? LEADER_MIN_LEN : 3) && (!ROLES_AFTER_SWITCH || sit.switched))
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
        int const slots = LEADER_COUNT - std::max(freshLeaders, better_claims(CR_LEADER, L)); // 6d: claims heard count too
        std::sort(ranking.begin(), ranking.end());
        for (int i = 0; i < slots && i < static_cast<int>(ranking.size()); i++)
            if (ranking[i].second == me)
                my.leader = true;
    }

    // Heir: once the enemy queen is dead ours only has to live, and the
    // longest of the rest grows (insurance for the longest-dragon tiebreak).
    my.heir = false;
    if (HEIR && sit.enemyQueenDead && !sit.ourQueenDead && !amQueen && !(GIANT_ONE && units < GIANT_MIN_UNITS))
    {
        my.heir = true;
        for (auto const& [id, s] : fleet)
            if (id != me && id != queenId && R - s.round <= 10 && (s.len > L || (s.len == L && id < me)))
                my.heir = false;
        if (better_claims(CR_HEIR, L) > 0)
            my.heir = false; // 6d: a better heir has claimed it
        if (gossip_beats_me(L))
            my.heir = false; // 6d'': a better holder is known fleet-wide
        if (my.heir)
            my.leader = true;
    }

    // P5 giant: from GIANT_ROUND the longest of the rest stops splitting and
    // grows on every map, so the queen is never our only long dragon.
    my.giant = false;
    if (GIANT_ALL && R >= GIANT_ROUND && (!ROLES_AFTER_SWITCH || sit.switched) && !amQueen && !sit.ourQueenDead && L >= GIANT_MIN_LEN && !my.heir &&
        !(GIANT_ONE && units < GIANT_MIN_UNITS))
    {
        my.giant = true;
        for (auto const& [id, s] : fleet)
        {
            if (id == me || id == queenId)
                continue;
            if (R - s.round <= 10 && (s.len > L || (s.len == L && id < me)))
                my.giant = false;
            // F1: another leader we heard of lately that is at least as long: it is the giant, not us
            if (GIANT_ONE && s.leader && R - s.round <= 30 && (s.len > L || (s.len == L && id < me)))
                my.giant = false;
        }
        if (better_claims(CR_GIANT, L) > 0)
            my.giant = false; // 6d: a better giant has claimed it
        if (gossip_beats_me(L))
            my.giant = false; // 6d'': a better holder is known fleet-wide
        if (my.giant)
            my.leader = true;
    }
    // F1: the giant / heir role ended - so does the leadership it gave (it
    // left half the fleet as non-splitting "leaders" on big maps).
    if (GIANT_ONE && !amQueen)
    {
        if (my.roleLeader && !my.giant && !my.heir)
            my.leader = false;
        my.roleLeader = my.giant || my.heir;
    }
    // 6d: holders re-announce their roles every 3 rounds (staggered by id).
    if (ROLE_CLAIMS && !amQueen && R % 3 == me % 3)
    {
        if (my.giant)
            queue_fact(msg_role(CR_GIANT, me, L, R), 55, 1);
        if (my.heir)
            queue_fact(msg_role(CR_HEIR, me, L, R), 55, 1);
        if (my.leader)
            queue_fact(msg_role(CR_LEADER, me, L, R), 50, 1);
    }

    my.wall = wall_now() && !amQueen && !my.leader && !my.hunter && !my.guard && !my.defender && wall_slot(me) && L >= WALL_MIN_LEN;

    // Big open map late in the game: length decides the tiebreaks there and
    // long dragons do not trap themselves, so the fleet turns into length.
    // Small or kelp-dense maps keep their numbers.
    sit.openMap = sit.consolidate = false;
    if (mapProf.openSized)
    {
        int known = 0, kelp = 0;
        for (int code = 0; code < 2 * N; code++)
            if (edgeType[code] != E_UNK)
            {
                known++;
                kelp += edgeType[code] == E_KELP;
            }
        sit.openMap = known > 0 && kelp <= OPEN_KELP_MAX * known;
        sit.consolidate = CONSOLIDATE && sit.openMap && R >= CONSOL_ROUND;
    }
    if (DYN_ALLOC)
        sit.consolidate = sit.switched; // every map: after the switch, length

    // Majority on a smaller map: hunt them down, win by elimination.
    sit.majorityHunt = MAJ_HUNT && mapProf.small && !sit.survival && sit.distinctEnemies >= 1 && units >= MAJ_MIN_UNITS &&
                   units * 10 >= MAJ_RATIO_X10 * sit.enemyEst;

    // Modes (sections 13-15).
    bool enemyNearby = false;
    for (auto const& e : enemyReps)
        if (R - e.round <= 30 && manhattan(e.cell, head) <= 12)
            enemyNearby = true;
    if (sit.localThreat >= 2 || (sit.localThreat >= 1 && my.role == ROLE_COMBAT))
        sit.mode = MODE_COMBAT;
    else if (sit.transitioned && !enemyNearby && sit.localThreat == 0)
        sit.mode = MODE_FARM;
    else if (sit.transitioned)
        sit.mode = MODE_PRESERVE;
    else
        sit.mode = MODE_NORMAL;
}

// Our half: nearer our queen's start than its mirror image (theirs).
int enemy_spawn_guess()
{
    if (ourQueenSpawn < 0)
        return -1;
    if (symKnown >= 0)
        return mirror_cell(symKnown, ourQueenSpawn);
    for (int k = 0; k < 3; k++)
        if (symMask & (1 << k))
            return mirror_cell(k, ourQueenSpawn);
    return -1;
}
int halfMargin(int c) // > 0 on our side, < 0 on theirs
{
    int const es = enemy_spawn_guess();
    if (es < 0 || es == ourQueenSpawn)
        return 0;
    return manhattan(c, es) - manhattan(c, ourQueenSpawn);
}

int est_enemy_units()
{
    return std::max(sit.enemyEst, 1);
}

// Best guess of the enemy queen's head: in view, recently reported, or the
// mirror image of our queen's start (one guess per symmetry still possible).
// Their feeders die next to their queen: three or more recent death drops
// close together (and not ours) mark where she probably is.
int feeder_queen_guess()
{
    if (!FEEDER_TRAIL || R < 100)
        return -1;
    std::vector<int> ev;
    for (auto const& e : dropEv)
    {
        if (R - e.round > 20 || e.nearOurQueen)
            continue;
        bool ours = false; // one of our dragons died there (a trade or an accident)
        for (auto const& dg : dangers)
            if (std::abs(dg.round - e.round) <= 3 && manhattan(dg.cell, e.cell) <= 3)
                ours = true;
        if (!ours)
            ev.push_back(e.cell);
    }
    int best = -1, bestCnt = 2;
    for (int a : ev)
    {
        int cnt = 0;
        for (int b : ev)
            if (b != a && manhattan(a, b) <= 5)
                cnt++;
        if (cnt >= bestCnt)
        {
            bestCnt = cnt;
            best = a;
        }
    }
    return best;
}

int enemy_queen_target()
{
    for (auto const& h : heads)
        if (!h.mine && h.id == enemyQueenId)
            return h.cell;
    int best = -1, bestRound = -1;
    for (auto const& e : enemyReps)
        if (e.id == enemyQueenId && R - e.round <= 30 && e.round > bestRound)
        {
            best = e.cell;
            bestRound = e.round;
        }
    if (best >= 0)
        return best;
    int const trail = feeder_queen_guess();
    if (trail >= 0)
        return trail;
    if (ourQueenSpawn < 0)
        return -1;
    std::vector<int> ks;
    for (int k = 0; k < 3; k++)
        if (symMask & (1 << k))
            ks.push_back(k);
    if (ks.empty())
        return -1;
    return mirror_cell(ks[me % ks.size()], ourQueenSpawn);
}

// Running guess of the round-500 tiebreaks: queen length, then longest
// dragon, then total length. -1 = unknown.
struct TieState
{
    int ourQ = -1, theirQ = -1, ourLong = 0, theirLong = -1;
};
TieState tie_state()
{
    TieState t;
    if (sit.ourQueenDead)
        t.ourQ = 0;
    else if (me == queenId)
        t.ourQ = L;
    else if (auto const q = fleet.find(queenId); q != fleet.end() && R - q->second.round <= 60)
        t.ourQ = q->second.len;
    if (sit.enemyQueenDead)
        t.theirQ = 0;
    else if (auto const e = enemyLenMem.find(enemyQueenId); e != enemyLenMem.end() && R - e->second.round <= 150)
        t.theirQ = e->second.len;
    t.ourLong = L;
    for (auto const& [id, st] : fleet)
        if (R - st.round <= 60)
            t.ourLong = std::max(t.ourLong, st.len);
    for (auto const& [id, lm] : enemyLenMem)
        if (R - lm.round <= 100)
            t.theirLong = std::max(t.theirLong, lm.len);
    return t;
}

// Giant (insurance for the longest-dragon tiebreak): our longest non-queen
// leader we know of, -1 if none.
int giant_id()
{
    int best = -1, bestLen = 0;
    if (my.leader && !amQueen)
    {
        best = me;
        bestLen = L;
    }
    for (auto const& [id, st] : fleet)
        if (id != queenId && st.leader && R - st.round <= 30 && (st.len > bestLen || (st.len == bestLen && id < best)))
        {
            best = id;
            bestLen = st.len;
        }
    return best;
}

// Is our queen already long enough? Past max(QUEEN_FEED_CAP, their queen's
// known length + margin) feeding her more only adds risk.
bool queen_fed_enough()
{
    TieState const t = tie_state();
    if (t.ourQ < 0)
        return false;
    int const cap = std::max(QUEEN_FEED_CAP, t.theirQ >= 0 ? t.theirQ + QUEEN_FEED_MARGIN : 0);
    return t.ourQ >= cap;
}

} // namespace

// ===== topology.hpp =====
// L1 TOPOLOGY - corridors, pockets and dead ends, free loops for the queen, corridor tests.
// (Architecture_v2 step 7b replaces the walks below with bridge / loop lookups.)
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

namespace {

bool topo_loop_near(std::vector<int> const& b, int len); // static topology, below

// Open sides of a cell (kelp and unexplored edges excluded): <= 2 is a corridor.
int open_sides(int c)
{
    int k = 0;
    for (int d = 0; d < 4; d++)
        k += cross(c, d) >= 0;
    return k;
}

// A cell inside a 1-wide dead end (a pocket): following the corridor cells
// from it reaches a dead end before any open water.
bool in_pocket(int c)
{
    if (open_sides(c) >= 3)
        return false;
    std::vector<int> q{c};
    std::unordered_set<int> seen{c};
    for (std::size_t i = 0; i < q.size() && q.size() <= 12; i++)
    {
        int const x = q[i];
        int const os = open_sides(x);
        if (os <= 1)
            return true;
        if (os >= 3)
            continue; // opens out this way
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(x, d);
            if (n >= 0 && seen.insert(n).second)
                q.push_back(n);
        }
    }
    return false;
}

// Stepping from `from` into `to`: does the 1-wide corridor ahead end in a
// dead end before it opens out? (Unexplored edges count as closed.)
bool pocket_ahead(int from, int to)
{
    int prev = from, c = to;
    for (int k = 0; k < 24; k++)
    {
        int const os = open_sides(c);
        if (os <= 1)
            return true;
        if (os >= 3)
            return false;
        int next = -1;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n >= 0 && n != prev)
                next = n;
        }
        if (next < 0)
            return true;
        prev = c;
        c = next;
    }
    return false;
}

// F7: stepping from `from` into `to`, does a FULLY SEEN 1-wide corridor end
// in a dead end? Any unexplored edge on the way: unknown, not a dead end.
bool known_dead_end(int from, int to)
{
    int prev = from, c = to;
    for (int k = 0; k < 24; k++)
    {
        int open = 0, unk = 0, next = -1;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n == -2 || n == -3)
                unk++;
            else if (n >= 0)
            {
                open++;
                if (n != prev)
                    next = n;
            }
        }
        if (unk > 0 || open >= 3)
            return false;
        if (open <= 1 || next < 0)
            return true;
        prev = c;
        c = next;
    }
    return false;
}

// A queen can never be trapped while she can reach a free loop longer than
// she is: she can circle it for ever and turn round there (a 2x2 for length
// <= 3, verified in the engine; a k x k ring holds 4(k-1) cells). Unexplored
// edges do not count here: pockets with unseen walls are how queens die.
bool ring_free(int tl, int k)
{
    std::vector<int> ring;
    int c = tl;
    int const dirs[4] = {1, 2, 3, 0}; // E along the top, S down the right, W along the bottom, N up the left
    for (int side = 0; side < 4; side++)
        for (int i = 0; i < k - 1; i++)
        {
            ring.push_back(c);
            if (edgeType[edge_code(c, dirs[side])] != E_OPEN)
                return false;
            c = nb(c, dirs[side]);
        }
    if (c != tl)
        return false;
    for (int x : ring)
        if ((vis[x] && occId[x] >= 0 && occId[x] != me) || wallStamp[x] == wstamp)
            return false;
    return true;
}

bool loop_reachable(std::vector<int> const& b, int len)
{
    int k = 2;
    while (4 * (k - 1) <= len)
        k++;
    if (k > 5)
        return !TOPO_LOOP || topo_loop_near(b, len); // long queens: no k x k ring test; 7b: a loop region longer than her
    BodyScope const sb(b, len);
    std::unordered_map<int, int> dist{{b[0], 0}};
    std::vector<int> q{b[0]};
    for (std::size_t i = 0; i < q.size(); i++)
    {
        int const c = q[i], dc = dist[c];
        if (ring_free(c, k))
            return true;
        if (dc >= QUEEN_LOOP_DEPTH)
            continue;
        for (int d = 0; d < 4; d++)
        {
            if (edgeType[edge_code(c, d)] != E_OPEN)
                continue;
            int const n = nb(c, d);
            if (dist.count(n) || !passable(n, dc + 1))
                continue;
            dist[n] = dc + 1;
            q.push_back(n);
        }
    }
    return false;
}

// Optional queen split: does she still have a loop with the child's body (the
// tail part) standing where it is?
bool split_keeps_loop(int child)
{
    if (!QUEEN_LOOP_SAFE || static_cast<int>(body.size()) < L)
        return true;
    std::vector<int> front(body.begin(), body.begin() + (L - child));
    WallScope ws;
    for (int i = L - child; i < L; i++)
        ws.wall(body[i]);
    return loop_reachable(front, L - child);
}

// Does a 1-wide corridor lead from `start` to `goal` (within maxSteps)?
// Walks only corridor cells (<= 2 open sides) and stops at visible dragons,
// so it finds the queen's head from the side she is facing.
bool corridor_leads_to(int start, int goal, int maxSteps)
{
    std::unordered_map<int, int> dist{{start, 0}};
    std::vector<int> q{start};
    for (std::size_t i = 0; i < q.size(); i++)
    {
        int const c = q[i], dc = dist[c];
        if (c == goal)
            return true;
        if (dc >= maxSteps || (c != start && open_sides(c) > 2))
            continue;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n < 0 || dist.count(n))
                continue;
            if (n != goal && vis[n] && occId[n] >= 0)
                continue;
            dist[n] = dc + 1;
            q.push_back(n);
        }
    }
    return false;
}


// ---------------------------------------------------------------------------
// Static topology of the known map (Architecture_v2 step 7b)
// ---------------------------------------------------------------------------
// A dragon can never turn round where there is no cycle: its own tail blocks
// the cell it came from. So known water splits into LOOP REGIONS (2-edge-
// connected components: every edge in one lies on a cycle) joined by BRIDGES.
// Crossing a bridge leads into a BRANCH. A branch with no loop longer than the
// dragon and no unexplored edge is a trap it can only leave by splitting.
// Edges are the known crossings (open water and portals with a known far end);
// an unexplored edge or an unlocated portal marks its cell as FRONTIER (the
// map may go on there). Rebuilt lazily whenever an edge is learned.
struct Branch
{
    int cells = 0;
    int maxCap = 0;    // largest loop region in the branch (0: no cycle at all)
    int spawns = 0;    // known spawn tiles
    bool frontier = false;
};
Branch merge(Branch a, Branch const& b)
{
    a.cells += b.cells;
    a.maxCap = std::max(a.maxCap, b.maxCap);
    a.spawns += b.spawns;
    a.frontier = a.frontier || b.frontier;
    return a;
}
struct Topology
{
    int version = -1;
    std::vector<int> comp;            // cell -> loop region
    std::vector<int> cap;             // region -> cells (its longest cycle is at most this; a region of 1 has none)
    std::vector<char> bridge;         // edge key -> crossing it splits the known map
    std::vector<int> parent;          // region -> parent region in the bridge forest (-1: a root)
    std::vector<Branch> own, down, up; // region itself / its subtree / everything beyond its parent bridge
} topo;

// One physical edge seen from either side: a portal's two edges count as one.
int edge_key(int c, int d)
{
    int const code = edge_code(c, d);
    return edgeType[code] == E_PORTAL && edgePartner[code] >= 0 ? std::min(code, edgePartner[code]) : code;
}

void topo_build()
{
    topo.version = edgeVersion;
    static std::vector<int> start, to, key, tin, low, order;
    start.assign(N + 1, 0);
    to.clear();
    key.clear();
    static std::vector<char> front;
    front.assign(N, 0);
    for (int c = 0; c < N; c++)
    {
        start[c] = static_cast<int>(to.size());
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n == -2 || n == -3)
                front[c] = 1;
            else if (n >= 0 && n != c)
            {
                to.push_back(n);
                key.push_back(edge_key(c, d));
            }
        }
    }
    start[N] = static_cast<int>(to.size());

    // Bridges: iterative Tarjan (no recursion - the wasm stack is small). The
    // edge we arrived by is skipped by its key, so parallel edges are cycles.
    topo.bridge.assign(2 * N, 0);
    tin.assign(N, -1);
    low.assign(N, 0);
    struct Frame
    {
        int v, parentKey, next;
    };
    static std::vector<Frame> st;
    st.clear();
    int timer = 0;
    for (int s = 0; s < N; s++)
    {
        if (tin[s] >= 0)
            continue;
        tin[s] = low[s] = timer++;
        st.push_back({s, -1, start[s]});
        while (!st.empty())
        {
            int const v = st.back().v;
            if (st.back().next < start[v + 1])
            {
                int const i = st.back().next++;
                int const u = to[i];
                if (key[i] == st.back().parentKey)
                    continue;
                if (tin[u] >= 0)
                    low[v] = std::min(low[v], tin[u]);
                else
                {
                    tin[u] = low[u] = timer++;
                    st.push_back({u, key[i], start[u]});
                }
            }
            else
            {
                int const pk = st.back().parentKey;
                st.pop_back();
                if (!st.empty())
                {
                    int const p = st.back().v;
                    low[p] = std::min(low[p], low[v]);
                    if (low[v] > tin[p])
                        topo.bridge[pk] = 1;
                }
            }
        }
    }

    // Loop regions: connected by non-bridge edges.
    topo.comp.assign(N, -1);
    topo.cap.clear();
    topo.own.clear();
    static std::vector<int> q;
    for (int s = 0; s < N; s++)
    {
        if (topo.comp[s] >= 0)
            continue;
        int const id = static_cast<int>(topo.cap.size());
        Branch b;
        q.assign(1, s);
        topo.comp[s] = id;
        for (std::size_t k = 0; k < q.size(); k++)
        {
            int const c = q[k];
            b.cells++;
            b.spawns += nextSpawn[c] >= 0 || gapHi[c] > 0;
            b.frontier = b.frontier || front[c];
            for (int i = start[c]; i < start[c + 1]; i++)
                if (!topo.bridge[key[i]] && topo.comp[to[i]] < 0)
                {
                    topo.comp[to[i]] = id;
                    q.push_back(to[i]);
                }
        }
        b.maxCap = b.cells >= 2 ? b.cells : 0;
        topo.cap.push_back(b.cells);
        topo.own.push_back(b);
    }

    // Bridge forest over the regions (flat arrays, reused between builds: on a half-explored big map most
    // unexplored cells are regions of their own); branch summaries in both directions.
    int const R0 = static_cast<int>(topo.cap.size());
    static std::vector<int> fStart, fAdj, chBuf;
    fStart.assign(R0 + 1, 0);
    for (int c = 0; c < N; c++)
        for (int i = start[c]; i < start[c + 1]; i++)
            if (topo.bridge[key[i]])
                fStart[topo.comp[c] + 1]++;
    for (int r = 0; r < R0; r++)
        fStart[r + 1] += fStart[r];
    fAdj.assign(fStart[R0], 0);
    {
        static std::vector<int> fill;
        fill.assign(fStart.begin(), fStart.end() - 1);
        for (int c = 0; c < N; c++)
            for (int i = start[c]; i < start[c + 1]; i++)
                if (topo.bridge[key[i]])
                    fAdj[fill[topo.comp[c]]++] = topo.comp[to[i]];
    }
    topo.parent.assign(R0, -2);
    topo.down.assign(R0, Branch{});
    topo.up.assign(R0, Branch{});
    order.clear();
    for (int r = 0; r < R0; r++)
    {
        if (topo.parent[r] != -2)
            continue;
        topo.parent[r] = -1;
        std::size_t const first = order.size();
        order.push_back(r);
        for (std::size_t k = first; k < order.size(); k++)
            for (int i = fStart[order[k]]; i < fStart[order[k] + 1]; i++)
                if (topo.parent[fAdj[i]] == -2)
                {
                    topo.parent[fAdj[i]] = order[k];
                    order.push_back(fAdj[i]);
                }
    }
    for (int k = static_cast<int>(order.size()) - 1; k >= 0; k--) // children before parents
    {
        int const x = order[k];
        topo.down[x] = merge(topo.down[x], topo.own[x]);
        if (topo.parent[x] >= 0)
            topo.down[topo.parent[x]] = merge(topo.down[topo.parent[x]], topo.down[x]);
    }
    static std::vector<Branch> pre;
    for (int p : order) // parents before children
    {
        if (fStart[p] == fStart[p + 1])
            continue; // a leaf (most regions): no children to fill in
        chBuf.clear();
        for (int i = fStart[p]; i < fStart[p + 1]; i++)
            if (topo.parent[fAdj[i]] == p && std::find(chBuf.begin(), chBuf.end(), fAdj[i]) == chBuf.end())
                chBuf.push_back(fAdj[i]);
        Branch const base = merge(topo.up[p], topo.own[p]);
        pre.assign(chBuf.size() + 1, Branch{});
        for (std::size_t i = 0; i < chBuf.size(); i++)
            pre[i + 1] = merge(pre[i], topo.down[chBuf[i]]);
        Branch suf;
        for (int i = static_cast<int>(chBuf.size()) - 1; i >= 0; i--)
        {
            topo.up[chBuf[i]] = merge(merge(base, pre[i]), suf);
            suf = merge(suf, topo.down[chBuf[i]]);
        }
    }
}

// Rebuilt at most every TOPO_EVERY rounds (CPU). A stale topology can only MISS a new trap, never invent one: a
// branch judged a trap is fully explored, so no new edge can touch it or reconnect it.
int topoBuiltR = -1000;
void topo_refresh()
{
    if (static_cast<int>(topo.comp.size()) != N || (topo.version != edgeVersion && R - topoBuiltR >= TOPO_EVERY))
    {
        topo_build();
        topoBuiltR = R;
    }
}

// The branch a step from `from` in direction d leads into; false when the step
// does not cross a bridge (it stays inside a loop region).
bool topo_beyond(int from, int d, Branch& out)
{
    topo_refresh();
    int const n = cross(from, d);
    if (n < 0 || !topo.bridge[edge_key(from, d)])
        return false;
    int const a = topo.comp[from], b = topo.comp[n];
    if (topo.parent[b] == a)
        out = topo.down[b];
    else if (topo.parent[a] == b)
        out = topo.up[a];
    else
        return false;
    return true;
}

// 7d: the loop region of cell c can hold a dragon of length len (it can circle and turn round there).
bool topo_room(int c, int len)
{
    topo_refresh();
    int const cap = topo.cap[topo.comp[c]];
    return cap >= 4 && cap >= len + 1;
}

// A known trap for a dragon of length len: the step crosses a bridge into a
// fully explored branch that has no loop longer than the dragon.
bool topo_trap(int from, int d, int len)
{
    Branch b;
    return topo_beyond(from, d, b) && !b.frontier && b.maxCap <= len;
}

// Long queens (no k x k ring test): a loop region longer than her within
// QUEEN_LOOP_DEPTH steps of her head through known open water.
bool topo_loop_near(std::vector<int> const& b, int len)
{
    topo_refresh();
    BodyScope const sb(b, len);
    std::unordered_map<int, int> dist{{b[0], 0}};
    std::vector<int> q{b[0]};
    for (std::size_t i = 0; i < q.size(); i++)
    {
        int const c = q[i], dc = dist[c];
        if (topo.cap[topo.comp[c]] >= std::max(2, len + 1) || topo.own[topo.comp[c]].frontier)
            return true;
        if (dc >= QUEEN_LOOP_DEPTH)
            continue;
        for (int d = 0; d < 4; d++)
        {
            if (edgeType[edge_code(c, d)] != E_OPEN)
                continue;
            int const n = nb(c, d);
            if (dist.count(n) || !passable(n, dc + 1))
                continue;
            dist[n] = dc + 1;
            q.push_back(n);
        }
    }
    return false;
}


#ifdef TOPO_DIAG
// Debug: the known map as the topology sees it ('o' loop region, '.' no cycle,
// '?' frontier, ' ' unknown cell, '=' cell on a bridge end), plus counts.
void topo_render(std::FILE* f)
{
    topo_refresh();
    int bridges = 0, loopCells = 0, treeCells = 0;
    for (int k = 0; k < 2 * N; k++)
        bridges += topo.bridge[k];
    std::fprintf(f, "round %d dragon %d: %d regions, %d bridge edges\n", R, me, static_cast<int>(topo.cap.size()), bridges);
    for (int y = 0; y < H; y++)
    {
        std::string row;
        for (int x = 0; x < W; x++)
        {
            int const c = cell_of(x, y), r = topo.comp[c];
            bool known = false, onBridge = false;
            for (int d = 0; d < 4; d++)
            {
                known = known || cross(c, d) != -2;
                onBridge = onBridge || (cross(c, d) >= 0 && topo.bridge[edge_key(c, d)]);
            }
            bool cellFront = false;
            for (int d = 0; d < 4; d++)
                cellFront = cellFront || cross(c, d) == -2 || cross(c, d) == -3;
            char ch = !known ? ' ' : topo.cap[r] >= 2 ? (onBridge ? '=' : 'o') : '.';
            if (known && cellFront && ch != '.')
                ch = ch == '=' ? '+' : '?';
            loopCells += topo.cap[r] >= 2;
            treeCells += known && topo.cap[r] < 2;
            row += ch;
        }
        std::fprintf(f, "%s\n", row.c_str());
    }
    std::fprintf(f, "loop cells %d, no-cycle cells %d\n", loopCells, treeCells);
    // Raw dump for the offline check (tools: the Python reference recomputes bridges from these edges).
    std::fprintf(f, "W %d H %d\nEDGES ", W, H);
    for (int k = 0; k < 2 * N; k++)
        std::fputc('0' + edgeType[k], f);
    std::fprintf(f, "\nPARTNERS");
    for (int k = 0; k < 2 * N; k++)
        if (edgePartner[k] > k)
            std::fprintf(f, " %d:%d", k, edgePartner[k]);
    std::fprintf(f, "\nBRIDGES");
    for (int k = 0; k < 2 * N; k++)
        if (topo.bridge[k])
            std::fprintf(f, " %d", k);
    std::fprintf(f, "\nCAPS");
    for (int c = 0; c < N; c++)
        std::fprintf(f, " %d", topo.cap[topo.comp[c]]);
    std::fprintf(f, "\n");
}
#endif

} // namespace

// ===== commander.hpp =====
// L3 COMMAND - team-level assignment every dragon computes the same way: pearl matching,
// hunter / wall slots, pack membership, prey assignment.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

namespace {

// Target selection (sections 6-10, 18)
struct PearlCand
{
    int cell, appear;
    double conf;
};

// C: bounded path BFS from a teammate's head (unexplored edges open, bodies
// passable - they move), for its distance to each candidate pearl.
std::vector<int> mStamp, mDist, mQ;
int mS = 0;
void match_bfs(int src)
{
    if (static_cast<int>(mStamp.size()) != N)
    {
        mStamp.assign(N, 0);
        mDist.assign(N, 0);
    }
    mS++;
    mQ.clear();
    mQ.push_back(src);
    mStamp[src] = mS;
    mDist[src] = 0;
    for (std::size_t i = 0; i < mQ.size(); i++)
    {
        int const c = mQ[i];
        if (mDist[c] >= MATCH_DEPTH)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int const n = step_open(c, k);
            if (n < 0 || mStamp[n] == mS)
                continue;
            mStamp[n] = mS;
            mDist[n] = mDist[c] + 1;
            mQ.push_back(n);
        }
    }
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
    std::vector<Agent> agents{{me, body[0], -1, my.leader}};
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
    // C: each teammate's path distance to each pearl (manhattan ignores kelp:
    // in mazes it sent the wrong dragon and lost races we were nearer for).
    std::vector<std::vector<int>> pathEta;
    if (MATCH_PATHS)
    {
        pathEta.assign(agents.size(), {});
        for (std::size_t ai = 0; ai < agents.size(); ai++)
        {
            if ((agents[ai].id == me && !MATCH_SELF_SAME) || manhattan(agents[ai].cell, body[0]) > 2 * MATCH_DEPTH)
                continue; // far teammates cannot compete for our pearls: manhattan is enough
            bool anyNear = false;
            for (auto const& p : pearls)
                if (manhattan(agents[ai].cell, p.cell) <= MATCH_DEPTH)
                    anyNear = true;
            if (!anyNear)
                continue;
            match_bfs(agents[ai].cell);
            pathEta[ai].assign(pearls.size(), -1);
            for (std::size_t pi = 0; pi < pearls.size(); pi++)
                if (mStamp[pearls[pi].cell] == mS)
                    pathEta[ai][pi] = mDist[pearls[pi].cell];
        }
    }
    std::vector<Pair> pairs;
    for (int pi = 0; pi < static_cast<int>(pearls.size()); pi++)
    {
        auto const& p = pearls[pi];
        auto const cl = claims.find(p.cell);
        for (int ai = 0; ai < static_cast<int>(agents.size()); ai++)
        {
            auto const& a = agents[ai];
            int eta;
            if (a.id == me && !(MATCH_PATHS && MATCH_SELF_SAME))
                eta = distHead[p.cell];
            else if (cl != claims.end() && cl->second.id == a.id)
                eta = std::max(0, cl->second.eta - (R - cl->second.round));
            else if (MATCH_PATHS)
            {
                int const pe = pathEta[ai].empty() ? -1 : pathEta[ai][pi];
                eta = pe >= 0 ? pe : std::max(manhattan(a.cell, p.cell), MATCH_DEPTH + 1);
            }
            else
                eta = manhattan(a.cell, p.cell);
            if (eta >= INF)
                continue;
            int const wait = p.appear - R;
            // Future spawn: travel plus half the time spent waiting there.
            double cost = (CAMP_JOB && wait > 0 ? eta + 0.5 * std::max(0, wait - eta) : std::max(eta, wait)) + 1.0;
            cost /= p.conf;
            if (a.leader)
                cost *= a.id == queenId ? (EXPANSION && sit.queenSplits ? 1.0 : 0.3) : 0.6; // feed the queen (once she grows), then the leaders
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
    std::vector<int> assigned(agents.size(), -1);
    for (auto const& pr : pairs)
    {
        if (pearlUsed[pr.pearl] || (pr.agent >= 0 && agentUsed[pr.agent]))
            continue;
        pearlUsed[pr.pearl] = 1;
        if (pr.agent < 0)
            continue;
        agentUsed[pr.agent] = 1;
        assigned[pr.agent] = pr.pearl;
        if (agents[pr.agent].id == me && !MATCH_SWAP)
            return pr.pearl;
    }
    if (!MATCH_SWAP || assigned[0] < 0)
        return assigned[0];
    // 7i: local improvement - swap pearls with a nearby teammate when that lowers the pair's total cost (every
    // dragon computes the same swap from the same picture, so both sides agree).
    std::unordered_map<long long, double> cost;
    for (auto const& pr : pairs)
        if (pr.agent >= 0)
        {
            long long const k = static_cast<long long>(pr.agent) * 1000 + pr.pearl;
            auto const it = cost.find(k);
            if (it == cost.end() || pr.cost < it->second)
                cost[k] = pr.cost;
        }
    auto c = [&](int a, int p) {
        auto const it = cost.find(static_cast<long long>(a) * 1000 + p);
        return it == cost.end() ? 1e18 : it->second;
    };
    int const pm = assigned[0];
    int best = pm;
    double bestGain = 1.0; // at least a step's worth
    for (int j = 1; j < static_cast<int>(agents.size()); j++)
    {
        int const pj = assigned[j];
        if (pj < 0 || manhattan(agents[0].cell, agents[j].cell) > MATCH_SWAP_RANGE)
            continue;
        double const gain = c(0, pm) + c(j, pj) - c(0, pj) - c(j, pm);
        if (gain > bestGain)
        {
            bestGain = gain;
            best = pj;
        }
    }
    return best;
}

// Packs: hunters (and, once our queen is dead, every small dragon) share out
// prey the same way pearls are shared: every dragon solves the same greedy
// matching from what the fleet knows, so packs form without negotiation.
int hunter_slot(int id)
{
    std::uint32_t const hh = static_cast<std::uint32_t>(id) * 2246822519u + 3266489917u;
    return static_cast<int>((hh >> 13) % 10);
}

bool hunts_now()
{
    return HUNTERS && !(sit.enemyQueenDead && !sit.ourQueenDead) && (R >= HUNT_START || !sit.expansion) && R >= OPEN_FARM_ROUND;
}

// P8 wall: when it forms (later on bigger maps) and who is in it.
int wall_round() { return std::min(260, 120 + N / 40); }
bool wall_now() { return WALL && HUNTERS && !sit.openPlan && mapProf.wall && R >= wall_round() && units >= WALL_FORM_RATIO * est_enemy_units(); }
bool wall_slot(int id)
{
    int const sl = hunter_slot(id);
    return kSlots.wall(sl);
}

// Would teammate `id` (length len) be in a pack right now?
bool pack_member(int id, int len, bool leader)
{
    if (id == queenId || leader)
        return false;
    if (ENDGAME_PACKS && sit.ourQueenDead && len <= 6)
        return true;
    if (wall_now() && wall_slot(id) && len >= WALL_MIN_LEN)
        return true;
    return hunts_now() && kSlots.hunter(hunter_slot(id)) && len >= HUNTER_LEN;
}

struct Prey
{
    int cell, id, len;
    bool queen;
};
std::vector<Prey> preyList;
int myPrey = -1, myPackSize = 0, myPackRank = 0;

void assign_prey()
{
    preyList.clear();
    myPrey = -1;
    myPackSize = 0;
    myPackRank = 0;
    // Our queen is dead: only the longest dragon can still win it. If theirs
    // has not been seen for 100 rounds she is most likely dead too (theirs
    // die early), so their longest dragons come first.
    bool const giantHunt = GIANT_HUNT && sit.ourQueenDead && R >= GIANT_HUNT_ROUND && (sit.enemyQueenDead || R - sit.lastEqSeenR >= 100);
    int qt = sit.enemyQueenDead || giantHunt ? -1 : enemy_queen_target();
    // Big map, her position not seen lately: she hides at home among her
    // swarm, and the long walk there only feeds it (ladder: never reached).
    if (HUNT_FAR_SMALL && qt >= 0 && !mapProf.small && !sit.ourQueenDead && R - sit.lastEqSeenR > HUNT_FRESH)
        qt = -1;
    if (qt >= 0)
        preyList.push_back({qt, enemyQueenId, 0, true});
    // Long enemy dragons: when the queens no longer decide the game, or while
    // nobody knows where the enemy queen is.
    if (sit.ourQueenDead || sit.enemyQueenDead || qt < 0)
        for (auto const& e : enemyReps)
            if (e.id != enemyQueenId && R - e.round <= 6 && e.len >= (giantHunt ? GIANT_PREY_MIN : PREY_MIN_LEN))
                preyList.push_back({e.cell, e.id, e.len, false});
    if (preyList.empty())
        return;

    struct Agent
    {
        int id, cell;
    };
    std::vector<Agent> agents{{me, body[0]}};
    for (auto const& [id, st] : fleet)
        if (id != me && R - st.round <= 4 && pack_member(id, st.len, st.leader))
            agents.push_back({id, st.cell});
    struct Pair
    {
        double cost;
        int a, p;
    };
    std::vector<Pair> pairs;
    for (int pi = 0; pi < static_cast<int>(preyList.size()); pi++)
        for (int ai = 0; ai < static_cast<int>(agents.size()); ai++)
        {
            int const d = agents[ai].id == me ? dist_to(distHead, preyList[pi].cell) : manhattan(agents[ai].cell, preyList[pi].cell);
            if (d >= INF)
                continue;
            double const c = d - (preyList[pi].queen ? 8.0 : (giantHunt ? 1.0 : 0.25) * preyList[pi].len) + agents[ai].id * 1e-6;
            pairs.push_back({c, ai, pi});
        }
    std::sort(pairs.begin(), pairs.end(), [](Pair const& x, Pair const& y) { return x.cost < y.cost; });
    std::vector<int> taken(preyList.size(), 0);
    std::vector<char> used(agents.size(), 0);
    std::vector<int> assignedTo(agents.size(), -1);
    for (auto const& pr : pairs)
    {
        int const cap = preyList[pr.p].queen ? 1000 : PACK_SIZE;
        if (used[pr.a] || taken[pr.p] >= cap)
            continue;
        used[pr.a] = 1;
        assignedTo[pr.a] = pr.p;
        taken[pr.p]++;
        if (agents[pr.a].id == me)
            myPrey = pr.p;
    }
    if (myPrey >= 0)
    {
        myPackSize = taken[myPrey];
        // Rank in the pack (by id): each member takes its own side of her.
        int rank = 0;
        for (std::size_t a = 0; a < agents.size(); a++)
            if (assignedTo[a] == myPrey && agents[a].id < me)
                rank++;
        myPackRank = rank;
    }
}

// Pack mates (other than us) within `r` of a cell, from fleet reports.
int mates_near(int cell, int r)
{
    int k = 0;
    for (auto const& [id, st] : fleet)
        if (id != me && R - st.round <= 3 && pack_member(id, st.len, st.leader) && manhattan(st.cell, cell) <= r)
            k++;
    return k;
}

} // namespace

// ===== intent.hpp =====
// L5 INTENT - goal choosers: pearls, exploring, camping, outposts, zones, wall line, portals,
// the queen's rest / flee / bunker, guard screens.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

namespace {

// The goal a dragon is heading for this turn (shown in the indicator and the trace).
enum Intent
{
    I_EXPLORE,
    I_PEARL,
    I_LATE_SAFE,
    I_EVADE,
    I_FLEE,
    I_HUNT,
    I_HUNT_QUEEN,
    I_FEED,
    I_HUNTER,
    I_PACK,
    I_LURK,
    I_REGROUP,
    I_WALL,
    I_INTERCEPT,
    I_POST,
    I_SCREEN,
    I_GUARD,
    I_GUARD_LINE,
    I_RESCUE,
    I_BUNKER,
    I_REST,
    I_HOME,
    I_PORTAL,
    I_PUSH,
    I_DEFEND,
    I_CAMP,
    I_ZONE,
    I_OUTPOST,
    I_CORK,
};
// 6b: the pearl leash of each role - a role's duty replaces the pearl goal unless the pearl is at most this close
// (one table instead of literals spread over the target steps; same values as before).
struct PearlLeash
{
    int hunt = 4, majHunt = 2, feed = 6, pack = 2, wall = 2, screen = 3, guard = 4, defender = 10;
    int queenAfterTheirs = 3; // our queen once theirs is dead: no pearl trips beyond this
    int zone = ZONE_OVERRIDE, scoutMargin = SCOUT_PEARL_MARGIN;
};
constexpr PearlLeash kLeash{};

char const* intent_name(Intent i)
{
    static char const* const names[] = {"explore", "pearl", "late-safe", "evade", "flee", "hunt", "hunt-queen", "feed", "hunter", "pack", "lurk", "regroup", "wall", "intercept", "post", "screen", "guard", "guard-line", "rescue", "bunker", "rest", "home", "portal", "push", "defend", "camp", "zone", "outpost", "cork"};
    return names[i];
}

std::vector<PearlCand> gather_pearls()
{
    compute_voronoi();
    if (REGION_VALUE && !amQueen)
        update_region_value();
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
        else if (nextSpawn[c] >= R &&
                 nextSpawn[c] - R <= (CAMP_JOB && !amQueen ? (OPEN_HOLD && R < OPEN_HOLD_END && distHead[c] <= HOLD_DIST // O3
                                                                  ? std::max(distHead[c] + CAMP_LEAD, HOLD_WAIT)
                                                                  : (distHead[c] < INF ? distHead[c] : 0) + CAMP_LEAD)
                                               : std::max(25, (distHead[c] < INF ? distHead[c] : 0) + PREPOS_SLACK)))
        {
            conf = nextEst[c] ? 0.35 : CAMP_CONF;
            appear = nextSpawn[c];
        }
        if (conf <= 0 || distHead[c] >= INF || skipPearl[c] > R)
            continue;
        int const eta = distHead[c];
        if (eta > 60)
            continue;
        // Contested: an enemy head is clearly closer (A: by path, reports included).
        if (VORONOI && static_cast<int>(vorEnemy.size()) == N)
        {
            if (vorEnemy[c] + 1 < eta)
                conf *= (AGGRO_EXPANSION && sit.expansion && !amQueen) ? 0.8 : 0.35;
        }
        else
            for (auto const& h : heads)
                if (!h.mine && manhattan(h.cell, c) + 1 < eta)
                    conf *= (AGGRO_EXPANSION && sit.expansion && !amQueen) ? 0.8 : 0.35;
        if (OWN_HALF && halfMargin(c) > 0)
            conf *= OWN_HALF_BONUS; // ours first: raiders feed on what we leave
        // v2.14: the queen keeps to her home ground, away from enemy heads, and
        // never goes into a pocket (a dead end is where escorts finish her).
        bool const herDrop = FEED_FIX && amQueen && eta <= FEED_EAT_DIST && pearlState[c] == 1 && qThreat[c] >= INF;
        if (amQueen && QUEEN_HOME && !queenStarvingG && !herDrop)
        {
            if (ourQueenSpawn >= 0 && manhattan(c, ourQueenSpawn) > queenLeash)
            {
                // 7d: a little further out is fine where she can still turn round (a loop region longer than
                // her) and we get there first.
                int const far = manhattan(c, ourQueenSpawn);
                bool const roomy = QUEEN_TOPO_LEASH && far <= 2 * queenLeash && topo_room(c, L) && vor_margin(c) >= 2;
                if (!roomy)
                    continue;
            }
            bool clear = true;
            for (auto const& h : heads)
                if (!h.mine && manhattan(h.cell, c) <= QUEEN_PEARL_CLEAR)
                    clear = false;
            if (VORONOI && static_cast<int>(vorEnemy.size()) == N && vorEnemy[c] <= eta)
                clear = false; // A: an enemy gets there as soon as she does
            if (!clear || in_pocket(c))
                continue;
        }
        // Pearls right by the queen are hers: teammates going for them box her in.
        if (QUEEN_HOME && !amQueen)
        {
            bool byQueen = false;
            for (auto const& h : heads)
                if (h.mine && h.id == queenId && manhattan(h.cell, c) <= (FEED_FIX && R >= FEED_KEEP_ROUND ? FEED_KEEP_RADIUS : 3))
                    byQueen = true; // feeders die within 3 of her: their drops are for her
            if (byQueen)
                continue;
        }
        // A fountain pocket takes one miner at a time (two in a 1-wide dead
        // end block each other); a teammate already closer has it.
        if (!amQueen && fountain(c))
        {
            bool taken = false;
            for (auto const& h : heads)
                if (h.mine && h.id != me && manhattan(h.cell, c) <= 3 && manhattan(h.cell, c) < eta)
                    taken = true;
            if (taken)
                continue;
            conf *= 1.2; // renewable: worth the trip
        }
        // P7: a pearl in a rich area is worth more than a lone one at the end
        // of a corridor (the dragon stays among more pearls afterwards).
        if (REGION_VALUE && !amQueen)
            conf *= 1.0 + RV_PEARL_W * rv_size_factor() * rv_norm(c);
        // In survival mode she does not go near them at all.
        if (amQueen && sit.survival)
        {
            bool nearEnemy = false;
            for (auto const& e : enemyReps)
                if (R - e.round <= 10 && manhattan(e.cell, c) <= 8)
                    nearEnemy = true;
            if (nearEnemy)
                continue;
        }
        // The queen leaves pearls near recently reported enemies to others.
        if (amQueen)
            for (auto const& e : enemyReps)
                if (R - e.round <= 10 && manhattan(e.cell, c) <= 6)
                {
                    conf *= 0.25;
                    break;
                }
        // Dangerous area reported by the fleet.
        for (auto const& dg : dangers)
            if (dg.cause == 1 && R - dg.round <= 20 && manhattan(dg.cell, c) <= 3)
                conf *= my.leader ? 0.3 : 0.7;
        out.push_back({c, appear, conf});
    }
    if (out.size() > 60)
    {
        std::nth_element(out.begin(), out.begin() + 60, out.end(), [](PearlCand const& a, PearlCand const& b) {
            return std::max(distHead[a.cell], a.appear - R) / a.conf < std::max(distHead[b.cell], b.appear - R) / b.conf;
        });
        out.resize(60);
    }
    // Big timed prizes: tiles spawning together in a cluster draw several
    // campers (one per tile through the matching); a small bonus each.
    if (CAMP_JOB)
        for (auto& pc : out)
        {
            if (pc.appear <= R)
                continue;
            int mates = 0;
            for (auto const& o : out)
                if (&o != &pc && o.appear > R && std::abs(o.appear - pc.appear) <= 3 && manhattan(o.cell, pc.cell) <= 3)
                    mates++;
            pc.conf *= 1.0 + 0.1 * std::min(mates, 3);
        }
    return out;
}

// F6: is the map centre worth pulling toward? Yes while most of it is
// unseen; once seen, only if it holds spawn tiles (Australia's is empty water).
// 7a: spawn-tile density by 2D prefix sums, rebuilt once a turn: any box count in O(1) (clipped at the map edge
// like the diamond loop below).
std::vector<int> densP;
int densRound = -1;
int dens_box(int cx, int cy, int r)
{
    if (densRound != R || static_cast<int>(densP.size()) != (W + 1) * (H + 1))
    {
        densRound = R;
        densP.assign((W + 1) * (H + 1), 0);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                int const t = y * W + x;
                int const v = nextSpawn[t] >= 0 || pearlState[t] != 0 || gapHi[t] > 0;
                densP[(y + 1) * (W + 1) + x + 1] = v + densP[y * (W + 1) + x + 1] + densP[(y + 1) * (W + 1) + x] - densP[y * (W + 1) + x];
            }
    }
    int const x0 = std::max(0, cx - r), x1 = std::min(W - 1, cx + r), y0 = std::max(0, cy - r), y1 = std::min(H - 1, cy + r);
    auto P = [](int y, int x) { return densP[y * (W + 1) + x]; };
    return P(y1 + 1, x1 + 1) - P(y0, x1 + 1) - P(y1 + 1, x0) + P(y0, x0);
}

int spawn_tiles_near(int c, int r)
{
    if (DENSITY_BOX)
        return dens_box(c % W, c / W, std::max(1, r - 1)); // a (2r-1)^2 box: about the diamond's area
    int n = 0;
    int const cx = c % W, cy = c / W;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r + std::abs(dy); dx <= r - std::abs(dy); dx++)
        {
            int const x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= W || y >= H)
                continue;
            int const t = y * W + x;
            n += nextSpawn[t] >= 0 || pearlState[t] != 0 || gapHi[t] > 0;
        }
    return n;
}
double centre_pull()
{
    static int round = -1;
    static double val = 1.0;
    if (round == R)
        return val;
    round = R;
    int const r = std::max(3, (W + H) / 12);
    int const ctr = (H / 2) * W + W / 2;
    int seen = 0, cells = 0;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r + std::abs(dy); dx <= r - std::abs(dy); dx++)
        {
            int const x = W / 2 + dx, y = H / 2 + dy;
            if (x < 0 || y < 0 || x >= W || y >= H)
                continue;
            cells++;
            seen += seenRound[y * W + x] >= 0;
        }
    val = (seen * 2 < cells || spawn_tiles_near(ctr, r) >= 2) ? 1.0 : 0.0;
    return val;
}

// 6c v3 (INTENT_MEMORY 3): the head start, in steps, that a chooser gives the goal this dragon picked last round
// for the same intent - a new goal must be closer by COMMIT_MARGIN steps and by 30%. None with an enemy head in view.
double keep_bonus(int c, int kind)
{
    if (INTENT_MEMORY != 3 || amQueen || c != lastGoal || kind != lastGoalIntent || R - lastGoalR > 1 || distHead[c] >= INF)
        return 0;
    for (auto const& h : heads)
        if (!h.mine)
            return 0;
    return std::max<double>(COMMIT_MARGIN, 0.3 * distHead[c]);
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
    for (int pass = amQueen ? 1 : 0; pass < 2 && best < 0; pass++)
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
            if (REGION_VALUE && !amQueen && R < RV_EXPLORE_END)
            {
                // P7: the opening heads for the middle of the map, where both
                // sides meet and pearl fields usually are.
                int const cx = std::abs(c % W - W / 2), cy = std::abs(c / W - H / 2);
                double const mid = std::max(0.0, 1.0 - static_cast<double>(cx + cy) / std::max(1, (W + H) / 4));
                score -= RV_EXPLORE_W * rv_size_factor() * mid * (CENTRE_KNOWN ? centre_pull() : 1.0);
            }
            if (VORONOI && !amQueen && R >= 10)
            {
                // A: hold and push our side of the border; stay out of theirs.
                int const mg = vor_margin(c);
                bool const push = OPEN_PUSH && R < OPEN_PUSH_END; // O2: the opening claims ground
                if (mg < -3)
                    score += push ? 0.0 : VOR_DEEP_PEN;
                else if (mg >= (push ? -3 : 0) && mg <= 4)
                    score -= push ? 2 * VOR_FRONT_BONUS : VOR_FRONT_BONUS;
            }
            if (my.role == ROLE_EXPLORER)
                score -= 0.3 * dh; // explorers push further out
            for (auto const& dg : dangers)
                if (dg.cause == 1 && R - dg.round <= 30 && manhattan(dg.cell, c) <= 4)
                    score += my.leader ? 200 : 40;
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

// Nearest spawn tile no teammate is already sitting by; sooner spawns first.
int choose_camp_goal(bool ownHalf = false)
{
    std::vector<int> mates;
    for (auto const& h : heads)
        if (h.mine && h.id != me)
            mates.push_back(h.cell);
    for (auto const& [id, s] : fleet)
        if (id != me && R - s.round <= 6)
            mates.push_back(s.cell);
    int best = -1;
    double bestScore = 1e18;
    for (int c = 0; c < N; c++)
    {
        if (nextSpawn[c] < 0 || distHead[c] >= 30 || skipPearl[c] > R)
            continue;
        bool taken = false;
        for (int m : mates)
            if (manhattan(m, c) <= 2)
                taken = true;
        if (taken)
            continue;
        double const wait = nextSpawn[c] >= R ? nextSpawn[c] - R : 15; // spawned earlier: next time unknown
        if (CAMP_MAX_WAIT > 0 && wait > CAMP_MAX_WAIT + distHead[c])
            continue; // F6: a long wait - explore or collect elsewhere instead
        if (VORONOI && static_cast<int>(vorEnemy.size()) == N && vorEnemy[c] + 1 < distHead[c])
            continue; // A: they get there first
        double score = distHead[c] + 0.5 * std::max(0.0, wait - distHead[c]);
        if (REGION_VALUE && !amQueen)
            score -= RV_CAMP_W * rv_size_factor() * rv_norm(c); // P7: rich areas first
        if (ownHalf)
        {
            int const hm = halfMargin(c);
            if (hm <= 0)
                continue;
            score += 0.7 * hm; // near the middle: where raiders come through
        }
        score -= keep_bonus(c, I_CAMP);
        if (score < bestScore)
        {
            bestScore = score;
            best = c;
        }
    }
    return best;
}

// P8 outposts: the pearl-richest known spots, OUTPOST_SEP apart (2-8 of
// them by map size). An idle collector takes the nearest one that is not
// crowded by teammates already.
std::vector<int> outposts;
int outpostRound = -100;
int choose_outpost()
{
    if (R - outpostRound >= 10)
    {
        outpostRound = R;
        update_region_value();
        outposts.clear();
        int const K = mapProf.outposts;
        std::vector<int> cand;
        for (int c = 0; c < N; c++)
            if (regionVal[c] >= OUTPOST_MIN_RV && seenRound[c] >= 0 && (OUTPOST_MIN_MARGIN <= -99 || halfMargin(c) >= OUTPOST_MIN_MARGIN))
                cand.push_back(c);
        std::sort(cand.begin(), cand.end(), [](int a, int b) { return regionVal[a] > regionVal[b]; });
        if (OUTPOST_MIN_TILES > 0)
            std::erase_if(cand, [](int c) { return spawn_tiles_near(c, RV_RAD) < OUTPOST_MIN_TILES; });
        std::sort(cand.begin(), cand.end(), [](int a, int b) { return regionVal[a] > regionVal[b]; });
        for (int c : cand)
        {
            bool ok = true;
            for (int o : outposts)
                if (manhattan(o, c) < OUTPOST_SEP)
                    ok = false;
            if (ok)
                outposts.push_back(c);
            if (static_cast<int>(outposts.size()) >= K)
                break;
        }
    }
    std::vector<int> mates;
    for (auto const& h : heads)
        if (h.mine && h.id != me)
            mates.push_back(h.cell);
    for (auto const& [id, st] : fleet)
        if (id != me && R - st.round <= 6)
            mates.push_back(st.cell);
    int best = -1;
    double bestScore = 1e18;
    for (int o : outposts)
    {
        if (distHead[o] >= 40)
            continue;
        int crowd = 0;
        for (int m : mates)
            if (manhattan(m, o) <= 5)
                crowd++;
        double const sc = distHead[o] + OUTPOST_CROWD * crowd - keep_bonus(o, I_OUTPOST);
        if (sc < bestScore)
        {
            bestScore = sc;
            best = o;
        }
    }
    return best;
}

// OPEN_PLAN zones: the richest spots we know (our outposts + zones teammates
// reported). Each collector takes the zone worth most to it: richness shared
// by the teammates already working it, minus the walk - rich zones draw more
// dragons, thin ones fewer, and the split adapts every turn.
int choose_zone()
{
    choose_outpost(); // refreshes `outposts`
    std::vector<std::pair<int, int>> zs;
    for (int o : outposts)
        zs.push_back({o, spawn_tiles_near(o, RV_RAD)});
    for (auto const& z : zoneReps)
        if (R - z.round <= 80)
            zs.push_back({z.cell, z.tiles});
    std::vector<int> mates;
    for (auto const& h : heads)
        if (h.mine && h.id != me && h.id != queenId)
            mates.push_back(h.cell);
    for (auto const& [id, st] : fleet)
        if (id != me && id != queenId && R - st.round <= 6)
            mates.push_back(st.cell);
    int best = -1;
    double bestV = -1e18;
    for (auto const& [c, tiles] : zs)
    {
        if (c < 0 || c >= N || distHead[c] >= INF || distHead[c] > 60 || tiles < ZONE_TILES)
            continue;
        if (OUTPOST_MIN_MARGIN > -99 && halfMargin(c) < OUTPOST_MIN_MARGIN)
            continue;
        int assigned = 0;
        for (int m : mates)
            if (manhattan(m, c) <= ZONE_RAD)
                assigned++;
        double const v = static_cast<double>(tiles) / (assigned + 1) - ZONE_DIST_W * (distHead[c] - keep_bonus(c, I_ZONE));
        if (v > bestV)
        {
            bestV = v;
            best = c;
        }
    }
    return best;
}

// P8 wall: this member's place in the line advancing on the enemy's home -
// members spread WALL_GAP apart across the line of advance (by id rank); a
// member well ahead of the rest waits (-1) so the line arrives together.
int wall_goal()
{
    int const es0 = enemy_spawn_guess();
    if (es0 < 0 || ourQueenSpawn < 0 || body.empty())
        return -1;
    // Not clearly stronger: hold the middle line between the two homes.
    double const frac = units >= WALL_PUSH_RATIO * est_enemy_units() ? 1.0 : 0.5;
    int const es = cell_of(std::clamp(static_cast<int>(std::lround(ourQueenSpawn % W + frac * (es0 % W - ourQueenSpawn % W))), 0, W - 1),
                           std::clamp(static_cast<int>(std::lround(ourQueenSpawn / W + frac * (es0 / W - ourQueenSpawn / W))), 0, H - 1));
    std::vector<int> ids{me};
    double othersDist = 0;
    int others = 0;
    for (auto const& [id, st] : fleet)
        if (id != me && id != queenId && !st.leader && R - st.round <= 4 && wall_slot(id) && st.len >= WALL_MIN_LEN)
        {
            ids.push_back(id);
            othersDist += manhattan(st.cell, es);
            others++;
        }
    if (others > 0 && manhattan(body[0], es) < othersDist / others - WALL_LEAD)
        return -1;
    std::sort(ids.begin(), ids.end());
    int const rank = static_cast<int>(std::find(ids.begin(), ids.end(), me) - ids.begin());
    int const n = static_cast<int>(ids.size());
    double const ax = es % W - ourQueenSpawn % W, ay = es / W - ourQueenSpawn / W;
    double const al = std::max(1.0, std::sqrt(ax * ax + ay * ay));
    double const off = (rank - (n - 1) / 2.0) * WALL_GAP;
    int const gx = std::clamp(static_cast<int>(std::lround(es % W - ay / al * off)), 0, W - 1);
    int const gy = std::clamp(static_cast<int>(std::lround(es / W + ax / al * off)), 0, H - 1);
    int best = -1, bestD = INF;
    for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++)
        {
            int const x = gx + dx, y = gy + dy;
            if (x < 0 || y < 0 || x >= W || y >= H)
                continue;
            int const c = y * W + x;
            if (distHead[c] >= INF)
                continue;
            int const d = std::abs(dx) + std::abs(dy);
            if (d < bestD)
            {
                bestD = d;
                best = c;
            }
        }
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

// The queen's resting spot: the most open known water inside her leash
// (cells all round with four open sides), never in a pocket.
int choose_rest()
{
    if (ourQueenSpawn < 0)
        return -1;
    int best = -1;
    double bestScore = -1e18;
    int const hx = ourQueenSpawn % W, hy = ourQueenSpawn / W;
    for (int dy = -QUEEN_LEASH; dy <= QUEEN_LEASH; dy++)
        for (int dx = -QUEEN_LEASH + std::abs(dy); dx <= QUEEN_LEASH - std::abs(dy); dx++)
        {
            int const c = cell_of(((hx + dx) % W + W) % W, ((hy + dy) % H + H) % H);
            if (seenRound[c] < 0 || distHead[c] >= INF || in_pocket(c))
                continue;
            double sc = 0;
            for (int ey = -2; ey <= 2; ey++)
                for (int ex = -2 + std::abs(ey); ex <= 2 - std::abs(ey); ex++)
                {
                    int const n = cell_of(((c % W + ex) % W + W) % W, ((c / W + ey) % H + H) % H);
                    sc += seenRound[n] >= 0 ? open_sides(n) : 1;
                }
            sc -= 0.5 * (std::abs(dx) + std::abs(dy));
            for (auto const& e : enemyReps)
                if (R - e.round <= 20 && manhattan(e.cell, c) <= 6)
                    sc -= 10;
            if (sc > bestScore)
            {
                bestScore = sc;
                best = c;
            }
        }
    return best;
}

// F5: the safest open cell within 8 steps of the queen: outside every enemy
// reach (two turns if possible), far from enemy heads, open on all sides,
// near our own dragons, never in a pocket.
int choose_flee()
{
    int best = -1;
    double bestScore = -1e18;
    int const hd = body.empty() ? -1 : body[0];
    if (hd < 0)
        return -1;
    for (int c = 0; c < N; c++)
    {
        int const d = distHead[c];
        if (d < 1 || d > 8 || qThreat[c] < INF || in_pocket(c) || seenRound[c] < 0)
            continue;
        double sc = 2.0 * open_sides(c);
        if (QUEEN_LOOK2 && static_cast<int>(qThreat2.size()) == N && qThreat2[c] >= INF)
            sc += 6;
        int nearE = 99;
        for (auto const& h : heads)
            if (!h.mine)
                nearE = std::min(nearE, manhattan(h.cell, c));
        for (auto const& e : enemyReps)
            if (R - e.round <= 3)
                nearE = std::min(nearE, manhattan(e.cell, c));
        sc += 1.5 * std::min(nearE, 10);
        int mates = 0;
        for (auto const& h : heads)
            if (h.mine && h.id != me && manhattan(h.cell, c) <= 4)
                mates++;
        for (auto const& [id, st] : fleet)
            if (id != me && R - st.round <= 3 && manhattan(st.cell, c) <= 4)
                mates++;
        sc += 1.0 * std::min(mates, 4) - 0.4 * d;
        if (sc > bestScore)
        {
            bestScore = sc;
            best = c;
        }
    }
    return best;
}

// 7j: chokepoint post - the first bridge (topology) on the queen's shortest way toward the enemy side, within
// CHOKE_RANGE steps of her: the cell on her side of it. One guard there sees (and can block) everything that comes
// through. -1 when her way out has no bridge that close.
int choke_post_search(int qCell);
int choke_post(int qCell)
{
    static int cacheR = -1, cacheQ = -1, cacheV = -1; // once per turn
    if (cacheR != R || cacheQ != qCell)
    {
        cacheR = R;
        cacheQ = qCell;
        cacheV = choke_post_search(qCell);
    }
    return cacheV;
}
int choke_post_search(int qCell)
{
    int const es = enemy_spawn_guess();
    if (qCell < 0 || es < 0)
        return -1;
    topo_refresh();
    static std::vector<int> par, q;
    par.assign(N, -2);
    q.assign(1, qCell);
    par[qCell] = -1;
    int hit = -1;
    for (std::size_t i = 0; i < q.size() && hit < 0; i++)
        for (int d = 0; d < 4; d++)
        {
            int const n = step_open(q[i], d); // their side is mostly unexplored: plan through it (only bridges need known edges)
            if (n < 0 || par[n] != -2)
                continue;
            par[n] = q[i];
            if (n == es || manhattan(n, es) <= 2)
            {
                hit = n;
                break;
            }
            q.push_back(n);
        }
    if (hit < 0)
        return -1;
    std::vector<int> path; // queen -> enemy side
    for (int c = hit; c != -1; c = par[c])
        path.push_back(c);
    std::reverse(path.begin(), path.end());
    for (int i = 0; i + 1 < static_cast<int>(path.size()) && i < CHOKE_RANGE; i++)
        for (int d = 0; d < 4; d++)
            if (cross(path[i], d) == path[i + 1] && cross(path[i], d) >= 0 && topo.bridge[edge_key(path[i], d)])
                return i >= 2 ? path[i] : -1; // not right next to her: a guard there would box her in
    return -1;
}

// Screen post: a cell SCREEN_MIN..SCREEN_MAX steps from the queen along her
// row (even ids) or column (odd ids), on the side nearer the enemy queen.
// Sonar runs along rows and columns, so from there a ping reaches her.
int screen_post(int qCell)
{
    // Face the threat: the nearest enemy reported near her lately, else the
    // enemy queen (or their side of the map).
    int eq = -1, ed = INF;
    if (SCREENS_MORE)
        for (auto const& e : enemyReps)
            if (R - e.round <= 10 && manhattan(e.cell, qCell) <= 15 && manhattan(e.cell, qCell) < ed)
            {
                ed = manhattan(e.cell, qCell);
                eq = e.cell;
            }
    if (eq < 0)
        eq = enemy_queen_target();
    int best = -1, bestScore = INF;
    bool const row = (me & 1) == 0;
    for (int off = -SCREEN_MAX; off <= SCREEN_MAX; off++)
    {
        if (std::abs(off) < SCREEN_MIN)
            continue;
        int const x = qCell % W, y = qCell / W;
        int const nx = row ? ((x + off) % W + W) % W : x, ny = row ? y : ((y + off) % H + H) % H;
        int const c = cell_of(nx, ny);
        if (distHead[c] >= INF || manhattan(c, qCell) < SCREEN_MIN)
            continue;
        int const sc = (eq >= 0 ? 4 * manhattan(c, eq) : 0) + distHead[c];
        if (sc < bestScore)
        {
            bestScore = sc;
            best = c;
        }
    }
    return best;
}

// Bunker: once the enemy queen is dead, ours only has to stay alive. A queen
// of length 2-3 can circle a 2x2 box for ever (verified in the engine; at
// length 4 her own tail blocks the loop). Boxes with few open sides are safer.
int bunkerBox = -1;

// The box with top-left tl, in loop order (tl, east, south-east, south), when
// all four links inside it are known open water.
bool box_cells(int tl, std::array<int, 4>& out)
{
    auto const open = [](int c, int d) { return edgeType[edge_code(c, d)] == E_OPEN; };
    if (!open(tl, 1) || !open(tl, 2))
        return false;
    int const e = nb(tl, 1), s = nb(tl, 2), se = nb(e, 2);
    if (!open(e, 2) || !open(s, 1) || se == tl || e == s)
        return false;
    out = {tl, e, se, s};
    return true;
}

// Ways into the box from outside (unexplored edges count as open).
int box_exposure(std::array<int, 4> const& bx)
{
    int k = 0;
    for (int c : bx)
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n == -1 || std::find(bx.begin(), bx.end(), n) != bx.end())
                continue;
            k++;
        }
    return k;
}

// Usable now: no other dragon in it, not in enemy reach, and no pearl in it or
// due there (eating fills the box: a queen of length 4 in a 2x2 has no move).
bool box_usable(std::array<int, 4> const& bx)
{
    for (int c : bx)
    {
        if (vis[c] && occId[c] >= 0 && occId[c] != me)
            return false;
        if (qThreat[c] < INF)
            return false;
        if (pearlState[c] == 1 || obsNext[c] >= 0)
            return false;
        for (int k = 0; k < 4; k++)
        {
            std::uint8_t const t = edgeType[edge_code(c, k)];
            if (t != E_OPEN && t != E_KELP)
                return false; // no portals or unexplored edges around the bunker
        }
    }
    return true;
}

int choose_bunker()
{
    int best = -1;
    double bestS = 1e18;
    std::array<int, 4> bx{};
    for (int c = 0; c < N; c++)
    {
        if (distHead[c] > BUNKER_SEARCH && c != body[0])
            continue;
        if (!box_cells(c, bx) || !box_usable(bx))
            continue;
        int d = INF;
        for (int x : bx)
            d = std::min(d, x == body[0] ? 0 : distHead[x]);
        if (d >= INF)
            continue;
        double sc = 8.0 * box_exposure(bx) + d;
        for (int x : bx)
            if (qMargin[x] <= QUEEN_MARGIN)
                sc += 6.0 * (QUEEN_MARGIN + 1 - qMargin[x]);
        for (auto const& e : enemyReps)
            if (R - e.round <= 10)
                sc += 40.0 / (1 + manhattan(e.cell, c));
        if (sc < bestS)
        {
            bestS = sc;
            best = c;
        }
    }
    return best;
}

} // namespace

// ===== tactics.hpp =====
// L4 TACTICS - move candidates, adjacency and threat terms, the queen's two-turn lookahead,
// her free exits, strike paths.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

namespace {

// Decision
struct Cand
{
    int d = 0, dest = -1;
    bool legal = false, blind = false, attack = false, eat = false;
    bool ok1 = false, ok2 = false;
    int area = 0;
    double dive = -1e9; // net segments won by entering a dead end (+0.5 if we get out whole); non-queens
    bool surv = false;  // queen: some way on through known or unexplored water
    int dt = INF;       // path distance to the target after this move
    bool noLoop = false; // queen (length <= 3): no free 2x2 reachable afterwards
    double score = -1e18;
    std::array<double, 4> bands{}; // score split by severity band (BAND_LIFE .. BAND_STYLE)
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
bool child_can_escape(int childLen, bool growth = false)
{
    if (static_cast<int>(body.size()) < L)
        return true; // tail is out of view, well away from whatever traps the head
    std::vector<int> child(body.rbegin(), body.rbegin() + childLen);
    WallScope ws;
    for (int i = 0; i < L - childLen; i++)
        ws.wall(body[i]);
    BodyScope const sb(child, childLen);
    int const need = growth && SPLIT_ROOM ? std::max(6, 2 * childLen + 4) : std::min(childLen + 1, 6);
    FloodRes const f = flood(child[0], child.back(), std::max(childLen + 8, need + 2));
    // A growth split is optional: its child must have room to live, not just
    // to take a step (children born into pockets died within a few rounds).
    return (f.tail && !(growth && SPLIT_ROOM)) || f.area >= need;
}

// P3c: after a move to `dest` with body `nb` (newLen long), how many cells
// can she put her head on next turn (free + paid steps, her own body in the
// way except its last segment) that no enemy can reach within two turns and
// that are not dead ends? Capped at 8.
int look2_safe_cells(int dest, std::vector<int> const& nb, int newLen)
{
    l2S++;
    int const reachQ = std::min(LOOK2_QUEEN_REACH, (newLen + 3) / 4 + (LOOK2_PAID ? std::max(0, newLen - 2) : 0));
    for (int i = 0; i + 1 < newLen && i < static_cast<int>(nb.size()); i++)
        l2Stamp[nb[i]] = l2S; // her body after the move (the last segment moves on)
    l2Q.clear();
    l2Q.push_back(dest);
    l2Stamp[dest] = l2S;
    l2Dist[dest] = 0;
    int safe = 0;
    for (std::size_t i = 0; i < l2Q.size() && safe < 8; i++)
    {
        int const c = l2Q[i];
        if (l2Dist[c] >= reachQ)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int const n = cross(c, k);
            if (n < 0 || l2Stamp[n] == l2S || (vis[n] && occId[n] >= 0 && occId[n] != me))
                continue;
            l2Stamp[n] = l2S;
            l2Dist[n] = l2Dist[c] + 1;
            l2Q.push_back(n);
            if (qThreat2[n] >= INF && seenRound[n] >= 0 && open_sides(n) >= 2)
                safe++;
        }
    }
    return safe;
}

double queen_threat_pen(int n)
{
    if (!QUEEN_HARD_THREAT)
    {
        if (qThreat[n] == 0)
            return 3e6;
        if (qThreat[n] < INF)
            return PAID_THREAT_PEN / static_cast<double>(qThreat[n]);
        return 0;
    }
    // Every enemy moves after the queen, and a head-on kills both whatever the
    // lengths, so paying segments to reach her is free for the attacker:
    // anything within free + paid reach is as good as a kill.
    double p = 0;
    if (qThreat[n] == 0)
        p = 3e6;
    else if (qThreat[n] < INF)
        p = HARD_THREAT_PEN - 20000.0 * std::min(qThreat[n], 10);
    if (qThreatCnt[n] > 1)
        p += THREAT_EXTRA_ATTACKER * (qThreatCnt[n] - 1);
    if (qThreat[n] == INF && qMargin[n] <= QUEEN_MARGIN)
        p += QUEEN_MARGIN_PEN * (QUEEN_MARGIN + 1 - qMargin[n]);
    return p;
}

// The part of queen_threat_pen that is only caution (not inside any reach yet).
double queen_margin_pen(int n)
{
    if (QUEEN_HARD_THREAT && qThreat[n] == INF && qMargin[n] <= QUEEN_MARGIN)
        return QUEEN_MARGIN_PEN * (QUEEN_MARGIN + 1 - qMargin[n]);
    return 0;
}

double end_threat(int n)
{
    double t = 0;
    for (auto const& hh : heads)
    {
        if (hh.mine)
        {
            if (manhattan(hh.cell, n) <= 1)
                t += 4000;
            continue;
        }
        if (amQueen && QUEEN_THREAT_MAP)
            continue; // added once below from the threat map
        int const reach = my.leader ? enemy_reach(hh) : 1;
        if (manhattan(hh.cell, n) <= reach)
            t += amQueen ? 3e6 : my.leader ? 300000 : THREAT_PEN;
    }
    if (amQueen && QUEEN_THREAT_MAP)
        t += queen_threat_pen(n);
    if (my.leader)
        for (int tc : enemyTails)
            if (manhattan(tc, n) <= 1)
                t += amQueen ? 400000 : 100000;
    return t;
}

// Shortest step sequence that ends in the enemy queen's head. Other dragons
// stand still during our turn; our own body follows us. Steps past the free
// ones cost a segment each, which does not matter: a head-on kills us anyway.
std::vector<int> regicide_path(int qcell, int maxSteps)
{
    set_body(body, L);
    std::unordered_map<int, int> dist, par, pdir;
    std::vector<int> q{body[0]};
    dist[body[0]] = 0;
    for (std::size_t i = 0; i < q.size(); i++)
    {
        int const c = q[i], dc = dist[c];
        if (dc >= maxSteps)
            continue;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n < 0 || dist.count(n))
                continue;
            if (n == qcell)
            {
                std::vector<int> dirs{d};
                for (int x = c; x != body[0]; x = par[x])
                    dirs.push_back(pdir[x]);
                std::reverse(dirs.begin(), dirs.end());
                return dirs;
            }
            if (!vis[n] || (occId[n] >= 0 && occId[n] != me) || !passable(n, dc + 1))
                continue;
            dist[n] = dc + 1;
            par[n] = c;
            pdir[n] = d;
            q.push_back(n);
        }
    }
    return {};
}

// Free cells next to the queen's head for her next move, with our body as it
// will be after a candidate move (newBody, head first; empty = as now).
int queen_exits(int qh, std::vector<int> const& newBody)
{
    int k = 0;
    for (int d = 0; d < 4; d++)
    {
        int const n = step_open(qh, d); // unexplored: may be open
        if (n < 0)
            continue;
        if (vis[n] && occId[n] >= 0 && occId[n] != me)
            continue; // another dragon (or her own body) is there
        if (std::find(newBody.begin(), newBody.end(), n) != newBody.end())
            continue; // we will be there
        k++;
    }
    return k;
}

} // namespace

// ===== act.hpp =====
// L6 ACT - split gates, issuing moves / sprints / splits, the sprint planner.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

namespace {

// Move cost by severity band (Architecture_v2 2.8). Every scoring term belongs to one band:
//   LIFE    certain death or a trap with nothing gained; the queen inside an enemy's strike reach; taking
//           the queen's last way out; a head-on with a teammate
//   SAFETY  proxies of danger: two-turn exposure, corners, no free loop, portals and unexplored cells for
//           the queen, next to enemy heads, crowding the queen
//   GOAL    progress to the goal, pearls, room, dives that pay, forced bunker / post loops
//   STYLE   spread from teammates, facing, danger zones, pinning enemies, boxing their queen
// Step 5: `total` is still the plain sum in the original order (identical play); `band` records the split.
enum Band { BAND_LIFE, BAND_SAFETY, BAND_GOAL, BAND_STYLE };
struct Cost
{
    double total = 0;
    std::array<double, 4> band{};
    void add(Band b, double v)
    {
        total += v;
        band[b] += v;
    }
    void set(Band b, double v) // replaces everything so far
    {
        total = v;
        band = {};
        band[b] = v;
    }
    void add_split(double v, Band b1, double v1, Band b2) // v in the total; v1 of it to b1, the rest to b2
    {
        total += v;
        band[b1] += v1;
        band[b2] += v - v1;
    }
};

// Numbers are the goal, but a crowded map kills small dragons for nothing.
bool room_for_more()
{
    if (EXPANSION && growth_open())
        return units < expansion_target();
    return units < std::min(unitLimit, std::max(2, N / SPLIT_DENSITY));
}

// The engine refuses every split once the team holds its unit limit. The
// queen's survival split must never be refused, so the others leave her
// QUEEN_SLOTS free (several dragons can split in the same round).
bool split_allowed(Controller& ct, int child)
{
    return ct.can_split(child) && (amQueen || units < unitLimit - QUEEN_SLOTS);
}

bool act_move(Controller& ct, int d, char const* why)
{
    ct.make_move(dir_of(d));
    note = why;
    prevBody = body;
    lastDirs = {d};
    if (traceF)
        traceAct = std::string("M") + DCH[d];
    return true;
}

bool act_split(Controller& ct, int child, char const* why)
{
    ct.do_split(child);
    note = why;
    prevBody = body;
    lastDirs.clear();
    if (traceF)
        traceAct = "S" + std::to_string(child);
    return true;
}

struct SprintPlan
{
    std::vector<int> dirs;
    double value = -1e18;
    bool kill = false;
};

std::vector<int> distGoal;

// Depth-first search over step sequences through cells in view. Other dragons
// do not move during our turn, so what we see is exact; our own body shifts
// step by step. Steps past freeN cost a segment each.
void sprint_dfs(std::vector<int> const& b, int len, int step, int maxSteps, int freeN, int eaten,
                std::vector<int>& dirs, std::vector<int>& eatenCells, bool useGoal, int goalStart, SprintPlan& best)
{
    int const h = b[0];
    for (int d = 0; d < 4; d++)
    {
        int const n = cross(h, d);
        if (n < 0 || !vis[n])
            continue;
        if (occId[n] >= 0 && occId[n] != me)
        {
            // Head-on with the enemy queen: worth any non-queen dragon.
            bool const canPay = step + 1 <= freeN || len >= 3;
            if (canPay && occHead[n] && occId[n] == enemyQueenId && !occMine[n] && !amQueen && !sit.enemyQueenDead && !best.kill)
            {
                dirs.push_back(d);
                best.dirs = dirs;
                best.kill = true;
                best.value = 1e12;
                dirs.pop_back();
            }
            continue;
        }
        set_body(b, len);
        if (!passable(n, 1))
            continue;
        bool const eat = pearl_at(n) && std::find(eatenCells.begin(), eatenCells.end(), n) == eatenCells.end();
        std::vector<int> nb2;
        nb2.reserve(b.size() + 1);
        nb2.push_back(n);
        nb2.insert(nb2.end(), b.begin(), b.end());
        int nl = len + (eat ? 1 : 0);
        if (!eat && static_cast<int>(b.size()) >= len)
            nb2.pop_back();
        if (step + 1 > freeN) // a paid step also removes one more tail segment
        {
            if (static_cast<int>(nb2.size()) >= nl)
                nb2.pop_back();
            nl--;
            if (nl < 2)
                continue;
        }
        if (eat)
            eatenCells.push_back(n);
        dirs.push_back(d);
        int const eatenNow = eaten + (eat ? 1 : 0);
        if (step + 1 >= 2 && !best.kill)
        {
            int const tailCell = static_cast<int>(nb2.size()) >= nl ? nb2.back() : -1;
            set_body(nb2, nl);
            int const cap = std::min(N, 3 * nl + 24);
            FloodRes const f = flood(n, tailCell, cap);
            int const area = f.area + (f.frontier && !amQueen ? nl : 0);
            if (f.tail || area >= 2 * nl + 4)
            {
                double v = eatenNow * 3000.0 + std::min(area, cap) * 15.0 - (L + eatenNow - nl) * 4000.0 - end_threat(n);
                if (QUEEN_EXIT_GUARD && !amQueen)
                    for (auto const& qh : heads)
                        if (qh.mine && qh.id == queenId && manhattan(qh.cell, n) <= 2)
                        {
                            std::vector<int> nowBody(body.begin(), body.begin() + std::min<int>(body.size(), L));
                            std::vector<int> after(nb2.begin(), nb2.begin() + std::min<int>(nb2.size(), nl));
                            int const before = queen_exits(qh.cell, nowBody), now = queen_exits(qh.cell, after);
                            if (now < before && now < 2)
                                v -= QUEEN_EXIT_PEN; // a sprint must not end in the queen's last way out
                        }
                if (useGoal && distGoal[n] < INF)
                    v += 150.0 * (goalStart - distGoal[n]);
                if (v > best.value)
                {
                    best.value = v;
                    best.dirs = dirs;
                }
            }
        }
        if (step + 1 < maxSteps && !best.kill)
            sprint_dfs(nb2, nl, step + 1, maxSteps, freeN, eatenNow, dirs, eatenCells, useGoal, goalStart, best);
        dirs.pop_back();
        if (eat)
            eatenCells.pop_back();
    }
}

bool act_moves(Controller& ct, std::vector<int> const& dirs, char const* why)
{
    std::vector<Direction> ds;
    for (int d : dirs)
        ds.push_back(dir_of(d));
    ct.make_moves(ds);
    note = why;
    prevBody = body;
    lastDirs = dirs;
    if (traceF)
    {
        traceAct = "M";
        for (int d : dirs)
            traceAct += DCH[d];
    }
    return true;
}

} // namespace

// ===== contact.hpp =====
// L4b CONTACT - the queen's contact search (R1) and its trigger (R8).
// When an enemy head is close and the move is hard, the queen looks two of her own moves ahead with the
// replies of the nearest enemies in between (max-min, Tron style) and judges the result by the ground she
// reaches before them, whether a free loop is still reachable, and her exits. The value is blended into
// her ordinary move score (score_moves), it does not replace it.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

namespace {

// Distance field with a stamp (no clearing between uses).
struct DField
{
    std::vector<int> d, st;
    int s = 0;
    void reset()
    {
        if (static_cast<int>(d.size()) != N)
        {
            d.assign(N, INF);
            st.assign(N, 0);
        }
        s++;
    }
    int get(int c) const { return st[c] == s ? d[c] : INF; }
    void set(int c, int v)
    {
        st[c] = s;
        d[c] = v;
    }
};

int csWork = 0;     // BFS cells popped in this search (R8 budget guard)
bool csAbort = false;
std::vector<int> csQ, restReach;

bool cs_budget_ok()
{
    if (csWork > CS_WORK_CAP)
        csAbort = true;
    return !csAbort;
}

// Enemy movement field from `src`: unexplored edges open, every dragon body in view blocks.
void cs_enemy_bfs(int src, int maxD, DField& f)
{
    f.reset();
    csQ.clear();
    csQ.push_back(src);
    f.set(src, 0);
    for (std::size_t i = 0; i < csQ.size(); i++)
    {
        int const c = csQ[i], dc = f.get(c);
        csWork++;
        if (dc >= maxD)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int const n = step_open(c, k);
            if (n < 0 || f.get(n) < INF || (vis[n] && occId[n] >= 0))
                continue;
            f.set(n, dc + 1);
            csQ.push_back(n);
        }
    }
}

// Queen movement field from her head after a move (body b, length len): known water only, timed bodies.
void cs_queen_bfs(std::vector<int> const& b, int len, int maxD, DField& f)
{
    BodyScope const sb(b, len);
    f.reset();
    csQ.clear();
    csQ.push_back(b[0]);
    f.set(b[0], 0);
    for (std::size_t i = 0; i < csQ.size(); i++)
    {
        int const c = csQ[i], dc = f.get(c);
        csWork++;
        if (dc >= maxD)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int const n = cross(c, k);
            if (n < 0 || f.get(n) < INF || !passable(n, dc + 1))
                continue;
            f.set(n, dc + 1);
            csQ.push_back(n);
        }
    }
}

// Body after one step (b head first, len); eats a pearl if there is one.
std::vector<int> cs_step(std::vector<int> const& b, int len, int n, int& outLen)
{
    bool const eat = pearl_at(n);
    std::vector<int> nb;
    nb.reserve(b.size() + 1);
    nb.push_back(n);
    nb.insert(nb.end(), b.begin(), b.end());
    outLen = len + (eat ? 1 : 0);
    if (!eat && static_cast<int>(b.size()) >= len)
        nb.pop_back();
    return nb;
}

struct CsEnemy
{
    int id, cell, len, freeR, fullR;
    std::vector<int> replies; // cells it can put its head on with free steps (no paid steps: it keeps its length)
};

struct CsResult
{
    bool ran = false, aborted = false;
    std::array<double, 4> val{}; // per direction, -1 (certain loss) .. 1
    std::array<bool, 4> has{};
};

// The search. cands: the analysed moves (legal / blind / dest / nb1); head: her head.
CsResult contact_search(std::array<Cand, 4> const& cands, int head)
{
    CsResult res;
    csWork = 0;
    csAbort = false;
    // Near enemies: the CS_ENEMIES closest visible heads within CS_NEAR.
    std::vector<CsEnemy> near;
    {
        std::vector<std::pair<int, HeadInfo const*>> byDist;
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, head) <= CS_NEAR)
                byDist.push_back({manhattan(h.cell, head), &h});
        std::sort(byDist.begin(), byDist.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        for (std::size_t i = 0; i < byDist.size() && static_cast<int>(near.size()) < CS_ENEMIES; i++)
        {
            HeadInfo const& h = *byDist[i].second;
            int const len = std::max(h.len, h.estLen);
            int const f = std::max(1, (len + 3) / 4);
            int const m = std::min(PAID_REACH_CAP, f + (PAID_REACH ? std::max(0, len - 2) : 0));
            near.push_back({h.id, h.cell, len, f, std::max(f, m), {}});
        }
    }
    if (near.empty())
        return res;
    std::unordered_set<int> nearIds;
    for (auto const& e : near)
        nearIds.insert(e.id);
    // Everyone else: where they can strike within two turns (as the look-2 map, without the near enemies).
    restReach.assign(N, INF);
    for (auto const& h : heads)
    {
        if (h.mine || nearIds.count(h.id))
            continue;
        int const len = std::max(h.len, h.estLen);
        int const f = std::max(1, (len + 3) / 4);
        int const m = std::min(PAID_REACH_CAP, f + (PAID_REACH ? std::max(0, len - 2) : 0));
        reach_bfs(h.cell, 2 * f, std::min(PAID_REACH_CAP, m + f), restReach);
    }
    {
        std::unordered_set<int> seen;
        for (auto const& h : heads)
            seen.insert(h.id);
        for (auto const& e : enemyReps)
        {
            int const age = R - e.round;
            if (age < 1 || age > REP_MAX_AGE || seen.count(e.id) || nearIds.count(e.id))
                continue;
            int const el = std::max(e.len, e.estLen);
            int const f1 = std::max(1, (el + 3) / 4);
            int const f = f1 * (age + 1);
            int const m = std::min(PAID_REACH_CAP, f + (PAID_REACH ? std::max(0, el - 2) : 0));
            reach_bfs(e.cell, f + f1, std::min(PAID_REACH_CAP, m + f1), restReach);
        }
    }
    // Each near enemy's reply cells (free steps only), and a distance field from every reply cell we use.
    static DField tmp;
    for (auto& e : near)
    {
        cs_enemy_bfs(e.cell, e.freeR, tmp);
        for (int c : csQ)
            if (c != e.cell && tmp.get(c) >= 1)
                e.replies.push_back(c);
    }
    static std::vector<DField> slots;
    if (slots.size() < 32)
        slots.resize(32);
    std::unordered_map<int, int> slotOf; // (enemy index * N + cell) -> slot
    int nSlots = 0;
    int const RT = CS_TERR_RADIUS;
    auto field_for = [&](int ei, int cell) -> DField const* {
        long long const key = static_cast<long long>(ei) * N + cell;
        auto const it = slotOf.find(static_cast<int>(key));
        if (it != slotOf.end())
            return &slots[it->second];
        if (nSlots >= static_cast<int>(slots.size()) || !cs_budget_ok())
            return nullptr;
        int const s = nSlots++;
        slotOf[static_cast<int>(key)] = s;
        CsEnemy const& e = near[ei];
        cs_enemy_bfs(cell, std::min(40, std::max(e.fullR, RT * e.freeR)), slots[s]);
        return &slots[s];
    };

    static DField qf;
    for (auto const& cd : cands)
    {
        if (!cd.legal || cd.blind || cd.dest < 0 || cd.nb1.empty())
            continue;
        int const d = cd.d;
        res.has[d] = true;
        // Inside some enemy's reach right now: their reply ends her (exact, every enemy moves after the queen).
        if (QUEEN_THREAT_MAP && qThreat[cd.dest] < INF)
        {
            res.val[d] = -1;
            continue;
        }
        int const len1 = L + (cd.eat ? 1 : 0);
        std::vector<int> const& b1 = cd.nb1;
        int const n1 = b1[0];
        // Her second moves (known water, legal with her body after the first move).
        struct Second
        {
            std::vector<int> b;
            int len;
            double base; // loop + exits part of the value (does not depend on the replies)
        };
        std::vector<Second> seconds;
        {
            BodyScope const sb(b1, len1);
            for (int k = 0; k < 4; k++)
            {
                int const n2 = cross(n1, k);
                if (n2 < 0 || !passable(n2, 1) || (vis[n2] && occId[n2] >= 0 && occId[n2] != me))
                    continue;
                int l2 = 0;
                std::vector<int> b2 = cs_step(b1, len1, n2, l2);
                seconds.push_back({std::move(b2), l2, 0.0});
            }
        }
        if (seconds.empty())
        {
            res.val[d] = -1; // no way on after this move
            continue;
        }
        for (auto& s : seconds)
        {
            bool const loop = loop_reachable(s.b, s.len);
            int ex = 0;
            {
                BodyScope const sb(s.b, s.len);
                for (int k = 0; k < 4; k++)
                {
                    int const n = cross(s.b[0], k);
                    ex += n >= 0 && passable(n, 1) && !(vis[n] && occId[n] >= 0 && occId[n] != me);
                }
            }
            s.base = CS_W_LOOP * (loop ? 1.0 : 0.0) + CS_W_EXITS * std::min(ex, 3) / 3.0;
        }
        // Replies to rank: the cells closest to her new head (the most threatening), top CS_REPLIES per enemy.
        std::vector<std::vector<int>> rep(near.size());
        for (std::size_t ei = 0; ei < near.size(); ei++)
        {
            rep[ei] = near[ei].replies;
            std::sort(rep[ei].begin(), rep[ei].end(), [&](int a, int b) { return manhattan(a, n1) < manhattan(b, n1) || (manhattan(a, n1) == manhattan(b, n1) && a < b); });
            if (static_cast<int>(rep[ei].size()) > CS_REPLIES)
                rep[ei].resize(CS_REPLIES);
            if (rep[ei].empty())
                rep[ei].push_back(near[ei].cell); // boxed in: assume it stays a threat where it is
        }
        // Queen fields for each second move (independent of the replies).
        std::vector<std::vector<std::pair<int, int>>> qCells(seconds.size()); // (cell, her distance)
        for (std::size_t si = 0; si < seconds.size() && cs_budget_ok(); si++)
        {
            cs_queen_bfs(seconds[si].b, seconds[si].len, RT, qf);
            for (int c : csQ)
                qCells[si].push_back({c, qf.get(c)});
        }
        if (csAbort)
            break;
        // max-min: the replies minimise, her second move maximises.
        double worst = 2.0;
        std::vector<int> idx(near.size(), 0);
        for (bool more = true; more && cs_budget_ok();)
        {
            std::vector<DField const*> ef(near.size());
            for (std::size_t ei = 0; ei < near.size(); ei++)
                ef[ei] = field_for(static_cast<int>(ei), rep[ei][idx[ei]]);
            if (csAbort)
                break;
            double best = -1.0;
            for (std::size_t si = 0; si < seconds.size(); si++)
            {
                auto const& s = seconds[si];
                int const n2 = s.b[0];
                bool dead = restReach[n2] < INF;
                for (std::size_t ei = 0; ei < near.size() && !dead; ei++)
                    dead = ef[ei] && ef[ei]->get(n2) <= near[ei].fullR;
                if (dead)
                    continue;
                int const fq = std::max(1, (s.len + 3) / 4);
                int terr = 0;
                for (auto const& [c, dq] : qCells[si])
                {
                    int const tq = (dq + fq - 1) / fq;
                    bool first = true;
                    for (std::size_t ei = 0; ei < near.size() && first; ei++)
                    {
                        int const de = ef[ei] ? ef[ei]->get(c) : INF;
                        if (de < INF && tq >= (de + near[ei].freeR - 1) / near[ei].freeR)
                            first = false;
                    }
                    terr += first;
                }
                csWork += static_cast<int>(qCells[si].size());
                // Ground saturates gently: 1 - e^(-cells / (2L + 8)) (a short queen with 14 cells is at 0.63, with 60 at 0.99).
                double const v = CS_W_TERR * (1.0 - std::exp(-static_cast<double>(terr) / (2 * s.len + 8))) + s.base;
                best = std::max(best, v);
            }
            worst = std::min(worst, best);
            // next joint reply
            more = false;
            for (std::size_t ei = 0; ei < near.size(); ei++)
            {
                if (++idx[ei] < static_cast<int>(rep[ei].size()))
                {
                    more = true;
                    break;
                }
                idx[ei] = 0;
            }
        }
        if (csAbort)
            break;
        res.val[d] = worst > 1.5 ? 0.0 : worst;
    }
    res.aborted = csAbort;
    res.ran = !csAbort;
    return res;
}

} // namespace

// ===== decide.hpp =====
// DECIDE - the per-turn decision, in stages: turn setup and candidate analysis (L4), the reflex
// table (L4), the target steps (L5), then feeding / splitting / move scoring (L6).
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

namespace {

// Per-turn values shared between the stages of decide() (reset every turn).
struct Turn
{
    int head{};
    int facing{};
    bool queenStarving{};
    bool sealedNoSplit{};
    std::array<Cand, 4> cands{};
    int cap{};
    bool anyLegal{};
    bool anySafe{};
    bool attackAvail{};
    int attackDir{};
    int attackLen{};
    double diveMin{};
    bool diving{};
    bool nextToEnemy{};
    bool queenAdjacent{};
    bool allIn{};
    int qNow{};
    std::unordered_set<int> stalkers{};
    bool wantAttack{};
    int freeN{};
    bool packer{};
    std::array<int, 4> qBox{};
    bool bunkerFresh{};
    int postDir{};
    bool capFeed{};
    std::vector<PearlCand> pearls{};
    int target{};
    int portalDir{};
    Intent why{};
    int pi{};
    bool hunter{};
    bool huntOk{};
    int feedId{};
    int feedLeader{};
    bool earlyFeed{};
    bool feedPhase{};
    int guardQ{};
    bool tieBunker{};
    bool bunkerMode{};
    int bunkerDir{};
    bool inBox{};
    bool campWait{};
    int bestD{};
};
Turn tn;

// Turn setup: queen spawn, starvation, sealed start, periodic news, threat map, distances.
bool prepare_turn(Controller& ct)
{
    auto& head = tn.head;
    auto& facing = tn.facing;
    auto& queenStarving = tn.queenStarving;
    auto& sealedNoSplit = tn.sealedNoSplit;
    head = body[0];
    facing = dir_index(ct.get_dir());
    claimTarget = -1;
    if (amQueen && ourQueenSpawn < 0)
        ourQueenSpawn = head;
    if (L > lastLenSeen)
        lastEatR = R; // grew: ate something
    lastLenSeen = L;
    // Starving queen: she is (almost) the only eater and has not eaten for a
    // while. Her portal / unexplored / loop rules would leave her at length
    // 2-3 for ever (no split, no fleet), so with no enemy in view they relax.
    bool enemyInView = false;
    for (auto const& h : heads)
        if (!h.mine)
            enemyInView = true;
    // OPEN_PLAN: a queen with no teammate near her cannot wait for others to
    // find the pearls - she explores herself (the starving relaxations).
    bool queenAlone = false;
    if (OPEN_PLAN && OPEN_QUEEN && sit.openPlan && amQueen && !enemyInView && !sit.lateSafe)
    {
        queenAlone = true;
        for (auto const& h : heads)
            if (h.mine && h.id != me && manhattan(h.cell, body[0]) <= QUEEN_ISO_RAD)
                queenAlone = false;
        for (auto const& [id, st] : fleet)
            if (id != me && R - st.round <= 5 && manhattan(st.cell, body[0]) <= QUEEN_ISO_RAD)
                queenAlone = false;
    }
    queenStarving = (STARVE_RELAX && amQueen && units <= 2 && R >= 30 && R - lastEatR >= STARVE_ROUNDS && !enemyInView && !sit.lateSafe) || queenAlone;
    queenStarvingG = queenStarving;
    headHist.push_back(head);
    if (headHist.size() > 16)
        headHist.erase(headHist.begin());
    // First look: is the queen in a pocket that kelp closes off inside her
    // view, with no portal? A split child there could never get out, so she
    // keeps her length and just circles (she needs one free cell to move).
    if (QUEEN_SEALED_NOSPLIT && amQueen && !sealedChecked)
    {
        sealedChecked = true;
        bool closed = true;
        std::vector<int> reg{head};
        std::unordered_set<int> inReg{head};
        for (std::size_t i = 0; i < reg.size() && closed; i++)
            for (int d = 0; d < 4 && closed; d++)
            {
                std::uint8_t const et = edgeType[edge_code(reg[i], d)];
                if (et == E_KELP)
                    continue;
                if (et != E_OPEN)
                {
                    closed = false; // portal or unexplored edge: not sealed
                    break;
                }
                int const n = nb(reg[i], d);
                if (!vis[n])
                {
                    closed = false; // the pocket runs out of view
                    break;
                }
                if (inReg.insert(n).second)
                    reg.push_back(n);
            }
        queenSealed = closed;
        sealedCells = closed ? static_cast<int>(reg.size()) : 0;
    }
    sealedNoSplit = amQueen && queenSealed && L <= sealedCells - 1;
    if (ourQueenSpawn < 0 && R <= 1)
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                ourQueenSpawn = h.cell;
    if (HUNTERS && ourQueenSpawn >= 0 && (R % 20 == me % 20 || (amQueen && R < 5)))
        queue_fact(msg_qspawn(ourQueenSpawn), amQueen ? 60 : 40, 1);
    // Q2c: their queen within my next-turn strike reach - say so every turn (if I strike, I die and say nothing);
    // the turn she is out of reach again, say "off".
    if (STRIKE_NEWS && PAID_REGICIDE && !amQueen && !sit.enemyQueenDead)
    {
        int qc = -1;
        for (auto const& h : heads)
            if (!h.mine && h.id == enemyQueenId)
            {
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                int const herSteps = std::max(1, (std::max(h.len, h.estLen) + 3) / 4);
                if (manhattan(h.cell, head) <= reach + herSteps + 1)
                    qc = h.cell;
            }
        if (qc >= 0)
        {
            queue_fact(msg_strike(qc, false), 96, 2);
            strikeAnnR = R;
        }
        else if (strikeAnnR == R - 1)
            queue_fact(msg_strike(head, true), 96, 2);
    }
    // OPEN_PLAN zones: standing in a rich spot - tell the fleet.
    if (ZONES && !body.empty() && R % ZONE_MSG_EVERY == me % ZONE_MSG_EVERY)
    {
        int const t = spawn_tiles_near(body[0], RV_RAD);
        if (t >= ZONE_TILES)
            queue_fact(msg_zone(body[0], t), 45, 1);
    }
    if (EQ_DEATH_DETECT && sit.enemyQueenDead && R % (EQ_INFER ? 3 : 8) == me % (EQ_INFER ? 3 : 8))
        queue_fact(msg_queen_dead(true), EQ_INFER ? 85 : 55, EQ_INFER ? 2 : 1); // keep telling: relays are lossy (F2: the queen must hear it)
    if (EQ_DEATH_DETECT && sit.ourQueenDead && R % 8 == (me + 4) % 8)
        queue_fact(msg_queen_dead(false), 55, 1);

    if (amQueen && QUEEN_THREAT_MAP)
    {
        compute_queen_threat();
        for (int c = 0; c < N; c++)
            if (qThreatVis[c] == 0 && !(vis[c] && occId[c] >= 0))
                contested[c] = 1;
    }
    set_body(body, L);
    plan_bfs(head, distHead);
    return false;
}

// L4 candidate analysis: the four moves (legal, safe, area, survival, loop) and dead-end dives.
bool analyse_moves(Controller& ct)
{
    auto& head = tn.head;
    auto& cands = tn.cands;
    auto& cap = tn.cap;
    auto& anyLegal = tn.anyLegal;
    auto& anySafe = tn.anySafe;
    auto& attackAvail = tn.attackAvail;
    auto& attackDir = tn.attackDir;
    auto& attackLen = tn.attackLen;
    auto& diveMin = tn.diveMin;
    auto& diving = tn.diving;
    // ---- Candidate moves ------------------------------------------------
    cap = std::min(N, 3 * L + 24);
    anyLegal = false, anySafe = false, attackAvail = false;
    attackDir = -1, attackLen = 0;
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
        // B1b: a cell we can see with no dragon on it is always enterable (that is the engine's rule); our body
        // model can hold stale segments after a portal crossing, so vision decides for visible cells.
        bool const visiblyFree = LEGAL_BY_VISION && vis[dest] && occId[dest] < 0 && wallStamp[dest] != wstamp;
        if ((!passable(dest, 1) && !visiblyFree) || (vis[dest] && occId[dest] == me))
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
        cd.area = f1.area + (f1.frontier && !amQueen ? nl : 0); // the queen trusts only known water
        // A snake cannot turn round in a corridor, so raw area is only trusted
        // when there is room to circle; reaching our own tail is always an exit.
        bool const frontierExit = FRONTIER_OPEN && !my.leader; // small dragons may explore narrow passages
        cd.ok1 = f1.tail || cd.area >= 2 * nl + 4 || (frontierExit && f1.frontier);

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
            if (f2.tail || f2.area + (f2.frontier && !amQueen ? nl2 : 0) >= 2 * nl2 + 2 || (frontierExit && f2.frontier))
                cd.ok2 = true;
        }
        if (amQueen && cd.ok2 && !queen_can_survive(nb1, nl))
            cd.ok1 = cd.ok2 = false; // a corridor with no way back
        // Pocket check: a portal box or a small walled area can look roomy to
        // the flood (it counts water beyond the box's other portals, and our
        // tail as an exit). Can the body really keep moving in there?
        if (POCKET_CHECK && !amQueen && cd.ok2)
        {
            bool const viaPortal = dest != nb(head, d);
            bool const smallEnclosed = !f1.frontier && f1.area < 3 * nl;
            if ((viaPortal || smallEnclosed) && !queen_can_survive(nb1, nl, POCKET_NODE_CAP))
                cd.ok1 = cd.ok2 = false; // a trap - the dead-end dive logic decides if its pearls are worth it
        }
        // F7: a 1-wide dead end we can see to its bottom is a trap whatever
        // the flood says (a snake cannot turn round in it); the dive logic
        // below decides whether its pearls pay for the split.
        if (SLOT_CHECK && !amQueen && cd.ok1 && dest == nb(head, d) && known_dead_end(head, dest))
            cd.ok1 = cd.ok2 = false;
        // 7b: the same for a dead-end branch of any shape (a tree of corridors, a room with no loop longer than us).
        if (TOPO_TRAP && !amQueen && cd.ok1 && topo_trap(head, d, nl))
            cd.ok1 = cd.ok2 = false;
        if (QUEEN_LOOP_SAFE && amQueen)
            cd.noLoop = !loop_reachable(nb1, nl);
        if (cd.ok2)
            anySafe = true;
        cd.nb1 = std::move(nb1);
    }
    // Dead ends with pearls (non-queens): worth it when the pearls pay for
    // the survival split at the bottom (the user's compensation rule).
    double bestDive = -1e9;
    // Each dive node floods ~2L+4 cells, so the node budget shrinks with
    // length: a long dragon costs no more per candidate than a short one.
    diveCap = DIVE_NODE_CAP * 12 / std::max(12, 2 * L + 4);
    // Long dragons never mine dead ends (a giant in a pocket is a giant lost),
    // and their dive search is the costliest thing a turn can do.
    if (DIVE_ENABLE && !amQueen && L <= DIVE_MAX_LEN)
        for (auto& cd : cands)
            if (cd.legal && !cd.blind && !cd.ok1 && !cd.nb1.empty() && !contested[cd.dest])
            {
                diveNodes = 0;
                std::vector<int> ec;
                if (cd.eat)
                    ec.push_back(cd.dest);
                cd.dive = dive_rec(cd.nb1, L + (cd.eat ? 1 : 0), 1, cd.eat ? 1 : 0, ec);
                bestDive = std::max(bestDive, cd.dive);
            }
    // Pocket mining: with a teammate close by, a dive that only breaks even is
    // worth it - the stub left inside recycles into pearls for the teammate.
    bool mateNearHead = false;
    for (auto const& h : heads)
        if (h.mine && h.id != me && manhattan(h.cell, head) <= 5)
            mateNearHead = true;
    diveMin = mateNearHead && !amQueen ? DIVE_MATE_GAIN : DIVE_MIN_GAIN;
    diving = bestDive >= diveMin;
    // The queen with no safe move: still tell a corridor that leads on from a
    // known dead end (unexplored water counts as a way on here).
    if (QUEEN_SURV_RANK && amQueen && !anySafe)
        for (auto& cd : cands)
            if (cd.legal && !cd.blind && !cd.nb1.empty())
            {
                ignoreContested = false;
                std::vector<int> const& b1 = cd.nb1;
                set_body(b1, L + (cd.eat ? 1 : 0));
                pstamp++;
                survNodes = 0;
                pathStamp[b1[0]] = pstamp;
                pathTime[b1[0]] = 0;
                int const len1 = L + (cd.eat ? 1 : 0);
                cd.surv = survive_rec(b1[0], 0, std::min(2 * len1 + 6, 50), len1);
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
            if (cd.dive > -1e9)
                dbg += " dive" + std::to_string(cd.dive).substr(0, 4);
        }
        for (auto const& h : heads)
            dbg += " H" + std::to_string(h.id) + (h.mine ? "m@" : "e@") + std::to_string(h.cell % W) + "," + std::to_string(h.cell / W) + "l" + std::to_string(h.len);
        for (int tc : enemyTails)
            dbg += " T@" + std::to_string(tc % W) + "," + std::to_string(tc / W);
        ct.output_log(dbg);
    }
#endif
    return false;
}

// L4 contact: enemy heads next to us, stalkers of our queen, which head-on trades we want.
bool read_contact(Controller& ct)
{
    auto& head = tn.head;
    auto& cands = tn.cands;
    auto& attackAvail = tn.attackAvail;
    auto& attackDir = tn.attackDir;
    auto& attackLen = tn.attackLen;
    auto& nextToEnemy = tn.nextToEnemy;
    auto& queenAdjacent = tn.queenAdjacent;
    auto& allIn = tn.allIn;
    auto& qNow = tn.qNow;
    auto& stalkers = tn.stalkers;
    auto& wantAttack = tn.wantAttack;
    auto& freeN = tn.freeN;
    nextToEnemy = is_adjacent_enemy_head(head);
    queenAdjacent = false;
    for (auto const& cd : cands)
        if (cd.attack && occId[cd.dest] == enemyQueenId && !sit.enemyQueenDead)
        {
            queenAdjacent = true;
            attackDir = cd.d;
        }
    allIn = sit.ourQueenDead && !sit.enemyQueenDead; // we lose at round 500 unless their queen dies
    // v2.14 anti-escort: an enemy head that stays within 2 of our queen for a
    // few rounds is stalking her (runs beside her along a wall until she is
    // boxed). The queen reports it; any short teammate strikes it.
    qNow = -1;
    if (!sit.ourQueenDead)
    {
        if (amQueen)
            qNow = head;
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qNow = h.cell;
        auto const qf = fleet.find(queenId);
        if (qNow < 0 && qf != fleet.end() && R - qf->second.round <= 1)
            qNow = qf->second.cell;
    }
    if (ANTI_ESCORT && qNow >= 0)
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, qNow) <= 2)
            {
                auto& es = escortSeen[h.id];
                if (es.first != R)
                {
                    es.second = es.first == R - 1 ? es.second + 1 : 1;
                    es.first = R;
                }
                if (es.second >= ESCORT_ROUNDS)
                    stalkers.insert(h.id);
            }
    if (amQueen)
        for (auto const& h : heads)
            if (stalkers.count(h.id))
            {
                queue_fact(msg_danger(h.cell, 1), 88, 2);
                queue_fact(msg_enemy(h.cell, h.id, h.len), 86, 2);
            }
    int stalkerDir = -1;
    for (auto const& cd : cands)
        if (cd.attack && stalkers.count(occId[cd.dest]))
            stalkerDir = cd.d;
    // A head-on kills both dragons. While expanding, numbers are what we are
    // building, so only trade up: the enemy queen, a longer enemy, or when we
    // clearly outnumber them.
    bool const evenTrades = !(EXPANSION && sit.expansion);
    // v2.14: their dragons rarely attack ours, so a 1-for-1 only helps a swarm
    // that out-produces us. Trade up only (or with a clear majority).
    // Not outnumbered (by the scaled count): even trades as before (an
    // opponent that attacks our dragons must be answered).
    bool const tradeUp = !TRADE_UP || allIn || sit.majorityHunt || attackLen >= TRADE_UP_RATIO * L || (!sit.outnumbered && !sit.survival);
    wantAttack = attackAvail && !amQueen &&
                            ((queenAdjacent) || (stalkerDir >= 0 && !my.leader) ||
                             (!my.leader && tradeUp && (my.role == ROLE_COMBAT || sit.mode == MODE_COMBAT || allIn) &&
                              (evenTrades ? (L <= attackLen + 1 || units > est_enemy_units() + 1 || L <= 4 || allIn)
                                          : (attackLen > L || units > est_enemy_units() + 1 || allIn)) &&
                              // Swarms out-produce us: a 1-for-1 trade only pays while we are ahead.
                              (!SAFE_TRADES || allIn || (!sit.survival && (attackLen > L || !sit.outnumbered)))));
    if (!queenAdjacent && stalkerDir >= 0 && !my.leader)
        attackDir = stalkerDir;
    freeN = std::min(free_steps(), MAX_SPRINT);
    return false;
}

// Reflex: strike the visible enemy queen with free + paid steps.
bool rx_regicide_paid(Controller& ct)
{
    auto& head = tn.head;
    // ---- 0. Sprint into the enemy queen ("one smells blood") -----------------
    if (PAID_REGICIDE && !amQueen && !sit.enemyQueenDead)
        for (auto const& h : heads)
            if (!h.mine && h.id == enemyQueenId)
            {
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(h.cell, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(head, 1), 70, 2);
                    if (EQ_DEATH_DETECT) // (never cast: a dragon that dies in its move sends no sonar - Q2c covers it)
                        queue_fact(msg_queen_dead(true), 99, 4); // certain: she moved before us
                    return act_moves(ct, path, "regicide");
                }
            }
    return false;
}

// Reflex: strike a stalker running beside our queen.
bool rx_escort_hit(Controller& ct)
{
    auto& stalkers = tn.stalkers;
    if (ANTI_ESCORT && !amQueen && !my.leader && L <= 8 && !stalkers.empty())
        for (auto const& h : heads)
            if (!h.mine && stalkers.count(h.id))
            {
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(h.cell, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(h.cell, 1), 75, 2);
                    return act_moves(ct, path, "escort-hit");
                }
            }
    return false;
}

// Reflex: a short dragon strikes a longer enemy head (never trades down).
bool rx_snipe(Controller& ct)
{
    // Snipe: a short dragon that can put its head on an enemy head this turn
    // (free + paid steps) and is no longer than it strikes: a head-on kills
    // both, so it never trades down. Witnesses report the death drops.
    if (SNIPE && !amQueen && !my.leader && L <= SNIPE_MAX_LEN &&
        (!SAFE_TRADES || (!sit.survival && units >= est_enemy_units() + 4 && units >= 8)))
    {
        std::vector<HeadInfo const*> targets;
        for (auto const& h : heads)
            if (!h.mine && h.id != enemyQueenId && (sit.majorityHunt || (TRADE_UP && sit.outnumbered ? h.len >= TRADE_UP_RATIO * L : h.len >= L)))
                targets.push_back(&h);
        std::sort(targets.begin(), targets.end(), [](HeadInfo const* a, HeadInfo const* b) { return a->len > b->len; });
        int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
        for (auto const* h : targets)
        {
            std::vector<int> const path = regicide_path(h->cell, reach);
            set_body(body, L);
            if (!path.empty())
            {
                queue_fact(msg_danger(h->cell, 1), 70, 2);
                return act_moves(ct, path, "snipe");
            }
        }
    }
    return false;
}

// Reflex: pack members strike their prey (or any long enemy) in reach.
bool rx_pack_strike(Controller& ct)
{
    auto& allIn = tn.allIn;
    auto& packer = tn.packer;
    // Pack members: work out the shared prey, and strike a long enemy dragon
    // when its head is within reach (a short dragon for a long one).
    packer = HUNTERS && !amQueen && !my.leader && !my.guard && (my.hunter || my.wall || (ENDGAME_PACKS && sit.ourQueenDead && L <= 6)) &&
                        (allIn || !(SAFE_TRADES && (sit.survival || sit.outnumbered)));
    if (packer)
    {
        assign_prey();
        int const preyId = myPrey >= 0 ? preyList[myPrey].id : -1;
        for (auto const& h : heads)
            if (!h.mine && h.id != enemyQueenId && (h.id == preyId || h.len >= std::max(PREY_MIN_LEN, L + 4)))
            {
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(h.cell, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(h.cell, 1), 70, 2);
                    return act_moves(ct, path, "pack-strike");
                }
            }
    }
    return false;
}

// Reflex: free-step sprint into the enemy queen.
bool rx_regicide_sprint(Controller& ct)
{
    auto& head = tn.head;
    auto& queenAdjacent = tn.queenAdjacent;
    auto& freeN = tn.freeN;
    if (!amQueen && !sit.enemyQueenDead && !queenAdjacent)
    {
        bool near = false;
        for (auto const& h : heads)
            if (!h.mine && h.id == enemyQueenId && manhattan(h.cell, head) <= std::min(freeN + 1, 3))
                near = true;
        if (near)
        {
            SprintPlan kp;
            std::vector<int> dirs, eatenCells;
            sprint_dfs(body, L, 0, std::min(freeN + 1, 3), freeN, 0, dirs, eatenCells, false, 0, kp);
            set_body(body, L);
            if (kp.kill)
            {
                queue_fact(msg_danger(head, 1), 70, 2);
                if (EQ_DEATH_DETECT)
                    queue_fact(msg_queen_dead(true), 99, 4);
                return act_moves(ct, kp.dirs, "regicide");
            }
        }
    }
    return false;
}

// Reflex (STALKER_INTERCEPT, off): strike heads that threaten our queen.
bool rx_intercept(Controller& ct)
{
    // Short teammates strike enemy heads that have our queen within their
    // reach (one of ours for one attacker; the queen moves first and cannot
    // dodge a sprint).
    if (STALKER_INTERCEPT && !amQueen && !my.leader && !sit.ourQueenDead && L <= 6)
    {
        int qc = -1;
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qc = h.cell;
        auto const q = fleet.find(queenId);
        if (qc < 0 && q != fleet.end() && R - q->second.round <= 2)
            qc = q->second.cell;
        if (qc >= 0)
            for (auto const& h : heads)
            {
                if (h.mine)
                    continue;
                int const el = std::max(h.len, h.estLen);
                if (manhattan(h.cell, qc) > (el + 3) / 4 + std::max(0, el - 2) + 1)
                    continue;
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(h.cell, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(h.cell, 1), 75, 2);
                    return act_moves(ct, path, "intercept");
                }
            }
    }
    return false;
}

// Reflex (QUEEN_PAID_ESCAPE, off): the queen sprints out of every reach.
bool rx_queen_paid_escape(Controller& ct)
{
    auto& cands = tn.cands;
    auto& anyLegal = tn.anyLegal;
    // The queen: every single step ends inside some enemy's reach. Sprint out
    // with free steps, paying segments if needed (length is cheaper than her).
    if (QUEEN_PAID_ESCAPE && QUEEN_HARD_THREAT && QUEEN_THREAT_MAP && amQueen && anyLegal)
    {
        double bestSingle = 1e18;
        for (auto const& cd : cands)
            if (cd.legal && !cd.blind && cd.dest >= 0)
                bestSingle = std::min(bestSingle, queen_threat_pen(cd.dest));
        if (bestSingle >= 0.5 * HARD_THREAT_PEN)
        {
            SprintPlan ep;
            std::vector<int> dirs, eatenCells;
            int const steps = std::min(4, free_steps() + std::max(0, L - 2));
            if (steps >= 2)
                sprint_dfs(body, L, 0, steps, free_steps(), 0, dirs, eatenCells, false, 0, ep);
            set_body(body, L);
            if (!ep.dirs.empty() && ep.value > -0.5 * HARD_THREAT_PEN)
            {
                int el = 0;
                std::vector<int> const endBody = simulate_steps(body, L, ep.dirs, el);
                bool const ok = !endBody.empty() && queen_can_survive(endBody, el) &&
                                !(QUEEN_LOOP_SAFE && !loop_reachable(endBody, el));
                set_body(body, L);
                if (ok)
                    return act_moves(ct, ep.dirs, "q-escape");
            }
        }
    }
    return false;
}

// Reflex: no legal move - unwind split, queen stall, last attack, save split, or die harmlessly.
bool rx_no_legal_move(Controller& ct)
{
    auto& head = tn.head;
    auto& facing = tn.facing;
    auto& anyLegal = tn.anyLegal;
    auto& attackAvail = tn.attackAvail;
    auto& attackDir = tn.attackDir;
    auto& nextToEnemy = tn.nextToEnemy;
    // ---- 1. Death is certain: survival split (sections 3, 12) --------------
    if (!anyLegal)
    {
        // Boxed in by our own body: split so that the blocking segment becomes
        // the child's tail. The child moves later this round, which frees that
        // cell, and the front part keeps as many segments as possible.
        if (static_cast<int>(body.size()) >= L)
        {
            int bestKeep = -1;
            for (int d = 0; d < 4; d++)
            {
                int const n = cross(head, d);
                if (n < 0)
                    continue;
                for (int i = 2; i <= L - 2; i++)
                    if (body[i] == n && i > bestKeep)
                        bestKeep = i;
            }
            if (bestKeep >= 2 && split_allowed(ct, L - bestKeep) && (!amQueen || bestKeep >= L - 2 || bestKeep >= 4 || L < 6))
                return act_split(ct, L - bestKeep, "unwind-split");
        }
        // Stalling only helps if something around the head will move away:
        // another dragon, or our own last two segments (they leave as the child).
        bool willClear = false;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(head, d);
            if (n < 0)
                continue;
            if (otherRel[n])
                willClear = true;
            for (int i = std::max(0, static_cast<int>(body.size()) - 2); i < static_cast<int>(body.size()); i++)
                if (body[i] == n && static_cast<int>(body.size()) >= L)
                    willClear = true;
        }
        if (amQueen && willClear && L >= 4 && split_allowed(ct, 2))
        {
            // Splitting is not a move: the queen keeps its head, survives this
            // turn and keeps all but 2 segments (the child takes the tail).
            queue_fact(msg_danger(head, nextToEnemy ? 1 : 2), 80, 3);
            return act_split(ct, 2, "queen-stall");
        }
        if (attackAvail && !my.leader && L < 6 && !(QUEEN_NO_SUICIDE && amQueen && L >= 4 && split_allowed(ct, L - 2)))
            return act_move(ct, attackDir, "last-attack");
        if (L >= 4 && split_allowed(ct, L - 2))
        {
            queue_fact(msg_danger(head, nextToEnemy ? 1 : 2), 80, 3);
            return act_split(ct, L - 2, "save-split");
        }
        if (attackAvail)
            return act_move(ct, attackDir, "last-attack");
        // Die without taking a teammate along: never into a friendly head
        // (least of all the queen's); kelp, bodies or our own neck are harmless.
        int deathDir = -1, deathRank = 99;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(head, d);
            int rank = 2;
            if (n >= 0 && vis[n] && occId[n] >= 0 && occHead[n] && occMine[n])
                rank = occId[n] == queenId ? 9 : 8;
            else if (n == -3 || (n >= 0 && !vis[n]))
                rank = 0; // unknown far side: might even survive
            else if (n >= 0 && occId[n] >= 0 && !occMine[n])
                rank = 1; // enemy body
            if (rank < deathRank)
            {
                deathRank = rank;
                deathDir = d;
            }
        }
        return act_move(ct, deathDir >= 0 ? deathDir : facing, "doomed");
    }
    return false;
}

// Reflex: no safe single step - sprint to safety.
bool rx_escape_sprint(Controller& ct)
{
    auto& anySafe = tn.anySafe;
    auto& diving = tn.diving;
    auto& freeN = tn.freeN;
    if (!anySafe && !diving && (freeN >= 2 || L >= 5))
    {
        SprintPlan ep;
        std::vector<int> dirs, eatenCells;
        sprint_dfs(body, L, 0, std::min(freeN + (L >= 5 ? 1 : 0), 3), freeN, 0, dirs, eatenCells, false, 0, ep);
        set_body(body, L);
        bool escOk = ep.kill || ep.value > (amQueen && QUEEN_THREAT_MAP ? -1e6 : QUEEN_EXIT_GUARD ? -0.5 * QUEEN_EXIT_PEN : -1e17);
        if (escOk && amQueen && QUEEN_LOOP_SAFE && !ep.kill && !ep.dirs.empty())
        {
            int el = 0;
            std::vector<int> const endBody = simulate_steps(body, L, ep.dirs, el);
            if (!endBody.empty() && !loop_reachable(endBody, el))
                escOk = false; // an escape into a pocket only delays it
            set_body(body, L);
        }
        if (escOk)
            return act_moves(ct, ep.dirs, ep.kill ? "regicide" : "escape");
    }
    return false;
}

// Reflex: trapped - survival split (a pearl one step away is eaten first).
bool rx_trap_split(Controller& ct)
{
    auto& head = tn.head;
    auto& cands = tn.cands;
    auto& anySafe = tn.anySafe;
    auto& diving = tn.diving;
    auto& nextToEnemy = tn.nextToEnemy;
    if (!anySafe && !diving && !amQueen && L >= 4 && split_allowed(ct, L - 2) && child_can_escape(L - 2))
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
    return false;
}

// Reflex: the queen is about to be struck - her length lives on in a child.
bool rx_insure_split(Controller& ct)
{
    auto& head = tn.head;
    auto& cands = tn.cands;
    auto& anyLegal = tn.anyLegal;
    // Insurance: the queen is about to be struck - every move ends inside an
    // enemy's reach and a visible enemy head is within its free steps of
    // her. She splits off all but 2: if she dies, the length lives on in the
    // child (the tail end, away from the striker) instead of vanishing.
    if (QUEEN_INSURE && !QUEEN_NO_SUICIDE && amQueen && anyLegal && QUEEN_THREAT_MAP && L >= QUEEN_INSURE_LEN && static_cast<int>(body.size()) >= L)
    {
        bool allHit = true, closeStriker = false;
        for (auto const& cd : cands)
            if (cd.legal && (cd.blind || cd.dest < 0 || qThreat[cd.dest] >= INF))
                allHit = false;
        for (auto const& h : heads)
            if (!h.mine)
            {
                int const el = std::max(h.len, h.estLen);
                if (manhattan(h.cell, head) <= (el + 3) / 4 + 1)
                    closeStriker = true;
            }
        if (allHit && closeStriker && split_allowed(ct, L - 2) && child_can_escape(L - 2))
        {
            queue_fact(msg_danger(head, 1), 80, 3);
            return act_split(ct, L - 2, "insure-split");
        }
    }
    return false;
}

// Reflex: boxed in for good next to a teammate - become pearls for it.
bool rx_recycle(Controller& ct)
{
    auto& head = tn.head;
    auto& cands = tn.cands;
    auto& anySafe = tn.anySafe;
    auto& diving = tn.diving;
    // Recycle: boxed in for good (every move leaves less room than our own
    // length) with a teammate close by. The drop is the same either way;
    // dying now, next to the teammate, means it actually gets eaten.
    if (RECYCLE && !anySafe && !diving && !amQueen && !my.leader && L >= 3 && body.size() >= 2 && (!SURV_ECON || units >= RECYCLE_MIN_UNITS))
    {
        bool boxed = true, pearlFirst = false;
        for (auto const& cd : cands)
        {
            if (!cd.legal)
                continue;
            if (cd.blind || cd.ok1 || cd.area >= L)
                boxed = false;
            pearlFirst |= cd.eat;
        }
        bool mateNear = false;
        for (auto const& h : heads)
            if (h.mine && h.id != me)
                for (int i = 0; i < L && i < static_cast<int>(body.size()); i++)
                    if (manhattan(h.cell, body[i]) <= RECYCLE_MATE_DIST)
                        mateNear = true;
        if (boxed && !pearlFirst && mateNear)
            for (int d = 0; d < 4; d++)
                if (nb(head, d) == body[1])
                {
                    queue_fact(msg_danger(head, 2), 60, 1);
                    return act_move(ct, d, "recycle");
                }
    }
    return false;
}

// Reflex: a wanted head-on attack.
bool rx_attack(Controller& ct)
{
    auto& head = tn.head;
    auto& attackDir = tn.attackDir;
    auto& wantAttack = tn.wantAttack;
    // ---- 2. Head-on attack (sections 15, 16) ------------------------------
    if (wantAttack)
    {
        queue_fact(msg_danger(head, 1), 70, 2);
        return act_move(ct, attackDir, "attack");
    }
    return false;
}

// Target: guards infer the queen's bunker box from vision; cap feeding.
bool tg_bunker_watch(Controller& ct)
{
    auto& head = tn.head;
    auto& qBox = tn.qBox;
    auto& bunkerFresh = tn.bunkerFresh;
    auto& postDir = tn.postDir;
    auto& capFeed = tn.capFeed;
    // ---- Target ----------------------------------------------------------
    // Cap feeding: once the fleet is at its cap, more pearls cannot become
    // dragons, so the spare ones go to the queen - only while she is safe
    // (known position, no enemy reported near her lately) and we are not in
    // survival mode. Self-throttling: a feed drops the fleet below the cap.
    // Sonar rarely reaches a guard, so a guard that sees her also works it out:
    // late in the game (or once a bunker is likely), a length-3 queen whose
    // whole body sits inside one 2x2 box two rounds running is bunkered there.
    if (BUNKER_GUARDS && !amQueen && !sit.ourQueenDead && (sit.lateSafe || sit.enemyQueenDead || R >= TIE_BUNKER_ROUND))
        for (auto const& h : heads)
            if (h.mine && h.id == queenId && h.len == 3)
            {
                std::vector<int> qcells;
                for (int c = 0; c < N; c++)
                    if (vis[c] && occId[c] == queenId)
                        qcells.push_back(c);
                if (static_cast<int>(qcells.size()) != 3)
                    break;
                int const tls[4] = {h.cell, nb(h.cell, 3), nb(h.cell, 0), nb(nb(h.cell, 3), 0)};
                std::array<int, 4> bx{};
                for (int tl : tls)
                {
                    if (!box_cells(tl, bx))
                        continue;
                    bool all = true;
                    for (int c : qcells)
                        if (std::find(bx.begin(), bx.end(), c) == bx.end())
                            all = false;
                    if (!all)
                        continue;
                    if (inferTl == tl && inferR == R - 1)
                    {
                        qBunkerTl = tl;
                        qBunkerR = R;
                    }
                    inferTl = tl;
                    inferR = R;
                    break;
                }
            }
    // The queen is bunkered (announced or seen within the last 5 rounds): her box.
    bunkerFresh = BUNKER_GUARDS && !amQueen && !sit.ourQueenDead && qBunkerTl >= 0 && R - qBunkerR <= 5 && box_cells(qBunkerTl, qBox);
    postDir = -1;
    capFeed = false;
    // Open maps only: on crowded maps the feeders' bodies wall her in.
    if (CAP_FEED && sit.openMap && !sit.ourQueenDead && !sit.survival && R >= CAP_FEED_ROUND && !room_for_more())
    {
        int qk = amQueen ? head : -1;
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qk = h.cell;
        auto const qf = fleet.find(queenId);
        if (qk < 0 && qf != fleet.end() && R - qf->second.round <= 30)
            qk = qf->second.cell;
        if (qk >= 0)
        {
            capFeed = !queen_fed_enough();
            for (auto const& e : enemyReps)
                if (R - e.round <= 10 && manhattan(e.cell, qk) <= 10)
                    capFeed = false;
        }
    }
    return false;
}

// Target (economy): the matched pearl, with commitment.
bool tg_pearl(Controller& ct)
{
    auto& pearls = tn.pearls;
    auto& target = tn.target;
    auto& portalDir = tn.portalDir;
    auto& why = tn.why;
    auto& pi = tn.pi;
    pearls = gather_pearls();
    target = -1;
    portalDir = -1;
    why = I_EXPLORE;
    pi = choose_pearl(pearls);
    // Commitment: switching pearls every other turn wastes the moves made so
    // far (in a maze the two pearls are often in different corridors).
    if (TARGET_COMMIT && lastPearl >= 0 && (!amQueen || QUEEN_COMMIT))
    {
        int prevIdx = -1;
        for (int k = 0; k < static_cast<int>(pearls.size()); k++)
            if (pearls[k].cell == lastPearl)
                prevIdx = k;
        if (prevIdx >= 0 && prevIdx != pi && distHead[lastPearl] < INF)
        {
            int const dPrev = distHead[lastPearl];
            int const dNew = pi >= 0 ? distHead[pearls[pi].cell] : INF;
            bool const clearlyBetter = pi >= 0 && dNew + COMMIT_MARGIN <= dPrev && dNew * 10 <= dPrev * 7;
            if (!clearlyBetter)
                pi = prevIdx;
        }
    }
    if (pi >= 0)
    {
        target = pearls[pi].cell;
        claimTarget = target;
        claimEta = distHead[target];
        why = I_PEARL;
    }
    return false;
}

// Target (queen): late / lead safety, evasion, flight from a crowd.
bool tg_queen_caution(Controller& ct)
{
    auto& head = tn.head;
    auto& target = tn.target;
    auto& why = tn.why;
    // Last rounds: a living queen beats any pearl (their queen is often dead
    // already and we cannot know it). Only pearls right next to her that no
    // enemy can reach; otherwise she keeps to the ground she is on.
    // v2.14 lead safe: ahead on queen length from LEAD_ROUND (their queens are
    // short and hidden), so a pearl trip can only lose the game.
    bool leadSafe = false;
    if (LEAD_SAFE && amQueen && !sit.ourQueenDead && !sit.enemyQueenDead && R >= LEAD_ROUND)
    {
        TieState const lt = tie_state();
        leadSafe = (lt.theirQ >= 0 && L > lt.theirQ) || (lt.theirQ < 0 && L >= LEAD_MIN_LEN);
    }
    if (amQueen && (sit.lateSafe || leadSafe) && target >= 0)
    {
        bool risky = distHead[target] > 3 || (QUEEN_THREAT_MAP && qThreat[target] < INF);
        for (auto const& e : enemyReps)
            if (R - e.round <= 10 && manhattan(e.cell, target) <= 8)
                risky = true;
        if (FEED_FIX && QUEEN_THREAT_MAP && pearl_at(target) && distHead[target] <= FEED_EAT_DIST && qThreat[target] >= INF)
            risky = false; // a drop right by her that no enemy can reach this turn
        if (risky)
        {
            target = -1;
            claimTarget = -1;
            why = I_LATE_SAFE;
        }
    }
    if (QUEEN_EVADE && QUEEN_HARD_THREAT && amQueen && target >= 0 && (qThreat[target] < INF || qMargin[target] <= 1))
    {
        target = -1;
        claimTarget = -1;
        why = I_EVADE;
    }
    // F5: a crowd of enemy heads round the queen (swarm maps): no pearl, no
    // rest - walk to the safest open water nearby, away from them and toward
    // our own dragons.
    if (QUEEN_CROWD > 0 && amQueen && !sit.ourQueenDead)
    {
        int crowd = 0;
        std::unordered_set<int> counted;
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, head) <= CROWD_RAD)
            {
                crowd++;
                counted.insert(h.id);
            }
        for (auto const& e : enemyReps)
            if (R - e.round <= 2 && !counted.count(e.id) && manhattan(e.cell, head) <= CROWD_RAD)
            {
                crowd++;
                counted.insert(e.id);
            }
        if (crowd >= QUEEN_CROWD)
        {
            int const f = choose_flee();
            if (f >= 0)
            {
                target = f;
                claimTarget = -1;
                why = I_FLEE;
            }
        }
    }
    return false;
}

// Target: combat dragons and majority hunters chase enemies.
bool tg_hunt(Controller& ct)
{
    auto& head = tn.head;
    auto& allIn = tn.allIn;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& hunter = tn.hunter;
    auto& huntOk = tn.huntOk;
    // Combat dragons hunt enemies, preferring the ones near our leaders. When
    // we clearly outnumber them, small dragons also hunt the long enemies.
    bool const outnumber = units > sit.enemyEst + 3 && sit.distinctEnemies > 0;
    hunter = !my.leader && ((sit.mode == MODE_COMBAT && my.role == ROLE_COMBAT) || (outnumber && L <= 5));
    // v2.12 gate, also for this chase: never while outnumbered or in survival mode.
    huntOk = allIn || !(HUNT_GATE && (sit.survival || sit.outnumbered));
    // Majority on a smaller map: every spare dragon hunts, any enemy will do.
    bool const majHunter = sit.majorityHunt && !amQueen && !my.leader && !my.guard;
    if ((hunter && huntOk) || majHunter ? (target < 0 || distHead[target] > (majHunter ? kLeash.majHunt : kLeash.hunt)) : false)
    {
        int best = -1, bestD = majHunter ? 24 : 16;
        for (auto const& e : enemyReps)
        {
            if (R - e.round > 3)
                continue;
            if (!majHunter && (TRADE_UP && sit.outnumbered ? e.len < std::max(6, TRADE_UP_RATIO * L) : (!(sit.mode == MODE_COMBAT && my.role == ROLE_COMBAT) && e.len < 6)))
                continue;
            int dd = dist_to(distHead, e.cell);
            if (majHunter && dd < INF)
            {
                // One hunter per enemy: leave it to a teammate that is nearer
                // (several converging on one target run into each other).
                bool nearerMate = false;
                for (auto const& h : heads)
                    if (h.mine && h.id != me && h.id != queenId && manhattan(h.cell, e.cell) < manhattan(head, e.cell))
                        nearerMate = true;
                for (auto const& [id, st] : fleet)
                    if (id != me && id != queenId && R - st.round <= 2 && manhattan(st.cell, e.cell) + 1 < manhattan(head, e.cell))
                        nearerMate = true;
                if (nearerMate)
                    continue;
            }
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
            why = I_HUNT;
        }
    }
    return false;
}

// Target: chase the enemy queen when she is close.
bool tg_hunt_queen(Controller& ct)
{
    auto& allIn = tn.allIn;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& hunter = tn.hunter;
    auto& huntOk = tn.huntOk;
    // The enemy queen decides the game: chase her when she is close, always
    // for combat dragons, and with everyone once our own queen is gone.
    if (!amQueen && !sit.enemyQueenDead && (!my.leader || allIn))
    {
        int qc = -1, qround = -1;
        for (auto const& e : enemyReps)
            if (e.id == enemyQueenId && R - e.round <= 4 && e.round > qround)
            {
                qc = e.cell;
                qround = e.round;
            }
        if (qc >= 0)
        {
            int const dq = dist_to(distHead, qc);
            int const range = allIn ? 60 : HUNT_QUEEN_RANGE;
            if (dq <= range && (dq <= 6 || allIn || (huntOk && R >= OPEN_FARM_ROUND && (my.role == ROLE_COMBAT || hunter))))
            {
                target = qc;
                why = I_HUNT_QUEEN;
            }
        }
    }
    return false;
}

// Target (7g): cork - their queen is in a corridor with one way out to open water: take a cell of that corridor
// before she can leave it (our body then seals her in; any head-on with her is a trade we always want).
bool tg_cork(Controller& ct)
{
    auto& head = tn.head;
    auto& target = tn.target;
    auto& why = tn.why;
    if (!CORK || amQueen || my.leader || sit.enemyQueenDead)
        return false;
    int e = -1, eLen = 0;
    for (auto const& h : heads)
        if (!h.mine && h.id == enemyQueenId)
        {
            e = h.cell;
            eLen = std::max(h.len, h.estLen);
        }
    if (e >= 0)
        DIAG("cork-queen-seen");
    if (e < 0 || topo_room(e, eLen))
        return false; // not seen, or she can turn round where she is
    DIAG("cork-queen-in-corridor");
    int exitDir = -1, exits = 0;
    for (int d = 0; d < 4; d++)
    {
        int const n = cross(e, d);
        if (n < 0 || (vis[n] && occId[n] >= 0))
            continue;
        Branch b;
        bool const bridge = topo_beyond(e, d, b);
        if (!bridge || b.frontier || b.maxCap > eLen)
        {
            exits++;
            exitDir = d;
        }
    }
    if (exits != 1)
        return false;
    // Walk the corridor out from her head; the first cell we reach a turn sooner than she can is the cork.
    int const herSpeed = std::max(1, (eLen + 3) / 4), mySpeed = std::max(1, free_steps());
    int prev = e, c = cross(e, exitDir), best = -1;
    for (int k = 1; k <= CORK_RANGE && c >= 0; k++)
    {
        if (distHead[c] < INF && (distHead[c] + mySpeed - 1) / mySpeed + 1 <= (k + herSpeed - 1) / herSpeed)
        {
            best = c;
            break;
        }
        int next = -1, ways = 0;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n >= 0 && n != prev)
            {
                ways++;
                next = n;
            }
        }
        if (ways != 1)
            break; // the corridor ends: open water, no cork
        prev = c;
        c = next;
    }
    if (best < 0 || best == head)
        return false;
    DIAG("cork-target");
    target = best;
    why = I_CORK;
    return false;
}

// Target: feeders walk to the queen / giant.
bool tg_feed(Controller& ct)
{
    auto& head = tn.head;
    auto& bunkerFresh = tn.bunkerFresh;
    auto& capFeed = tn.capFeed;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& feedId = tn.feedId;
    auto& feedLeader = tn.feedLeader;
    auto& earlyFeed = tn.earlyFeed;
    auto& feedPhase = tn.feedPhase;
    // Farming endgame (section 13): feeders walk to our queen (or, without
    // her, to the nearest leader).
    feedLeader = -1, feedId = -1;
    // When feeding starts: open maps turn numbers into length from
    // CONSOL_ROUND; behind on queen length we start earlier (sooner still on
    // open maps); otherwise only the last FEED_WINDOW rounds.
    TieState const ts = TIE_TRACK ? tie_state() : TieState{};
    bool const behindQ = TIE_TRACK && !sit.ourQueenDead && !sit.enemyQueenDead &&
                         ((ts.ourQ >= 0 && ts.theirQ >= 0 && ts.ourQ <= ts.theirQ) ||
                          (QUEEN_HOME && R >= LEAD_ROUND && ts.ourQ <= 3) ||
                          (FEED_FIX && R >= LEAD_ROUND && ts.theirQ < 0 && ts.ourQ >= 0 && ts.ourQ < FEED_UNKNOWN_GOAL)); // v2.14: a home queen eats little (unknown counts as short)
    int feedStart = sit.consolidate ? (DYN_ALLOC ? sit.switchRound : CONSOL_ROUND) : 500 - FEED_WINDOW;
    if (behindQ)
        feedStart = std::min(feedStart, sit.openMap ? FEED_BEHIND_OPEN : FEED_BEHIND_CONGESTED);
    if (capFeed)
        feedStart = std::min(feedStart, R); // the fleet is full: its spare pearls go to the queen
    earlyFeed = R < 500 - FEED_WINDOW; // guards, hunters and defenders keep their jobs until the last rounds
    // A length-2 dragon cannot feed (it would leave one pearl and could not
    // even squeeze into its neck usefully): it would only crowd her.
    feedPhase = !my.leader && L >= 3 && R >= feedStart && sit.mode != MODE_COMBAT && sit.localThreat == 0 &&
                           !(my.guard && bunkerFresh) && // her posts come first
                           !(sit.majorityHunt && earlyFeed) && // winning on numbers: keep hunting until the last rounds
                           !(earlyFeed && ((my.guard && !(DYN_ALLOC && sit.switched && !my.screen)) || my.hunter || my.wall || my.defender)) && !(FEED_ONLY_IF_CONTEST && sit.enemyQueenDead && !sit.ourQueenDead);
    if (feedPhase)
    {
        int bestD = INF, longestId = -1, longestLen = -1;
        // Giant: on open maps a share of the feeders grows it too (it wins
        // the longest-dragon tiebreak if the queens end up dead or tied).
        int const gid = GIANT && (sit.consolidate || (GIANT_ALL && R >= GIANT_ROUND)) && !sit.ourQueenDead && (me % GIANT_SHARE == 0) ? giant_id() : -1;
        if (gid >= 0 && gid != me)
        {
            for (auto const& h : heads)
                if (h.mine && h.id == gid)
                {
                    feedLeader = h.cell;
                    feedId = gid;
                }
            auto const g = fleet.find(gid);
            if (feedLeader < 0 && g != fleet.end() && R - g->second.round <= 30)
            {
                feedLeader = g->second.cell;
                feedId = gid;
            }
        }
        if (sit.ourQueenDead && R >= 500 - LATE_LONGEST_ONLY)
            for (auto const& [id, s] : fleet)
                if (s.leader && R - s.round <= 5 && s.len > longestLen)
                {
                    longestLen = s.len;
                    longestId = id;
                }
        // A queen long enough to win on length gets no more: the extra length
        // only makes her harder to keep alive. Late on open maps the giant
        // takes it instead; otherwise the dragon keeps its length.
        bool const queenFull = !sit.ourQueenDead && queen_fed_enough();
        if (queenFull && feedId < 0 && sit.consolidate)
        {
            int const g2 = giant_id();
            auto const g = fleet.find(g2);
            if (g2 >= 0 && g2 != me && g != fleet.end() && R - g->second.round <= 30)
            {
                feedLeader = g->second.cell;
                feedId = g2;
            }
        }
        for (auto const& [id, s] : fleet)
        {
            if (feedId >= 0 || queenFull)
                break; // feeding the giant, or nobody
            bool const ok = sit.ourQueenDead ? s.leader && (longestId < 0 || id == longestId) : id == queenId;
            if (ok && R - s.round <= (DYN_ALLOC && sit.switched ? 100 : sit.consolidate || capFeed ? 30 : 5) && manhattan(s.cell, head) < bestD)
            {
                bestD = manhattan(s.cell, head);
                feedLeader = s.cell;
                feedId = id;
            }
        }
        for (auto const& h : heads)
            if (h.mine && !queenFull && !(gid >= 0 && feedId == gid) && (sit.ourQueenDead ? (fleet.count(h.id) && fleet[h.id].leader && (longestId < 0 || h.id == longestId)) : h.id == queenId))
            {
                feedLeader = h.cell;
                feedId = h.id;
            }
        // v2.14: she keeps to her home, so without a fresh report go there and
        // look (the sacrifice itself waits until she is in view).
        if (QUEEN_HOME && feedLeader < 0 && feedId < 0 && !sit.ourQueenDead && !queenFull && ourQueenSpawn >= 0 && distHead[ourQueenSpawn] < INF)
        {
            feedLeader = ourQueenSpawn;
            feedId = queenId;
        }
        // Open map, nowhere to find her: the longest leader in view grows instead.
        if (sit.consolidate && feedLeader < 0)
        {
            int bestLen = 0;
            for (auto const& h : heads)
                if (h.mine && h.id != me && fleet.count(h.id) && fleet[h.id].leader && h.len > bestLen)
                {
                    bestLen = h.len;
                    feedLeader = h.cell;
                    feedId = h.id;
                }
        }
        // Queue: a swarm of feeders walls her in, so with enough of them
        // around her already the rest wait their turn further out.
        int crowd = 0;
        if (feedLeader >= 0)
            for (auto const& h : heads)
                if (h.mine && h.id != me && h.id != feedId && manhattan(h.cell, feedLeader) <= 5)
                    crowd++;
        bool const waitTurn = feedLeader >= 0 && crowd >= FEED_CROWD && manhattan(head, feedLeader) > 4;
        if (feedLeader >= 0 && !waitTurn && (target < 0 || distHead[target] > kLeash.feed))
        {
            target = feedLeader;
            why = I_FEED;
        }
    }
    return false;
}

// Target: pack prey, lurking, regrouping.
bool tg_pack(Controller& ct)
{
    auto& head = tn.head;
    auto& packer = tn.packer;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& feedPhase = tn.feedPhase;
    // Packs: go for the shared prey; the first one there waits a little for
    // a mate so the pack arrives together (alone, it just goes in).
    if (packer && !feedPhase && (L >= HUNTER_LEN || sit.ourQueenDead || my.wall) && myPrey >= 0 &&
        (target < 0 || why != I_PEARL || distHead[target] > kLeash.pack))
    {
        Prey const& pr = preyList[myPrey];
        if (dist_to(distHead, pr.cell) < INF)
        {
            target = pr.cell;
            why = pr.queen ? I_HUNTER : I_PACK;
            bool preyInView = false;
            for (auto const& h : heads)
                if (!h.mine && h.id == pr.id)
                    preyInView = true;
            // Lurk: her position is fresh but she is out of our view. Wait just
            // outside her 7x7 view, each pack member on its own side; the
            // moment she steps toward one she is in its view and reach, and the
            // regicide sprint fires. Two sides: she cannot step away from both.
            int lurkCell = -1;
            if (LURK && pr.queen && !preyInView)
            {
                int qc = -1, qr = -1;
                for (auto const& e : enemyReps)
                    if (e.id == enemyQueenId && e.round > qr)
                    {
                        qr = e.round;
                        qc = e.cell;
                    }
                if (qc >= 0 && R - qr <= LURK_FRESH)
                {
                    static int const off[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                    int const r = myPackRank % 4;
                    int const ix = ((qc % W + off[r][0] * LURK_DIST) % W + W) % W;
                    int const iy = ((qc / W + off[r][1] * LURK_DIST) % H + H) % H;
                    int const ideal = cell_of(ix, iy);
                    int bestScore = INF;
                    for (int dy = -2; dy <= 2; dy++)
                        for (int dx = -2 + std::abs(dy); dx <= 2 - std::abs(dy); dx++)
                        {
                            int const c = cell_of(((ix + dx) % W + W) % W, ((iy + dy) % H + H) % H);
                            if (distHead[c] >= INF || cheb(c, qc) < LURK_DIST)
                                continue;
                            int const sc = distHead[c] + 2 * manhattan(c, ideal);
                            if (sc < bestScore)
                            {
                                bestScore = sc;
                                lurkCell = c;
                            }
                        }
                }
            }
            if (lurkCell >= 0)
            {
                target = lurkCell;
                why = I_LURK;
            }
            else if (PACK_REGROUP > 0 && myPackSize >= 2 && !preyInView && manhattan(head, pr.cell) <= PACK_REGROUP &&
                     mates_near(pr.cell, PACK_REGROUP + 3) == 0)
            {
                target = -1;
                why = I_REGROUP;
            }
        }
    }
    return false;
}

// Target: the wall line.
bool tg_wall(Controller& ct)
{
    auto& packer = tn.packer;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& feedPhase = tn.feedPhase;
    // P8 wall: no prey known - the wall advances on their home, side by side.
    if (my.wall && packer && !feedPhase && myPrey < 0 && (target < 0 || why != I_PEARL || distHead[target] > kLeash.wall))
    {
        int const g = wall_goal();
        if (g >= 0)
        {
            target = g;
            why = I_WALL;
        }
    }
    return false;
}

// Target: guards intercept, post round the bunker, screen, or stay near the queen.
bool tg_guard(Controller& ct)
{
    auto& head = tn.head;
    auto& qBox = tn.qBox;
    auto& bunkerFresh = tn.bunkerFresh;
    auto& postDir = tn.postDir;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& feedPhase = tn.feedPhase;
    auto& guardQ = tn.guardQ;
    // Guards: intercept enemies closing on the queen, otherwise screen her
    // from a few tiles out (crowding her walls her in).
    guardQ = -1;
    if (my.guard && !feedPhase)
    {
        int qCellG = -1;
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qCellG = h.cell;
        auto const q = fleet.find(queenId);
        if (qCellG < 0 && q != fleet.end() && R - q->second.round <= (my.screen ? 60 : 3))
            qCellG = q->second.cell;
        if (qCellG < 0 && bunkerFresh)
            qCellG = qBunkerTl; // she is in her announced box
        if (qCellG < 0 && BUNKER_GUARDS && sit.lateSafe && q != fleet.end() && R - q->second.round <= 60)
            qCellG = q->second.cell; // last rounds: find her (she bunkers), then post
        guardQ = qCellG;
        if (qCellG >= 0)
        {
            int threat = -1, threatD = INF;
            for (auto const& h : heads)
                if (!h.mine && manhattan(h.cell, qCellG) <= GUARD_INTERCEPT && manhattan(h.cell, qCellG) < threatD)
                {
                    threat = h.cell;
                    threatD = manhattan(h.cell, qCellG);
                }
            if (threat >= 0)
            {
                // A head-on kills both: one guard for one attacker near the queen.
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(threat, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(threat, 1), 75, 2);
                    return act_moves(ct, path, "intercept");
                }
                target = threat;
                why = I_INTERCEPT;
            }
            else if (bunkerFresh)
            {
                // Post: a 2x2 next to her box covering the most of its open
                // sides, not held by a teammate. Circled at length <= 3 it
                // keeps 3 of its 4 cells filled: no enemy reaches her there.
                std::vector<int> expo;
                for (int c : qBox)
                    for (int d = 0; d < 4; d++)
                    {
                        int const n = cross(c, d);
                        if (n >= 0 && std::find(qBox.begin(), qBox.end(), n) == qBox.end() &&
                            std::find(expo.begin(), expo.end(), n) == expo.end())
                            expo.push_back(n);
                    }
                std::array<int, 4> best{};
                int bestTl = -1;
                double bestScore = -1e18;
                std::array<int, 4> pb{};
                for (int e : expo)
                {
                    int const tls[4] = {e, nb(e, 3), nb(e, 0), nb(nb(e, 3), 0)};
                    for (int tl : tls)
                    {
                        if (!box_cells(tl, pb))
                            continue;
                        bool bad = false;
                        int cover = 0, dmin = INF;
                        for (int c : pb)
                        {
                            if (std::find(qBox.begin(), qBox.end(), c) != qBox.end())
                                bad = true; // never on her box
                            if (vis[c] && occId[c] >= 0 && occId[c] != me && !(occMine[c] && occId[c] == queenId))
                                bad = true; // someone is there (a teammate holds it, or an enemy)
                            if (pearlState[c] == 1 && std::find(body.begin(), body.end(), c) == body.end())
                                bad = true; // a pearl in the loop would make a length-3 guard 4
                            if (std::find(expo.begin(), expo.end(), c) != expo.end())
                                cover++;
                            dmin = std::min(dmin, c == head ? 0 : distHead[c]);
                        }
                        for (auto const& [id, st] : fleet)
                            if (id != me && id != queenId && R - st.round <= 2 && std::find(pb.begin(), pb.end(), st.cell) != pb.end())
                                bad = true; // reported holding it
                        if (bad || dmin >= INF)
                            continue;
                        double const sc = 100.0 * cover - dmin;
                        if (sc > bestScore)
                        {
                            bestScore = sc;
                            bestTl = tl;
                            best = pb;
                        }
                    }
                }
                if (bestTl >= 0)
                {
                    bool const inPost = std::find(best.begin(), best.end(), head) != best.end();
                    target = -1;
                    why = I_POST;
                    if (inPost && L >= 4)
                    {
                        int const child = L >= 6 ? L - 3 : L - 2; // keep 3 (2 from length 4-5)
                        if (split_allowed(ct, child) && child_can_escape(child))
                            return act_split(ct, child, "post-trim");
                    }
                    if (inPost && L <= 3)
                    {
                        for (int d = 0; d < 4; d++)
                        {
                            int const n = cross(head, d);
                            if (n < 0 || std::find(best.begin(), best.end(), n) == best.end())
                                continue;
                            if (std::find(body.begin(), body.end(), n) != body.end() || (vis[n] && occId[n] >= 0))
                                continue;
                            if (pearl_at(n) && L >= 3)
                                continue;
                            postDir = d;
                        }
                    }
                    else if (!inPost)
                    {
                        int bd = INF;
                        for (int c : best)
                            if (distHead[c] < bd)
                            {
                                bd = distHead[c];
                                target = c;
                            }
                    }
                }
            }
            else if (my.screen && screen_post(qCellG) >= 0)
            {
                // Stand watch out toward the enemy: attackers are seen (and
                // reported to her) before they are within striking reach.
                int const post = screen_post(qCellG);
                if (manhattan(head, post) > 1)
                {
                    target = post;
                    why = I_SCREEN;
                }
                else if (target >= 0 && distHead[target] > kLeash.screen)
                    target = -1;
            }
            else if (CHOKE_GUARDS && !my.screen && (DIAG("guard-branch"), choke_post(qCellG) >= 0) && distHead[choke_post(qCellG)] < INF)
            {
                DIAG("choke-used");
                // 7j: hold the chokepoint on her way to the enemy side (guards circle near it).
                int const post = choke_post(qCellG);
                if (manhattan(head, post) > 1)
                {
                    target = post;
                    why = I_GUARD;
                }
                else if (target >= 0 && distHead[target] > kLeash.screen)
                    target = -1;
            }
            else if (manhattan(head, qCellG) > GUARD_RING && distHead[qCellG] < INF)
            {
                target = qCellG;
                why = I_GUARD;
            }
            else
            {
                if (target >= 0 && distHead[target] > kLeash.guard)
                    target = -1; // stay around here; no long trips while on duty
                // Line up with her: a ping only reaches a dragon on the same
                // row or column, so a guard there can pass her its reports.
                int const qx = qCellG % W, qy = qCellG / W, hx = head % W, hy = head / W;
                if (SCREENS_MORE && target < 0 && hx != qx && hy != qy)
                {
                    int best = -1, bd = INF;
                    for (int axis = 0; axis < 2; axis++)
                        for (int off = -GUARD_RING; off <= GUARD_RING; off++)
                        {
                            if (std::abs(off) < 3)
                                continue;
                            int const c = axis == 0 ? cell_of(((qx + off) % W + W) % W, qy) : cell_of(qx, ((qy + off) % H + H) % H);
                            if (distHead[c] < bd && (!vis[c] || occId[c] < 0))
                            {
                                bd = distHead[c];
                                best = c;
                            }
                        }
                    if (best >= 0 && bd <= 6)
                    {
                        target = best;
                        why = I_GUARD_LINE;
                    }
                }
            }
        }
    }

    if (guardQ >= 0 && target < 0 && why != I_POST)
        why = I_GUARD; // idle on station: no exploring away from the queen
    return false;
}

// Target: rescue a stalked queen.
bool tg_rescue(Controller& ct)
{
    auto& qNow = tn.qNow;
    auto& stalkers = tn.stalkers;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& feedPhase = tn.feedPhase;
    // v2.14 rescue: a stalker reported next to our queen (or seen): short
    // teammates nearby come and strike it (one of ours for her life).
    if (ANTI_ESCORT && !amQueen && !my.leader && L <= 8 && !sit.ourQueenDead && !feedPhase && why != I_INTERCEPT &&
        why != I_POST)
    {
        auto const qf = fleet.find(queenId);
        int const qc = qNow >= 0 ? qNow : (qf != fleet.end() && R - qf->second.round <= 3 ? qf->second.cell : -1);
        if (qc >= 0)
        {
            int best = -1, bd = RESCUE_RANGE + 1;
            for (auto const& dg : dangers)
                if (dg.cause == 1 && R - dg.round <= 1 && manhattan(dg.cell, qc) <= 3)
                {
                    int const dd = dist_to(distHead, dg.cell);
                    if (dd < bd)
                    {
                        bd = dd;
                        best = dg.cell;
                    }
                }
            for (auto const& h : heads)
                if (!h.mine && stalkers.count(h.id) && distHead[h.cell] < bd)
                {
                    bd = distHead[h.cell];
                    best = h.cell;
                }
            if (best >= 0)
            {
                target = best;
                why = I_RESCUE;
            }
        }
    }
    return false;
}

// Target (queen): stay put once theirs is dead; bunker box.
bool tg_queen_bunker(Controller& ct)
{
    auto& head = tn.head;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& tieBunker = tn.tieBunker;
    auto& bunkerMode = tn.bunkerMode;
    auto& bunkerDir = tn.bunkerDir;
    auto& inBox = tn.inBox;
    // Once the enemy queen is dead ours only has to stay alive: no long
    // trips for pearls, the guards come to her.
    if (GUARDS && amQueen && sit.enemyQueenDead && target >= 0 && distHead[target] > kLeash.queenAfterTheirs)
        target = -1;

    // Bunker: head for the chosen box, then circle it.
    // Tie bunker: late in the game with both queens short and theirs no
    // longer than ours, queen length is a tie at best and the next tiebreak
    // is the longest dragon, so ours just circles at her length (<= 3).
    tieBunker = false;
    if (TIE_BUNKER && BUNKER && amQueen && !sit.enemyQueenDead && !sit.ourQueenDead && R >= TIE_BUNKER_ROUND && L <= 3)
    {
        auto const it = enemyLenMem.find(enemyQueenId);
        tieBunker = TIE_FORCE || (it != enemyLenMem.end() && it->second.exact && R - it->second.round <= TIE_SEEN_ROUNDS && it->second.len <= L);
    }
    bool const lateBunker = LATE_SAFE && BUNKER && amQueen && sit.lateSafe && L <= 3 && !sit.ourQueenDead; // circle at her length
    // F2: their queen was seen and has not been seen for EQ_GONE_ROUNDS - most
    // likely dead (queens that roam get killed; we lost games where only ours
    // had to live). Bunker without the trim, and only if ours is not shorter
    // than theirs was.
    bool eqProbDead = false;
    if (EQ_INFER && amQueen && !sit.enemyQueenDead && !sit.ourQueenDead && R >= EQ_INFER_ROUND && (sit.lastEqSeenR >= 0 || EQ_INFER_UNSEEN) && R - sit.lastEqSeenR >= EQ_GONE_ROUNDS)
    {
        auto const it = enemyLenMem.find(enemyQueenId);
        eqProbDead = it == enemyLenMem.end() || it->second.len <= L;
    }
    bunkerMode = BUNKER && amQueen && (sit.enemyQueenDead || tieBunker || lateBunker || eqProbDead) && !sit.ourQueenDead;
    bunkerDir = -1;
    inBox = false;
    if (bunkerMode)
    {
        std::array<int, 4> bx{};
        if (bunkerBox < 0 || !box_cells(bunkerBox, bx) || !box_usable(bx))
            bunkerBox = choose_bunker();
        if (bunkerBox >= 0 && box_cells(bunkerBox, bx))
        {
            inBox = std::find(bx.begin(), bx.end(), head) != bx.end();
            why = I_BUNKER;
            target = -1;
            if (inBox && L <= 3)
            {
                for (int d = 0; d < 4; d++)
                {
                    int const n = cross(head, d);
                    if (n < 0 || std::find(bx.begin(), bx.end(), n) == bx.end())
                        continue;
                    if (std::find(body.begin(), body.end(), n) != body.end())
                        continue; // the tail still blocks the cell it leaves
                    if (vis[n] && occId[n] >= 0)
                        continue;
                    if (pearl_at(n) && L >= 3)
                    {
                        bunkerBox = -1; // a pearl in the loop would jam it: pick another box next turn
                        continue;
                    }
                    bunkerDir = d;
                }
            }
            else if (!inBox)
            {
                int bd = INF;
                for (int x : bx)
                    if (distHead[x] < bd)
                    {
                        bd = distHead[x];
                        target = x;
                    }
            }
        }
    }
    return false;
}

// Target (queen): rest spot, then home.
bool tg_queen_rest(Controller& ct)
{
    auto& head = tn.head;
    auto& queenStarving = tn.queenStarving;
    auto& target = tn.target;
    auto& why = tn.why;
    auto& bunkerMode = tn.bunkerMode;
    // v2.14 rest: with nothing safe to eat she waits in open water near home
    // (drifting along walls with no goal is how queens end up in corners).
    if (QUEEN_HOME && amQueen && !bunkerMode && target < 0 && !queenStarving && ourQueenSpawn >= 0)
    {
        if (restCell < 0 || R % 25 == 0 || (vis[restCell] && occId[restCell] >= 0 && occId[restCell] != me) || distHead[restCell] >= INF)
            restCell = choose_rest();
        if (restCell >= 0 && manhattan(head, restCell) > 1 && distHead[restCell] < INF)
        {
            target = restCell;
            why = I_REST;
        }
    }
    // v2.14 home: with nothing to eat inside her leash the queen walks back
    // home (their stalkers hunt queens that roam).
    if (QUEEN_HOME && amQueen && !bunkerMode && target < 0 && !queenStarving && ourQueenSpawn >= 0 && manhattan(head, ourQueenSpawn) > queenLeash + 4 && queenLeash < INF &&
        distHead[ourQueenSpawn] < INF)
    {
        target = ourQueenSpawn;
        why = I_HOME;
    }
    return false;
}

// Target: portal scouts and the portal rush.
bool tg_portal(Controller& ct)
{
    auto& target = tn.target;
    auto& portalDir = tn.portalDir;
    auto& why = tn.why;
    // Portals: whole regions (and most of the pearls on some maps) lie behind
    // portals whose far end nobody has seen. Scouts open them early: one
    // crossing teaches everyone the pair, and paths then use it.
    // Only worth the risk when the portals lead somewhere we cannot walk to:
    // our reachable area (unexplored edges count as open) is a small part of
    // the map.
    int reachable = 0;
    for (int c = 0; c < N; c++)
        reachable += distHead[c] < INF;
    bool const boxedIn = reachable * 100 < SCOUT_REACH_PCT * N;
    int const scoutMaxLen = POCKET_CHECK ? BLIND_PORTAL_MAX_LEN : 12;
    bool const scout = PORTAL_SCOUTS && (boxedIn || PORTAL_ALWAYS) && !amQueen && !my.leader && L <= scoutMaxLen && (my.hunterSlot || my.role == ROLE_PORTAL) &&
                       (why == I_PEARL || why == I_EXPLORE);
    if (!amQueen && !my.leader && L <= scoutMaxLen && (scout || (target < 0 && (boxedIn || my.role == ROLE_PORTAL))) &&
        !(my.guard && why != I_EXPLORE))
    {
        int pd = -1;
        int const pg = choose_portal_goal(pd);
        bool take = pg >= 0 && (target < 0 || why != I_PEARL || distHead[pg] + kLeash.scoutMargin <= distHead[target]);
        if (!PORTAL_SCOUTS)
            take = take && target < 0 && my.role == ROLE_PORTAL;
        if (take)
        {
            target = pg;
            portalDir = pd;
            why = I_PORTAL;
        }
    }
    // P9 portal rush: an unexplored portal right here - the nearest short
    // dragon goes through now (anything but a pearl next to it waits).
    if (PORTAL_RUSH && !amQueen && !my.leader && L <= scoutMaxLen && !(my.guard && why != I_EXPLORE) &&
        why != I_PORTAL && !(why == I_PEARL && target >= 0 && distHead[target] <= PORTAL_RUSH_PEARL))
    {
        int pd = -1;
        int const pg = choose_portal_goal(pd);
        if (pg >= 0 && distHead[pg] <= PORTAL_RUSH_DIST)
        {
            bool nearer = false;
            for (auto const& h : heads)
                if (h.mine && h.id != me && h.id != queenId && manhattan(h.cell, pg) < distHead[pg])
                    nearer = true;
            if (!nearer)
            {
                target = pg;
                portalDir = pd;
                why = I_PORTAL;
            }
        }
    }
    return false;
}

// Target: idle hunter-group dragons push toward the enemy side while expanding.
bool tg_push(Controller& ct)
{
    auto& target = tn.target;
    auto& why = tn.why;
    if (AGGRO_EXPANSION && target < 0 && my.hunterSlot && sit.expansion && !my.leader && !my.guard && !(SAFE_TRADES && (sit.survival || sit.outnumbered)))
    {
        // Take the middle early: idle hunter-group dragons head for the enemy
        // side (the mirror image of our queen's start), past the centre.
        int const qt = sit.enemyQueenDead ? -1 : enemy_queen_target();
        if (qt >= 0 && distHead[qt] < INF)
        {
            target = qt;
            why = I_PUSH;
        }
    }
    return false;
}

// Target: defenders hold our half.
bool tg_defend(Controller& ct)
{
    auto& target = tn.target;
    auto& why = tn.why;
    auto& feedPhase = tn.feedPhase;
    if (my.defender && !feedPhase && why != I_INTERCEPT &&
        (target < 0 || halfMargin(target) <= 0 || distHead[target] > kLeash.defender))
    {
        int const dg = choose_camp_goal(true);
        if (dg >= 0)
        {
            target = dg;
            why = I_DEFEND;
        }
    }
    return false;
}

// Target (economy): opening camp by a spawn tile.
bool tg_camp_open(Controller& ct)
{
    auto& target = tn.target;
    auto& why = tn.why;
    // Opening: pearls appear on spawn tiles, so an idle dragon waits by one
    // rather than wandering off to explore (we lost most opening pearl races).
    if (target < 0 && R < OPEN_CAMP_ROUNDS && !amQueen && !my.guard && why == I_EXPLORE)
    {
        int const cg = choose_camp_goal();
        if (cg >= 0 && distHead[cg] <= 20)
        {
            target = cg;
            why = I_CAMP;
        }
    }
    return false;
}

// Target (economy): rich zones.
bool tg_zone(Controller& ct)
{
    auto& head = tn.head;
    auto& target = tn.target;
    auto& why = tn.why;
    // OPEN_PLAN zones: idle collectors, and collectors whose pearl is far,
    // go to the zone that needs them most (allocation by richness).
    if (ZONES && !amQueen && !my.leader && !my.guard && !my.hunter && !my.wall && !my.defender &&
        ((target < 0 && why == I_EXPLORE) || (why == I_PEARL && target >= 0 && distHead[target] > kLeash.zone)))
    {
        int const z = choose_zone();
        if (z >= 0 && manhattan(head, z) > 2 && (target < 0 || distHead[z] + 2 < distHead[target]))
        {
            target = z;
            why = I_ZONE;
        }
    }
    return false;
}

// Target (economy): outposts.
bool tg_outpost(Controller& ct)
{
    auto& head = tn.head;
    auto& target = tn.target;
    auto& why = tn.why;
    // P8 outposts: an idle collector holds the richest spot nobody holds yet.
    if (OUTPOSTS && target < 0 && !amQueen && !my.leader && !my.guard && !my.hunter && !my.wall && why == I_EXPLORE)
    {
        int const op = choose_outpost();
        if (op >= 0 && manhattan(head, op) > 2)
        {
            target = op;
            why = I_OUTPOST;
        }
    }
    return false;
}

// Target (economy): explore, or camp when nothing is unseen.
bool tg_explore(Controller& ct)
{
    auto& queenStarving = tn.queenStarving;
    auto& target = tn.target;
    auto& why = tn.why;
    if (target < 0 && !(GUARDS && amQueen && sit.enemyQueenDead) && !(amQueen && QUEEN_AVOID_UNKNOWN && !queenStarving) && why != I_GUARD &&
        why != I_REGROUP && why != I_EVADE)
    {
        target = choose_explore_goal();
        // Nothing unseen left to look at: wait by a spawn tile instead of
        // revisiting old ground (pearls appear there, not on the way).
        if (IDLE_CAMP && !amQueen && !my.guard && (target < 0 || seenRound[target] >= 0))
        {
            int const cg = choose_camp_goal();
            if (cg >= 0)
            {
                target = cg;
                why = I_CAMP;
            }
        }
    }
    return false;
}

// Commit: loop-stall, camp give-up, stall and chase bookkeeping for the chosen target.
bool commit_target(Controller& ct)
{
    auto& target = tn.target;
    auto& why = tn.why;
    // 6c: intent memory - a zone / outpost / camp / explore / wall goal is kept while it is still the same kind of
    // goal and reachable, unless the new one is clearly closer (4+ steps and 30%); stops goal flip-flops.
    bool enemyInSight = false;
    for (auto const& h : heads)
        enemyInSight = enemyInSight || !h.mine;
    // INTENT_MEMORY 2 (user): only while no enemy head is in view - with one in sight the dragon reacts at once.
    // INTENT_MEMORY 3 (v3): only records the goal; the choosers' keep_bonus() does the keeping (intent.hpp).
    if (INTENT_MEMORY && !amQueen)
    {
        bool const sticky = (why == I_ZONE || why == I_OUTPOST || why == I_CAMP ||
                             (INTENT_MEMORY <= 2 && (why == I_EXPLORE || why == I_WALL))) &&
                            !(INTENT_MEMORY >= 2 && enemyInSight);
        if (INTENT_MEMORY <= 2 && sticky && why == lastGoalIntent && lastGoal >= 0 && lastGoal != target && target >= 0 && distHead[lastGoal] < INF &&
            lastGoal != body[0] && R - lastGoalR <= 1)
        {
            int const dOld = distHead[lastGoal], dNew = distHead[target];
            if (!(dNew + COMMIT_MARGIN <= dOld && dNew * 10 <= dOld * 7))
                target = lastGoal;
        }
        lastGoal = sticky ? target : -1;
        lastGoalIntent = why;
        lastGoalR = R;
    }
    // v2.14 loop stall: circling one 2x2 for rounds without eating while the
    // pearl stays out of reach (a maze target that keeps flipping): drop it.
    if (LOOP_STALL && why == I_PEARL && target >= 0 && distHead[target] > 3 && R - lastEatR >= LOOP_STALL_ROUNDS &&
        static_cast<int>(headHist.size()) >= LOOP_STALL_ROUNDS)
    {
        bool tight = true;
        for (std::size_t i = headHist.size() - LOOP_STALL_ROUNDS; i < headHist.size() && tight; i++)
            for (std::size_t j = i + 1; j < headHist.size(); j++)
                if (manhattan(headHist[i], headHist[j]) > 2)
                    tight = false;
        if (tight)
            skipPearl[target] = R + 30;
    }
    // F6: camped next to a tile for CAMP_GIVEUP rounds and nothing spawned:
    // the timing guess was wrong - leave it alone for a while.
    if (CAMP_GIVEUP > 0 && why == I_CAMP && target >= 0)
    {
        if (target != campTile)
        {
            campTile = target;
            campArriveR = -1;
        }
        if (distHead[target] <= 1 && campArriveR < 0)
            campArriveR = R;
        if (campArriveR >= 0 && R - campArriveR >= CAMP_GIVEUP && !pearl_at(target))
        {
            skipPearl[target] = R + 40;
            campTile = -1;
        }
    }
    // Stalled: no step closer to this pearl for STALL_ROUNDS turns (it is
    // behind kelp we keep running into): leave it to others for a while.
    if (why == I_PEARL && target >= 0)
    {
        int const dNow = distHead[target];
        if (target != stallTarget)
        {
            stallTarget = target;
            stallBest = dNow;
            stallRound = R;
        }
        else if (dNow < stallBest)
        {
            stallBest = dNow;
            stallRound = R;
        }
        else if (STALL_ROUNDS > 0 && R - stallRound >= STALL_ROUNDS)
        {
            skipPearl[target] = R + STALL_SKIP;
            stallTarget = -1;
        }
        // F4: the timer above restarts whenever the target flips between two
        // pearls; count the rounds each pearl is chased in total instead, and
        // cap a run of "pearl" rounds with nothing eaten.
        if (static_cast<int>(chaseCnt.size()) != N)
        {
            chaseCnt.assign(N, 0);
            chaseLast.assign(N, -100);
        }
        if (R - chaseLast[target] > 15)
            chaseCnt[target] = 0;
        chaseCnt[target]++;
        chaseLast[target] = R;
        if (dryStart < 0 || lastEatR > dryStart)
            dryStart = R;
        if ((chaseCnt[target] >= CHASE_MAX && R - lastEatR >= CHASE_MAX) || (R - dryStart >= DRY_MAX && R - lastEatR >= DRY_MAX))
        {
            skipPearl[target] = R + CHASE_SKIP;
            chaseCnt[target] = 0;
            dryStart = R;
        }
        lastPearl = target;
    }
    else
        lastPearl = -1;
    return false;
}

// Act: feeding sacrifice next to the queen / giant.
bool feed_sacrifice(Controller& ct)
{
    auto& head = tn.head;
    auto& feedId = tn.feedId;
    auto& feedLeader = tn.feedLeader;
    auto& earlyFeed = tn.earlyFeed;
    auto& feedPhase = tn.feedPhase;
    // ---- 3. Feeding sacrifice (section 13) ----------------------------------
    if (feedPhase && feedLeader >= 0 && L >= 3 && units > (earlyFeed ? cons_floor() : 2) && feedId != me)
    {
        // In view: die within 3 cells of her head (the drop lands where she
        // can eat it in a move or two). Out of view: a fresh report within 4.
        bool inView = false, close = false;
        for (auto const& h : heads)
            if (h.mine && h.id == feedId)
            {
                inView = true;
                close = cheb(h.cell, head) <= 3;
                if (FEED_FIX)
                {
                    // P4: the drop must be on her side of the kelp - a short water path to her head.
                    int pd = INF;
                    for (int d = 0; d < 4; d++)
                    {
                        int const n = cross(h.cell, d);
                        if (n >= 0 && distHead[n] < INF)
                            pd = std::min(pd, distHead[n] + 1);
                    }
                    close = pd <= FEED_PATH;
                }
            }
        if (!inView && !FEED_FIX) // P4: out of view we cannot tell which side of the kelp she is on
        {
            auto const ls = fleet.find(feedId);
            close = ls != fleet.end() && R - ls->second.round <= 2 && cheb(ls->second.cell, head) <= 4;
        }
        // 7e: delivery - the pearls fall on our head and every second segment; die only when most of them land
        // within a few water steps of her head (else keep closing in).
        if (FEED_DELIVERY && close && inView)
        {
            int fc = -1;
            for (auto const& h : heads)
                if (h.mine && h.id == feedId)
                    fc = h.cell;
            queen_dist_bfs(fc, FEED_DROP_DIST + 1);
            int drops = 0, good = 0;
            for (int i = 0; i < static_cast<int>(body.size()) && i < L; i += 2)
            {
                drops++;
                good += qDist[body[i]] <= FEED_DROP_DIST;
            }
            if (good * 3 < drops * 2)
                close = false;
        }
        // Squeeze into the neck: the body turns into pearls next to the leader.
        if (close && body.size() >= 2)
            for (int d = 0; d < 4; d++)
                if (nb(head, d) == body[1])
                    return act_move(ct, d, "feed-leader");
    }
    return false;
}

// Act: splits (bunker trim, queen split, grow, lane, pearl, half, keep).
bool plan_split(Controller& ct)
{
    auto& head = tn.head;
    auto& sealedNoSplit = tn.sealedNoSplit;
    auto& cands = tn.cands;
    auto& anyLegal = tn.anyLegal;
    auto& anySafe = tn.anySafe;
    auto& nextToEnemy = tn.nextToEnemy;
    auto& pearls = tn.pearls;
    auto& pi = tn.pi;
    auto& tieBunker = tn.tieBunker;
    auto& bunkerMode = tn.bunkerMode;
    auto& inBox = tn.inBox;
    // ---- 4. Splits (sections 2, 8, 14) ---------------------------------------
    // v2.14: inside a dead end an optional split leaves the head part to die
    // long; eat to the bottom, then split all but 2.
    // v2.15 (P1): only a real dead end - every legal move continues into a
    // 1-wide corridor that ends. "No safe move" alone is common in corridor
    // mazes and crowds, and blocking splits there stalled the whole fleet.
    if (anySafe)
        miningTrip = false; // back in open water: the trip is over
    bool inDeadEnd = FOUNTAINS && !amQueen && anyLegal && !anySafe;
    // A break-even trip (2 pearls in, a 2-cell stub left) only pays by
    // splitting inside: that is how a lone miner turns into a fleet.
    if (DEADEND_STRICT && !miningTrip)
        inDeadEnd = false;
    if (inDeadEnd && DEADEND_STRICT)
        for (auto const& cd : cands)
            if (cd.legal && (cd.blind || cd.dest < 0 || cd.dest != nb(head, cd.d) || !pocket_ahead(head, cd.dest)))
                inDeadEnd = false; // a way on that is not a dead end (or a portal): splits stay allowed
    bool const calm = !nextToEnemy && !inDeadEnd;
    // Runners: on big maps a share of the fleet stays long enough for two
    // free steps a turn, covers distance, and buds off children of 3+.
    bool const isRunner = (RUNNERS && !sit.openPlan && !amQueen && !my.leader && !my.guard && !my.hunter && my.role == ROLE_EXPLORER && mapProf.runners &&
                           growth_open()) ||
                          my.defender; // defenders stay medium length too: a 2-long blocker is easily stepped round
    int const growChild = isRunner ? L - RUNNER_KEEP : L / 2;
    if (amQueen && bunkerMode && sit.enemyQueenDead && !tieBunker && !sealedNoSplit && L >= 4 && (inBox || qThreat[head] == INF))
    {
        int const child = L - 2; // keep 2: one stray pearl then still leaves the loop a free cell
        if (split_allowed(ct, child))
            return act_split(ct, child, "bunker-trim");
    }
    if (amQueen && !bunkerMode && !sealedNoSplit)
    {
        if (EXPANSION)
        {
            // Numbers first: the queen splits too (head-ons kill whatever the
            // length, so a short queen is no easier to kill and harder to trap).
            bool crowded = false;
            for (auto const& h : heads)
                if (manhattan(h.cell, head) <= 3)
                    crowded = true;
            // v2.14: the child stays on our tail cells this round, so she needs
            // two ways on that are not dead ends without them.
            int realExits = 0;
            for (int d = 0; d < 4; d++)
            {
                int const n = cross(head, d);
                if (n >= 0 && !(vis[n] && occId[n] >= 0) && !(QUEEN_HOME && n == nb(head, d) && pocket_ahead(head, n)))
                    realExits++;
            }
            // User: the queen splits at the beginning (our own start crowd does not count, enemies do).
            bool enemyClose = false;
            for (auto const& h : heads)
                if (!h.mine && manhattan(h.cell, head) <= 3)
                    enemyClose = true;
            if (QUEEN_START_SPLIT && R < QSS_END && calm && !sit.consolidate && sit.queenSplits && anySafe && !enemyClose && L >= 4 &&
                (!QUEEN_HOME || realExits >= 2) && room_for_more() && split_allowed(ct, L / 2) && child_can_escape(L / 2, true) &&
                split_keeps_loop(L / 2))
                return act_split(ct, L / 2, "queen-start");
            if (calm && !sit.consolidate && sit.queenSplits && anySafe && !crowded && L >= 4 && (!QUEEN_HOME || (realExits >= 2 && ((ADAPT_SPLIT && sit.queenOpening) || (OPEN_PLAN && OPEN_QUEEN && sit.openPlan) || (OPEN_QUEEN_SPLIT && sit.openPlan && R < OQS_END) || (units < QUEEN_SPLIT_UNITS && (L - L / 2 >= QUEEN_MIN_KEEP || (QUEEN_FIRST_SPLIT && units <= 3 && L >= 4)))))) &&
                room_for_more() &&
                split_allowed(ct, L / 2) && child_can_escape(L / 2, true) && split_keeps_loop(L / 2))
                return act_split(ct, L / 2, "queen-split");
        }
        else if (calm && R < EARLY_SPLIT_ROUNDS && units < QUEEN_SEED_UNITS && L >= 5 && split_allowed(ct, 2) && child_can_escape(2, true) &&
                 split_keeps_loop(2))
            return act_split(ct, 2, "queen-seed");
    }
    else if (calm && !sit.consolidate && !my.wall && (!my.hunter || (FAST_OPEN && sit.expansion) || (SPLIT_MORE && R < EARLY_SPLIT_ROUNDS) || (HUNTER_SPLIT_LEN > 0 && L >= HUNTER_SPLIT_LEN)) &&
             L >= (isRunner ? RUNNER_SPLIT_LEN : EARLY_SPLIT_MIN_LEN) && (EXPANSION ? growth_open() && !my.leader : R < EARLY_SPLIT_ROUNDS) &&
             room_for_more() && split_allowed(ct, growChild) && child_can_escape(growChild, true))
        return act_split(ct, growChild, "grow-fleet");

    // Lane split: the tail sits among spawn tiles nobody else holds, so the
    // child is born right where the pearls keep coming.
    if (LANE_SPLIT && !sit.consolidate && calm && !amQueen && !my.leader && !my.guard && (!my.hunter || sit.expansion) && L >= 4 &&
        (!isRunner || L - 2 >= RUNNER_KEEP) && units < std::min(unitLimit - 2, std::max(2, N / LANE_DENSITY)) &&
        static_cast<int>(body.size()) >= L && split_allowed(ct, 2))
    {
        int const tail = body.back(), tx = tail % W, ty = tail / W;
        int tiles = 0;
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3 + std::abs(dy); dx <= 3 - std::abs(dy); dx++)
                if (nextSpawn[cell_of((tx + dx + W) % W, (ty + dy + H) % H)] >= 0)
                    tiles++;
        bool mateNear = false;
        for (auto const& h : heads)
            if (h.mine && h.id != me && manhattan(h.cell, tail) <= 4)
                mateNear = true;
        if (tiles >= LANE_TILES && !mateNear && child_can_escape(2, true))
            return act_split(ct, 2, "lane-split");
    }

    if (calm && !sit.consolidate && !my.leader && !my.hunter && L >= 4 && (!isRunner || L - 2 >= RUNNER_KEEP) && pi >= 0 && room_for_more() && split_allowed(ct, 2) &&
        static_cast<int>(body.size()) >= L)
    {
        int const tail = body.back();
        std::vector<int> child{tail, body[L - 2]};
        {
            WallScope ws; // the front part stays where it is this turn
            for (int i = 0; i < L - 2; i++)
                ws.wall(body[i]);
            BodyScope const sb(child, 2);
            plan_bfs(tail, distTmp);
        }
        int const pc = pearls[pi].cell;
        int const dt = distTmp[pc], dh = distHead[pc];
        if (dt <= PEARL_BEHIND_MAX && dt + PEARL_BEHIND_GAIN <= dh && child_can_escape(2, true))
            return act_split(ct, 2, "pearl-split");
    }

    // Half-split: mid-game a very long dragon is slow to turn and easy to trap
    // on a crowded map; two of half the length hold more ground. On open maps
    // the leaders keep growing (length decides the tiebreaks there).
    if (HALF_SPLIT && calm && !amQueen && !my.heir && L >= HALF_SPLIT_LEN && growth_open() && (!my.leader || !sit.openMap) &&
        room_for_more() && split_allowed(ct, L / 2) && child_can_escape(L / 2, true))
        return act_split(ct, L / 2, "half-split");

    if (calm && !sit.consolidate && sit.transitioned && !my.leader && !my.hunter && sit.mode == MODE_PRESERVE && L >= PRESERVE_SPLIT_LEN && room_for_more() &&
        split_allowed(ct, L / 2) && child_can_escape(L / 2, true))
        return act_split(ct, L / 2, "keep-fleet");

#ifdef FLEET_DEBUG
    {
        std::string dbg = std::string("target ") + intent_name(why) + " " + (target >= 0 ? std::to_string(target % W) + "," + std::to_string(target / W) : "-") +
                          " d" + (target >= 0 ? std::to_string(distHead[target]) : "-") + " pearls";
        for (auto const& p : pearls)
            dbg += " " + std::to_string(p.cell % W) + "," + std::to_string(p.cell / W);
        ct.output_log(dbg);
    }
#endif
    return false;
}

// Act: score the candidate moves (cost terms).
bool score_moves(Controller& ct)
{
    auto& head = tn.head;
    auto& facing = tn.facing;
    auto& queenStarving = tn.queenStarving;
    auto& cands = tn.cands;
    auto& cap = tn.cap;
    auto& anySafe = tn.anySafe;
    auto& diveMin = tn.diveMin;
    auto& allIn = tn.allIn;
    auto& qBox = tn.qBox;
    auto& bunkerFresh = tn.bunkerFresh;
    auto& postDir = tn.postDir;
    auto& pearls = tn.pearls;
    auto& target = tn.target;
    auto& portalDir = tn.portalDir;
    auto& why = tn.why;
    auto& pi = tn.pi;
    auto& guardQ = tn.guardQ;
    auto& bunkerMode = tn.bunkerMode;
    auto& bunkerDir = tn.bunkerDir;
    auto& campWait = tn.campWait;
    auto& bestD = tn.bestD;
    // ---- 5. Best move ------------------------------------------------------
    // Box their queen in: we see her but have no strike this turn. Their
    // queens die when boxed (a body she runs into kills her; theirs even
    // self-destruct). A head-on with her is a trade we always want.
    int eqHead = -1;
    if (BOX_QUEEN && !amQueen && !my.leader && !sit.enemyQueenDead && why != I_FEED &&
        (allIn || !(SAFE_TRADES && (sit.survival || sit.outnumbered))))
        for (auto const& h : heads)
            if (!h.mine && h.id == enemyQueenId)
                eqHead = h.cell;
    int const eqExitsNow = eqHead >= 0 ? queen_exits(eqHead, body) : 0;
    // Camping: the job tile has not spawned yet and we are close. A pearl
    // (almost) never appears under a body, so wait beside the tile and step
    // on in the spawn round.
    campWait = CAMP_JOB && why == I_PEARL && pi >= 0 && pi < static_cast<int>(pearls.size()) &&
                          pearls[pi].cell == target && pearls[pi].appear > R && !pearl_at(target) && distHead[target] <= 3;
    // Our queen in a 1-wide corridor: where is she (vision, else fresh sonar)?
    int qCorr = -1;
    if (CORRIDOR_GUARD && !amQueen && !sit.ourQueenDead)
    {
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qCorr = h.cell;
        auto const q = fleet.find(queenId);
        if (qCorr < 0 && q != fleet.end() && R - q->second.round <= 2)
            qCorr = q->second.cell;
        if (qCorr >= 0 && (open_sides(qCorr) > 2 || manhattan(qCorr, head) > 14))
            qCorr = -1;
    }
    std::vector<int> teamCells;
    for (int c = 0; c < N; c++)
        if (vis[c] && occId[c] >= 0 && occId[c] != me && occMine[c])
            teamCells.push_back(c);

    // Teammates must not wall the queen in: measure her free space with our
    // body where it is now and where each move would put it.
    int qHead = -1, qLen = 0, qNeed = 0, qBase = 0;
    if (QUEEN_ROOM && !amQueen)
        for (auto const& h : heads)
            if (h.mine && h.id == queenId && manhattan(h.cell, head) <= 5)
            {
                qHead = h.cell;
                auto const q = fleet.find(queenId);
                qLen = std::max(h.len, q != fleet.end() && R - q->second.round <= 3 ? q->second.len : 0);
                qNeed = 2 * qLen + 4;
                set_body(body, L);
                ignoreContested = true;
                qBase = flood(qHead, -1, qNeed + 4).area;
                ignoreContested = false;
            }

    // Give the queen room: where is she (vision, else sonar status)?
    int qCell = -1;
    if (QUEEN_CLEAR > 0 && !amQueen && !sit.ourQueenDead && why != I_FEED)
    {
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qCell = h.cell;
        auto const q = fleet.find(queenId);
        if (qCell < 0 && q != fleet.end() && R - q->second.round <= 2)
            qCell = q->second.cell;
        if (qCell >= 0 && manhattan(qCell, head) <= QUEEN_CLEAR + 4)
            queen_dist_bfs(qCell, QUEEN_CLEAR + 2);
        else
            qCell = -1;
    }

    if (BUNKER_GUARDS && amQueen && bunkerMode && bunkerBox >= 0)
    {
        qBunkerTl = bunkerBox;
        qBunkerR = R;
        queue_fact(msg_bunker(bunkerBox), 72, 1); // guards come and post round her box
    }
    if (bunkerDir < 0 && postDir >= 0)
        bunkerDir = postDir; // a guard circling its post: same forced loop step
    if (bunkerDir >= 0 && cands[bunkerDir].legal)
    {
        cands[bunkerDir].ok1 = cands[bunkerDir].ok2 = true; // the box loop never traps a queen of length <= 3
        cands[bunkerDir].surv = true;
    }
    bestD = -1;
    double bestScore = -1e18;
#ifdef BAND_DIAG
    int plainD = -1;
#endif
    for (auto& cd : cands)
    {
        if (!cd.legal)
            continue;
        Cost cost;
        cost.set(BAND_SAFETY, cd.d == bunkerDir ? 900000.0 : 0.0); // the forced bunker / post loop: her safe circle
        if (qCell >= 0 && !cd.blind && cd.dest >= 0)
        {
            // Teammates crowding the queen (her children start right behind
            // her) seal her into corridors; getting out of her way is free.
            int const dq = qDist[cd.dest], dnow = qDist[head];
            if (dq <= QUEEN_CLEAR && dq <= dnow)
                cost.add(BAND_SAFETY, -(QUEEN_CLEAR_PEN * (QUEEN_CLEAR + 1 - dq) * (open_sides(cd.dest) <= 2 ? 2.0 : 1.0)));
        }
        if (guardQ >= 0 && !cd.blind && cd.dest >= 0 && why != I_INTERCEPT)
        {
            int const dg = manhattan(guardQ, cd.dest);
            if (dg < GUARD_MIN && dg <= manhattan(guardQ, head))
                cost.add(BAND_SAFETY, -(GUARD_MIN_PEN * (GUARD_MIN - dg)));
        }
        if (qHead >= 0 && !cd.blind && !cd.nb1.empty() && why != I_FEED)
        {
            set_body(cd.nb1, L + (cd.eat ? 1 : 0));
            ignoreContested = true;
            int const qa = flood(qHead, -1, qNeed + 4).area;
            ignoreContested = false;
            if (qa < qNeed && qa < qBase)
                cost.add(BAND_SAFETY, -(QUEEN_ROOM_PEN * (1.0 + static_cast<double>(qBase - qa) / qNeed)));
        }
        if (cd.blind)
        {
            // Unknown far side: only for small, expendable dragons (section 5).
            bool const wanted = my.role == ROLE_PORTAL || my.role == ROLE_EXPLORER;
            if (my.leader)
                cost.set(BAND_SAFETY, anySafe ? -1e7 : -5e5); // better than a certain trap
            else if (POCKET_CHECK && L > BLIND_PORTAL_MAX_LEN)
                cost.set(BAND_SAFETY, anySafe ? -2e6 : -5e5); // could be a box too small to turn round in
            else if (portalDir == cd.d && target == head)
                cost.set(BAND_GOAL, 5000);
            else
                cost.set(BAND_SAFETY, wanted && L <= 6 ? -500 : -4000); // unknown far side
            if (!anySafe)
                cost.add(BAND_GOAL, 3000);
        }
        else
        {
            // Risk is judged against its reward. A trap with nothing gained is
            // an avoidable death; a trap that pays a pearl is acceptable,
            // cheapest when a survival split can save all but 2 segments.
            int const lenAfter = L + (cd.eat ? 1 : 0);
            bool const canSave = lenAfter >= 4 && units < unitLimit;
            // No pearl compensates for losing the queen.
            if (!amQueen && !cd.ok1 && cd.dive >= diveMin)
                cost.add(BAND_GOAL, 2500.0 * cd.dive - 1500.0); // a dead end whose pearls pay for the split
            else if (QUEEN_SURV_RANK && amQueen && !anySafe && !cd.ok2)
                cost.add(BAND_LIFE, -(cd.surv ? 5e5 : (QUEEN_HARD_THREAT ? QUEEN_TRAP_PEN : 1e6)));
            else if (!cd.ok2)
            {
                // A trap that pays a pearl is a trade (goal); one with nothing gained is a death (life).
                if (cd.eat && !amQueen && !(cd.dive > -1e9 && cd.dive < 0))
                    cost.add(BAND_GOAL, -(canSave ? 2500.0 : 2500.0 + 900.0 * lenAfter));
                else
                    cost.add(BAND_LIFE, -((amQueen && QUEEN_HARD_THREAT) ? QUEEN_TRAP_PEN : 1e6));
            }
            else if (!cd.ok1)
            {
                if (cd.eat && !amQueen)
                    cost.add(BAND_GOAL, -1500.0);
                else
                    cost.add(BAND_SAFETY, -(amQueen ? 2e5 : 2e4));
            }
            cost.add(BAND_GOAL, std::min(cd.area, cap) * (amQueen ? 60.0 : 15.0));
            if (QUEEN_CORRIDOR_MATE && amQueen && cd.dest >= 0 && open_sides(cd.dest) <= 2)
                for (auto const& h : heads)
                    if (h.mine && h.id != me && manhattan(h.cell, cd.dest) <= 10 && corridor_leads_to(cd.dest, h.cell, 10))
                    {
                        cost.add(BAND_LIFE, -QUEEN_EXIT_PEN); // a teammate in the same corridor: neither of us could turn round
                        break;
                    }
            if (amQueen && L >= QUEEN_LONG_LEN && cd.dest >= 0 && open_sides(cd.dest) <= 2)
                cost.add(BAND_SAFETY, -QUEEN_CORRIDOR_PEN); // a long queen cannot turn round in a corridor: one blocker ahead kills her
            if (cd.eat)
                cost.add(BAND_GOAL, my.leader ? 6000 : 3000);
            if (target >= 0)
            {
                set_body(cd.nb1, L + (cd.eat ? 1 : 0));
                plan_bfs(cd.dest, distTmp);
                int dt = dist_to(distTmp, target);
                if (portalDir >= 0 && target != head && cd.dest == target)
                    dt = 0;
                cd.dt = dt;
                if (campWait)
                    cost.add(BAND_GOAL, -(cd.dest == target ? 50000.0 : 150.0 * std::max(0, std::min(dt, 200) - 1)));
                else
                    cost.add(BAND_GOAL, -(150.0 * std::min(dt, 200)));
            }
            for (auto const& h : heads)
            {
                int const reach = (!h.mine && my.leader) ? enemy_reach(h) : 1;
                if (manhattan(h.cell, cd.dest) > reach)
                    continue;
                if (!h.mine && amQueen && QUEEN_THREAT_MAP)
                    continue; // the threat map below covers it
                if (!h.mine && amQueen)
                    cost.add(BAND_LIFE, -3e6);
                else if (!h.mine && my.leader)
                    cost.add(BAND_SAFETY, -(L >= h.len + 2 ? 200000 : 500000));
                else if (!h.mine && SURV_ECON && sit.survival)
                    cost.add(BAND_SAFETY, -SURV_THREAT_PEN);
                else if (!h.mine && sit.mode == MODE_COMBAT && my.role == ROLE_COMBAT)
                    cost.add(BAND_GOAL, -500); // a combat dragon closing in: wanted
                else if (!h.mine)
                    cost.add(BAND_SAFETY, -THREAT_PEN);
                else
                    cost.add(BAND_SAFETY, -(my.leader || amQueen ? 4000 : TEAM_ADJ_PEN)); // two heads side by side can meet
            }
            if (amQueen && QUEEN_THREAT_MAP)
            {
                double const tp = queen_threat_pen(cd.dest), mp = queen_margin_pen(cd.dest);
                cost.add_split(-tp, BAND_LIFE, -(tp - mp), BAND_SAFETY); // in reach: life; just outside: safety
            }
            int esc2 = -1; // P3c: two-turn-safe cells she can reach next turn (-1: not checked)
            if (QUEEN_LOOK2 && amQueen && QUEEN_THREAT_MAP && look2Active && cd.dest >= 0 && !cd.nb1.empty())
            {
                esc2 = look2_safe_cells(cd.dest, cd.nb1, L + (cd.eat ? 1 : 0));
                cost.add(BAND_SAFETY, esc2 == 0 ? -LOOK2_PEN : LOOK2_CELL_BONUS * esc2);
            }
            if (QUEEN_HOME && amQueen && cd.dest >= 0 && cd.dest == nb(head, cd.d) && pocket_ahead(head, cd.dest))
                cost.add(BAND_LIFE, -QUEEN_TRAP_PEN); // a dead end: no pearl in it is worth the queen
            else if (TOPO_TRAP && amQueen && cd.dest >= 0 && topo_trap(head, cd.d, L + (cd.eat ? 1 : 0)))
                cost.add(BAND_LIFE, -QUEEN_TRAP_PEN); // 7b: a dead-end branch of any shape that cannot hold her
            // v2.14 pursuit: enemy heads move too. A cell with one way on is a
            // coffin once one comes; every way on within their next two steps
            // is no better. Near them, open water beats hugging a wall.
            if (PURSUIT_SAFE && amQueen && cd.dest >= 0 && cd.d != bunkerDir)
            {
                int nearE = INF, nearAny = INF;
                for (auto const& h : heads)
                    if (h.id != me)
                    {
                        nearAny = std::min(nearAny, manhattan(h.cell, cd.dest));
                        if (!h.mine)
                            nearE = std::min(nearE, manhattan(h.cell, cd.dest));
                    }
                // Teammates move too: one stepping into her last way on boxes her
                // in, and in a 1-wide corridor one coming the other way does.
                if (open_sides(cd.dest) <= 2 && open_sides(head) >= 3)
                    cost.add(BAND_SAFETY, -(nearAny <= 6 ? CORR_ENTER_PEN : CORR_ENTER_PEN / 5));
                {
                    int ex = 0, exHit = 0;
                    int const nl = L + (cd.eat ? 1 : 0);
                    for (int d2 = 0; d2 < 4; d2++)
                    {
                        int const n = cross(cd.dest, d2);
                        if (n < 0 || n == head)
                            continue;
                        bool blocked = vis[n] && occId[n] >= 0 && occId[n] != me;
                        for (int i = 0; i + 1 < nl && i < static_cast<int>(cd.nb1.size()); i++)
                            if (cd.nb1[i] == n)
                                blocked = true;
                        if (blocked)
                            continue;
                        ex++;
                        for (auto const& h : heads)
                            if (!h.mine && manhattan(h.cell, n) <= 2 * std::max(1, (std::max(h.len, h.estLen) + 3) / 4))
                            {
                                exHit++;
                                break;
                            }
                    }
                    if (LOOK2_SKIP_CORNER && esc2 > 0)
                        ; // P3c: a two-turn-safe way on exists - the corner rules are only proxies for that
                    else if (ex <= 1)
                        cost.add(BAND_SAFETY, -((nearE <= PURSUIT_DIST || nearAny <= 3) ? CORNER_PEN : CORNER_PEN / 5));
                    else if (exHit >= ex && nearE <= PURSUIT_DIST)
                        cost.add(BAND_SAFETY, -ALL_EXITS_PEN);
                    if (nearE <= 3)
                        cost.add(BAND_SAFETY, QUEEN_MOBILITY * ex);
                }
            }
            if (cd.noLoop && cd.d != bunkerDir)
                cost.add(BAND_SAFETY, -(queenStarving ? 50000 : QUEEN_LOOP_PEN));
            if (amQueen && queenSealed && cd.eat && L + 1 > sealedCells - 1)
                cost.add(BAND_LIFE, -2e6); // eating would fill the sealed pocket: no free cell left to move into
            if (amQueen && !vis[cd.dest])
                cost.add(BAND_SAFETY, -(queenStarving ? 1e5 : 5e5)); // a portal whose far side we cannot see: only as a last resort
            if (amQueen && QUEEN_AVOID_PORTAL && !queenStarving)
            {
                std::uint8_t const et = edgeType[edge_code(head, cd.d)];
                if (et != E_OPEN && et != E_KELP && et != E_UNK)
                    cost.add(BAND_SAFETY, -1e6); // crossing a portal: never by choice
                for (int k = 0; k < 4; k++)
                {
                    std::uint8_t const t2 = edgeType[edge_code(cd.dest, k)];
                    if (t2 != E_OPEN && t2 != E_KELP && t2 != E_UNK)
                        cost.add(BAND_SAFETY, -150000); // next to a portal mouth
                }
            }
            if (amQueen && QUEEN_AVOID_UNKNOWN && !queenStarving)
            {
                for (int k = 0; k < 4; k++)
                    if (edgeType[edge_code(cd.dest, k)] == E_UNK)
                        cost.add(BAND_SAFETY, -30000);
                if (seenRound[cd.dest] < 0)
                    cost.add(BAND_SAFETY, -200000);
            }
            if (!amQueen && !vis[cd.dest])
            {
                // Blind portal exit: we cannot see who stands there. Never risk
                // landing on the queen, who moves first and may be right there.
                cost.add(BAND_GOAL, -3000);
                auto const q = fleet.find(queenId);
                if (q != fleet.end() && R - q->second.round <= 3 && manhattan(q->second.cell, cd.dest) <= 2 + (R - q->second.round))
                    cost.add(BAND_LIFE, -2e6); // could land on our queen
            }
            if (amQueen) // stay clear of where the fleet saw enemies recently, even out of view
                for (auto const& e : enemyReps)
                {
                    if (R - e.round > 8)
                        continue;
                    int const dd = manhattan(e.cell, cd.dest);
                    if (dd <= 7)
                        cost.add(BAND_SAFETY, -(4000.0 * (8 - dd)));
                }
            if (amQueen) // keep the queen well away from enemy heads, not just out of reach
                for (auto const& h : heads)
                    if (!h.mine)
                    {
                        int const dd = manhattan(h.cell, cd.dest);
                        if (dd <= 5)
                            cost.add(BAND_SAFETY, -(25000.0 * (6 - dd)));
                    }
            if (my.leader) // an enemy that splits sends a new head out of its tail
                for (int tc : enemyTails)
                    if (manhattan(tc, cd.dest) <= 1)
                        cost.add(BAND_SAFETY, -(amQueen ? 400000 : 100000));
            for (auto const& dg : dangers)
                if (R - dg.round <= 30 && manhattan(dg.cell, cd.dest) <= 3)
                    cost.add(BAND_STYLE, -(my.leader ? 3000 : 300));
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
                    double const mult = h.id == enemyQueenId ? 3.0 : 1.0;
                    if (earea <= h.len + 1)
                        cost.add(BAND_STYLE, 3500 * mult);
                    else
                        cost.add(BAND_STYLE, mult * 1500.0 * std::max(0.0, 1.0 - static_cast<double>(earea) / ecap));
                }
            if (!my.leader && why != I_FEED)
                for (int tc : teamCells)
                    cost.add(BAND_STYLE, -(150.0 / (1 + manhattan(tc, cd.dest))));
            if (PORTAL_ROW && cd.dest >= 0 && cd.dest != nb(head, cd.d))
            {
                // Through a portal: a teammate whose head is at (or next to) the
                // far end may be stepping the other way this very round, and
                // the two heads would meet. The lower id goes first.
                auto clash = [&](int id, int cell) {
                    int const dd = manhattan(cell, cd.dest);
                    return dd == 0 || (dd == 1 && id < me);
                };
                bool hit = false;
                for (auto const& h : heads)
                    if (h.mine && h.id != me && clash(h.id, h.cell))
                        hit = true;
                for (auto const& [id, st] : fleet)
                    if (id != me && R - st.round <= 1 && clash(id, st.cell))
                        hit = true;
                if (hit)
                    cost.add(BAND_LIFE, -PORTAL_ROW_PEN);
            }
            if (eqHead >= 0 && !cd.nb1.empty())
            {
                int const after = queen_exits(eqHead, cd.nb1);
                cost.add(BAND_STYLE, BOX_BONUS * (eqExitsNow - after));
                if (after == 0)
                    cost.add(BAND_STYLE, BOX_BONUS); // no way out: she must split or die
            }
            if (bunkerFresh && cd.dest >= 0 && std::find(qBox.begin(), qBox.end(), cd.dest) != qBox.end())
                cost.add(BAND_LIFE, -QUEEN_EXIT_PEN); // her bunker box is hers alone
            if (qCorr >= 0 && cd.dest >= 0 && open_sides(cd.dest) <= 2 && manhattan(cd.dest, qCorr) <= manhattan(head, qCorr) &&
                corridor_leads_to(cd.dest, qCorr, 12))
                cost.add(BAND_LIFE, -QUEEN_EXIT_PEN); // never walk into her corridor towards her: she cannot turn round there
            bool const onPost = bunkerFresh && why == I_POST; // her way out is her box loop, which nobody enters
            if (QUEEN_EXIT_GUARD && !amQueen && !onPost && !cd.nb1.empty()) // feeders too: they can give their pearls from 4 cells away
                for (auto const& h : heads)
                    if (h.mine && h.id == queenId && manhattan(h.cell, cd.dest) <= 2)
                    {
                        std::vector<int> nowBody(body.begin(), body.begin() + std::min<int>(body.size(), L));
                        std::vector<int> after(cd.nb1.begin(), cd.nb1.begin() + std::min<int>(cd.nb1.size(), L + (cd.eat ? 1 : 0)));
                        int const before = queen_exits(h.cell, nowBody), now = queen_exits(h.cell, after);
                        if (now < before && now < 2)
                            cost.add(BAND_LIFE, -QUEEN_EXIT_PEN); // never take one of the queen's last ways out
                    }
            if (!amQueen && !onPost && (why != I_FEED || sit.consolidate))
                for (auto const& h : heads)
                    if (h.mine && h.id == queenId)
                    {
                        // Do not box the queen in: her own body is mostly out of
                        // our view, so keep clear of her head instead.
                        int const dq = manhattan(h.cell, cd.dest);
                        if (dq <= 2)
                            cost.add(BAND_SAFETY, -QUEEN_SPACE_PEN);
                        else if (dq == 3)
                            cost.add(BAND_SAFETY, -(QUEEN_SPACE_PEN / 3.5));
                    }
            if (cd.d == facing)
                cost.add(BAND_STYLE, 3);
        }
        cd.score = cost.total;
        cd.bands = cost.band;
        // 6a: compare by band so no amount of a lower band can outweigh a higher one. BAND_COMPARE 1: life,
        // then safety, then goal + style. 2: life, then the plain sum of the rest. 0: one plain sum (v2.16).
        bool better = cd.score > bestScore;
        if (BAND_COMPARE == 2 && bestD >= 0)
        {
            auto const& o = cands[bestD].bands;
            auto const& b = cd.bands;
            better = b[BAND_LIFE] != o[BAND_LIFE] ? b[BAND_LIFE] > o[BAND_LIFE] : cd.score - b[BAND_LIFE] > bestScore - o[BAND_LIFE];
        }
        else if (BAND_COMPARE == 1 && bestD >= 0)
        {
            auto const& o = cands[bestD].bands;
            auto const& b = cd.bands;
            better = b[BAND_LIFE] != o[BAND_LIFE]     ? b[BAND_LIFE] > o[BAND_LIFE]
                     : b[BAND_SAFETY] != o[BAND_SAFETY] ? b[BAND_SAFETY] > o[BAND_SAFETY]
                                                        : b[BAND_GOAL] + b[BAND_STYLE] > o[BAND_GOAL] + o[BAND_STYLE];
        }
        if (better)
        {
            bestScore = cd.score;
            bestD = cd.d;
        }
#ifdef BAND_DIAG
        if (plainD < 0 || cd.score > cands[plainD].score)
            plainD = cd.d;
#endif
    }
#ifdef BAND_DIAG
    // Debug: turns where the band comparison chose another move than the plain sum (FLEET_DIAG=<file>).
    if (char const* df = std::getenv("FLEET_DIAG"); df && plainD >= 0 && plainD != bestD)
        if (std::FILE* f = std::fopen(df, "a"))
        {
            auto const& a = cands[bestD].bands;
            auto const& b = cands[plainD].bands;
            std::fprintf(f, "%d %d q%d L%d chose %c A%.0f B%.0f C%.0f D%.0f | sum %c A%.0f B%.0f C%.0f D%.0f | %s\n", R, me, amQueen ? 1 : 0, L,
                         DCH[bestD], a[0], a[1], a[2], a[3], DCH[plainD], b[0], b[1], b[2], b[3], intent_name(why));
            std::fclose(f);
        }
#endif
    // R8 / R1: the queen's contact search (contact.hpp) when an enemy is close and the move is hard.
    if (amQueen && QUEEN_THREAT_MAP && !bunkerMode && bestD >= 0)
    {
        bool inRange = false;
        int close6 = 0;
        for (auto const& h : heads)
            if (!h.mine)
            {
                int const el = std::max(h.len, h.estLen);
                int const f = std::max(1, (el + 3) / 4);
                int const m = std::min(PAID_REACH_CAP, f + (PAID_REACH ? std::max(0, el - 2) : 0));
                inRange = inRange || manhattan(h.cell, head) <= std::max(f, m) + CS_TRIG_EXTRA;
                close6 += manhattan(h.cell, head) <= 6;
            }
        if (inRange)
        {
            int safe = 0;
            double s1 = -1e18, s2 = -1e18;
            for (auto const& cd : cands)
                if (cd.legal)
                {
                    safe += !cd.blind && cd.dest >= 0 && cd.ok1 && cd.ok2 && qThreat[cd.dest] >= INF;
                    if (cd.score > s1)
                    {
                        s2 = s1;
                        s1 = cd.score;
                    }
                    else if (cd.score > s2)
                        s2 = cd.score;
                }
            double const margin = s2 > -1e17 ? s1 - s2 : 1e18;
            bool const trig = safe <= 1 || close6 >= 2 || margin < CS_MARGIN;
            [[maybe_unused]] int const heurD = bestD;
            int searchD = -1;
            CsResult cr;
            if (trig && CONTACT_SEARCH)
            {
                cr = contact_search(cands, head);
                set_body(body, L);
                if (cr.ran)
                {
                    for (auto const& cd : cands)
                        if (cr.has[cd.d] && (searchD < 0 || cr.val[cd.d] > cr.val[searchD]))
                            searchD = cd.d;
                    if (CONTACT_SEARCH == 1 && BAND_COMPARE == 0)
                    {
                        bestScore = -1e18;
                        for (auto& cd : cands)
                        {
                            if (!cd.legal)
                                continue;
                            if (cr.has[cd.d])
                            {
                                double const add = CS_ALPHA * CS_SCALE * cr.val[cd.d];
                                cd.score += add;
                                cd.bands[BAND_SAFETY] += add;
                            }
                            if (cd.score > bestScore)
                            {
                                bestScore = cd.score;
                                bestD = cd.d;
                            }
                        }
                    }
                }
            }
#ifdef QDIAG
            // R9: one line per contact turn - trigger, heuristic / search / final choice, search values, effort.
            if (char const* df = std::getenv("FLEET_DIAG"))
                if (std::FILE* f = std::fopen(df, "a"))
                {
                    std::fprintf(f, "C %c %d L%d trig%d safe%d close%d margin%.0f heur%c search%c final%c ran%d abort%d work%d |", myTeam == Team::A ? 'A' : 'B', R, L,
                                 trig ? 1 : 0, safe, close6, std::min(margin, 9e9), DCH[heurD], searchD >= 0 ? DCH[searchD] : '-', DCH[bestD], cr.ran ? 1 : 0,
                                 cr.aborted ? 1 : 0, csWork);
                    for (auto const& cd : cands)
                        if (cr.has[cd.d])
                            std::fprintf(f, " %c:%.2f", DCH[cd.d], cr.val[cd.d]);
                    std::fprintf(f, "\n");
                    std::fclose(f);
                }
#endif
        }
    }
    set_body(body, L);
    if (bestD < 0)
        bestD = facing;
#ifdef QDEBUG
    if (amQueen)
    {
        int nearE = INF;
        for (auto const& h : heads)
            if (!h.mine)
                nearE = std::min(nearE, manhattan(h.cell, head));
        if (nearE <= 8)
        {
            std::string dbg = std::string("QDBG ") + intent_name(why) + " best" + DCH[bestD];
            for (auto const& cd : cands)
            {
                dbg += std::string(" ") + DCH[cd.d] + (cd.legal ? "" : "x");
                if (!cd.legal || cd.dest < 0)
                    continue;
                dbg += "@" + std::to_string(cd.dest % W) + "," + std::to_string(cd.dest / W) + "s" + std::to_string(static_cast<long long>(cd.score)) + "[A" + std::to_string(static_cast<long long>(cd.bands[BAND_LIFE])) +
                       " B" + std::to_string(static_cast<long long>(cd.bands[BAND_SAFETY])) + "]" +
                       "t" + (qThreat[cd.dest] >= INF ? std::string("-") : std::to_string(qThreat[cd.dest])) + "o" + std::to_string(cd.ok1) +
                       std::to_string(cd.ok2) + "a" + std::to_string(cd.area);
            }
            for (auto const& h : heads)
                if (!h.mine)
                    dbg += " E" + std::to_string(h.id) + "@" + std::to_string(h.cell % W) + "," + std::to_string(h.cell / W) + "l" +
                           std::to_string(h.len) + "/" + std::to_string(h.estLen);
            ct.output_log(dbg);
        }
    }
#endif
    return false;
}

// Act: give up a pearl behind a bad dead end, extend the move with free steps, and move.
bool finish_move(Controller& ct)
{
    auto& head = tn.head;
    auto& cands = tn.cands;
    auto& diveMin = tn.diveMin;
    auto& freeN = tn.freeN;
    auto& pearls = tn.pearls;
    auto& target = tn.target;
    auto& portalDir = tn.portalDir;
    auto& why = tn.why;
    auto& pi = tn.pi;
    auto& campWait = tn.campWait;
    auto& bestD = tn.bestD;
    // The way to the pearl only leads into a dead end that does not pay:
    // stop circling outside and let the matching pick another pearl.
    if (pi >= 0 && target == pearls[pi].cell && distHead[target] < INF)
    {
        bool closer = false, closerOk = false;
        for (auto const& cd : cands)
            if (cd.legal && !cd.blind && cd.dt < distHead[target])
            {
                closer = true;
                closerOk |= cd.ok1 || cd.dive >= diveMin;
            }
        if (closer && !closerOk)
            skipPearl[target] = R + SKIP_PEARL_ROUNDS;
    }

    // Free steps: keep going along the chosen direction while it pays (more
    // pearls, more progress) and the turn still ends somewhere safe.
    Cand const& c1 = cands[bestD];
    if (DEADEND_STRICT && !amQueen && c1.legal && !c1.blind && !c1.ok1 && c1.dive >= std::max(diveMin, 1.0))
        miningTrip = true; // going in for pearls that pay for the stub: no split until the bottom
    // A paid step wins a pearl race an enemy head would otherwise win
    // (theirs do this constantly); never for the queen.
    bool paidRace = false;
    if (PAID_PEARL_RACE && !amQueen && !sit.survival && L >= 3 && why == I_PEARL && target >= 0 && distHead[target] == 2)
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, target) <= 2)
                paidRace = true;
    if ((freeN >= 2 || paidRace) && c1.legal && !c1.blind && c1.ok2 && target >= 0 && portalDir < 0 && !c1.nb1.empty() &&
        !(amQueen && SURVIVAL && sit.survival) && !campWait)
    {
        stamp++; // distances to the goal ignore our own body
        plan_bfs(target, distGoal);
        set_body(body, L);
        int const g0 = distGoal[head];
        bool const useGoal = g0 < INF;
        int const nl = L + (c1.eat ? 1 : 0);
        int const cap1 = std::min(N, 3 * nl + 24);
        double const v1 = (c1.eat ? 3000.0 : 0.0) + std::min(c1.area, cap1) * 15.0 - end_threat(c1.dest) +
                          (useGoal && distGoal[c1.dest] < INF ? 150.0 * (g0 - distGoal[c1.dest]) : 0.0);
        SprintPlan sp;
        std::vector<int> dirs{bestD}, eatenCells;
        if (c1.eat)
            eatenCells.push_back(c1.dest);
        sprint_dfs(c1.nb1, nl, 1, paidRace ? std::max(freeN, 2) : freeN, freeN, c1.eat ? 1 : 0, dirs, eatenCells, useGoal, g0, sp);
        set_body(body, L);
        bool sprintOk = sp.kill || (sp.value > v1 + 300 && sp.dirs.size() >= 2);
        if (paidRace && !sp.kill && sp.dirs.size() >= 2)
        {
            // Only when the sprint really lands on the raced pearl (the paid
            // step costs more than one pearl is worth in the plan's own score).
            int c = head;
            bool hits = false;
            for (int d : sp.dirs)
            {
                c = c >= 0 ? cross(c, d) : -1;
                hits |= c == target;
            }
            sprintOk = sprintOk || (hits && sp.value > v1 - 1500);
        }
        if (sprintOk && amQueen && !sp.kill)
        {
            int el = 0;
            std::vector<int> const endBody = simulate_steps(body, L, sp.dirs, el);
            sprintOk = !endBody.empty() && queen_can_survive(endBody, el) &&
                       !(QUEEN_LOOP_SAFE && !loop_reachable(endBody, el));
            if (sprintOk && PURSUIT_SAFE)
            {
                int const e = endBody[0];
                int nearE = INF;
                for (auto const& h : heads)
                    if (!h.mine)
                        nearE = std::min(nearE, manhattan(h.cell, e));
                if (nearE <= PURSUIT_DIST + 1)
                {
                    int ex = 0;
                    for (int d2 = 0; d2 < 4; d2++)
                    {
                        int const n = cross(e, d2);
                        if (n < 0)
                            continue;
                        bool blocked = vis[n] && occId[n] >= 0 && occId[n] != me;
                        for (int i = 0; i + 1 < el && i < static_cast<int>(endBody.size()); i++)
                            if (endBody[i] == n)
                                blocked = true;
                        ex += !blocked;
                    }
                    if (ex <= 1)
                        sprintOk = false; // a corner with an enemy near: one step and she is boxed
                }
            }
            set_body(body, L);
        }
        if (sprintOk)
            return act_moves(ct, sp.dirs, sp.kill ? "regicide" : intent_name(why));
    }
    act_move(ct, bestD, intent_name(why));
    return true;
}

// One turn: analyse, then the first reflex that fires, else a target, then feed / split / move.
using Stage = bool (*)(Controller&);

void decide(Controller& ct)
{
    tn = Turn{};
    prepare_turn(ct);
    analyse_moves(ct);
    read_contact(ct);
    // Reflexes, in priority order: the first that acts ends the turn.
    static Stage const reflexes[] = {
        rx_regicide_paid,
        rx_escort_hit,
        rx_snipe,
        rx_pack_strike,
        rx_regicide_sprint,
        rx_intercept,
        rx_queen_paid_escape,
        rx_no_legal_move,
        rx_escape_sprint,
        rx_trap_split,
        rx_insure_split,
        rx_recycle,
        rx_attack,
    };
    for (Stage rx : reflexes)
        if (rx(ct))
            return;
    // Target: each step may keep or replace the goal so far (order matters); guards may act.
    static Stage const targetSteps[] = {
        tg_bunker_watch,
        tg_pearl,
        tg_queen_caution,
        tg_hunt,
        tg_hunt_queen,
        tg_cork,
        tg_feed,
        tg_pack,
        tg_wall,
        tg_guard,
        tg_rescue,
        tg_queen_bunker,
        tg_queen_rest,
        tg_portal,
        tg_push,
        tg_defend,
        tg_camp_open,
        tg_zone,
        tg_outpost,
        tg_explore,
        commit_target,
    };
    for (Stage tg : targetSteps)
        if (tg(ct))
            return;
    if (feed_sacrifice(ct) || plan_split(ct))
        return;
    score_moves(ct);
    finish_move(ct);
}

} // namespace

// ===== speak.hpp =====
// L7 SPEAK - the four sonar rays each turn: what to send and in which direction.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).

namespace {

// Sonar output (section 4): four messages a turn, N/E/S/W.
// Direction of a sonar ray that runs straight to our queen (same row or
// column, the shorter way round), or -1.
int queen_sonar_dir()
{
    if (me == queenId || sit.ourQueenDead || body.empty())
        return -1;
    auto const q = fleet.find(queenId);
    if (q == fleet.end() || R - q->second.round > 3)
        return -1;
    int const h = body[0], qc = q->second.cell;
    int const hx = h % W, hy = h / W, qx = qc % W, qy = qc / W;
    if (hy == qy && hx != qx)
        return ((qx - hx + W) % W) <= ((hx - qx + W) % W) ? 1 : 3;
    if (hx == qx && hy != qy)
        return ((qy - hy + H) % H) <= ((hy - qy + H) % H) ? 2 : 0;
    return -1;
}

void send_sonars(Controller& ct)
{
    std::vector<std::uint64_t> out;
    out.push_back(msg_status());
    if (QUEEN_POS_RELAY && amQueen)
        out.push_back(out[0]); // two directions a turn: every teammate in line hears where she is
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
    std::array<int, 4> dirOf{};
    for (int i = 0; i < 4; i++)
        dirOf[i] = (i + offset) & 3;
    // Aim the most urgent enemy news at the queen when she is on our row or
    // column (a ping stops at the first dragon it meets, so nearer is better).
    int const qd = AIM_REPORTS ? queen_sonar_dir() : -1;
    if (qd >= 0)
    {
        int best = -1;
        for (int i = 0; i < static_cast<int>(out.size()); i++)
        {
            int type;
            std::uint64_t payload;
            if (unpack(out[i], type, payload) && (type == T_ENEMY || type == T_DANGER))
            {
                best = i;
                break; // facts are already in priority order
            }
        }
        if (best >= 0)
            for (int i = 0; i < 4; i++)
                if (dirOf[i] == qd)
                {
                    if (i >= static_cast<int>(out.size()))
                        out.resize(i + 1, out[0]);
                    std::swap(out[i], out[best]);
                    break;
                }
    }
    for (int i = 0; i < static_cast<int>(out.size()) && i < 4; i++)
    {
        ct.send_sonar(dir_of(dirOf[i]), out[i]);
        if (traceF)
            traceAct += std::string(" ") + DCH[dirOf[i]] + std::to_string(out[i]);
    }
}

} // namespace


namespace {

// Fallback: any move that is not immediately fatal.
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
    // Boxed in: a survival split keeps the head (the child takes the tail).
    if (ct.can_split(2) && ct.get_length() >= 4)
    {
        ct.do_split(2);
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
    set_map_profile();
    me = ct.get_id();
    myTeam = ct.get_team().value;
    unitLimit = game.get_unit_limit();
    rng.seed(static_cast<std::uint32_t>(me) * 7919u + 17u);
    // Map files list team A's dragons first, so A usually owns id 0; vision and
    // sonar correct this as soon as either queen is seen or heard.
    queenId = me <= 1 ? me : (myTeam == Team::A ? 0 : 1);
    enemyQueenId = 1 - queenId;
    amQueen = me == queenId;
    my.role = role_for(me);

    edgeType.assign(2 * N, E_UNK);
    obsEdge.assign(2 * N, E_UNK);
    edgePid.assign(2 * N, -1);
    edgePartner.assign(2 * N, -1);
    seenRound.assign(N, -1);
    obsNext.assign(N, -1);
    nextSpawn.assign(N, -1);
    nextEst.assign(N, 0);
    pearlState.assign(N, 0);
    dropRound.assign(N, -1);
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
    distGoal.assign(N, INF);
    pathStamp.assign(N, 0);
    pathTime.assign(N, 0);
    skipPearl.assign(N, 0);
    planSteps.assign(N, 0);
    qThreat.assign(N, INF);
    qThreatVis.assign(N, INF);
    qThreatCnt.assign(N, 0);
    qThreat2.assign(N, INF);
    l2Stamp.assign(N, 0);
    l2Dist.assign(N, 0);
    qMargin.assign(N, INF);
    qDist.assign(N, INF);
    thStamp.assign(N, 0);
    thDist.assign(N, 0);
    thQ.reserve(N);
    bfsQ.reserve(N);
    bfsT.reserve(N);
}

} // namespace

int main()
{
    auto [ct, game] = unswbc::init();
    init_world(ct, game);
#ifdef MSG_SELFTEST
    if (char const* df = std::getenv("FLEET_DIAG"); df && me <= 1)
        if (std::FILE* f = std::fopen(df, "a"))
        {
            msg_selftest(f);
            std::fclose(f);
        }
#endif
    if (char const* dir = std::getenv("FLEET_TRACE"))
    {
        std::string const path = std::string(dir) + "/" + (myTeam == Team::A ? "A" : "B") + std::to_string(me) + ".txt";
        traceF = std::fopen(path.c_str(), "w");
    }

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
            strike_news_check();
            fleet[me] = {L, body[0], R, my.leader};
            compute_fleet_state();

            decide(ct);
#ifdef QDIAG
            // Debug (6d): who believes it holds a one-of-a-kind role this round.
            if (my.giant || my.heir || (my.leader && !amQueen))
                if (char const* df = std::getenv("FLEET_DIAG"))
                    if (std::FILE* f = std::fopen(df, "a"))
                    {
                        std::fprintf(f, "G %c %d id%d giant%d heir%d leader%d L%d gossip%d/%d/a%d claimsG%d betterG%d claimsL%d betterL%d\n", myTeam == Team::A ? 'A' : 'B', R, me,
                                     my.giant ? 1 : 0, my.heir ? 1 : 0, my.leader ? 1 : 0, L, gBest.gl, gBest.gh, R - gBest.born, static_cast<int>(roleClaims[CR_GIANT].size()),
                                     better_claims(CR_GIANT, L), static_cast<int>(roleClaims[CR_LEADER].size()), better_claims(CR_LEADER, L));
                        std::fclose(f);
                    }
            // Debug (item Q): every dragon notes the round it first learns their queen is dead.
            static bool eqKnown = false;
            if (sit.enemyQueenDead && !eqKnown)
            {
                eqKnown = true;
                if (char const* df = std::getenv("FLEET_DIAG"))
                    if (std::FILE* f = std::fopen(df, "a"))
                    {
                        std::fprintf(f, "EQ %c %d id%d queen%d\n", myTeam == Team::A ? 'A' : 'B', R, me, amQueen ? 1 : 0);
                        std::fclose(f);
                    }
            }
            // Debug (item Q): the queen's view whenever an enemy head is within 10, for the head-on death study.
            if (char const* df = std::getenv("FLEET_DIAG"); df && amQueen && !body.empty())
            {
                int nearE = INF;
                for (auto const& h : heads)
                    if (!h.mine)
                        nearE = std::min(nearE, manhattan(h.cell, body[0]));
                if (nearE <= 10 || getenv("QDIAG_ALL"))
                    if (std::FILE* f = std::fopen(df, "a"))
                    {
                        std::fprintf(f, "Q %c %d L%d @%d,%d note=%s anySafe=%d eqDead=%d eqSeen=%d |", myTeam == Team::A ? 'A' : 'B', R, L, body[0] % W,
                                     body[0] / W, note.c_str(), tn.anySafe ? 1 : 0, sit.enemyQueenDead ? 1 : 0, R - sit.lastEqSeenR);
                        for (auto const& cd : tn.cands)
                        {
                            if (!cd.legal || cd.dest < 0)
                            {
                                std::fprintf(f, " %c:x", DCH[cd.d]);
                                continue;
                            }
                            std::fprintf(f, " %c:%d,%d t%d t2%d ok%d%d A%.0f B%.0f", DCH[cd.d], cd.dest % W, cd.dest / W,
                                         qThreat[cd.dest] >= INF ? -1 : qThreat[cd.dest], qThreat2[cd.dest] >= INF ? -1 : 1,
                                         cd.ok1 ? 1 : 0, cd.ok2 ? 1 : 0, cd.bands[0], cd.bands[1]);
                        }
                        std::fprintf(f, " | E");
                        for (auto const& h : heads)
                            if (!h.mine)
                                std::fprintf(f, " %d@%d,%d l%d/%d d%d", h.id, h.cell % W, h.cell / W, h.len, h.estLen, manhattan(h.cell, body[0]));
                        std::fprintf(f, " | rep");
                        for (auto const& e : enemyReps)
                            if (R - e.round <= 3)
                                std::fprintf(f, " %d@%d,%d l%d a%d", e.id, e.cell % W, e.cell / W, std::max(e.len, e.estLen), R - e.round);
                        std::fprintf(f, "\n");
                        std::fclose(f);
                    }
            }
#endif
#ifdef TOPO_DIAG
            if (char const* df = std::getenv("FLEET_DIAG"); df && amQueen && R == TOPO_DIAG)
                if (std::FILE* f = std::fopen(df, "a"))
                {
                    topo_render(f);
                    std::fclose(f);
                }
#endif
            acted = true;
            send_sonars(ct);

            static char const* const roleName[] = {"col", "exp", "prt", "cmb"};
            static char const* const modeName[] = {"nrm", "farm", "keep", "war"};
            std::string ind = std::string(amQueen ? "QUEEN " : my.leader ? "LEAD " : "") + roleName[my.role] + " " + modeName[sit.mode] + " " + note +
                              " L" + std::to_string(L);
            if (symKnown >= 0)
                ind += " s" + std::to_string(symKnown);
            if (my.screen || my.guard)
                ind += my.screen ? " SCR" : " GRD";
            if (my.wall)
                ind += " WAL";
            if (LOG_PHASE)
                ind += " P" + std::to_string(sit.pressureMax) + (sit.switched ? "*" : "");
            ct.set_indicator_string(ind);
        }
        catch (...)
        {
            if (!acted)
                fallback_move(ct);
        }
        if (traceF)
        {
            std::fprintf(traceF, "%d %d %d %d %s | %s\n", R, me, L, body.empty() ? -1 : body[0], traceAct.c_str(), note.c_str());
            std::fflush(traceF);
            traceAct = "fallback";
        }
        unswbc::end_turn();
    }
}

