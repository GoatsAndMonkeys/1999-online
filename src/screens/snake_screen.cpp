// snake_screen.cpp — Snake, the way it looked on a 1999 desktop: a DOS-era
// shareware arcade game on a 16-colour blue arena, chunky bevelled bricks,
// a score / lives / level line on top and "LEVEL 2 - PUSH SPACE" boxes
// between levels. T switches to a second theme, a green phone LCD.
//
// The whole client area is one image: the game draws into a 308x117 buffer
// of palette roles (Role), which blit() turns into colours for the theme.
// Text uses a built-in 3x5 pixel font at 2x, so it looks like screen text of
// the time and not like the UI font.
//
// Game: 51x17 cells of 6 px. Arrows steer, Space / Enter pauses. Six apples
// clear a level; the next level has another wall layout (8 layouts, then they
// repeat one speed faster). Three lives. Speed 1..9 and walls / wrap (the
// outer wall, or wrap around the edges) are chosen on the title screen.
//
// Saved in <config>/snake.json (mm::store, nothing in --shots): speed, walls,
// theme and the top-5 table. Brag (key 8 / B) posts one short line to the
// public channel through Messenger::send(), only after a confirm box and an
// explicit Enter; demo states never send.
#include "screens.h"

#include "messenger.h"
#include "router.h"
#include "settings.h"
#include "widgets.h"
#include "win.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace mm::ui {
namespace {

// ── Geometry ────────────────────────────────────────────────────────────────
constexpr int kImgW = kClientW - 4;          // 308: inside the sunken frame
constexpr int kImgH = kClientH - 4;          // 117
constexpr int kStatusH = 12;
constexpr int kCell = 6;
constexpr int kCols = 51, kRows = 17;        // border cells included
constexpr int kOx = (kImgW - kCols * kCell) / 2;   // 1
constexpr int kOy = kStatusH;                // arena top
constexpr int kArenaH = kRows * kCell;       // 102

constexpr int kSpeeds = 9;
constexpr uint16_t kStepMs[kSpeeds] = {230, 195, 165, 140, 120, 100, 85, 72, 60};
constexpr int kLayouts = 8;
constexpr int kApplesPerLevel = 6;
constexpr int kLives = 3;
constexpr int kTableSize = 5;
constexpr size_t kNameMax = 9;

// ── Palette roles ───────────────────────────────────────────────────────────
enum Role : uint8_t {
    R_BG, R_ARENA, R_WALL, R_WALL_HI, R_WALL_LO, R_OBST, R_OBST_HI, R_OBST_LO,
    R_SNAKE, R_SNAKE_LO, R_EYE, R_FOOD, R_FOOD_HI, R_STEM,
    R_LABEL, R_VALUE, R_TEXT, R_BOX, R_BOX_EDGE, R_BOX_TEXT, R_BOX_HI, R_SHADOW,
    R_SEL, R_SEL_TEXT, R_T1, R_T2, R_T3, R_T4, R_T5, R_DIM,
    R_COUNT
};

// EGA / VGA 16-colour values.
constexpr uint32_t kBlack = 0x000000, kBlue = 0x0000aa, kGreen = 0x00aa00, kCyan = 0x00aaaa,
                   kRed = 0xaa0000, kBrown = 0xaa5500, kGray = 0xaaaaaa,
                   kDGray = 0x555555, kLBlue = 0x5555ff, kLGreen = 0x55ff55, kLCyan = 0x55ffff,
                   kLRed = 0xff5555, kLMagenta = 0xff55ff, kYellow = 0xffff55, kWhite = 0xffffff;

constexpr uint32_t kDos[R_COUNT] = {
    kBlack, kBlue, kRed, kLRed, 0x550000, kCyan, kLCyan, 0x004444,
    kYellow, kBrown, kBlack, kLRed, kWhite, kLGreen,
    kLCyan, kYellow, kWhite, kRed, kWhite, kWhite, kYellow, kBlack,
    kCyan, kBlack, kLRed, kYellow, kLGreen, kLCyan, kLMagenta, kGray,
};
// Phone LCD: background, ink, and a mid tone for shadows.
constexpr uint32_t kLcdBg = 0xa7c07b, kLcdInk = 0x26331c, kLcdMid = 0x8aa265;
constexpr uint32_t kLcd[R_COUNT] = {
    kLcdBg, kLcdBg, kLcdInk, kLcdInk, kLcdBg, kLcdInk, kLcdInk, kLcdBg,
    kLcdInk, kLcdBg, kLcdBg, kLcdInk, kLcdBg, kLcdInk,
    kLcdInk, kLcdInk, kLcdInk, kLcdBg, kLcdInk, kLcdInk, kLcdInk, kLcdMid,
    kLcdInk, kLcdBg, kLcdInk, kLcdInk, kLcdInk, kLcdInk, kLcdInk, kLcdInk,
};

// ── 3x5 pixel font (rows top to bottom, bit 2 = left column) ────────────────
const uint8_t* glyph(char ch) {
    static const uint8_t kDigits[10][5] = {
        {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 3, 1, 7}, {5, 5, 7, 1, 1},
        {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 2, 2}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
    };
    static const uint8_t kLetters[26][5] = {
        {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6}, {3, 4, 4, 4, 3}, {6, 5, 5, 5, 6}, {7, 4, 6, 4, 7},
        {7, 4, 6, 4, 4}, {3, 4, 5, 5, 3}, {5, 5, 7, 5, 5}, {7, 2, 2, 2, 7}, {1, 1, 1, 5, 2},
        {5, 5, 6, 5, 5}, {4, 4, 4, 4, 7}, {5, 7, 7, 5, 5}, {6, 5, 5, 5, 5}, {2, 5, 5, 5, 2},
        {6, 5, 6, 4, 4}, {2, 5, 5, 6, 3}, {6, 5, 6, 5, 5}, {3, 4, 2, 1, 6}, {7, 2, 2, 2, 2},
        {5, 5, 5, 5, 7}, {5, 5, 5, 5, 2}, {5, 5, 7, 7, 5}, {5, 5, 2, 5, 5}, {5, 5, 2, 2, 2},
        {7, 1, 2, 4, 7},
    };
    static const uint8_t kBlank[5] = {0, 0, 0, 0, 0};
    static const struct { char c; uint8_t rows[5]; } kPunct[] = {
        {'-', {0, 0, 7, 0, 0}}, {':', {0, 2, 0, 2, 0}}, {'.', {0, 0, 0, 0, 2}},
        {'!', {2, 2, 2, 0, 2}}, {'?', {6, 1, 2, 0, 2}}, {'/', {1, 1, 2, 4, 4}},
        {'\'', {2, 2, 0, 0, 0}}, {'=', {0, 7, 0, 7, 0}}, {'_', {0, 0, 0, 0, 7}},
        {'(', {2, 4, 4, 4, 2}}, {')', {2, 1, 1, 1, 2}}, {'>', {4, 2, 1, 2, 4}},
        {'<', {1, 2, 4, 2, 1}}, {'+', {0, 2, 7, 2, 0}}, {'*', {0, 5, 2, 5, 0}},
        {',', {0, 0, 0, 2, 4}}, {'#', {5, 7, 5, 7, 5}},
    };
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    if (ch >= '0' && ch <= '9') return kDigits[ch - '0'];
    if (ch >= 'A' && ch <= 'Z') return kLetters[ch - 'A'];
    for (const auto& p : kPunct)
        if (p.c == ch) return p.rows;
    return kBlank;
}

// ── Pixel surface ───────────────────────────────────────────────────────────
struct Surface {
    std::vector<uint8_t> px = std::vector<uint8_t>(static_cast<size_t>(kImgW) * kImgH, R_BG);

    void set(int x, int y, uint8_t r) {
        if (x >= 0 && y >= 0 && x < kImgW && y < kImgH) px[static_cast<size_t>(y) * kImgW + x] = r;
    }
    void rect(int x, int y, int w, int h, uint8_t r) {
        for (int j = y; j < y + h; ++j)
            for (int i = x; i < x + w; ++i) set(i, j, r);
    }
    void frame(int x, int y, int w, int h, uint8_t r) {
        rect(x, y, w, 1, r); rect(x, y + h - 1, w, 1, r);
        rect(x, y, 1, h, r); rect(x + w - 1, y, 1, h, r);
    }
    static int text_w(const std::string& s, int scale = 2) {
        return s.empty() ? 0 : static_cast<int>(s.size()) * 4 * scale - scale;
    }
    // Returns the x after the text (one gap included).
    int text(int x, int y, const std::string& s, uint8_t r, int scale = 2) {
        for (char ch : s) {
            const uint8_t* g = glyph(ch);
            for (int row = 0; row < 5; ++row)
                for (int col = 0; col < 3; ++col)
                    if (g[row] & (4 >> col)) rect(x + col * scale, y + row * scale, scale, scale, r);
            x += 4 * scale;
        }
        return x;
    }
    void text_c(int cx, int y, const std::string& s, uint8_t r, int scale = 2) {
        text(cx - text_w(s, scale) / 2, y, s, r, scale);
    }
    // DOS dialog: drop shadow, fill, double-line border.
    void dialog(int x, int y, int w, int h) {
        rect(x + 3, y + 3, w, h, R_SHADOW);
        rect(x, y, w, h, R_BOX);
        frame(x + 1, y + 1, w - 2, h - 2, R_BOX_EDGE);
        frame(x + 3, y + 3, w - 6, h - 6, R_BOX_EDGE);
    }
};

// ── Game model ──────────────────────────────────────────────────────────────
enum class Dir : uint8_t { Up, Down, Left, Right };
constexpr int kDx[4] = {0, 0, -1, 1};
constexpr int kDy[4] = {-1, 1, 0, 0};
bool opposite(Dir a, Dir b) {
    return kDx[int(a)] + kDx[int(b)] == 0 && kDy[int(a)] + kDy[int(b)] == 0;
}

struct Cell {
    int x = 0, y = 0;
    bool operator==(const Cell& o) const { return x == o.x && y == o.y; }
};

struct Rng {
    uint32_t s = 1999;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    int below(int n) { return n <= 0 ? 0 : static_cast<int>(next() % static_cast<uint32_t>(n)); }
};

int idx(int x, int y) { return y * kCols + x; }

// Wall layouts for interior cells x 1..49, y 1..15 (all original).
void add_layout(std::vector<uint8_t>& w, int layout) {
    auto hline = [&](int x0, int x1, int y) { for (int x = x0; x <= x1; ++x) w[idx(x, y)] = 2; };
    auto vline = [&](int x, int y0, int y1) { for (int y = y0; y <= y1; ++y) w[idx(x, y)] = 2; };
    switch (layout % kLayouts) {
        case 0: break;                                              // open field
        case 1: hline(12, 38, 8); break;                            // the bar
        case 2: vline(16, 4, 12); vline(34, 4, 12); break;          // two posts
        case 3:                                                     // broken cross
            hline(9, 20, 8); hline(30, 41, 8); vline(25, 2, 5); vline(25, 11, 14); break;
        case 4:                                                     // four corners
            hline(7, 15, 3); vline(7, 3, 6); hline(35, 43, 3); vline(43, 3, 6);
            hline(7, 15, 13); vline(7, 10, 13); hline(35, 43, 13); vline(43, 10, 13); break;
        case 5:                                                     // the vault
            hline(15, 23, 4); hline(27, 35, 4); hline(15, 23, 12); hline(27, 35, 12);
            vline(15, 4, 6); vline(15, 10, 12); vline(35, 4, 6); vline(35, 10, 12); break;
        case 6:                                                     // switchbacks
            hline(1, 36, 4); hline(14, 49, 8); hline(1, 36, 12); break;
        case 7:                                                     // pillars
            for (int px : {8, 16, 24, 32, 40})
                for (int py : {4, 11}) { hline(px, px + 1, py); hline(px, px + 1, py + 1); }
            break;
    }
}

struct Game {
    std::deque<Cell> body;        // front = head
    std::vector<uint8_t> occ = std::vector<uint8_t>(kCols * kRows, 0);
    std::vector<uint8_t> wall = std::vector<uint8_t>(kCols * kRows, 0);   // 1 border, 2 obstacle
    Dir dir = Dir::Right;
    Cell food{-1, -1};
    int grow = 0;
    bool walls = true;
    int layout = 0;
    Rng rng;

    enum class Step { Moved, Ate, Died, Full };

    void setup(int lay, bool with_walls) {
        layout = lay;
        walls = with_walls;
        std::fill(wall.begin(), wall.end(), 0);
        if (walls) {
            for (int x = 0; x < kCols; ++x) wall[idx(x, 0)] = wall[idx(x, kRows - 1)] = 1;
            for (int y = 0; y < kRows; ++y) wall[idx(0, y)] = wall[idx(kCols - 1, y)] = 1;
        }
        add_layout(wall, lay);
        respawn();
    }
    void respawn() {
        std::fill(occ.begin(), occ.end(), 0);
        body.clear();
        grow = 0;
        dir = Dir::Right;
        static const int kOrder[] = {8, 7, 9, 6, 10, 5, 11, 2, 14, 3, 13};
        int row = 8;
        for (int r : kOrder) {
            bool clear = true;
            for (int x = 2; x <= 17; ++x) clear = clear && !wall[idx(x, r)];
            if (clear) { row = r; break; }
        }
        for (int x = 7; x >= 3; --x) {
            body.push_back({x, row});
            occ[idx(x, row)] = 1;
        }
        if (food.x < 0 || blocked(food)) place_food();
    }
    bool blocked(const Cell& c) const { return wall[idx(c.x, c.y)] || occ[idx(c.x, c.y)]; }
    bool place_food() {
        std::vector<int> free;
        for (int i = 0; i < kCols * kRows; ++i)
            if (!wall[i] && !occ[i]) free.push_back(i);
        // Keep apples off the row right in front of a fresh snake.
        if (free.empty()) { food = {-1, -1}; return false; }
        const int pick = free[static_cast<size_t>(rng.below(static_cast<int>(free.size())))];
        food = {pick % kCols, pick / kCols};
        return true;
    }
    // Where the head would go; false if off the field with walls on.
    bool ahead(Dir d, Cell& out) const {
        Cell n{body.front().x + kDx[int(d)], body.front().y + kDy[int(d)]};
        if (n.x < 0 || n.y < 0 || n.x >= kCols || n.y >= kRows) {
            if (walls) return false;
            n.x = (n.x + kCols) % kCols;
            n.y = (n.y + kRows) % kRows;
        }
        out = n;
        return true;
    }
    bool safe(Dir d) const {
        Cell n;
        if (!ahead(d, n) || wall[idx(n.x, n.y)]) return false;
        if (!occ[idx(n.x, n.y)]) return true;
        return n == body.back() && grow == 0 && !(n == food);
    }
    Step step() {
        Cell n;
        if (!ahead(dir, n) || wall[idx(n.x, n.y)]) return Step::Died;
        const bool eat = n == food;
        const Cell tail = body.back();
        const bool tail_moves = grow == 0 && !eat;
        if (occ[idx(n.x, n.y)] && !(tail_moves && n == tail)) return Step::Died;
        if (tail_moves) {
            occ[idx(tail.x, tail.y)] = 0;
            body.pop_back();
        } else if (grow > 0) {
            --grow;
        }
        body.push_front(n);
        occ[idx(n.x, n.y)] = 1;
        if (!eat) return Step::Moved;
        grow += 2;                       // three segments per apple in all
        return place_food() ? Step::Ate : Step::Full;
    }
    // Attract mode: head for the apple, never into anything if it can help it.
    void autopilot() {
        const Cell h = body.front();
        int best = -1, best_d = 1 << 30;
        for (int d = 0; d < 4; ++d) {
            const Dir dd = static_cast<Dir>(d);
            if (opposite(dd, dir) || !safe(dd)) continue;
            Cell n;
            ahead(dd, n);
            int dx = std::abs(n.x - food.x), dy = std::abs(n.y - food.y);
            if (!walls) { dx = std::min(dx, kCols - dx); dy = std::min(dy, kRows - dy); }
            const int dist = dx + dy + (dd == dir ? 0 : 1);
            if (dist < best_d) { best_d = dist; best = d; }
        }
        (void)h;
        if (best >= 0) dir = static_cast<Dir>(best);
    }
};

struct Score {
    std::string name;
    int score = 0;
    int level = 1;
    int speed = 5;
    bool walls = true;
};

std::string upper_name(const std::string& n) {
    std::string out;
    for (char ch : n) {
        if (out.size() >= kNameMax) break;
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        out += (ch >= 32 && ch < 127) ? ch : '?';
    }
    return out;
}

std::string fmtn(const char* f, int v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

// The line Brag posts on the public channel.
std::string brag_text(int score, int level, int speed, bool walls) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "I scored %d at Snake (level %d, speed %d%s) on 1999 Online. Beat that!",
                  score, level, speed, walls ? "" : ", wrap");
    return buf;
}

// ── Screen ──────────────────────────────────────────────────────────────────
class SnakeScreen final : public Screen {
public:
    explicit SnakeScreen(Ctx& ctx) : Screen(ctx) {
        load();
        rng_.s = lv_tick_get() | 1u;
        reset_attract();
    }
    ~SnakeScreen() override { save(); }

    const char* title() const override { return "Snake"; }
    bool windowed() const override { return true; }

    std::string hints() const override {
        switch (mode_) {
            case Mode::Playing: return "Arrows:steer  Space:pause  Esc:pause";
            case Mode::Ready:   return "Space/arrow:go  Esc:pause";
            case Mode::Paused:  return "Space:resume  Esc:quit";
            case Mode::Dying:   return "Ouch!";
            case Mode::Brag:    return "Enter:post it  Esc:cancel";
            default:            return "Space:play  </>:speed  W:walls  T:theme  Esc:back";
        }
    }

    SoftKeys softkeys() const override {
        SoftKeys k;
        switch (mode_) {
            case Mode::Title:
                k.set(0, "PLAY").set(1, fmtn("SPEED %d", speed_)).set(2, walls_ ? "WALLS" : "WRAP")
                 .set(3, "SCORES").set(4, "BRAG", best() > 0);
                k.left.text = MM_SOFT_BACK;
                break;
            case Mode::Paused:
                k.set(0, "RESUME").set(1, "END").set(4, lcd_ ? "DOS" : "LCD");
                k.left.text = MM_SOFT_BACK;
                break;
            case Mode::Over:
                k.set(0, "AGAIN").set(1, "MENU").set(4, "BRAG", last_.score > 0);
                k.left.text = MM_SOFT_BACK;
                break;
            case Mode::Scores:
                k.set(0, "PLAY").set(1, "MENU").set(4, "BRAG", best() > 0);
                k.left.text = MM_SOFT_BACK;
                break;
            case Mode::Brag:
                k.set(0, "POST").set(4, "CANCEL");
                break;
            default: break;   // in play: the hint strip
        }
        return k;
    }

    void build(lv_obj_t* body) override {
        vbuf_.assign(static_cast<size_t>(kImgW) * kImgH, 0xff000000u);
        vdsc_ = lv_image_dsc_t{};
        vdsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        vdsc_.header.cf = LV_COLOR_FORMAT_XRGB8888;
        vdsc_.header.w = kImgW;
        vdsc_.header.h = kImgH;
        vdsc_.header.stride = kImgW * 4;
        vdsc_.data = reinterpret_cast<const uint8_t*>(vbuf_.data());
        vdsc_.data_size = static_cast<uint32_t>(vbuf_.size() * 4);

        frame_ = win::frame(body, 0, 0, kClientW, kClientH, win::Edge::Sunken,
                            lcd_ ? kLcdBg : kBlack);
        img_ = lv_image_create(frame_);
        lv_image_set_src(img_, &vdsc_);
        lv_obj_set_pos(img_, 2, 2);
        lv_obj_set_size(img_, kImgW, kImgH);
        render();
    }

    void on_hide() override {
        if (mode_ == Mode::Playing || mode_ == Mode::Ready) mode_ = Mode::Paused;
        save();
    }

    void tick(uint32_t now) override {
        if (demo_ || !img_) return;
        bool dirty = false;
        if (now - blink_ms_ >= 400) {
            blink_ms_ = now;
            blink_ = !blink_;
            dirty = mode_ != Mode::Playing;
        }
        switch (mode_) {
            case Mode::Title:
                if (now - step_ms_ >= 110) {
                    step_ms_ = now;
                    attract_.autopilot();
                    const auto r = attract_.step();
                    if (r == Game::Step::Died || r == Game::Step::Full || attract_.body.size() > 60) reset_attract();
                    dirty = true;
                }
                break;
            case Mode::Playing:
                if (now - step_ms_ >= kStepMs[eff_speed() - 1]) {
                    step_ms_ = now;
                    advance(now);
                    dirty = true;
                }
                break;
            case Mode::Dying:
                if (now - dying_ms_ >= 1300) {
                    finish_life();
                    ctx_.router.refresh_chrome();
                }
                dirty = true;
                break;
            default: break;
        }
        if (dirty) render();
    }

    bool on_key(const KeyEvent& e) override {
        if (demo_) demo_ = false;
        const int slot = softkey_slot(e);
        const bool back = e.key == Key::B || slot == kSoftLeft;
        const bool go = e.key == Key::A || slot == 0;
        const char ch = e.key == Key::Char ? static_cast<char>(e.ch) : 0;
        if (ch == 't' && mode_ != Mode::Playing) {
            lcd_ = !lcd_;
            save();
            ctx_.router.rebuild();
            return true;
        }
        switch (mode_) {
            case Mode::Title:
                if (back) return false;                      // the Router pops us
                if (go || ch == 'p') { new_game(); break; }
                if (e.key == Key::Left || e.key == Key::Down) { speed_ = std::max(1, speed_ - 1); save(); break; }
                if (e.key == Key::Right || e.key == Key::Up) { speed_ = std::min(kSpeeds, speed_ + 1); save(); break; }
                if (slot == 1 || ch == 's') { speed_ = speed_ % kSpeeds + 1; save(); break; }
                if (slot == 2 || ch == 'w') { walls_ = !walls_; save(); break; }
                if (slot == 3 || ch == 'h') { mode_ = Mode::Scores; new_rank_ = -1; break; }
                if ((slot == 4 || ch == 'b') && best() > 0) open_brag(table_.front());
                break;
            case Mode::Ready:
                if (back || ch == 'p') { mode_ = Mode::Paused; break; }
                if (is_direction(e.key)) {
                    const Dir d = to_dir(e.key);
                    if (!opposite(d, game_.dir)) game_.dir = d;
                } else if (!go) {
                    break;
                }
                mode_ = Mode::Playing;
                step_ms_ = lv_tick_get();
                break;
            case Mode::Playing:
                if (is_direction(e.key)) {
                    if (!e.repeat && queue_.size() < 3) queue_.push_back(to_dir(e.key));
                    return true;
                }
                if (go || back || ch == 'p') mode_ = Mode::Paused;
                break;
            case Mode::Paused:
                if (back) return false;                      // quit to the menu
                if (go || ch == 'p') { mode_ = Mode::Playing; step_ms_ = lv_tick_get(); break; }
                if (slot == 1) { to_title(); break; }
                if (slot == 4) { lcd_ = !lcd_; save(); ctx_.router.rebuild(); return true; }
                break;
            case Mode::Dying:
                if (back) return false;
                break;
            case Mode::Over:
                if (back || slot == 1) { to_title(); break; }
                if (go) { new_game(); break; }
                if ((slot == 4 || ch == 'b') && last_.score > 0) open_brag(last_);
                break;
            case Mode::Scores:
                if (back || slot == 1) { to_title(); break; }
                if (go) { new_game(); break; }
                if ((slot == 4 || ch == 'b') && best() > 0) open_brag(table_.front());
                break;
            case Mode::Brag:
                if (back || slot == 4) { mode_ = brag_from_; break; }
                if (go) { post_brag(); mode_ = brag_from_; }
                break;
        }
        render();
        return true;                                         // arrows never leave the screen
    }

    int demo_state_count() const override { return 6; }
    const char* demo_state_name(int i) const override {
        static const char* kNames[] = {"title", "ready", "playing", "gameover", "brag", "lcd"};
        return (i >= 0 && i < 6) ? kNames[i] : "main";
    }
    void set_demo_state(int i) override {
        demo_ = true;
        blink_ = true;
        lcd_ = false;
        speed_ = 5;
        walls_ = true;
        table_ = {
            {"KD2ABC-m", 2850, 5, 7, true},
            {my_name(), 1950, 3, 5, true},
            {"N0DE", 900, 2, 4, false},
            {"Wardriver", 600, 2, 5, true},
            {"Lurker", 150, 1, 3, true},
        };
        new_rank_ = -1;
        switch (i) {
            default:
            case 0:
                reset_attract();
                for (int s = 0; s < 70; ++s) { attract_.autopilot(); attract_.step(); }
                mode_ = Mode::Title;
                break;
            case 1:
                start_level(2, 2);
                score_ = 450;
                lives_ = 3;
                mode_ = Mode::Ready;
                break;
            case 2:
            case 5:
                demo_mid_game();
                mode_ = Mode::Playing;
                lcd_ = i == 5;
                break;
            case 3:
                demo_mid_game();
                last_ = table_[1];
                score_ = last_.score;
                lives_ = 0;
                new_rank_ = 1;
                mode_ = Mode::Over;
                break;
            case 4:
                demo_mid_game();
                last_ = table_[1];
                score_ = last_.score;
                lives_ = 0;
                brag_ = last_;
                brag_from_ = Mode::Over;
                mode_ = Mode::Brag;
                break;
        }
    }

private:
    enum class Mode : uint8_t { Title, Ready, Playing, Paused, Dying, Over, Scores, Brag };

    static Dir to_dir(Key k) {
        switch (k) {
            case Key::Up: return Dir::Up;
            case Key::Down: return Dir::Down;
            case Key::Left: return Dir::Left;
            default: return Dir::Right;
        }
    }

    int eff_speed() const { return std::min(kSpeeds, speed_ + (level_ - 1) / kLayouts); }
    int best() const { return table_.empty() ? 0 : table_.front().score; }
    std::string my_name() const {
        const std::string& n = messenger().my_name();
        return n.empty() ? std::string("You") : n;
    }

    // ── Flow ──
    void reset_attract() {
        attract_.rng.s = rng_.next() | 1u;
        attract_.food = {-1, -1};
        attract_.setup(0, false);
    }
    void to_title() {
        mode_ = Mode::Title;
        reset_attract();
    }
    void new_game() {
        score_ = 0;
        lives_ = kLives;
        start_level(1, 0);
    }
    void start_level(int level, int apples) {
        level_ = level;
        apples_ = apples;
        game_.rng.s = rng_.next() | 1u;
        game_.food = {-1, -1};
        game_.setup(level - 1, walls_);
        queue_.clear();
        mode_ = Mode::Ready;
    }
    void advance(uint32_t now) {
        while (!queue_.empty()) {
            const Dir d = queue_.front();
            queue_.pop_front();
            if (d != game_.dir && !opposite(d, game_.dir)) { game_.dir = d; break; }
        }
        switch (game_.step()) {
            case Game::Step::Moved: break;
            case Game::Step::Ate:
            case Game::Step::Full:
                score_ += 10 * eff_speed() * level_;
                if (++apples_ >= kApplesPerLevel) {
                    start_level(level_ + 1, 0);
                    ctx_.router.refresh_chrome();
                }
                break;
            case Game::Step::Died:
                mode_ = Mode::Dying;
                dying_ms_ = now;
                ctx_.router.refresh_chrome();
                break;
        }
    }
    void finish_life() {
        if (--lives_ > 0) {
            game_.respawn();
            queue_.clear();
            mode_ = Mode::Ready;
            return;
        }
        last_ = {my_name(), score_, level_, speed_, walls_};
        new_rank_ = record(last_);
        mode_ = Mode::Over;
    }
    // Inserts into the table; the rank (0-based) or -1.
    int record(const Score& s) {
        if (s.score <= 0) return -1;
        size_t pos = 0;
        while (pos < table_.size() && table_[pos].score >= s.score) ++pos;
        if (pos >= static_cast<size_t>(kTableSize)) return -1;
        table_.insert(table_.begin() + static_cast<long>(pos), s);
        if (table_.size() > static_cast<size_t>(kTableSize)) table_.resize(kTableSize);
        save();
        return static_cast<int>(pos);
    }

    // ── Brag (sends only from post_brag(), after the confirm box) ──
    void open_brag(const Score& s) {
        brag_ = s;
        brag_from_ = mode_;
        mode_ = Mode::Brag;
    }
    void post_brag() {
        if (demo_) return;                                   // never from --shots
        const std::string text = brag_text(brag_.score, brag_.level, brag_.speed, brag_.walls);
        const bool ok = messenger().send(Messenger::kChannelNode, text);
        toast(ok ? "Bragged on " + Messenger::channel_name() : std::string("Could not post the brag"),
              1800, ok ? color::kGood : color::kWarn);
    }

    // ── Demo helpers ──
    void demo_mid_game() {
        level_ = 3;
        apples_ = 4;
        lives_ = 2;
        score_ = 450 + 4 * 10 * 5 * 3;
        game_.rng.s = 77;
        game_.food = {40, 6};
        game_.setup(2, true);
        game_.grow = 12;
        const struct { Dir d; int n; } kPath[] = {
            {Dir::Right, 5}, {Dir::Up, 5}, {Dir::Right, 10}, {Dir::Down, 9}, {Dir::Right, 4}};
        for (const auto& p : kPath) {
            game_.dir = p.d;
            for (int i = 0; i < p.n; ++i) game_.step();
        }
        game_.food = {40, 6};
    }

    // ── Files ──
    void load() {
        JsonValue v;
        if (store::load_json(store::path("snake.json"), v) != store::Load::Ok) return;
        speed_ = std::clamp(static_cast<int>(v.get_int("speed", 5)), 1, kSpeeds);
        walls_ = v.get_bool("walls", true);
        lcd_ = v.get_string("theme", "dos") == "lcd";
        table_.clear();
        for (const JsonValue& s : v["scores"].items()) {
            Score sc{s.get_string("name", "?"), static_cast<int>(s.get_int("score")),
                     static_cast<int>(s.get_int("level", 1)), static_cast<int>(s.get_int("speed", 5)),
                     s.get_bool("walls", true)};
            if (sc.score > 0 && table_.size() < static_cast<size_t>(kTableSize)) table_.push_back(sc);
        }
        std::stable_sort(table_.begin(), table_.end(),
                         [](const Score& a, const Score& b) { return a.score > b.score; });
    }
    void save() {
        if (demo_ || !store::enabled()) return;
        std::string j = "{\n  \"version\": 1,\n";
        j += "  \"speed\": " + std::to_string(speed_) + ",\n";
        j += std::string("  \"walls\": ") + (walls_ ? "true" : "false") + ",\n";
        j += std::string("  \"theme\": ") + (lcd_ ? "\"lcd\"" : "\"dos\"") + ",\n";
        j += "  \"scores\": [";
        for (size_t i = 0; i < table_.size(); ++i) {
            const Score& s = table_[i];
            j += i ? ",\n    " : "\n    ";
            j += "{\"name\": " + store::quote(s.name) + ", \"score\": " + std::to_string(s.score) +
                 ", \"level\": " + std::to_string(s.level) + ", \"speed\": " + std::to_string(s.speed) +
                 ", \"walls\": " + (s.walls ? "true" : "false") + "}";
        }
        j += table_.empty() ? "]\n}\n" : "\n  ]\n}\n";
        store::write_atomic(store::path("snake.json"), j);
    }

    // ── Drawing ──
    void draw_cell_block(int cx, int cy, uint8_t fill, uint8_t hi, uint8_t lo) {
        const int x = kOx + cx * kCell, y = kOy + cy * kCell;
        sf_.rect(x, y, kCell, kCell, fill);
        sf_.rect(x, y, kCell, 1, hi);
        sf_.rect(x, y, 1, kCell, hi);
        sf_.rect(x, y + kCell - 1, kCell, 1, lo);
        sf_.rect(x + kCell - 1, y, 1, kCell, lo);
    }

    void draw_arena(const Game& g, bool snake_visible) {
        sf_.rect(kOx, kOy, kCols * kCell, kArenaH, R_ARENA);
        for (int y = 0; y < kRows; ++y)
            for (int x = 0; x < kCols; ++x) {
                const uint8_t w = g.wall[idx(x, y)];
                if (w == 1) draw_cell_block(x, y, R_WALL, R_WALL_HI, R_WALL_LO);
                else if (w == 2) draw_cell_block(x, y, R_OBST, R_OBST_HI, R_OBST_LO);
            }
        if (g.food.x >= 0) {   // an apple
            const int x = kOx + g.food.x * kCell, y = kOy + g.food.y * kCell;
            sf_.rect(x + 1, y + 1, 4, 5, R_FOOD);
            sf_.rect(x, y + 2, 6, 3, R_FOOD);
            sf_.set(x + 2, y + 2, R_FOOD_HI);
            sf_.set(x + 3, y, R_STEM);
            sf_.set(x + 4, y, R_STEM);
        }
        if (!snake_visible) return;
        for (size_t i = 0; i < g.body.size(); ++i) {
            const Cell& c = g.body[i];
            const int x = kOx + c.x * kCell, y = kOy + c.y * kCell;
            sf_.rect(x, y, kCell, kCell, R_SNAKE);
            sf_.rect(x, y + kCell - 1, kCell, 1, R_SNAKE_LO);
            sf_.rect(x + kCell - 1, y, 1, kCell, R_SNAKE_LO);
            if (i == 0) {                                     // eyes, looking ahead
                const Dir d = g.dir;
                const int ex0 = d == Dir::Left ? 1 : (d == Dir::Right ? 3 : 1);
                const int ex1 = d == Dir::Left ? 1 : (d == Dir::Right ? 3 : 3);
                const int ey0 = d == Dir::Up ? 1 : (d == Dir::Down ? 3 : 1);
                const int ey1 = d == Dir::Up ? 1 : (d == Dir::Down ? 3 : 3);
                sf_.set(x + ex0, y + ey0, R_EYE);
                sf_.set(x + ex1, y + ey1, R_EYE);
            }
        }
    }

    // "LABEL value" pairs across the status line.
    void status_line() {
        sf_.rect(0, 0, kImgW, kStatusH, R_BG);
        const int y = 1;
        if (mode_ == Mode::Title) {
            const int x = sf_.text(2, y, "HI", R_LABEL) + 4;
            sf_.text(x, y, fmtn("%05d", best()), R_VALUE);
            const std::string r = fmtn("SPEED %d ", speed_) + (walls_ ? "WALLS" : "WRAP");
            sf_.text(kImgW - Surface::text_w(r) - 2, y, r, R_LABEL);
            return;
        }
        int x = 2;
        auto pair = [&](const char* label, const std::string& value) {
            x = sf_.text(x, y, label, R_LABEL) + 4;
            x = sf_.text(x, y, value, R_VALUE) + 10;
        };
        pair("SCORE", fmtn("%05d", score_));
        pair("LIVES", fmtn("%d", lives_));
        pair("LEVEL", fmtn("%d", level_));
        pair("HI", fmtn("%05d", std::max(best(), score_)));
    }

    void table_rows(int x, int y, int highlight) {
        for (int i = 0; i < kTableSize; ++i) {
            const int ry = y + i * 11;
            std::string line;
            if (i < static_cast<int>(table_.size())) {
                const Score& s = table_[static_cast<size_t>(i)];
                std::string name = upper_name(s.name);
                name.resize(kNameMax, ' ');
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%d %s %05d L%d S%d", i + 1, name.c_str(), s.score,
                              std::min(s.level, 9), s.speed);
                line = buf;
            } else {
                line = fmtn("%d ---------  -----", i + 1);
            }
            if (i == highlight && blink_) {
                sf_.rect(x - 2, ry - 1, Surface::text_w(line) + 4, 12, R_SEL);
                sf_.text(x, ry, line, R_SEL_TEXT);
            } else {
                sf_.text(x, ry, line, i == highlight ? R_BOX_HI : R_BOX_TEXT);
            }
        }
    }

    void render() {
        if (!img_) return;
        std::fill(sf_.px.begin(), sf_.px.end(), static_cast<uint8_t>(R_BG));
        status_line();
        const int cx = kImgW / 2;
        switch (mode_) {
            case Mode::Title: {
                draw_arena(attract_, true);
                // Big logo, one colour per letter, with a drop shadow.
                static const char* kLogo = "SNAKE";
                static const uint8_t kCols5[] = {R_T1, R_T2, R_T3, R_T4, R_T5};
                const int sc = 4, lw = Surface::text_w(kLogo, sc);
                int lx = cx - lw / 2;
                const int ly = kOy + 10;
                sf_.rect(lx - 6, ly - 4, lw + 12, 5 * sc + 9, R_ARENA);
                for (int i = 0; i < 5; ++i) {
                    const std::string ch(1, kLogo[i]);
                    sf_.text(lx + 2, ly + 2, ch, R_SHADOW, sc);
                    sf_.text(lx, ly, ch, kCols5[i], sc);
                    lx += 4 * sc;
                }
                const std::string sub = "1999 ONLINE EDITION";
                sf_.rect(cx - Surface::text_w(sub) / 2 - 3, kOy + 38, Surface::text_w(sub) + 6, 13, R_ARENA);
                sf_.text_c(cx, kOy + 40, sub, R_LABEL);
                sf_.dialog(cx - 80, kOy + 60, 160, 22);
                if (blink_) sf_.text_c(cx, kOy + 66, "PUSH SPACE", R_BOX_TEXT);
                const std::string keys = "ARROWS STEER - T THEME";
                sf_.rect(cx - Surface::text_w(keys) / 2 - 3, kOy + 88, Surface::text_w(keys) + 6, 13, R_ARENA);
                sf_.text_c(cx, kOy + 90, keys, R_DIM);
                break;
            }
            case Mode::Ready: {
                draw_arena(game_, true);
                const std::string l = fmtn("LEVEL %d", level_);
                sf_.dialog(cx - 76, kOy + 30, 152, 42);
                sf_.text_c(cx, kOy + 38, l, R_BOX_HI);
                sf_.text_c(cx, kOy + 54, "PUSH SPACE", R_BOX_TEXT);
                break;
            }
            case Mode::Playing:
                draw_arena(game_, true);
                break;
            case Mode::Dying:
                draw_arena(game_, ((lv_tick_get() - dying_ms_) / 150) % 2 == 1);
                break;
            case Mode::Paused:
                draw_arena(game_, true);
                sf_.dialog(cx - 76, kOy + 30, 152, 42);
                sf_.text_c(cx, kOy + 38, "PAUSED", R_BOX_HI);
                sf_.text_c(cx, kOy + 54, "SPACE RESUMES", R_BOX_TEXT);
                break;
            case Mode::Over:
            case Mode::Scores: {
                draw_arena(game_, mode_ == Mode::Over);
                const int bw = 204, bh = 98, bx = cx - bw / 2, by = kOy + 1;
                sf_.dialog(bx, by, bw, bh);
                if (mode_ == Mode::Over) {
                    sf_.text_c(cx, by + 6, "GAME OVER", R_BOX_HI);
                    const bool best_now = new_rank_ == 0;
                    sf_.text_c(cx, by + 18, best_now ? "NEW HIGH SCORE!" : fmtn("YOUR SCORE %05d", last_.score),
                               R_BOX_TEXT);
                } else {
                    sf_.text_c(cx, by + 6, "HIGH SCORES", R_BOX_HI);
                    sf_.text_c(cx, by + 18, fmtn("SPEED %d", speed_) + (walls_ ? " WALLS" : " WRAP"), R_BOX_TEXT);
                }
                sf_.rect(bx + 6, by + 30, bw - 12, 1, R_BOX_EDGE);
                table_rows(bx + 10, by + 35, new_rank_);
                break;
            }
            case Mode::Brag: {
                draw_arena(game_, false);
                std::string ch = upper_name(Messenger::channel_name());
                const int bw = 236, bx = cx - bw / 2;
                sf_.dialog(bx, kOy + 18, bw, 66);
                sf_.text_c(cx, kOy + 26, "BRAG ON " + ch + "?", R_BOX_HI);
                sf_.text_c(cx, kOy + 41, fmtn("SCORE %05d", brag_.score) + fmtn(" LEVEL %d", brag_.level),
                           R_BOX_TEXT);
                sf_.text_c(cx, kOy + 59, "ENTER POSTS  ESC NO", R_BOX_TEXT);
                break;
            }
        }
        blit();
    }

    void blit() {
        const uint32_t* pal = lcd_ ? kLcd : kDos;
        for (size_t i = 0; i < vbuf_.size(); ++i) vbuf_[i] = pal[sf_.px[i]] | 0xff000000u;
        if (lcd_) {   // LCD pixel grid: a faint line between the 6 px cells of the arena
            for (int y = kOy; y < kOy + kArenaH; ++y)
                for (int x = kOx; x < kOx + kCols * kCell; ++x) {
                    if ((x - kOx) % kCell != kCell - 1 && (y - kOy) % kCell != kCell - 1) continue;
                    uint32_t& p = vbuf_[static_cast<size_t>(y) * kImgW + x];
                    if ((p & 0xffffff) == kLcdBg) p = 0xff000000u | 0x9fb874;
                }
        }
        lv_obj_invalidate(img_);
    }

    // State.
    Mode mode_ = Mode::Title;
    Mode brag_from_ = Mode::Title;
    Game game_, attract_;
    Rng rng_;
    std::deque<Dir> queue_;
    int speed_ = 5;
    bool walls_ = true;
    bool lcd_ = false;
    int level_ = 1, apples_ = 0, lives_ = kLives, score_ = 0;
    std::vector<Score> table_;
    Score last_, brag_;
    int new_rank_ = -1;
    bool demo_ = false;
    bool blink_ = true;
    uint32_t blink_ms_ = 0, step_ms_ = 0, dying_ms_ = 0;

    // Drawing.
    Surface sf_;
    std::vector<uint32_t> vbuf_;
    lv_image_dsc_t vdsc_{};
    lv_obj_t* frame_ = nullptr;
    lv_obj_t* img_ = nullptr;
};

} // namespace

std::unique_ptr<Screen> make_snake_screen(Ctx& ctx) {
    return std::make_unique<SnakeScreen>(ctx);
}

} // namespace mm::ui
