// monster_screen.cpp — MonsterMesh, the game's own program inside 1999 Online:
// a 3x2 grid with every MonsterMesh option (the old home menu, in a window).
// The first five tiles are also soft keys 4..8 under the display (left to
// right in tile order); Wild Wi-Fi is the key 9 corner, key 3 goes back.
#include "screens.h"

#include "game_state.h"
#include "router.h"
#include "widgets.h"
#include "win.h"

namespace mm::ui {
namespace {

struct Tile {
    ScreenId id;
    const char* name;
    int dex;            // icon sprite
    const char* key;    // soft key label
};

constexpr Tile kTiles[] = {
    {ScreenId::Party,     "Party",      25,  "PARTY"},
    {ScreenId::GymSelect, "Gyms",       95,  "GYMS"},
    {ScreenId::Battle,    "Battle",     68,  "BATTLE"},
    {ScreenId::Daycare,   "Daycare",    113, "DAYCARE"},
    {ScreenId::Breeding,  "Breeding",   132, "BREED"},
    {ScreenId::Wifi,      "Wild Wi-Fi", 81,  LV_SYMBOL_WIFI},   // key 9 corner
};
constexpr int kTileCount = sizeof(kTiles) / sizeof(kTiles[0]);
constexpr int kCols = 3;
constexpr int kCellW = (kClientW - 4) / kCols;   // 102
constexpr int kCellH = 58;

class MonsterScreen final : public Screen {
public:
    explicit MonsterScreen(Ctx& ctx) : Screen(ctx) { nav_.cols = kCols; nav_.count = kTileCount; }

    const char* title() const override { return "MonsterMesh"; }
    bool windowed() const override { return true; }
    std::string hints() const override { return "Arrows:move  Enter:open  Esc:1999 Online"; }

    SoftKeys softkeys() const override {
        SoftKeys k;
        for (int i = 0; i < kSoftKeyCount; ++i) k.set(i, kTiles[i].key);
        k.right.text = kTiles[kSoftKeyCount].key;
        k.left.text = MM_SOFT_BACK;
        k.selected = nav_.sel < kSoftKeyCount ? nav_.sel : -1;
        return k;
    }

    void build(lv_obj_t* body) override {
        const Skin& s = skin();
        lv_obj_t* f = win::field(body, 0, 0, kClientW, kClientH);
        for (int i = 0; i < kTileCount; ++i) {
            const int x = 2 + (i % kCols) * kCellW;
            const int y = 2 + (i / kCols) * kCellH;
            sprite_box(f, kTiles[i].dex, x + 2, y + 6, 40, 40);
            names_[i] = win::text(f, kTiles[i].name, x + 44, y + 14, s.field_text, 0, true);
            lv_obj_set_style_pad_hor(names_[i], 2, 0);
            subs_[i] = win::text(f, "", x + 46, y + 28, s.dim, kCellW - 48, false);
        }
        update_subs();
        paint();
    }

    bool on_key(const KeyEvent& e) override {
        if (nav_.on_key(e)) {
            paint();
            return true;
        }
        const int slot = softkey_slot(e);
        if ((slot >= 0 && slot < kSoftKeyCount) || slot == kSoftRight) {
            nav_.sel = slot == kSoftRight ? kSoftKeyCount : slot;
            paint();
            open(nav_.sel);
            return true;
        }
        if (e.key == Key::A) {
            open(nav_.sel);
            return true;
        }
        return false;
    }

    void tick(uint32_t) override {
        if (ctx_.game.revision() != rev_) update_subs();
    }
    void on_show() override { update_subs(); }

private:
    void open(int i) {
        if (kTiles[i].id == ScreenId::Battle) ctx_.battle = BattleRequest{};   // practice battle
        ctx_.router.push(kTiles[i].id);
    }

    void paint() {
        const Skin& s = skin();
        for (int i = 0; i < kTileCount; ++i) {
            const bool on = i == nav_.sel;
            lv_obj_set_style_bg_color(names_[i], c(s.sel), 0);
            lv_obj_set_style_bg_opa(names_[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(names_[i], c(on ? s.sel_text : s.field_text), 0);
        }
    }

    void update_subs() {
        if (!subs_[0]) return;
        rev_ = ctx_.game.revision();
        auto& g = ctx_.game;
        const auto& d = g.daycare();
        set_text(subs_[0], fmt("%zu mons", g.party().size()));
        set_text(subs_[1], fmt("%d/8 badges", g.badge_count()));
        set_text(subs_[2], g.pending_challenge() ? "challenge!" : "practice");
        set_text(subs_[3], d.active ? fmt("%d near", d.neighbors) : std::string("idle"));
        set_text(subs_[4], fmt("%zu in box", g.breeding().size()));
        set_text(subs_[5], "wild mons");
    }

    GridNav nav_;
    lv_obj_t* names_[kTileCount] = {};
    lv_obj_t* subs_[kTileCount] = {};
    uint32_t rev_ = 0;
};

} // namespace

std::unique_ptr<Screen> make_monster_screen(Ctx& ctx) {
    return std::make_unique<MonsterScreen>(ctx);
}

} // namespace mm::ui
