// mines_screen.cpp — MeshMines: clear the field without waking a dead node.
//
// Classic mine-clearing rules: the first opening is never a mine (its 3x3
// neighbourhood is kept clear so it always opens an area), zeros flood fill,
// Enter on an opened number whose flags are all placed "chords" (opens the
// rest of its neighbours). The panel on the left has the mines-left counter,
// the tower (a little radio-tower face that reacts: calm / tense while you
// press / X eyes when you lose / shades when you win), the timer and the best
// time for the level. Best times are kept in settings.json:
//
//   mines.level            int  0 Beginner, 1 Intermediate, 2 Expert
//   mines.best.<level id>  int  best time in seconds (absent: none yet)
//
// The board, LEDs and tower are drawn as pixels into small XRGB8888 images
// (one LVGL object each instead of hundreds of cells), with every frame
// colour read from the current skin so it sits in all 1999 Online skins.
// Number / mine / flag colours are the game's own (dark skins get brighter
// numbers).
#include "screens.h"

#include "game_state.h"
#include "router.h"
#include "settings.h"
#include "widgets.h"
#include "win.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <vector>

namespace mm::ui {
namespace {

// ── Levels ──────────────────────────────────────────────────────────────────

struct Level {
    const char* id;        // settings key part
    const char* name;
    int cols, rows, mines;
    int cell;              // px
};

// Board area is 250 x 121 next to the 62 px panel: 12 px cells for 9 rows,
// 10 px cells for Expert's 11 rows.
constexpr Level kLevels[] = {
    {"beginner", "Beginner", 9, 9, 10, 12},
    {"intermediate", "Intermediate", 20, 9, 30, 12},
    {"expert", "Expert", 24, 11, 50, 10},
};
constexpr int kLevelCount = sizeof(kLevels) / sizeof(kLevels[0]);
constexpr int kMaxCells = 24 * 11;

constexpr int kPanelW = 62;
constexpr int kBoardAreaX = kPanelW;
constexpr int kBoardAreaW = kClientW - kPanelW;   // 250
constexpr int kLedW = 32, kLedH = 18;
constexpr int kFace = 24;
constexpr uint32_t kTenseMs = 220;

// ── Colours (game art; frame colours come from the skin) ───────────────────

constexpr uint32_t kNumLight[9] = {0, 0x0000f0, 0x007b00, 0xe00000, 0x000080,
                                   0x800000, 0x007b7b, 0x000000, 0x7b7b7b};
constexpr uint32_t kNumDark[9] = {0, 0x6aa8ff, 0x50d050, 0xff6060, 0xc090ff,
                                  0xffa050, 0x40e0e0, 0xf0f0f0, 0xb0b0b0};
constexpr uint32_t kMine = 0x141414;
constexpr uint32_t kBoom = 0xf02020;
constexpr uint32_t kFlag = 0xe01818;
constexpr uint32_t kLedBg = 0x100000;
constexpr uint32_t kLedOn = 0xff2a1a;
constexpr uint32_t kLedOff = 0x2c0604;
constexpr uint32_t kHead = 0xf8cc30;
constexpr uint32_t kInk = 0x1c1c1c;
constexpr uint32_t kMast = 0xb02020;

uint32_t mix(uint32_t a, uint32_t b, int t256) {
    auto ch = [&](int sh) {
        const int x = (a >> sh) & 0xff, y = (b >> sh) & 0xff;
        return static_cast<uint32_t>(x + (y - x) * t256 / 256) << sh;
    };
    return ch(16) | ch(8) | ch(0);
}

int luma(uint32_t c) {
    return (((c >> 16) & 0xff) * 3 + ((c >> 8) & 0xff) * 6 + (c & 0xff)) / 10;
}

// 6x8 bold digits 1..8 (own glyphs).
const char* const kGlyph[9][8] = {
    {},
    {"..##..", ".###..", "####..", "..##..", "..##..", "..##..", "..##..", "######"},
    {".####.", "##..##", "....##", "...##.", "..##..", ".##...", "##....", "######"},
    {"#####.", "....##", "....##", ".####.", "....##", "....##", "....##", "#####."},
    {"...##.", "..###.", ".####.", "##.##.", "######", "...##.", "...##.", "...##."},
    {"######", "##....", "##....", "#####.", "....##", "....##", "##..##", ".####."},
    {".####.", "##....", "##....", "#####.", "##..##", "##..##", "##..##", ".####."},
    {"######", "....##", "...##.", "...##.", "..##..", "..##..", ".##...", ".##..."},
    {".####.", "##..##", "##..##", ".####.", "##..##", "##..##", "##..##", ".####."},
};

// ── A small pixel image ─────────────────────────────────────────────────────

struct Pix {
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    lv_image_dsc_t dsc{};
    lv_obj_t* img = nullptr;

    void create(lv_obj_t* parent, int x, int y, int iw, int ih) {
        w = iw;
        h = ih;
        px.assign(static_cast<size_t>(w) * h, 0xff000000u);
        dsc = lv_image_dsc_t{};
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
        dsc.header.w = static_cast<uint32_t>(w);
        dsc.header.h = static_cast<uint32_t>(h);
        dsc.header.stride = static_cast<uint32_t>(w * 4);
        dsc.data = reinterpret_cast<const uint8_t*>(px.data());
        dsc.data_size = static_cast<uint32_t>(px.size() * 4);
        img = lv_image_create(parent);
        lv_image_set_src(img, &dsc);
        lv_image_set_antialias(img, false);
        lv_obj_set_pos(img, x, y);
        lv_obj_set_size(img, w, h);
    }
    void dot(int x, int y, uint32_t c) {
        if (x >= 0 && y >= 0 && x < w && y < h) px[static_cast<size_t>(y) * w + x] = c | 0xff000000u;
    }
    void rect(int x, int y, int rw, int rh, uint32_t c) {
        for (int j = std::max(0, y); j < std::min(h, y + rh); ++j)
            for (int i = std::max(0, x); i < std::min(w, x + rw); ++i) px[static_cast<size_t>(j) * w + i] = c | 0xff000000u;
    }
    void outline(int x, int y, int rw, int rh, uint32_t c) {
        rect(x, y, rw, 1, c);
        rect(x, y + rh - 1, rw, 1, c);
        rect(x, y, 1, rh, c);
        rect(x + rw - 1, y, 1, rh, c);
    }
    // Two-tone 3D edge of thickness t (raised: hi top-left, lo bottom-right).
    void bevel(int x, int y, int rw, int rh, int t, uint32_t tl, uint32_t br) {
        for (int k = 0; k < t; ++k) {
            rect(x + k, y + k, rw - 2 * k, 1, tl);
            rect(x + k, y + k, 1, rh - 2 * k, tl);
            rect(x + k + 1, y + rh - 1 - k, rw - 2 * k - 1, 1, br);
            rect(x + rw - 1 - k, y + k + 1, 1, rh - 2 * k - 1, br);
        }
    }
    void show() {
        if (img) lv_obj_invalidate(img);
    }
};

// ── 7-segment LED ───────────────────────────────────────────────────────────

// Segment bits: a b c d e f g = 0..6 ; 10 = "-", 11 = blank.
constexpr uint8_t kSeg[12] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f, 0x40, 0x00};

void draw_digit(Pix& p, int x, int y, int d) {
    constexpr int w = 8, h = 14;
    const uint8_t on = kSeg[d];
    auto seg = [&](int bit, int sx, int sy, int sw, int sh) {
        p.rect(x + sx, y + sy, sw, sh, (on >> bit) & 1 ? kLedOn : kLedOff);
    };
    seg(0, 2, 0, w - 4, 2);          // a
    seg(1, w - 2, 2, 2, 4);          // b
    seg(2, w - 2, 8, 2, 4);          // c
    seg(3, 2, h - 2, w - 4, 2);      // d
    seg(4, 0, 8, 2, 4);              // e
    seg(5, 0, 2, 2, 4);              // f
    seg(6, 2, 6, w - 4, 2);          // g
}

void draw_led(Pix& p, int value) {
    const Skin& s = skin();
    p.rect(0, 0, p.w, p.h, kLedBg);
    if (s.bevel) p.bevel(0, 0, p.w, p.h, 1, s.lo, s.hi);
    else p.outline(0, 0, p.w, p.h, s.lo);
    value = std::clamp(value, -99, 999);
    int d[3];
    if (value < 0) {
        d[0] = 10;
        d[1] = (-value) / 10;
        d[2] = (-value) % 10;
    } else {
        d[0] = value / 100;
        d[1] = value / 10 % 10;
        d[2] = value % 10;
    }
    for (int i = 0; i < 3; ++i) draw_digit(p, 3 + i * 9 + (i ? 0 : 0), 2, d[i]);
    p.show();
}

// ── Game ────────────────────────────────────────────────────────────────────

enum class Cell : uint8_t { Hidden, Open, Flag };
enum class Phase : uint8_t { Ready, Playing, Lost, Won };
enum class Mood : uint8_t { Calm, Tense, Dead, Cool };

struct Board {
    int cols = 9, rows = 9, mines = 10;
    bool mine[kMaxCells] = {};
    uint8_t adj[kMaxCells] = {};
    Cell cell[kMaxCells] = {};
    int opened = 0, flags = 0;
    int boom = -1;               // the mine that went off
    bool laid = false;
    uint32_t rng = 1;

    int n() const { return cols * rows; }
    uint32_t next() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    }
    void reset(const Level& lv, uint32_t seed) {
        cols = lv.cols;
        rows = lv.rows;
        mines = lv.mines;
        std::fill(std::begin(mine), std::end(mine), false);
        std::fill(std::begin(adj), std::end(adj), 0);
        std::fill(std::begin(cell), std::end(cell), Cell::Hidden);
        opened = flags = 0;
        boom = -1;
        laid = false;
        rng = seed ? seed : 0x9e3779b9u;
    }
    template <class F>
    void around(int i, F f) const {
        const int x = i % cols, y = i / cols;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < cols && ny < rows) f(ny * cols + nx);
            }
    }
    // Mines go anywhere but `safe` and (room permitting) its neighbours.
    void lay(int safe) {
        std::vector<bool> keep(n(), false);
        keep[safe] = true;
        if (n() - 9 >= mines) around(safe, [&](int j) { keep[j] = true; });
        int placed = 0;
        while (placed < mines) {
            const int j = static_cast<int>(next() % static_cast<uint32_t>(n()));
            if (keep[j] || mine[j]) continue;
            mine[j] = true;
            ++placed;
        }
        for (int i = 0; i < n(); ++i) {
            int k = 0;
            around(i, [&](int j) { k += mine[j]; });
            adj[i] = static_cast<uint8_t>(k);
        }
        laid = true;
    }
    // Opens one cell (flood fill on zeros). false: it was a mine.
    bool open(int i) {
        if (cell[i] != Cell::Hidden) return true;
        if (!laid) lay(i);
        if (mine[i]) {
            cell[i] = Cell::Open;
            boom = i;
            return false;
        }
        std::vector<int> stack{i};
        cell[i] = Cell::Open;
        ++opened;
        while (!stack.empty()) {
            const int k = stack.back();
            stack.pop_back();
            if (adj[k]) continue;
            around(k, [&](int j) {
                if (cell[j] == Cell::Hidden && !mine[j]) {
                    cell[j] = Cell::Open;
                    ++opened;
                    stack.push_back(j);
                }
            });
        }
        return true;
    }
    // Enter on an opened number with all its flags placed. false: a mine.
    bool chord(int i) {
        if (cell[i] != Cell::Open || !adj[i]) return true;
        int f = 0;
        around(i, [&](int j) { f += cell[j] == Cell::Flag; });
        if (f != adj[i]) return true;
        bool ok = true;
        around(i, [&](int j) {
            if (cell[j] == Cell::Hidden && !open(j)) ok = false;
        });
        return ok;
    }
    void flag(int i) {
        if (cell[i] == Cell::Hidden) {
            cell[i] = Cell::Flag;
            ++flags;
        } else if (cell[i] == Cell::Flag) {
            cell[i] = Cell::Hidden;
            --flags;
        }
    }
    bool won() const { return laid && boom < 0 && opened == n() - mines; }

    // One pass of trivial deductions (demo states only). true if it acted.
    bool solve_step() {
        bool acted = false;
        for (int i = 0; i < n(); ++i) {
            if (cell[i] != Cell::Open || !adj[i]) continue;
            int hid = 0, f = 0;
            around(i, [&](int j) {
                hid += cell[j] == Cell::Hidden;
                f += cell[j] == Cell::Flag;
            });
            if (!hid) continue;
            if (hid + f == adj[i]) {
                around(i, [&](int j) {
                    if (cell[j] == Cell::Hidden) flag(j);
                });
                return true;
            }
            if (f == adj[i]) {
                chord(i);
                return true;
            }
        }
        return acted;
    }
};

class MinesScreen final : public Screen {
public:
    explicit MinesScreen(Ctx& ctx) : Screen(ctx) {
        demo_ = !ctx_.game.files_enabled();
        if (demo_) {
            set_demo_state(0);
            return;
        }
        level_ = std::clamp(static_cast<int>(settings().get_int("mines.level", 0)), 0, kLevelCount - 1);
        for (int i = 0; i < kLevelCount; ++i)
            best_[i] = static_cast<int>(settings().get_int(std::string("mines.best.") + kLevels[i].id, 0));
        new_game();
    }

    const char* title() const override { return "MeshMines"; }
    bool windowed() const override { return true; }
    std::string hints() const override {
        return "Arrows:move  Enter:open  M:flag  N:new  L:level";
    }
    SoftKeys softkeys() const override {
        SoftKeys k;
        const bool live = phase_ == Phase::Ready || phase_ == Phase::Playing;
        k.set(0, "Flag", live);
        k.set(1, "New");
        k.set(2, "Level");
        k.set(4, "Best");
        k.left.text = MM_SOFT_BACK;
        return k;
    }

    int demo_state_count() const override { return 5; }
    const char* demo_state_name(int i) const override {
        static const char* const names[] = {"fresh", "midgame", "lost", "won", "expert"};
        return names[std::clamp(i, 0, 4)];
    }
    void set_demo_state(int i) override {
        demo_ = true;
        tense_until_ = 0;
        card_ = false;
        new_best_ = -1;
        best_[0] = 14;
        best_[1] = 61;
        best_[2] = 0;
        switch (i) {
            default:
            case 0:
                demo_board(1, 0, 0);
                cx_ = 9;
                cy_ = 4;
                break;
            case 1:
                demo_board(1, 0x5eed1999u, 47);
                demo_open(9, 4);
                for (int k = 0; k < 6 && b_.solve_step(); ++k) {}
                cx_ = 12;
                cy_ = 3;
                break;
            case 2: {
                demo_board(0, 0x1999abcdu, 23);
                demo_open(4, 4);
                for (int k = 0; k < 3 && b_.solve_step(); ++k) {}
                // Step on a mine next to the opened area.
                int hit = -1;
                for (int j = 0; j < b_.n() && hit < 0; ++j) {
                    if (!b_.mine[j] || b_.cell[j] != Cell::Hidden) continue;
                    b_.around(j, [&](int q) {
                        if (b_.cell[q] == Cell::Open) hit = j;
                    });
                }
                if (hit < 0) hit = static_cast<int>(std::find(b_.mine, b_.mine + b_.n(), true) - b_.mine);
                // A wrong flag, to show the crossed-out mark.
                for (int j = 0; j < b_.n(); ++j) {
                    if (!b_.mine[j] && b_.cell[j] == Cell::Hidden) {
                        b_.flag(j);
                        break;
                    }
                }
                cx_ = hit % b_.cols;
                cy_ = hit / b_.cols;
                b_.open(hit);
                phase_ = Phase::Lost;
                break;
            }
            case 3: {
                demo_board(0, 0x00c0ffeeu, 12);
                demo_open(4, 4);
                while (!b_.won()) {
                    if (b_.solve_step()) continue;
                    for (int j = 0; j < b_.n(); ++j) {     // a lucky guess
                        if (b_.cell[j] == Cell::Hidden && !b_.mine[j]) {
                            b_.open(j);
                            break;
                        }
                    }
                }
                finish_win(12);
                break;
            }
            case 4:
                demo_board(2, 0xfeedbeefu, 131);
                demo_open(12, 5);
                for (int k = 0; k < 10 && b_.solve_step(); ++k) {}
                tense_until_ = 1;                  // caught mid-press
                cx_ = 3;
                cy_ = 8;
                break;
        }
    }

    void build(lv_obj_t* body) override {
        const Skin& s = skin();
        const Level& lv = kLevels[level_];
        win::frame(body, 0, 0, kClientW, kClientH, win::Edge::None, s.face);

        lv_obj_t* panel = win::frame(body, 0, 0, kPanelW - 2, kClientH, win::Edge::Raised, s.face, s.radius);
        const int px = (kPanelW - 2 - kLedW) / 2;
        mines_led_.create(panel, px, 5, kLedW, kLedH);
        face_.create(panel, (kPanelW - 2 - kFace) / 2, 27, kFace, kFace);
        time_led_.create(panel, px, 55, kLedW, kLedH);
        level_lbl_ = centered(panel, 77, s.text, true);
        best_lbl_ = centered(panel, 91, s.text, false);
        size_lbl_ = centered(panel, 104, s.dim, false);

        const int bw = lv.cols * lv.cell + 4, bh = lv.rows * lv.cell + 4;
        board_.create(body, kBoardAreaX + (kBoardAreaW - bw) / 2, (kClientH - bh) / 2, bw, bh);

        card_box_ = nullptr;
        if (card_) build_card(body);
        paint_all();
    }

    bool on_key(const KeyEvent& e) override {
        const int slot = softkey_slot(e);
        if (card_) {
            card_ = false;
            if (e.key == Key::A && phase_ == Phase::Won) new_game();
            ctx_.router.rebuild();
            if (e.key == Key::A || e.key == Key::B || slot == 4 || (e.key == Key::Char && e.ch == 'b')) return true;
        }
        if (is_direction(e.key)) {
            const int cols = b_.cols, rows = b_.rows;
            if (e.key == Key::Left) cx_ = (cx_ + cols - 1) % cols;
            if (e.key == Key::Right) cx_ = (cx_ + 1) % cols;
            if (e.key == Key::Up) cy_ = (cy_ + rows - 1) % rows;
            if (e.key == Key::Down) cy_ = (cy_ + 1) % rows;
            paint_board();
            return true;
        }
        if (e.key == Key::A) {
            if (phase_ == Phase::Lost || phase_ == Phase::Won) {
                new_game();
                ctx_.router.rebuild();
            } else {
                press();
            }
            return true;
        }
        const char ch = e.key == Key::Char ? static_cast<char>(e.ch) : 0;
        if (slot == 0 || ch == 'm' || ch == 'g' || ch == 'q') {
            toggle_flag();
            return true;
        }
        if (slot == 1 || ch == 'n') {
            new_game();
            ctx_.router.rebuild();
            return true;
        }
        if (slot == 2 || ch == 'l' || e.key == Key::Tab) {
            level_ = (level_ + 1) % kLevelCount;
            if (!demo_) settings().set_int("mines.level", level_);
            new_game();
            ctx_.router.rebuild();
            return true;
        }
        if (slot == 4 || ch == 'b') {
            card_ = true;
            ctx_.router.rebuild();
            return true;
        }
        return false;
    }

    void tick(uint32_t now_ms) override {
        if (tense_until_ && !demo_ && static_cast<int32_t>(now_ms - tense_until_) >= 0) {
            tense_until_ = 0;
            paint_face();
        }
        if (phase_ == Phase::Playing && !demo_) {
            const int t = seconds();
            if (t != shown_secs_) paint_time();
        }
    }

private:
    // ── Game flow ───────────────────────────────────────────────────────────

    void new_game() {
        const uint32_t seed = static_cast<uint32_t>(std::time(nullptr)) * 2654435761u ^ lv_tick_get();
        b_.reset(kLevels[level_], seed);
        phase_ = Phase::Ready;
        cx_ = b_.cols / 2;
        cy_ = b_.rows / 2;
        final_secs_ = 0;
        new_best_ = -1;
        card_ = false;
    }

    void demo_board(int level, uint32_t seed, int secs) {
        level_ = level;
        b_.reset(kLevels[level_], seed ? seed : 0x1999u);
        phase_ = Phase::Ready;
        final_secs_ = secs;
    }
    void demo_open(int x, int y) {
        b_.open(y * b_.cols + x);
        phase_ = Phase::Playing;
    }

    int seconds() const {
        if (demo_ || phase_ != Phase::Playing) return final_secs_;
        return std::min(999, static_cast<int>((lv_tick_get() - start_ms_) / 1000) + 1);
    }

    void press() {
        const int i = cy_ * b_.cols + cx_;
        if (b_.cell[i] == Cell::Flag) return;
        if (phase_ == Phase::Ready && b_.cell[i] == Cell::Hidden) {
            phase_ = Phase::Playing;
            start_ms_ = lv_tick_get();
        }
        tense_until_ = lv_tick_get() + kTenseMs;
        if (!tense_until_) tense_until_ = 1;
        const bool ok = b_.cell[i] == Cell::Open ? b_.chord(i) : b_.open(i);
        if (!ok) {
            final_secs_ = seconds();
            phase_ = Phase::Lost;
            tense_until_ = 0;
        } else if (b_.won()) {
            finish_win(seconds());
            tense_until_ = 0;
            ctx_.router.rebuild();
            return;
        }
        paint_all();
    }

    void finish_win(int secs) {
        final_secs_ = secs;
        phase_ = Phase::Won;
        for (int j = 0; j < b_.n(); ++j)
            if (b_.mine[j] && b_.cell[j] == Cell::Hidden) b_.flag(j);
        if (best_[level_] == 0 || secs < best_[level_]) {
            best_[level_] = secs;
            new_best_ = level_;
            if (!demo_) settings().set_int(std::string("mines.best.") + kLevels[level_].id, secs);
        }
        card_ = true;
    }

    void toggle_flag() {
        if (phase_ == Phase::Lost || phase_ == Phase::Won) return;
        b_.flag(cy_ * b_.cols + cx_);
        paint_board();
        paint_mines();
    }

    // ── Drawing ─────────────────────────────────────────────────────────────

    static lv_obj_t* centered(lv_obj_t* parent, int y, uint32_t color, bool bold) {
        lv_obj_t* l = win::text(parent, "", 1, y, color, kPanelW - 4, bold);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        return l;
    }

    void paint_all() {
        paint_board();
        paint_mines();
        paint_time();
        paint_face();
        const Level& lv = kLevels[level_];
        if (level_lbl_) set_text(level_lbl_, level_ == 1 ? "Intermed." : lv.name);
        if (best_lbl_) set_text(best_lbl_, best_[level_] ? fmt("Best %d s", best_[level_]) : std::string("Best --"));
        if (size_lbl_) set_text(size_lbl_, fmt("%dx%d  %d", lv.cols, lv.rows, lv.mines));
    }

    void paint_mines() {
        if (mines_led_.img) draw_led(mines_led_, b_.mines - b_.flags);
    }
    void paint_time() {
        shown_secs_ = seconds();
        if (time_led_.img) draw_led(time_led_, shown_secs_);
    }

    Mood mood() const {
        if (phase_ == Phase::Lost) return Mood::Dead;
        if (phase_ == Phase::Won) return Mood::Cool;
        if (tense_until_) return Mood::Tense;
        return Mood::Calm;
    }

    // The tower: a round face with a little mast and radio waves on top.
    void paint_face() {
        Pix& p = face_;
        if (!p.img) return;
        const Skin& s = skin();
        p.rect(0, 0, p.w, p.h, s.face);
        if (s.bevel) p.bevel(0, 0, p.w, p.h, 2, s.hi, s.lo);
        else p.outline(0, 0, p.w, p.h, s.lo);
        const Mood m = mood();
        const int ox = m == Mood::Tense ? 1 : 0, oy = ox;   // pressed look

        // Mast with a beacon; waves (two rings when tense, none when dead).
        const uint32_t beacon = m == Mood::Dead ? 0x606060 : kFlag;
        p.rect(11 + ox, 4 + oy, 2, 5, kMast);
        p.rect(10 + ox, 8 + oy, 4, 1, kMast);
        p.rect(11 + ox, 1 + oy, 2, 4, beacon);
        p.rect(10 + ox, 2 + oy, 4, 2, beacon);
        if (m != Mood::Dead) {
            const uint32_t wave = s.dark ? 0x80c0ff : 0x2050c0;
            auto arc = [&](int x, int dir) {
                p.dot(x + ox, 2 + oy, wave);
                p.dot(x - dir + ox, 3 + oy, wave);
                p.dot(x - dir + ox, 4 + oy, wave);
                p.dot(x + ox, 5 + oy, wave);
            };
            arc(8, 1);
            arc(15, -1);
            if (m == Mood::Tense) {
                arc(6, 1);
                arc(17, -1);
            }
        }
        // Head: disc centred at (11.5, 14.5).
        const float cx = 11.5f + ox, cy = 14.5f + oy;
        for (int y = 7; y < 23; ++y)
            for (int x = 3; x < 21; ++x) {
                const float d = std::sqrt((x + .5f - cx) * (x + .5f - cx) + (y + .5f - cy) * (y + .5f - cy));
                if (d <= 7.2f) p.dot(x, y, d > 6.1f ? kInk : kHead);
            }
        auto at = [&](int x, int y, int w, int h, uint32_t c) { p.rect(x + ox, y + oy, w, h, c); };
        switch (m) {
            case Mood::Calm:
                at(8, 12, 2, 2, kInk);
                at(14, 12, 2, 2, kInk);
                at(8, 16, 1, 1, kInk);
                at(15, 16, 1, 1, kInk);
                at(9, 17, 6, 1, kInk);
                break;
            case Mood::Tense:
                at(8, 11, 2, 3, kInk);
                at(14, 11, 2, 3, kInk);
                at(10, 16, 4, 1, kInk);
                at(10, 19, 4, 1, kInk);
                at(10, 16, 1, 4, kInk);
                at(13, 16, 1, 4, kInk);
                break;
            case Mood::Dead:
                for (int k = 0; k < 3; ++k) {
                    at(7 + k, 11 + k, 1, 1, kInk);
                    at(9 - k, 11 + k, 1, 1, kInk);
                    at(14 + k, 11 + k, 1, 1, kInk);
                    at(16 - k, 11 + k, 1, 1, kInk);
                }
                at(9, 17, 6, 1, kInk);
                at(8, 18, 1, 1, kInk);
                at(15, 18, 1, 1, kInk);
                break;
            case Mood::Cool:
                at(6, 11, 12, 1, kInk);
                at(6, 12, 5, 2, kInk);
                at(13, 12, 5, 2, kInk);
                at(7, 12, 1, 1, 0xffffff);
                at(14, 12, 1, 1, 0xffffff);
                at(8, 16, 1, 1, kInk);
                at(15, 16, 1, 1, kInk);
                at(9, 17, 6, 1, kInk);
                break;
        }
        p.show();
    }

    void draw_mine(int x0, int y0, int cs) {
        Pix& p = board_;
        const float c = (cs - 1) / 2.f + 1.f;      // centre of the inner area
        const float r = cs >= 12 ? 3.3f : 2.7f;
        const int span = cs >= 12 ? 5 : 4;
        const int ic = static_cast<int>(c);
        for (int k = -span; k <= span; ++k) {   // spikes
            p.dot(x0 + ic + k, y0 + ic, mine_col_);
            p.dot(x0 + ic, y0 + ic + k, mine_col_);
        }
        const int dg = cs >= 12 ? 3 : 2;
        for (int k = -dg; k <= dg; ++k) {
            p.dot(x0 + ic + k, y0 + ic + k, mine_col_);
            p.dot(x0 + ic + k, y0 + ic - k, mine_col_);
        }
        for (int y = 0; y < cs; ++y)
            for (int x = 0; x < cs; ++x) {
                const float dx = x + .5f - (ic + .5f), dy = y + .5f - (ic + .5f);
                if (dx * dx + dy * dy <= r * r) p.dot(x0 + x, y0 + y, mine_col_);
            }
        p.rect(x0 + ic - 2, y0 + ic - 2, cs >= 12 ? 2 : 1, cs >= 12 ? 2 : 1, mine_col_ == kMine ? 0xffffff : 0x303030);
    }

    void draw_flag(int x0, int y0, int cs) {
        Pix& p = board_;
        const int pole = x0 + cs / 2 + 1;
        const int top = y0 + 2;
        const int bottom = y0 + cs - 3;
        p.rect(pole, top, 1, bottom - top, pole_col_);
        // pennant pointing left
        static const int w12[] = {2, 4, 5, 4, 2};
        static const int w10[] = {2, 4, 4, 2};
        const int* ws = cs >= 12 ? w12 : w10;
        const int nh = cs >= 12 ? 5 : 4;
        for (int k = 0; k < nh; ++k) p.rect(pole - ws[k], top + k, ws[k], 1, kFlag);
        // base
        p.rect(pole - 2, bottom - 1, 5, 1, pole_col_);
        p.rect(pole - 3, bottom, 7, 1, pole_col_);
    }

    void draw_cross(int x0, int y0, int cs) {
        Pix& p = board_;
        for (int k = 2; k < cs - 1; ++k) {
            p.dot(x0 + k, y0 + k, kBoom);
            p.dot(x0 + k, y0 + cs - k, kBoom);
        }
    }

    void paint_board() {
        Pix& p = board_;
        if (!p.img) return;
        const Skin& s = skin();
        const int cs = kLevels[level_].cell;
        const bool over = phase_ == Phase::Lost || phase_ == Phase::Won;

        // Dark skins: opened cells sink toward the (black) field so they read
        // apart from the raised ones, and the mine / flag pole turn light.
        const bool dark = luma(s.face) < 100;
        const uint32_t open_bg = dark ? mix(s.face, s.field, 150) : s.bevel ? s.face : s.field;
        const uint32_t grid = dark ? mix(open_bg, s.hi, 70) : s.bevel ? s.lo : mix(s.field, s.lo, 110);
        mine_col_ = dark ? 0xc4c4cc : kMine;
        pole_col_ = dark ? 0xd8d8e0 : kInk;
        const bool dark_open = luma(open_bg) < 110;
        const uint32_t* nums = dark_open ? kNumDark : kNumLight;
        // Flat skins: tiles a shade darker than the face with a 1px light /
        // dark edge, so they stand apart from the white opened cells.
        const uint32_t up_face = s.bevel ? s.face : mix(s.face, s.lo, 64);
        const uint32_t up_hi = s.bevel ? s.hi : mix(s.face, 0xffffff, 200);
        const uint32_t up_lo = s.lo;

        // Sunken well around the board.
        p.rect(0, 0, p.w, p.h, open_bg);
        if (s.bevel) p.bevel(0, 0, p.w, p.h, 2, s.lo, s.hi);
        else p.bevel(0, 0, p.w, p.h, 2, s.lo, mix(s.lo, open_bg, 128));

        for (int i = 0; i < b_.n(); ++i) {
            const int x0 = 2 + (i % b_.cols) * cs, y0 = 2 + (i / b_.cols) * cs;
            const Cell st = b_.cell[i];
            const bool mine = b_.mine[i];
            const bool show_mine = over && phase_ == Phase::Lost && mine && st == Cell::Hidden;
            if (st == Cell::Open || show_mine) {
                p.rect(x0, y0, cs, cs, i == b_.boom ? kBoom : open_bg);
                p.rect(x0, y0, cs, 1, grid);
                p.rect(x0, y0, 1, cs, grid);
                if (mine) {
                    draw_mine(x0, y0, cs);
                } else if (b_.adj[i]) {
                    const int a = b_.adj[i];
                    const int gx = x0 + 1 + (cs - 1 - 6) / 2, gy = y0 + 1 + (cs - 1 - 8 + 1) / 2;
                    for (int y = 0; y < 8; ++y)
                        for (int x = 0; x < 6; ++x)
                            if (kGlyph[a][y][x] == '#') p.dot(gx + x, gy + y, nums[a]);
                }
                continue;
            }
            // Raised cell.
            p.rect(x0, y0, cs, cs, up_face);
            if (s.bevel) p.bevel(x0, y0, cs, cs, cs >= 12 ? 2 : 1, up_hi, up_lo);
            else {
                p.bevel(x0, y0, cs, cs, 1, up_hi, up_lo);
            }
            if (st == Cell::Flag) {
                draw_flag(x0, y0, cs);
                if (phase_ == Phase::Lost && !mine) {
                    // A wrong flag: the mark crossed out.
                    p.rect(x0 + 1, y0 + 1, cs - 2, cs - 2, up_face);
                    draw_mine(x0, y0, cs);
                    draw_cross(x0, y0, cs);
                }
            }
        }

        // Cursor (hidden once the game is over).
        if (!over) {
            const int x0 = 2 + cx_ * cs, y0 = 2 + cy_ * cs;
            const int ds = std::abs(luma(s.sel) - luma(up_face));
            const int dt = std::abs(luma(s.text) - luma(up_face));
            const uint32_t cur = ds >= 100 || ds >= dt ? s.sel : s.text;
            p.outline(x0, y0, cs, cs, cur);
            p.outline(x0 + 1, y0 + 1, cs - 2, cs - 2, cur);
        }
        p.show();
    }

    void build_card(lv_obj_t* body) {
        const Skin& s = skin();
        constexpr int w = 156, h = 82;
        const int x = kBoardAreaX + (kBoardAreaW - w) / 2, y = (kClientH - h) / 2;
        card_box_ = win::card(body, x, y, w, h);
        std::string head;
        if (phase_ == Phase::Won) head = new_best_ == level_ ? fmt("Cleared in %d s - new best!", final_secs_)
                                                             : fmt("Cleared in %d s!", final_secs_);
        else head = "Best times";
        win::text(card_box_, head, 6, 4, s.card_text, w - 12, true);
        for (int i = 0; i < kLevelCount; ++i) {
            const int ry = 20 + i * 13;
            const bool mark = i == new_best_;
            win::text(card_box_, kLevels[i].name, 10, ry, s.card_text, 90, mark);
            win::text(card_box_, best_[i] ? fmt("%3d s", best_[i]) : std::string("  --"), w - 46, ry,
                      s.card_text, 40, mark);
        }
        win::text(card_box_, phase_ == Phase::Won ? "Enter: new game" : "Any key: close", 6, h - 16,
                  s.card_text, w - 12);
    }

    // ── State ───────────────────────────────────────────────────────────────

    Board b_;
    int level_ = 0;
    Phase phase_ = Phase::Ready;
    int cx_ = 0, cy_ = 0;
    uint32_t start_ms_ = 0;
    uint32_t tense_until_ = 0;
    int final_secs_ = 0;
    int shown_secs_ = -1;
    int best_[kLevelCount] = {};
    int new_best_ = -1;
    bool card_ = false;
    bool demo_ = false;

    uint32_t mine_col_ = kMine, pole_col_ = kInk;
    Pix board_, mines_led_, time_led_, face_;
    lv_obj_t* level_lbl_ = nullptr;
    lv_obj_t* best_lbl_ = nullptr;
    lv_obj_t* size_lbl_ = nullptr;
    lv_obj_t* card_box_ = nullptr;
};

} // namespace

std::unique_ptr<Screen> make_mines_screen(Ctx& ctx) {
    return std::make_unique<MinesScreen>(ctx);
}

} // namespace mm::ui
