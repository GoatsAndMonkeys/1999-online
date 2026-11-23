// menu_screen.cpp — 1999 Online home: one launcher window with a 4-column grid
// of program icons that scrolls by row. The game lives in its own program, MonsterMesh.
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
    const char* symbol;   // LVGL symbol, "buddy"/"bolt" for a drawn icon, or nullptr to use the sprite
    int dex;              // sprite icon for the game screens
};

constexpr Tile kTiles[] = {
    {ScreenId::Buddies,   "Buddy List",   "buddy",              0},
    {ScreenId::Space,     "Mesh Space",   LV_SYMBOL_HOME,       0},
    {ScreenId::Amp,       "MeshAmp",      "bolt",               0},
    {ScreenId::Monster,   "MonsterMesh",  nullptr,              25},
    {ScreenId::Mailbox,   "Mailbox",      LV_SYMBOL_ENVELOPE,   0},
    {ScreenId::Map,       "MeshMap",      LV_SYMBOL_GPS,        0},
    {ScreenId::Hood,      "Neighborhood", LV_SYMBOL_WIFI,       0},
    {ScreenId::Ringtones, "Ringtones",    LV_SYMBOL_AUDIO,      0},
    {ScreenId::Beats,     "MeshBeat",     LV_SYMBOL_BELL,       0},
    {ScreenId::Snake,     "Snake",        nullptr,              23},
    {ScreenId::Mines,     "MeshMines",    LV_SYMBOL_WARNING,    0},
    {ScreenId::Puzzle,    "Bit Quest",    LV_SYMBOL_SHUFFLE,    0},
    {ScreenId::Calc,      "Calculator",   LV_SYMBOL_PLUS,       0},
    {ScreenId::Savers,    "Screen Savers",LV_SYMBOL_IMAGE,      0},
    {ScreenId::Radio,     "Radio Setup",  LV_SYMBOL_SETTINGS,   0},
    {ScreenId::Skins,     "Skins",        LV_SYMBOL_TINT,       0},
};
constexpr int kTileCount = sizeof(kTiles) / sizeof(kTiles[0]);
constexpr int kCols = 4;
constexpr int kCellW = (kClientW - 4) / kCols;   // 77
constexpr int kCellH = 58;

class MenuScreen final : public Screen {
public:
    explicit MenuScreen(Ctx& ctx) : Screen(ctx) { nav_.cols = kCols; nav_.count = kTileCount; }

    const char* title() const override { return "1999 Online"; }
    bool windowed() const override { return true; }
    std::string hints() const override { return "Arrows:move  Enter:open  S:skin  Esc(hold):exit"; }

    void build(lv_obj_t* body) override {
        const Skin& s = skin();
        lv_obj_t* f = win::field(body, 0, 0, kClientW, kClientH);
        field_ = f;
        lv_obj_add_flag(f, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(f, LV_SCROLLBAR_MODE_AUTO);
        for (int i = 0; i < kTileCount; ++i) {
            const int x = 2 + (i % kCols) * kCellW;
            const int y = 2 + (i / kCols) * kCellH;
            const std::string sym = kTiles[i].symbol ? kTiles[i].symbol : "";
            if (sym == "buddy") {
                draw_buddy(f, x + kCellW / 2, y + 4);
            } else if (sym == "bolt") {
                draw_bolt(f, x + kCellW / 2, y + 4);
            } else if (kTiles[i].symbol) {
                lv_obj_t* ic = label(f, kTiles[i].symbol, 0, 0, font_icon(), s.sel == s.field_text ? s.dim : s.sel);
                lv_obj_update_layout(ic);
                lv_obj_set_pos(ic, x + (kCellW - lv_obj_get_width(ic)) / 2, y + 9);
            } else {
                sprite_box(f, kTiles[i].dex, x + (kCellW - 40) / 2, y + 1, 40, 40);
            }
            names_[i] = win::text(f, kTiles[i].name, 0, y + 42, s.field_text, 0, false);
            lv_obj_set_style_pad_hor(names_[i], 2, 0);
            lv_obj_update_layout(names_[i]);
            lv_obj_set_x(names_[i], x + (kCellW - lv_obj_get_width(names_[i])) / 2);
        }
        paint();
    }

    bool on_key(const KeyEvent& e) override {
        if (nav_.on_key(e)) {
            paint();
            return true;
        }
        if (e.key == Key::A) {
            open(nav_.sel);
            return true;
        }
        if (e.key == Key::Char && e.ch == 's') {
            ctx_.router.apply_skin(skin_index() + 1);
            toast(std::string("Skin: ") + skin().name, 1200);
            return true;
        }
        return false;
    }

private:
    // Original icons (not copies of any product logo): a little yellow buddy
    // figure, and a lightning bolt on a dark diamond.
    static void blob(lv_obj_t* p, int x, int y, int w, int h, int r, uint32_t bg, uint32_t line) {
        lv_obj_t* o = box(p, x, y, w, h);
        lv_obj_set_style_radius(o, r, 0);
        lv_obj_set_style_bg_color(o, c(bg), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, c(line), 0);
    }
    static void draw_buddy(lv_obj_t* p, int cx, int y) {
        const uint32_t fill = 0xffcc00, line = 0x403000;
        blob(p, cx - 12, y + 16, 7, 14, 3, fill, line);    // arms
        blob(p, cx + 5, y + 16, 7, 14, 3, fill, line);
        blob(p, cx - 8, y + 13, 16, 22, 6, fill, line);    // body
        blob(p, cx - 6, y, 12, 12, 6, fill, line);         // head
    }
    static void draw_bolt(lv_obj_t* p, int cx, int y) {
        lv_obj_t* d = box(p, cx - 12, y + 6, 24, 24);
        lv_obj_set_style_bg_color(d, c(0x2a2a3c), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(d, 1, 0);
        lv_obj_set_style_border_color(d, c(0xc8b464), 0);
        lv_obj_set_style_transform_rotation(d, 450, 0);
        lv_obj_set_style_transform_pivot_x(d, 12, 0);
        lv_obj_set_style_transform_pivot_y(d, 12, 0);
        lv_obj_t* b = label(p, LV_SYMBOL_CHARGE, 0, 0, font_icon(), 0xffb400);
        lv_obj_update_layout(b);
        lv_obj_set_pos(b, cx - lv_obj_get_width(b) / 2, y + 7);
    }

    void open(int i) {
        if (kTiles[i].id == ScreenId::Space) ctx_.peer = 0;                    // my own Mesh Space
        ctx_.router.push(kTiles[i].id);
    }

    void paint() {
        const Skin& s = skin();
        for (int i = 0; i < kTileCount; ++i) {
            const bool on = i == nav_.sel;
            lv_obj_set_style_bg_color(names_[i], c(s.sel), 0);
            lv_obj_set_style_bg_opa(names_[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            if (on && field_) {
                // Keep the selected row in view: the grid is taller than the window.
                const int row = i / kCols;
                const int top = row * kCellH;
                const int view = kClientH - 4;
                int sy = lv_obj_get_scroll_y(field_);
                if (top < sy) sy = top;
                else if (top + kCellH > sy + view) sy = top + kCellH - view;
                lv_obj_scroll_to_y(field_, sy, LV_ANIM_OFF);
            }
            lv_obj_set_style_text_color(names_[i], c(on ? s.sel_text : s.field_text), 0);
        }
    }

    GridNav nav_;
    lv_obj_t* names_[kTileCount] = {};
    lv_obj_t* field_ = nullptr;
};

} // namespace

std::unique_ptr<Screen> make_menu_screen(Ctx& ctx) {
    return std::make_unique<MenuScreen>(ctx);
}

} // namespace mm::ui
