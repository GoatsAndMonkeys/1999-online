// puzzle_screen.cpp — Bit Quest: a top-down tile puzzle set inside a 1999 PC.
//
// Bit, a courier robot, collects every data packet on a board and docks at the
// modem before the Y2K clock hits zero. Rules and the ten boards live in
// puzzle_game.h (no LVGL, so a scratch solver can check every board).
//
// Layout (312x121 client): an 11x9-tile viewport of 13 px tiles on the left,
// one XRGB image redrawn from pixel-art sprites, centred on Bit and clamped to
// the board edges; a side panel on the right (board, clock, packets left,
// keycards, a message line). The title screen lists the boards; a board
// unlocks when the one before it is cleared.
//
// Keys: arrows (or F/Z/X/C) move, R restarts, Enter pauses, Esc goes back to
// the board list (and from there out of the game).
//
// Saved in settings.json (nothing in --shots):
//   bitquest.unlocked  int  boards open, 1..kLevelCount
//   bitquest.best.N    int  most seconds left when board N (1-based) was cleared
#include "screens.h"

#include "puzzle_game.h"
#include "router.h"
#include "settings.h"
#include "widgets.h"
#include "win.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace mm::ui {
namespace {

namespace bq = bitquest;

// ── Geometry ────────────────────────────────────────────────────────────────
constexpr int kTile = 13;
constexpr int kViewCols = 11, kViewRows = 9;
constexpr int kViewW = kTile * kViewCols;        // 143
constexpr int kViewH = kTile * kViewRows;        // 117
constexpr int kFrameW = kViewW + 4;              // sunken frame around the view
constexpr int kPanelX = kFrameW + 4;             // 151
constexpr int kPanelW = kClientW - kPanelX;      // 161

// ── Pixel art ───────────────────────────────────────────────────────────────
// Sprites are 13x13 strings; one char per pixel, '.' transparent.
uint32_t pal(char c) {
    switch (c) {
        case 'k': return 0x101014;
        case 'w': return 0xf4f4f4;
        case 'l': return 0xb8bec8;
        case 'm': return 0x80868f;
        case 'd': return 0x4c5058;
        case 'y': return 0xf8d830;
        case 'o': return 0xf08828;
        case 'c': return 0x58e8ff;
        case 'C': return 0x1890b8;
        case 'B': return 0x2c4c9c;
        case 'r': return 0xe83838;
        case 'R': return 0x881818;
        case 'e': return 0xd8cca8;       // modem beige
        case 'E': return 0xf4ecd4;
        case 'f': return 0x9c9070;
        default:  return 0xff00ff;
    }
}

using Art = std::array<const char*, kTile>;

const Art kArtWall = {
    "wwwwwwwwwwwwd",
    "wllllllllllmd",
    "wlmmmmmmmmlmd",
    "wlmkkkkkkmlmd",
    "wlmkddddkmlmd",
    "wlmkddddkmlmd",
    "wlmkkkkkkmlmd",
    "wlmmmmmmmmlmd",
    "wllllllllllmd",
    "wlmlmlmlmllmd",
    "wllllllllllmd",
    "wmmmmmmmmmmmd",
    "ddddddddddddd",
};
const Art kArtPacket = {
    ".............",
    "......c......",
    ".....cwc.....",
    "....cwccc....",
    "...cwccccc...",
    "..ccccccccC..",
    ".cccccccccCC.",
    "..cccccccCC..",
    "...CccccCC...",
    "....CccCC....",
    ".....CCC.....",
    "......C......",
    ".............",
};
// X / Z: card colour and its shade.
const Art kArtCard = {
    ".............",
    ".............",
    ".kkkkkkkkkkk.",
    ".kXXXXXXXXXk.",
    ".kkkkkkkkkkk.",
    ".kXXXXXXXXXk.",
    ".kXwwXXyyXXk.",
    ".kXXXXXyyXXk.",
    ".kXwwwwXXXXk.",
    ".kZZZZZZZZZk.",
    ".kkkkkkkkkkk.",
    ".............",
    ".............",
};
const Art kArtDoor = {
    "kkkkkkkkkkkkk",
    "kXXXXZkXXXXZk",
    "kZZZZZkZZZZZk",
    "kkkkkkkkkkkkk",
    "kXXZkwwwkXXZk",
    "kZZZkwkwkZZZk",
    "kkkwwwwwwwkkk",
    "kXXwwwkwwwXZk",
    "kZZwwwkwwwZZk",
    "kkkwwwwwwwkkk",
    "kXXXXZkXXXXZk",
    "kZZZZZkZZZZZk",
    "kkkkkkkkkkkkk",
};
const Art kArtFloppy = {
    "kkkkkkkkkkkkk",
    "kBBlllllllBBk",
    "kBBlldddllBBk",
    "kBBlldddllBBk",
    "kBBlllllllBBk",
    "kBBBBBBBBBBBk",
    "kBwwwwwwwwwBk",
    "kBwmmmmmmmwBk",
    "kBwwwwwwwwwBk",
    "kBwmmmmmwwwBk",
    "kBwwwwwwwwwBk",
    "kBBBBBBBBBBBk",
    "kkkkkkkkkkkkk",
};
// L: status LEDs (red while offline, green when the modem can dial).
const Art kArtModem = {
    ".............",
    ".............",
    ".............",
    "..kkkkkkkkk..",
    ".kEEEEEEEEEk.",
    "kEeeeeeeeeeek",
    "keLeLeLeeeeek",
    "keeeeeeeeeffk",
    "kfffffffffffk",
    "kkkkkkkkkkkkk",
    ".kk.......kk.",
    ".............",
    ".............",
};
const Art kArtBitDown = {
    "......y......",
    "......m......",
    "...kkkkkkk...",
    "..kwwwwwwwk..",
    "..kwkkkkkwk..",
    "..kwkcwckwk..",
    "..kwkkkkkwk..",
    "..kwwwwwwwk..",
    "...kkkkkkk...",
    "..kmlllllmk..",
    ".kmlloyollmk.",
    ".kmlllllllmk.",
    "..kk.....kk..",
};
const Art kArtBitUp = {
    "......y......",
    "......m......",
    "...kkkkkkk...",
    "..kwwwwwwwk..",
    "..kwwwwwwwk..",
    "..kwwmmmwwk..",
    "..kwwwwwwwk..",
    "..kwwwwwwwk..",
    "...kkkkkkk...",
    "..kmlllllmk..",
    ".kmllllllmmk.",
    ".kmlllllllmk.",
    "..kk.....kk..",
};
const Art kArtBitLeft = {
    "......y......",
    "......m......",
    "...kkkkkkk...",
    "..kwwwwwwwk..",
    "..kkkkkwwwk..",
    "..kcwckwwwk..",
    "..kkkkkwwwk..",
    "..kwwwwwwwk..",
    "...kkkkkkk...",
    "..kmlllllmk..",
    ".kmoylllllmk.",
    ".kmlllllllmk.",
    "..kk.....kk..",
};
const Art kArtBugA = {
    ".............",
    "...k.....k...",
    "....k...k....",
    ".....kkk.....",
    "..k.krrrk.k..",
    "...krwrwrk...",
    "..kRrrkrrRk..",
    ".k.kRrkrRk.k.",
    "...kRrkrRk...",
    "..k.kRkRk.k..",
    ".k...kkk...k.",
    ".............",
    ".............",
};
const Art kArtBugB = {
    ".............",
    "...k.....k...",
    "....k...k....",
    ".....kkk.....",
    ".k..krrrk..k.",
    "...krwrwrk...",
    "..kRrrkrrRk..",
    "..kkRrkrRkk..",
    "...kRrkrRk...",
    ".k..kRkRk..k.",
    "..k..kkk..k..",
    ".............",
    ".............",
};
const Art kArtSpark = {
    ".............",
    "...y.........",
    "....y....o...",
    "....yy..o....",
    ".....wy.o....",
    "...o..wyy....",
    "..o....wy....",
    ".......yw..o.",
    "......y..yo..",
    ".....y....y..",
    "....o.....y..",
    "..........o..",
    ".............",
};

enum SpriteId : int {
    S_WALL, S_PACKET, S_CARD0, S_CARD1, S_CARD2, S_DOOR0, S_DOOR1, S_DOOR2, S_FLOPPY,
    S_MODEM_OFF, S_MODEM_ON, S_BIT_DOWN, S_BIT_UP, S_BIT_LEFT, S_BIT_RIGHT,
    S_BUG_A, S_BUG_B, S_SPARK_A, S_SPARK_B, S_COUNT
};

struct Sprite {
    uint32_t px[kTile * kTile];   // ARGB, alpha 0 = transparent
    lv_image_dsc_t dsc;
};

// Card colours red / green / blue, each {colour, shade}.
constexpr uint32_t kCardCol[3][2] = {{0xe83838, 0x901c1c}, {0x38c858, 0x1c7830}, {0x3c78f0, 0x1c3c90}};

void make_sprite(Sprite& s, const Art& art, uint32_t x_col = 0, uint32_t z_col = 0,
                 uint32_t led = 0, bool mirror = false) {
    for (int y = 0; y < kTile; ++y) {
        for (int x = 0; x < kTile; ++x) {
            const char ch = art[y][mirror ? kTile - 1 - x : x];
            uint32_t v = 0;
            if (ch == 'X') v = x_col;
            else if (ch == 'Z') v = z_col;
            else if (ch == 'L') v = led;
            else if (ch != '.') v = pal(ch);
            s.px[y * kTile + x] = ch == '.' ? 0u : (0xff000000u | v);
        }
    }
    s.dsc = lv_image_dsc_t{};
    s.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    s.dsc.header.w = kTile;
    s.dsc.header.h = kTile;
    s.dsc.header.stride = kTile * 4;
    s.dsc.data = reinterpret_cast<const uint8_t*>(s.px);
    s.dsc.data_size = sizeof(s.px);
}

const Sprite* sprites() {
    static Sprite s[S_COUNT];
    static bool ready = false;
    if (!ready) {
        ready = true;
        make_sprite(s[S_WALL], kArtWall);
        make_sprite(s[S_PACKET], kArtPacket);
        for (int i = 0; i < 3; ++i) {
            make_sprite(s[S_CARD0 + i], kArtCard, kCardCol[i][0], kCardCol[i][1]);
            make_sprite(s[S_DOOR0 + i], kArtDoor, kCardCol[i][0], kCardCol[i][1]);
        }
        make_sprite(s[S_FLOPPY], kArtFloppy);
        make_sprite(s[S_MODEM_OFF], kArtModem, 0, 0, 0xc02818);
        make_sprite(s[S_MODEM_ON], kArtModem, 0, 0, 0x40ff60);
        make_sprite(s[S_BIT_DOWN], kArtBitDown);
        make_sprite(s[S_BIT_UP], kArtBitUp);
        make_sprite(s[S_BIT_LEFT], kArtBitLeft);
        make_sprite(s[S_BIT_RIGHT], kArtBitLeft, 0, 0, 0, true);
        make_sprite(s[S_BUG_A], kArtBugA);
        make_sprite(s[S_BUG_B], kArtBugB);
        make_sprite(s[S_SPARK_A], kArtSpark);
        make_sprite(s[S_SPARK_B], kArtSpark, 0, 0, 0, true);
    }
    return s;
}

// ── Canned states for --shots ───────────────────────────────────────────────
// Moves for Game::move ('U' 'D' 'L' 'R'), '.' = wait; one world tick after each.
constexpr const char* kDemoWinL1 = "DRRDDDRRRRRRDDUUUULUURRRRRDDDDDDDD";
constexpr int kDemoMidLevel = 8;   // Overclock
constexpr const char* kDemoMidMoves = "RRDRDRRRUURDDDRLULLLLLULDDDDLUDR.LD.R";

int to_dir(char ch) {
    switch (ch) {
        case 'U': return bq::kUp;
        case 'D': return bq::kDown;
        case 'L': return bq::kLeft;
        case 'R': return bq::kRight;
        default:  return -1;
    }
}

const char* const kCardName[3] = {"red", "green", "blue"};

std::string clock_text(int ms) {
    const int s = std::max(0, (ms + 999) / 1000);
    return fmt("%d:%02d", s / 60, s % 60);
}

class PuzzleScreen final : public Screen {
public:
    explicit PuzzleScreen(Ctx& ctx) : Screen(ctx) {
        unlocked_ = static_cast<int>(std::clamp<int64_t>(
            mm::settings().get_int("bitquest.unlocked", 1), 1, bq::kLevelCount));
        for (int i = 0; i < bq::kLevelCount; ++i) {
            best_[i] = static_cast<int>(mm::settings().get_int(fmt("bitquest.best.%d", i + 1), -1));
        }
        sel_ = std::min(unlocked_ - 1, bq::kLevelCount - 1);
    }

    const char* title() const override { return "Bit Quest"; }
    bool windowed() const override { return true; }

    std::string hints() const override {
        switch (mode_) {
            case Mode::Title: return "Up/Dn:board  Enter:play  Esc:quit";
            case Mode::Play:
                return paused_ ? "Enter:resume  R:restart  Esc:boards"
                               : "Arrows:move  R:restart  Enter:pause  Esc:boards";
            case Mode::Won:   return last_level() ? "Enter:boards  R:replay" : "Enter:next  R:replay  Esc:boards";
            case Mode::Lost:  return "Enter:retry  Esc:boards";
        }
        return "";
    }

    SoftKeys softkeys() const override {
        SoftKeys k;
        switch (mode_) {
            case Mode::Title:
                k.set(0, "PLAY", sel_ < unlocked_);
                k.left.text = MM_SOFT_BACK;
                break;
            case Mode::Won:
                k.set(0, last_level() ? "BOARDS" : "NEXT").set(1, "REPLAY");
                k.left.text = MM_SOFT_BACK;
                break;
            case Mode::Lost:
                k.set(0, "RETRY").set(1, "BOARDS");
                k.left.text = MM_SOFT_BACK;
                break;
            default: break;   // in play: the hint strip
        }
        return k;
    }

    int demo_state_count() const override { return 5; }
    const char* demo_state_name(int i) const override {
        static const char* const kNames[] = {"title", "level1", "midplay", "complete", "y2k"};
        return (i >= 0 && i < 5) ? kNames[i] : "main";
    }
    void set_demo_state(int i) override {
        demo_ = true;
        paused_ = false;
        unlocked_ = 4;
        for (int& b : best_) b = -1;
        best_[0] = 47;
        best_[1] = 52;
        best_[2] = 71;
        switch (i) {
            case 0:
                mode_ = Mode::Title;
                sel_ = 3;
                break;
            case 1:
                start_level(0);
                break;
            case 2:
                start_level(kDemoMidLevel);
                replay(kDemoMidMoves);
                elapsed_ms_ = 41300;
                break;
            case 3:
                start_level(0);
                replay(kDemoWinL1);
                elapsed_ms_ = 14200;
                if (game_.outcome != bq::Outcome::Won) game_.outcome = bq::Outcome::Won;
                finish();
                break;
            default:
                start_level(5);
                replay("DUDUDDRRDDRRLLUURRRR");
                elapsed_ms_ = level().seconds * 1000;
                game_.outcome = bq::Outcome::Timeout;
                finish();
                break;
        }
    }

    void build(lv_obj_t* body) override {
        clear_refs();
        if (mode_ == Mode::Title) build_title(body);
        else build_play(body);
    }

    void on_hide() override {
        if (mode_ == Mode::Play && game_.outcome == bq::Outcome::Playing) paused_ = true;
        if (!demo_) mm::settings().flush();
    }
    void on_show() override { last_ms_ = 0; }

    bool on_key(const KeyEvent& e) override {
        const int slot = softkey_slot(e);
        switch (mode_) {
            case Mode::Title: return key_title(e, slot);
            case Mode::Play:  return key_play(e);
            case Mode::Won:
            case Mode::Lost:  return key_result(e, slot);
        }
        return false;
    }

    void tick(uint32_t now_ms) override {
        if (demo_) return;
        const uint32_t dt = last_ms_ == 0 ? 0 : std::min<uint32_t>(now_ms - last_ms_, 250);
        last_ms_ = now_ms;
        if (mode_ != Mode::Play || paused_ || !view_img_) return;
        elapsed_ms_ += static_cast<int>(dt);
        acc_ms_ += static_cast<int>(dt);
        bool redraw = false;
        while (acc_ms_ >= bq::kTickMs && game_.outcome == bq::Outcome::Playing) {
            acc_ms_ -= bq::kTickMs;
            game_.tick();
            ++anim_;
            redraw = true;
        }
        if (game_.outcome == bq::Outcome::Playing && time_left_ms() <= 0) {
            game_.outcome = bq::Outcome::Timeout;
        }
        if (game_.outcome != bq::Outcome::Playing) {
            finish();
            ctx_.router.rebuild();
            return;
        }
        if (redraw) render_view();
        refresh_panel();
    }

private:
    enum class Mode : uint8_t { Title, Play, Won, Lost };

    const bq::Level& level() const { return bq::kLevels[level_]; }
    bool last_level() const { return level_ + 1 >= bq::kLevelCount; }
    int time_left_ms() const { return level().seconds * 1000 - elapsed_ms_; }

    void clear_refs() {
        view_img_ = nullptr;
        list_.reset();
        clock_lbl_ = packets_lbl_ = msg_lbl_ = nullptr;
        for (auto& p : card_img_) p = nullptr;
        for (auto& p : card_lbl_) p = nullptr;
        shown_clock_.clear();
        shown_packets_ = shown_keys_[0] = shown_keys_[1] = shown_keys_[2] = -1;
        shown_msg_.clear();
    }

    // ── Game flow ───────────────────────────────────────────────────────────
    void start_level(int i) {
        level_ = std::clamp(i, 0, bq::kLevelCount - 1);
        game_.load(level());
        mode_ = Mode::Play;
        paused_ = false;
        elapsed_ms_ = 0;
        acc_ms_ = 0;
        anim_ = 0;
        last_ms_ = 0;
        msg_ = level().hint;
    }

    void replay(const char* moves) {
        for (const char* p = moves; *p && game_.outcome == bq::Outcome::Playing; ++p) {
            const int d = to_dir(*p);
            if (d >= 0) {
                game_.move(d);
                if (game_.outcome != bq::Outcome::Playing) break;
            }
            game_.tick();
            ++anim_;
        }
        note_message();
    }

    // The board ended (game_.outcome != Playing): record it and show the result.
    void finish() {
        if (game_.outcome == bq::Outcome::Won) {
            mode_ = Mode::Won;
            left_s_ = std::max(0, (time_left_ms() + 999) / 1000);   // same rounding as the clock
            new_best_ = left_s_ > best_[level_];
            if (new_best_) best_[level_] = left_s_;
            if (!last_level()) unlocked_ = std::max(unlocked_, level_ + 2);
            if (!demo_) {
                mm::settings().set_int("bitquest.unlocked", unlocked_);
                mm::settings().set_int(fmt("bitquest.best.%d", level_ + 1), best_[level_]);
            }
        } else {
            mode_ = Mode::Lost;
        }
    }

    // ── Keys ────────────────────────────────────────────────────────────────
    bool key_title(const KeyEvent& e, int slot) {
        if (e.key == Key::B) return false;                  // Router pops: leave the game
        if (e.key == Key::Up || e.key == Key::Down) {
            if (list_ && list_->on_key(e)) {
                sel_ = list_->selected();
                ctx_.router.refresh_chrome();
            }
            return true;
        }
        if (is_direction(e.key)) return true;
        if ((e.key == Key::A && !e.repeat) || slot == 0) {
            if (sel_ < unlocked_) {
                start_level(sel_);
                ctx_.router.rebuild();
            }
            return true;
        }
        return slot != kSoftNone && slot != kSoftLeft;
    }

    bool key_play(const KeyEvent& e) {
        if (e.key == Key::B) {
            mode_ = Mode::Title;
            sel_ = level_;
            ctx_.router.rebuild();
            return true;
        }
        if (e.key == Key::Char && e.ch == 'r') {
            start_level(level_);
            ctx_.router.rebuild();
            return true;
        }
        if (e.key == Key::A) {
            if (!e.repeat) {
                paused_ = !paused_;
                last_ms_ = 0;
                ctx_.router.rebuild();
            }
            return true;
        }
        if (is_direction(e.key)) {
            if (paused_ || game_.outcome != bq::Outcome::Playing) return true;
            const int d = e.key == Key::Up ? bq::kUp : e.key == Key::Down ? bq::kDown
                        : e.key == Key::Left ? bq::kLeft : bq::kRight;
            const int facing = game_.facing;
            const bool moved = game_.move(d);
            note_message();
            if (game_.outcome != bq::Outcome::Playing) {
                finish();
                ctx_.router.rebuild();
                return true;
            }
            if (moved || game_.facing != facing) render_view();
            refresh_panel();
            return true;
        }
        return true;   // nothing else leaks to the Router during play
    }

    bool key_result(const KeyEvent& e, int slot) {
        if (e.key == Key::B || (mode_ == Mode::Lost && slot == 1)) {
            const bool won = mode_ == Mode::Won;
            mode_ = Mode::Title;
            sel_ = std::min(level_ + (won && !last_level() ? 1 : 0), unlocked_ - 1);
            ctx_.router.rebuild();
            return true;
        }
        if (e.key == Key::A || slot == 0) {
            if (e.repeat) return true;
            if (mode_ == Mode::Lost) start_level(level_);
            else if (last_level()) { mode_ = Mode::Title; sel_ = level_; }
            else start_level(level_ + 1);
            ctx_.router.rebuild();
            return true;
        }
        if ((e.key == Key::Char && e.ch == 'r') || (mode_ == Mode::Won && slot == 1)) {
            start_level(level_);
            ctx_.router.rebuild();
            return true;
        }
        return true;
    }

    // Turns the game's last note into the panel message.
    void note_message() {
        const int nx = game_.px + bq::kDx[game_.facing], ny = game_.py + bq::kDy[game_.facing];
        switch (game_.note) {
            case bq::Note::Packet:
                msg_ = game_.packets_left == 0 ? "All packets in! The modem is online."
                                               : fmt("Packet grabbed. %d to go.", game_.packets_left);
                break;
            case bq::Note::Card: msg_ = "Keycard picked up."; break;
            case bq::Note::Door: msg_ = "Firewall down. Card used."; break;
            case bq::Note::NeedKey: {
                const int k = bq::key_index(game_.at(nx, ny));
                msg_ = fmt("Firewall. Needs a %s keycard.", k >= 0 ? kCardName[k] : "matching");
                break;
            }
            case bq::Note::ExitLocked:
                msg_ = fmt("Modem offline: %d packet%s left.", game_.packets_left,
                           game_.packets_left == 1 ? "" : "s");
                break;
            case bq::Note::Patched: msg_ = "Short grounded. Safe to cross."; break;
            case bq::Note::Port: msg_ = "Zap! Port jump."; break;
            case bq::Note::None: break;
        }
    }

    // ── Title ───────────────────────────────────────────────────────────────
    void build_title(lv_obj_t* body) {
        const Skin& s = skin();
        lv_obj_t* logo = win::frame(body, 0, 0, kFrameW, kClientH, win::Edge::Sunken, 0x0c2a1c);
        win::text(logo, "BIT QUEST", 10, 6, 0xf8d830, 0, true);
        win::text(logo, "a 1999 data run", 10, 20, 0x88c8a0);
        // Bit, 3x, next to a packet and the modem.
        const Sprite* sp = sprites();
        place_sprite(logo, sp[S_BIT_RIGHT], 12, 40, 3);
        place_sprite(logo, sp[S_PACKET], 64, 52, 2);
        place_sprite(logo, sp[S_MODEM_ON], 100, 52, 2);
        win::text_wrap(logo, "Collect every packet, then dock at the modem before Y2K.",
                       6, 84, kFrameW - 14, 0xd8e8dc);

        list_ = std::make_unique<win::ListBox>(body, kPanelX, 0, kPanelW, kClientH);
        std::vector<win::Row> rows;
        for (int i = 0; i < bq::kLevelCount; ++i) {
            win::Row r;
            const bool open = i < unlocked_;
            r.text = open ? fmt("%d %s", i + 1, bq::kLevels[i].name) : fmt("%d ????????", i + 1);
            r.right = !open ? "locked" : best_[i] >= 0 ? fmt("%d:%02d", best_[i] / 60, best_[i] % 60) : "new";
            r.dim = !open;
            r.selectable = open;
            r.tag = i;
            rows.push_back(r);
        }
        list_->set_rows(std::move(rows));
        list_->select(std::min(sel_, unlocked_ - 1));
        sel_ = list_->selected();
        (void)s;
    }

    // ── Play ────────────────────────────────────────────────────────────────
    void build_play(lv_obj_t* body) {
        const Skin& s = skin();
        lv_obj_t* fr = win::frame(body, 0, 0, kFrameW, kClientH, win::Edge::Sunken, 0x000000);
        view_buf_.assign(static_cast<size_t>(kViewW) * kViewH, 0xff000000u);
        view_dsc_ = lv_image_dsc_t{};
        view_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        view_dsc_.header.cf = LV_COLOR_FORMAT_XRGB8888;
        view_dsc_.header.w = kViewW;
        view_dsc_.header.h = kViewH;
        view_dsc_.header.stride = kViewW * 4;
        view_dsc_.data = reinterpret_cast<const uint8_t*>(view_buf_.data());
        view_dsc_.data_size = static_cast<uint32_t>(view_buf_.size() * 4);
        view_img_ = lv_image_create(fr);
        lv_obj_set_pos(view_img_, 0, 0);
        lv_obj_set_size(view_img_, kViewW, kViewH);
        render_view();
        lv_image_set_src(view_img_, &view_dsc_);

        // Side panel.
        const int x = kPanelX;
        win::text(body, fmt("Board %d of %d", level_ + 1, bq::kLevelCount), x, 0, s.dim, kPanelW);
        win::text(body, level().name, x, 12, s.text, kPanelW, true);

        lv_obj_t* clock = win::frame(body, x, 27, 62, 24, win::Edge::Sunken, 0x140404);
        win::text(clock, "Y2K", 3, 1, 0x903020);
        clock_lbl_ = label(clock, "", 3, 8, font_title(), 0xff4830, 56);
        const Sprite* sp = sprites();
        place_sprite(body, sp[S_PACKET], x + 68, 27, 1);
        packets_lbl_ = win::text(body, "", x + 84, 28, s.text, kPanelW - 84);
        win::text(body, "packets", x + 68, 40, s.dim);

        win::text(body, "Keycards", x, 55, s.dim);
        for (int i = 0; i < 3; ++i) {
            card_img_[i] = place_sprite(body, sp[S_CARD0 + i], x + i * 34, 67, 1);
            card_lbl_[i] = win::text(body, "", x + i * 34 + 15, 67, s.text, 18);
        }

        msg_lbl_ = win::text_wrap(body, "", x, 84, kPanelW - 2, s.text);
        refresh_panel();

        if (paused_ || mode_ == Mode::Won || mode_ == Mode::Lost) build_overlay(fr);
    }

    lv_obj_t* place_sprite(lv_obj_t* parent, const Sprite& sp, int x, int y, int scale) {
        lv_obj_t* img = lv_image_create(parent);
        lv_image_set_src(img, &sp.dsc);
        lv_image_set_antialias(img, false);
        if (scale > 1) {
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
            lv_obj_set_size(img, kTile * scale, kTile * scale);
        }
        lv_obj_set_pos(img, x, y);
        return img;
    }

    void build_overlay(lv_obj_t* fr) {
        const Skin& s = skin();
        const int w = kViewW - 16, h = 64;
        lv_obj_t* card = win::card(fr, 8, (kViewH - h) / 2, w, h);
        std::string head, line1, line2;
        uint32_t head_col = s.card_text;
        if (paused_ && mode_ == Mode::Play) {
            head = "PAUSED";
            line1 = "The clock is stopped.";
            line2 = "Enter to carry on.";
        } else if (mode_ == Mode::Won) {
            head = last_level() ? "HAPPY 2000!" : "BOARD CLEAR!";
            head_col = 0x208838;
            line1 = fmt("%s left on the clock", clock_text(left_s_ * 1000).c_str());
            line2 = last_level() ? "Every packet delivered."
                  : new_best_ ? "New best time!" : fmt("Best: %d:%02d left", best_[level_] / 60, best_[level_] % 60);
        } else {
            head_col = 0xc02020;
            switch (game_.outcome) {
                case bq::Outcome::Fried:   head = "ZAPPED!";   line1 = "Bit rolled onto a short."; break;
                case bq::Outcome::Bitten:  head = "BUGGED!";   line1 = "A bug got into Bit."; break;
                case bq::Outcome::Timeout: head = "Y2K!";      line1 = "The clock ran out at 0:00."; break;
                default:                   head = "OOPS";      line1 = ""; break;
            }
            line2 = "Enter to try again.";
        }
        win::text(card, head, 6, 4, head_col, w - 12, true);
        win::text(card, line1, 6, 22, s.card_text, w - 12);
        win::text(card, line2, 6, 38, s.card_text, w - 12);
    }

    void refresh_panel() {
        if (!clock_lbl_) return;
        const std::string ck = clock_text(time_left_ms());
        if (ck != shown_clock_) {
            shown_clock_ = ck;
            set_text(clock_lbl_, ck);
            const bool low = time_left_ms() <= 15000;
            lv_obj_set_style_text_color(clock_lbl_, c(low && (anim_ / 4) % 2 == 0 ? 0xffd040 : 0xff4830), 0);
        }
        if (game_.packets_left != shown_packets_) {
            shown_packets_ = game_.packets_left;
            set_text(packets_lbl_, game_.packets_left == 0 ? "done!" : fmt("x %d", game_.packets_left));
        }
        for (int i = 0; i < 3; ++i) {
            if (game_.keys[i] == shown_keys_[i]) continue;
            shown_keys_[i] = game_.keys[i];
            set_text(card_lbl_[i], game_.keys[i] > 0 ? fmt("%d", game_.keys[i]) : "-");
            lv_obj_set_style_image_opa(card_img_[i], game_.keys[i] > 0 ? LV_OPA_COVER : LV_OPA_30, 0);
        }
        if (msg_ != shown_msg_) {
            shown_msg_ = msg_;
            set_text(msg_lbl_, msg_);
        }
    }

    // ── Viewport drawing ────────────────────────────────────────────────────
    void put(int x, int y, uint32_t col) {
        if (x < 0 || y < 0 || x >= kViewW || y >= kViewH) return;
        view_buf_[static_cast<size_t>(y) * kViewW + x] = 0xff000000u | col;
    }

    void blit(const Sprite& sp, int x0, int y0) {
        for (int y = 0; y < kTile; ++y) {
            for (int x = 0; x < kTile; ++x) {
                const uint32_t v = sp.px[y * kTile + x];
                if (v >> 24) put(x0 + x, y0 + y, v & 0xffffff);
            }
        }
    }

    // Circuit board: dark green, a copper trace or a via picked by the cell.
    void draw_floor(int x0, int y0, int cx, int cy) {
        const uint32_t h = static_cast<uint32_t>(cx * 73856093u) ^ static_cast<uint32_t>(cy * 19349663u);
        for (int y = 0; y < kTile; ++y) {
            for (int x = 0; x < kTile; ++x) {
                uint32_t col = ((x + y) & 7) == 0 ? 0x123c2a : 0x0e3022;
                if (x == 0 || y == 0) col = 0x0a281c;
                if ((h & 3) == 0 && y == 6) col = 0x1e5c3a;
                if ((h & 3) == 1 && x == 6) col = 0x1e5c3a;
                if ((h & 7) == 2 && x >= 5 && x <= 7 && y >= 5 && y <= 7) col = (x == 6 && y == 6) ? 0x0a281c : 0xb89848;
                put(x0 + x, y0 + y, col);
            }
        }
    }

    void draw_short(int x0, int y0, int cx, int cy) {
        for (int y = 0; y < kTile; ++y) {
            for (int x = 0; x < kTile; ++x) {
                const bool burnt = ((x * 7 + y * 3 + cx + cy) % 5) == 0;
                put(x0 + x, y0 + y, burnt ? 0x2a0806 : 0x4a120c);
            }
        }
        const bool flip = ((anim_ / 2) + cx + cy) % 2 != 0;
        blit(sprites()[flip ? S_SPARK_B : S_SPARK_A], x0, y0);
    }

    // Data bus: rails and moving yellow chevrons pointing along `dir`.
    void draw_bus(int x0, int y0, int dir) {
        const int phase = static_cast<int>(anim_ % 6);
        for (int y = 0; y < kTile; ++y) {
            for (int x = 0; x < kTile; ++x) {
                int u, v;   // u along the bus (increasing in travel direction), v across it
                switch (dir) {
                    case bq::kRight: u = x; v = y; break;
                    case bq::kLeft:  u = kTile - 1 - x; v = y; break;
                    case bq::kDown:  u = y; v = x; break;
                    default:         u = kTile - 1 - y; v = x; break;
                }
                uint32_t col = 0x2a2e36;
                if (v == 0 || v == kTile - 1) col = 0x7c828c;
                else if (v == 1 || v == kTile - 2) col = 0x1a1c22;
                else {
                    const int m = ((u - std::abs(v - 6) - phase) % 6 + 6) % 6;
                    if (m < 2) col = 0xf8c828;
                }
                put(x0 + x, y0 + y, col);
            }
        }
    }

    // Port: pulsing purple rings around a white core.
    void draw_port(int x0, int y0) {
        const int phase = static_cast<int>(anim_ % 4);
        for (int y = 0; y < kTile; ++y) {
            for (int x = 0; x < kTile; ++x) {
                const int dx = x - 6, dy = y - 6;
                const int d = std::max(std::abs(dx), std::abs(dy)) + (std::abs(dx) + std::abs(dy)) / 3;
                uint32_t col;
                if (d <= 1) col = 0xffffff;
                else if (d > 7) col = 0x1a0c2a;
                else col = ((d + phase) % 4 < 2) ? 0xb860f0 : 0x5a2890;
                put(x0 + x, y0 + y, col);
            }
        }
    }

    void render_view() {
        if (!view_img_) return;
        const Sprite* sp = sprites();
        // Camera: the top-left cell shown; boards smaller than the view are centred.
        auto cam = [](int p, int size, int view) {
            if (size <= view) return -(view - size) / 2;
            return std::clamp(p - view / 2, 0, size - view);
        };
        const int ox = cam(game_.px, game_.w, kViewCols);
        const int oy = cam(game_.py, game_.h, kViewRows);
        for (int ty = 0; ty < kViewRows; ++ty) {
            for (int tx = 0; tx < kViewCols; ++tx) {
                const int cx = ox + tx, cy = oy + ty;
                const int x0 = tx * kTile, y0 = ty * kTile;
                if (cx < 0 || cy < 0 || cx >= game_.w || cy >= game_.h) {
                    for (int y = 0; y < kTile; ++y)
                        for (int x = 0; x < kTile; ++x) put(x0 + x, y0 + y, ((x ^ y) & 1) ? 0x05080a : 0x0a0e12);
                    continue;
                }
                const char t = game_.at(cx, cy);
                switch (t) {
                    case '#': blit(sp[S_WALL], x0, y0); break;
                    case '~': draw_short(x0, y0, cx, cy); break;
                    case 'T': draw_port(x0, y0); break;
                    case '<': case '>': case '^': case 'v': draw_bus(x0, y0, bq::bus_dir(t)); break;
                    default:
                        draw_floor(x0, y0, cx, cy);
                        switch (t) {
                            case '*': blit(sp[S_PACKET], x0, y0); break;
                            case 'r': case 'g': case 'b': blit(sp[S_CARD0 + bq::key_index(t)], x0, y0); break;
                            case 'R': case 'G': case 'B': blit(sp[S_DOOR0 + bq::key_index(t)], x0, y0); break;
                            case 'o': blit(sp[S_FLOPPY], x0, y0); break;
                            case 'E': {
                                const bool on = game_.packets_left == 0;
                                blit(sp[on && (anim_ / 3) % 2 ? S_MODEM_ON : on ? S_MODEM_ON : S_MODEM_OFF], x0, y0);
                                if (on && (anim_ / 3) % 2) {   // blink the LEDs
                                    for (int lx : {2, 4, 6}) put(x0 + lx, y0 + 6, 0xe8f0e0);
                                }
                                break;
                            }
                            default: break;
                        }
                        break;
                }
            }
        }
        for (const bq::Bug& b : game_.bugs) {
            const int tx = b.x - ox, ty = b.y - oy;
            if (tx < 0 || ty < 0 || tx >= kViewCols || ty >= kViewRows) continue;
            blit(sp[(anim_ / 2) % 2 ? S_BUG_B : S_BUG_A], tx * kTile, ty * kTile);
        }
        {
            const int tx = game_.px - ox, ty = game_.py - oy;
            static const SpriteId kFace[4] = {S_BIT_UP, S_BIT_DOWN, S_BIT_LEFT, S_BIT_RIGHT};
            if (game_.outcome == bq::Outcome::Fried) {
                blit(sp[S_SPARK_A], tx * kTile, ty * kTile);
            }
            blit(sp[kFace[game_.facing & 3]], tx * kTile, ty * kTile);
            if (game_.outcome == bq::Outcome::Fried || game_.outcome == bq::Outcome::Bitten) {
                for (int i = 0; i < kTile; ++i) {     // a red X over Bit
                    put(tx * kTile + i, ty * kTile + i, 0xff3020);
                    put(tx * kTile + kTile - 1 - i, ty * kTile + i, 0xff3020);
                }
            }
        }
        lv_obj_invalidate(view_img_);
    }

    // ── State ───────────────────────────────────────────────────────────────
    Mode mode_ = Mode::Title;
    bq::Game game_;
    int level_ = 0;
    int sel_ = 0;
    int unlocked_ = 1;
    int best_[bq::kLevelCount] = {};
    bool demo_ = false;
    bool paused_ = false;
    int elapsed_ms_ = 0;
    int acc_ms_ = 0;
    uint32_t last_ms_ = 0;
    uint32_t anim_ = 0;
    int left_s_ = 0;
    bool new_best_ = false;
    std::string msg_;

    std::vector<uint32_t> view_buf_;
    lv_image_dsc_t view_dsc_{};
    lv_obj_t* view_img_ = nullptr;
    std::unique_ptr<win::ListBox> list_;
    lv_obj_t* clock_lbl_ = nullptr;
    lv_obj_t* packets_lbl_ = nullptr;
    lv_obj_t* msg_lbl_ = nullptr;
    lv_obj_t* card_img_[3] = {};
    lv_obj_t* card_lbl_[3] = {};
    std::string shown_clock_, shown_msg_;
    int shown_packets_ = -1;
    int shown_keys_[3] = {-1, -1, -1};
};

} // namespace

std::unique_ptr<Screen> make_puzzle_screen(Ctx& ctx) {
    return std::make_unique<PuzzleScreen>(ctx);
}

} // namespace mm::ui
