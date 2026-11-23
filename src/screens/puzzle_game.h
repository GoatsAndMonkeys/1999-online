// puzzle_game.h — Bit Quest rules and levels, no LVGL (puzzle_screen.cpp draws
// it; a scratch solver can include this header on its own to check levels).
//
// Bit, a courier robot inside a 1999 PC, collects every data packet on a board
// and then docks at the modem before the Y2K clock runs out.
//
// The game is a grid of tiles (one char each, see Tile) plus the player and
// the bugs. Time moves in world ticks (kTickMs): a player on a data bus is
// carried one cell per tick; bugs move every kBugEvery ticks. The player moves
// whenever a key arrives, so the solver's "one move or a wait per tick" is a
// subset of what a person can do.
//
//   ' '  floor (circuit board)        '#'  wall (chip)
//   '*'  data packet                  'E'  modem exit (opens with 0 packets left)
//   'r' 'g' 'b'  keycards             'R' 'G' 'B'  firewall doors (eat one card)
//   'o'  floppy disk: pushable; pushed into a short it patches it (both go)
//   '~'  short circuit: fries Bit
//   '<' '>' '^' 'v'  data bus: carries Bit one cell per tick
//   'T'  port: entering one jumps Bit to the other port of the level
//   '@'  Bit's start (floor)          'h' / 'k'  bug, patrolling across / down
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace bitquest {

constexpr int kTickMs = 120;     // one world tick
constexpr int kBugEvery = 3;     // bugs step every 3rd tick (360 ms)

enum Dir : uint8_t { kUp, kDown, kLeft, kRight };
constexpr int kDx[4] = {0, 0, -1, 1};
constexpr int kDy[4] = {-1, 1, 0, 0};
inline int reverse(int d) { return d ^ 1; }

enum class Outcome : uint8_t { Playing, Won, Fried, Bitten, Timeout };

// Why the last move was refused (the screen shows a line for it).
enum class Note : uint8_t { None, NeedKey, ExitLocked, Packet, Card, Door, Patched, Port };

struct Level {
    const char* name;
    const char* hint;     // shown when the level starts
    int seconds;          // Y2K clock
    const char* map;      // rows separated by '\n', all the same width
};

struct Bug {
    int x = 0, y = 0;
    int dir = kRight;
};

inline int key_index(char c) {
    switch (c) {
        case 'r': case 'R': return 0;
        case 'g': case 'G': return 1;
        case 'b': case 'B': return 2;
        default: return -1;
    }
}

inline int bus_dir(char c) {
    switch (c) {
        case '^': return kUp;
        case 'v': return kDown;
        case '<': return kLeft;
        case '>': return kRight;
        default: return -1;
    }
}

class Game {
public:
    int w = 0, h = 0;
    std::string grid;             // w*h tiles
    int px = 0, py = 0;
    int facing = kDown;
    int keys[3] = {0, 0, 0};
    int packets_left = 0;
    int packets_total = 0;
    std::vector<Bug> bugs;
    Outcome outcome = Outcome::Playing;
    Note note = Note::None;
    uint32_t ticks = 0;
    bool bus_stuck = false;       // the bus could not carry Bit last tick

    void load(const Level& lv) {
        *this = Game{};
        const std::string m = lv.map;
        std::vector<std::string> rows;
        size_t at = 0;
        while (at <= m.size()) {
            const size_t nl = m.find('\n', at);
            const std::string row = m.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            if (!row.empty()) rows.push_back(row);
            if (nl == std::string::npos) break;
            at = nl + 1;
        }
        h = static_cast<int>(rows.size());
        for (const auto& r : rows) w = std::max(w, static_cast<int>(r.size()));
        grid.assign(static_cast<size_t>(w) * h, '#');
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < static_cast<int>(rows[y].size()); ++x) {
                char ch = rows[y][x];
                if (ch == '@') { px = x; py = y; ch = ' '; }
                else if (ch == 'h' || ch == 'k') {
                    bugs.push_back(Bug{x, y, ch == 'h' ? kRight : kDown});
                    ch = ' ';
                }
                else if (ch == '*') ++packets_total;
                at_(x, y) = ch;
            }
        }
        packets_left = packets_total;
    }

    char at(int x, int y) const {
        if (x < 0 || y < 0 || x >= w || y >= h) return '#';
        return grid[static_cast<size_t>(y) * w + x];
    }
    bool bug_at(int x, int y) const {
        for (const Bug& b : bugs) if (b.x == x && b.y == y) return true;
        return false;
    }
    bool on_bus() const { return bus_dir(at(px, py)) >= 0; }
    // Arrow keys steer only off a bus (or when the bus is blocked).
    bool can_steer() const { return outcome == Outcome::Playing && (!on_bus() || bus_stuck); }

    // Player input. true if Bit moved.
    bool move(int dir) {
        if (!can_steer()) return false;
        facing = dir;
        return step(dir);
    }

    // One world tick: the bus carries Bit, then (every kBugEvery) the bugs crawl.
    void tick() {
        if (outcome != Outcome::Playing) return;
        ++ticks;
        if (on_bus()) {
            const int d = bus_dir(at(px, py));
            bus_stuck = !step(d);
            if (outcome != Outcome::Playing) return;
        } else {
            bus_stuck = false;
        }
        if (ticks % kBugEvery == 0) move_bugs();
    }

private:
    char& at_(int x, int y) { return grid[static_cast<size_t>(y) * w + x]; }

    bool bug_can_enter(int x, int y) const {
        const char t = at(x, y);
        return (t == ' ' || bus_dir(t) >= 0) && !bug_at(x, y);
    }

    void move_bugs() {
        for (Bug& b : bugs) {
            for (int attempt = 0; attempt < 2; ++attempt) {
                const int nx = b.x + kDx[b.dir], ny = b.y + kDy[b.dir];
                if (bug_can_enter(nx, ny)) {
                    b.x = nx;
                    b.y = ny;
                    break;
                }
                b.dir = reverse(b.dir);
            }
            if (b.x == px && b.y == py) outcome = Outcome::Bitten;
        }
    }

    // Bit tries to enter the next cell in `dir` (by key or by bus).
    bool step(int dir) {
        note = Note::None;
        const int nx = px + kDx[dir], ny = py + kDy[dir];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) return false;
        char& t = at_(nx, ny);
        switch (t) {
            case '#':
                return false;
            case 'R': case 'G': case 'B': {
                int& k = keys[key_index(t)];
                if (k == 0) { note = Note::NeedKey; return false; }
                --k;
                t = ' ';
                note = Note::Door;
                break;
            }
            case 'E':
                if (packets_left > 0) { note = Note::ExitLocked; return false; }
                break;
            case 'o': {
                const int bx = nx + kDx[dir], by = ny + kDy[dir];
                const char bt = at(bx, by);
                if (bug_at(bx, by)) return false;
                if (bt == ' ') at_(bx, by) = 'o';
                else if (bt == '~') { at_(bx, by) = ' '; note = Note::Patched; }
                else return false;
                t = ' ';
                break;
            }
            default:
                break;
        }
        px = nx;
        py = ny;
        switch (t) {
            case '*': --packets_left; t = ' '; note = Note::Packet; break;
            case 'r': case 'g': case 'b': ++keys[key_index(t)]; t = ' '; note = Note::Card; break;
            case '~': outcome = Outcome::Fried; return true;
            case 'E': outcome = Outcome::Won; return true;
            case 'T':
                for (int i = 0; i < w * h; ++i) {
                    const int ox = i % w, oy = i / w;
                    if (grid[i] == 'T' && (ox != px || oy != py) && !bug_at(ox, oy)) {
                        px = ox;
                        py = oy;
                        note = Note::Port;
                        break;
                    }
                }
                break;
            default: break;
        }
        if (bug_at(px, py)) outcome = Outcome::Bitten;
        return true;
    }
};

// ── Levels ──────────────────────────────────────────────────────────────────
// Every level is checked by a breadth-first solver (one move or a wait per
// tick) and its clock is ~2.5x the solver's best time or more.
inline const Level kLevels[] = {
    {"Boot Sector", "Grab every data packet, then dock at the modem.", 60,
     "###############\n"
     "#@    #      *#\n"
     "#  *  #  ###  #\n"
     "#     #    #  #\n"
     "###  ###   #  #\n"
     "#  *       #  #\n"
     "#  ######  #  #\n"
     "#        * #  #\n"
     "#  ####    #  #\n"
     "#     #      E#\n"
     "###############\n"},
    {"Access Denied", "Firewalls eat one matching keycard each.", 75,
     "#################\n"
     "#@   #     #   *#\n"
     "#    #  *  #    #\n"
     "# r  R  b  B  g #\n"
     "#    #     #    #\n"
     "#*   #  *  #   *#\n"
     "############G####\n"
     "#E              #\n"
     "#######*        #\n"
     "#################\n"},
    {"Disk Shuffle", "Floppies slide when pushed. Mind the corners.", 90,
     "###############\n"
     "#@ #     #   *#\n"
     "#  o  *  o    #\n"
     "#  #     ##o###\n"
     "## ###o###*  ##\n"
     "#*   o   o   E#\n"
     "###############\n"},
    {"Short Circuit", "Sparks fry robots. Push a floppy in to ground one.", 120,
     "###############\n"
     "# @ #   *   ~E#\n"
     "#o  #  ~~~  ~ #\n"
     "# o   ~~*~~ ~ #\n"
     "#   #  ~~~  ~ #\n"
     "#~~~#       ~ #\n"
     "#   # o     ~ #\n"
     "#*  #     o ~*#\n"
     "###############\n"},
    {"Data Bus", "Buses carry you to the end of the line. No turning back.", 90,
     "#################\n"
     "#@  #######  *  #\n"
     "#   >>>>v##     #\n"
     "# * ####v## *   #\n"
     "########v##^### #\n"
     "#*  ####v##^    #\n"
     "#   ^###>>>^ *  #\n"
     "#   ^########## #\n"
     "# * ^<<<<<<<<   #\n"
     "#E  #         * #\n"
     "#################\n"},
    {"Bug Report", "Bugs crawl back and forth. Packets block their way.", 90,
     "#################\n"
     "#@      #      *#\n"
     "# ##### # ##### #\n"
     "#*    h        *#\n"
     "### ####### #####\n"
     "#    *   h      #\n"
     "# ############# #\n"
     "#E  h  *        #\n"
     "#################\n"},
    {"Port Jump", "Step into a port to jump to its twin.", 100,
     "###############\n"
     "#@  #  *  #  *#\n"
     "#   #     #   #\n"
     "# T #  b  R T #\n"
     "#  r#     #   #\n"
     "#*  ##B####  *#\n"
     "#E  #  *  #   #\n"
     "###############\n"},
    {"Firewall", "Cards burn on use. Spend them wisely.", 120,
     "#################\n"
     "#@   #  *  #   *#\n"
     "#  r R  o  G  b #\n"
     "#    #     #    #\n"
     "##R###  o  ###B##\n"
     "#r *#   ~   #*  #\n"
     "# g #  ~*~  #   #\n"
     "#   B   ~   R  E#\n"
     "#################\n"},
    {"Overclock", "Buses, bugs and sparks. Plan the order before you ride.", 150,
     "###################\n"
     "#@   #  *    #   *#\n"
     "#    #h      #  o #\n"
     "#  o   * k   >v   #\n"
     "#~~~~#   *   #v   #\n"
     "#    #h      #v   #\n"
     "#*   #########v  ~#\n"
     "#    h   *   #  ~E#\n"
     "###################\n"},
    {"Y2K", "Last board before midnight. Everything you know, at once.", 180,
     "#####################\n"
     "#@  #  *     #     *#\n"
     "# r R      h G      #\n"
     "#   #  #######  ~~~b#\n"
     "# o #  #*k#### o~*~ #\n"
     "#   #  #  ####  ~~~T#\n"
     "#~~~#  #  ####v## ###\n"
     "#   #  ## ####v#    #\n"
     "#  T#      g #v#  * #\n"
     "#*   ####B####>>    #\n"
     "###       h        E#\n"
     "#####################"},
};

inline constexpr int kLevelCount = static_cast<int>(sizeof(kLevels) / sizeof(kLevels[0]));

} // namespace bitquest
