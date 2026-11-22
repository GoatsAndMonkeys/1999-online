// gym_screen.cpp — Gyms: the 8 Kanto gym leaders (LORD door game rosters)
// plus the Indigo Plateau row, and a per-gym detail page with "Challenge".
//
// Data comes straight from the shared LORD tables (LordGyms.h / LordE4.h /
// LordLogic.h). LORD has no gym-order rule: every un-badged gym can be
// challenged; a gym whose badge you already hold can't be replayed (same
// anti-farm rule as mmterm's gym select). The Indigo Plateau needs all 8.
#include "screens.h"

#include "game_state.h"
#include "router.h"
#include "widgets.h"

#include "LordGyms.h"
#include "LordLogic.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace mm::ui {
namespace {

// Gym speciality (index into style.h's Gen 1 type list). LordGym has no type.
constexpr int kGymType[LORD_GYM_COUNT] = {
    5,    // Brock    Rock
    10,   // Misty    Water
    12,   // Surge    Electric
    11,   // Erika    Grass
    3,    // Koga     Poison
    13,   // Sabrina  Psychic
    9,    // Blaine   Fire
    4,    // Giovanni Ground
};

// style.h's type_name() abbreviates ("Electr"); the tiles have room for full words.
constexpr const char* kGymTypeName[LORD_GYM_COUNT] = {
    "Rock", "Water", "Electric", "Grass", "Poison", "Psychic", "Fire", "Ground",
};

constexpr int kLeague = LORD_GYM_COUNT;   // selection index of the Indigo row
constexpr int kSelCount = LORD_GYM_COUNT + 1;

// Grid geometry (body 320x138): two rows of 4 tiles, league row beneath.
constexpr int kGap = 3;
constexpr int kCols = 4;
constexpr int kTileW = (kBodyW - kGap * (kCols + 1)) / kCols;   // 76
constexpr int kTileH = 53;
constexpr int kLeagueY = kGap + 2 * (kTileH + kGap);            // 115
constexpr int kLeagueH = kBodyH - kLeagueY - kGap;               // 20

const LordGymTrainer* leader_of(int gym) {
    const LordGym* g = lordGym(static_cast<uint8_t>(gym));
    return g ? &g->trainers[lordGymLeaderIndex(g)] : nullptr;
}

uint8_t scaled(uint8_t lvl) { return lordScaleLevel(lvl, lordCurrentNgPlusTier(), false); }

// Leader's ace = highest-level mon (last one on ties, i.e. the closer).
const LordGymMon* ace_of(int gym) {
    const LordGymTrainer* t = leader_of(gym);
    if (!t || !t->count) return nullptr;
    const LordGymMon* best = &t->party[0];
    for (int i = 1; i < t->count; ++i)
        if (t->party[i].level >= best->level) best = &t->party[i];
    return best;
}

class GymScreen final : public Screen {
public:
    explicit GymScreen(Ctx& ctx) : Screen(ctx) {}

    const char* title() const override { return "Gyms"; }

    std::string hints() const override {
        if (detail_) return has_badge(sel_) ? "L/R:other gym  Esc:back"
                                            : "Enter:challenge  L/R:other gym  Esc:back";
        return "Arrows:move  Enter:open  1-8:gym  Esc:back";
    }

    void build(lv_obj_t* body) override {
        tiles_.fill(nullptr);
        league_ = nullptr;
        seen_badges_ = badges();
        if (detail_) build_detail(body);
        else build_grid(body);
    }

    // Keys 4..6: previous gym, open / fight, next gym. Key 3: back.
    SoftKeys softkeys() const override {
        SoftKeys k;
        k.left.text = MM_SOFT_BACK;
        if (detail_) k.set(0, "PREV").set(1, "FIGHT", !has_badge(sel_)).set(2, "NEXT");
        else k.set(0, "PREV").set(1, "OPEN").set(2, "NEXT");
        return k;
    }

    bool on_key(const KeyEvent& in) override {
        KeyEvent e = in;
        switch (softkey_slot(in)) {
            case 0: e.key = Key::Left; break;
            case 1: e.key = Key::A; break;
            case 2: e.key = Key::Right; break;
            case kSoftNone: break;
            case kSoftLeft:
                if (!detail_) return false;   // Router: back
                e.key = Key::B;
                break;
            default: return true;
        }
        return detail_ ? detail_key(e) : grid_key(e);
    }

    void tick(uint32_t) override {
        // Badges can change underneath us (a battle won, mmterm wrote lord.dat).
        if (!override_ && ctx_.game.badges() != seen_badges_) ctx_.router.rebuild();
    }

    int demo_state_count() const override { return 4; }
    const char* demo_state_name(int i) const override {
        static const char* const kNames[] = {"grid", "grid_badges", "detail", "league"};
        return (i >= 0 && i < 4) ? kNames[i] : "grid";
    }
    void set_demo_state(int i) override {
        switch (i) {
            default:
            case 0: override_.reset(); detail_ = false; sel_ = 0; break;
            case 1: override_ = 0x07; detail_ = false; sel_ = 3; col_ = 3; break;   // 3 badges, Erika next
            case 2: override_ = 0x07; detail_ = true;  sel_ = 4; col_ = 0; break;   // Koga detail
            case 3: override_ = 0xFF; detail_ = false; sel_ = kLeague; break;      // league open
        }
    }

private:
    // ── Badge view (demo override never touches the save) ──────────────────
    uint8_t badges() const { return override_ ? *override_ : ctx_.game.badges(); }
    bool has_badge(int g) const { return g >= 0 && g < 8 && ((badges() >> g) & 1); }
    int badge_count() const { return __builtin_popcount(badges()); }

    // ── Grid ────────────────────────────────────────────────────────────────
    void build_grid(lv_obj_t* body) {
        for (int i = 0; i < LORD_GYM_COUNT; ++i) {
            const LordGym* g = lordGym(static_cast<uint8_t>(i));
            if (!g) continue;
            const int x = kGap + (i % kCols) * (kTileW + kGap);
            const int y = kGap + (i / kCols) * (kTileH + kGap);
            const bool won = has_badge(i);
            lv_obj_t* t = panel(body, x, y, kTileW, kTileH);
            tiles_[i] = t;

            label(t, g->leaderName, 4, 1, font_body(), color::kText, won ? 54 : 68);
            if (won) label(t, LV_SYMBOL_OK, 61, 2, font_body(), color::kGood);
            label(t, g->city, 4, 16, font_small(), color::kDim, 68);
            if (const LordGymMon* a = ace_of(i)) sprite_box(t, a->species, kTileW - 32, kTileH - 32, 28, 28);
            chip(t, kGymTypeName[i], 4, 36, type_color(kGymType[i]));
        }

        // Indigo Plateau row.
        const bool open = badge_count() >= LORD_GYM_COUNT;
        lv_obj_t* r = panel(body, kGap, kLeagueY, kBodyW - 2 * kGap, kLeagueH);
        league_ = r;
        label(r, "Indigo Plateau", 6, 2, font_body(), open ? color::kAccent : color::kDim);
        // Badge case: one pip per gym, in its type colour once earned.
        for (int i = 0; i < LORD_GYM_COUNT; ++i) {
            lv_obj_t* p = box(r, 104 + i * 11, 5, 8, 8);
            lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(p, 1, 0);
            lv_obj_set_style_border_color(p, c(color::kBorder), 0);
            lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(p, has_badge(i) ? type_color(kGymType[i]) : c(color::kBar), 0);
        }
        const std::string st = open ? std::string("Elite Four  " LV_SYMBOL_RIGHT)
                                    : fmt("Locked  %d/8 badges", badge_count());
        lv_obj_t* s = label(r, st, 0, 3, font_small(), open ? color::kText : color::kDim);
        lv_obj_align(s, LV_ALIGN_TOP_RIGHT, -6, 3);
        paint();
    }

    void paint() {
        for (int i = 0; i < kSelCount; ++i) {
            lv_obj_t* o = i == kLeague ? league_ : tiles_[i];
            if (!o) continue;
            const bool s = i == sel_;
            lv_obj_set_style_border_color(o, c(s ? color::kAccent : color::kBorder), 0);
            lv_obj_set_style_border_width(o, s ? 2 : 1, 0);
            lv_obj_set_style_bg_color(o, c(s ? color::kPanelHi : color::kPanel), 0);
        }
    }

    bool grid_key(const KeyEvent& e) {
        const int before = sel_;
        switch (e.key) {
            case Key::Left:  sel_ = (sel_ + kSelCount - 1) % kSelCount; break;
            case Key::Right: sel_ = (sel_ + 1) % kSelCount; break;
            case Key::Up:
                if (sel_ == kLeague) sel_ = kCols + col_;
                else if (sel_ < kCols) sel_ = kLeague;
                else sel_ -= kCols;
                break;
            case Key::Down:
                if (sel_ == kLeague) sel_ = col_;
                else if (sel_ >= kCols) sel_ = kLeague;
                else sel_ += kCols;
                break;
            case Key::A: open(sel_); return true;
            default: return false;
        }
        if (sel_ < kLeague) col_ = sel_ % kCols;
        if (sel_ != before) paint();
        return true;
    }

    void open(int i) {
        if (i == kLeague) {
            if (badge_count() < LORD_GYM_COUNT)
                toast(fmt("Earn all 8 badges first (%d/8)", badge_count()), 2200, color::kWarn);
            else
                toast("Elite Four: coming soon");
            return;
        }
        detail_ = true;
        ctx_.router.rebuild();
    }

    // ── Detail ──────────────────────────────────────────────────────────────
    void build_detail(lv_obj_t* body) {
        const int gi = sel_;
        const LordGym* g = lordGym(static_cast<uint8_t>(gi));
        const LordGymTrainer* ldr = leader_of(gi);
        const LordGymMon* ace = ace_of(gi);
        if (!g || !ldr || !ace) return;
        const bool won = has_badge(gi);
        const int type = kGymType[gi];

        // Left card: ace, leader, town, badge, type.
        lv_obj_t* l = panel(body, kGap, kGap, 104, kBodyH - 2 * kGap);
        sprite_box(l, ace->species, 19, 0, 64, 64);
        label(l, g->leaderName, 5, 62, font_title(), color::kAccent, 94);
        label(l, fmt("%s Gym", g->city), 5, 80, font_small(), color::kText, 94);
        label(l, fmt("%s Badge", g->badgeName), 5, 93, font_small(), won ? color::kGood : color::kDim, 94);
        chip(l, kGymTypeName[gi], 5, 110, type_color(type));
        if (won) label(l, LV_SYMBOL_OK " earned", 52, 109, font_small(), color::kGood);

        // Right: leader's team.
        constexpr int rx = 112, rw = kBodyW - rx - kGap;   // 205
        label(body, "Leader's team", rx, 3, font_small(), color::kDim);
        label(body, fmt("%d/8", gi + 1), rx + rw - 22, 3, font_small(), color::kDim, 22);
        const int n = std::min<int>(ldr->count, 6);
        const int step = std::min(40, rw / std::max(1, n));
        int ace_lvl = 0;
        for (int i = 0; i < n; ++i) {
            const LordGymMon& m = ldr->party[i];
            const int lvl = scaled(m.level);
            ace_lvl = std::max(ace_lvl, lvl);
            const int x = rx + i * step;
            sprite_box(body, m.species, x + (step - 32) / 2, 15, 32, 32);
            lv_obj_t* lv = label(body, fmt("L%d", lvl), 0, 48, font_small(),
                                 &m == ace ? color::kAccent : color::kText);
            lv_obj_update_layout(lv);
            lv_obj_set_x(lv, x + (step - lv_obj_get_width(lv)) / 2);
        }
        label(body, fmt("%d trainers in a row, %s last", g->trainerCount, g->leaderName),
              rx, 62, font_small(), color::kDim, rw);

        // Recommended level vs. party.
        const int rec = scaled(g->minLevelHint);
        const auto& party = ctx_.game.party();
        int best = 0;
        for (const auto& m : party) best = std::max<int>(best, m.level);
        uint32_t vcol = color::kBad;
        const char* verdict = "Underleveled";
        if (best >= ace_lvl) { vcol = color::kGood; verdict = "Ready"; }
        else if (best >= rec) { vcol = color::kWarn; verdict = "Close"; }
        label(body, fmt("Rec. L%d+   Ace L%d", rec, ace_lvl), rx, 76, font_body(), color::kText);
        if (!party.empty()) {
            lv_obj_t* v = chip(body, verdict, 0, 78, c(vcol));
            lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -kGap, 78);
        }

        label(body, "Party", rx, 95, font_small(), color::kDim);
        if (party.empty()) {
            label(body, "no party loaded", rx + 34, 95, font_small(), color::kDim);
        }
        for (size_t i = 0; i < party.size() && i < 6; ++i) {
            const int lvl = party[i].level;
            const uint32_t col = lvl >= ace_lvl ? color::kGood : lvl >= rec ? color::kWarn : color::kBad;
            chip(body, fmt("L%d", lvl), rx + 34 + static_cast<int>(i) * 28, 95, c(col));
        }

        // Action button.
        lv_obj_t* btn = panel(body, rx, 112, rw, 22);
        lv_obj_t* bl;
        if (won) {
            lv_obj_set_style_bg_color(btn, c(color::kBar), 0);
            bl = label(btn, LV_SYMBOL_OK "  Badge earned - gym cleared", 0, 0, font_body(), color::kGood);
        } else {
            lv_obj_set_style_bg_color(btn, c(color::kAccent), 0);
            lv_obj_set_style_border_color(btn, c(color::kAccent), 0);
            bl = label(btn, fmt(LV_SYMBOL_PLAY "  Challenge %s", g->leaderName), 0, 0,
                       font_body(), color::kBg);
        }
        lv_obj_center(bl);
    }

    bool detail_key(const KeyEvent& e) {
        switch (e.key) {
            case Key::Left:
            case Key::Right: {
                const int d = e.key == Key::Left ? LORD_GYM_COUNT - 1 : 1;
                sel_ = (sel_ + d) % LORD_GYM_COUNT;
                col_ = sel_ % kCols;
                ctx_.router.rebuild();
                return true;
            }
            case Key::Up:
            case Key::Down:
                return true;
            case Key::A: challenge(); return true;
            case Key::B:
                detail_ = false;
                ctx_.router.rebuild();
                return true;
            default: return false;
        }
    }

    void challenge() {
        const LordGym* g = lordGym(static_cast<uint8_t>(sel_));
        if (!g) return;
        if (has_badge(sel_)) {
            toast(fmt("You already have the %s Badge", g->badgeName), 2200, color::kWarn);
            return;
        }
        if (ctx_.game.party().empty()) {
            toast("No party loaded", 2200, color::kBad);
            return;
        }
        ctx_.battle = BattleRequest{};
        ctx_.battle.kind = BattleRequest::Kind::Gym;
        ctx_.battle.gym = sel_;
        ctx_.router.push(ScreenId::Battle);
    }

    int sel_ = 0;          // 0..7 gym, 8 = Indigo Plateau
    int col_ = 0;          // last gym column (for Up/Down through the league row)
    bool detail_ = false;
    std::optional<uint8_t> override_;   // demo-only badge set (never saved)
    uint8_t seen_badges_ = 0;
    std::array<lv_obj_t*, LORD_GYM_COUNT> tiles_{};
    lv_obj_t* league_ = nullptr;
};

} // namespace

std::unique_ptr<Screen> make_gym_screen(Ctx& ctx) {
    return std::make_unique<GymScreen>(ctx);
}

} // namespace mm::ui
