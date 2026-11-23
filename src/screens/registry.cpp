#include "router.h"
#include "screens.h"
#include "widgets.h"
#include "win.h"

namespace mm::ui {

std::unique_ptr<Screen> make_screen(ScreenId id, Ctx& ctx) {
    switch (id) {
        case ScreenId::Menu:      return make_menu_screen(ctx);
        case ScreenId::Party:     return make_party_screen(ctx);
        case ScreenId::Daycare:   return make_daycare_screen(ctx);
        case ScreenId::Breeding:  return make_breeding_screen(ctx);
        case ScreenId::GymSelect: return make_gym_screen(ctx);
        case ScreenId::Battle:    return make_battle_screen(ctx);
        case ScreenId::Wifi:      return make_wifi_screen(ctx);
        case ScreenId::Buddies:   return make_buddy_screen(ctx);
        case ScreenId::Im:        return make_im_screen(ctx);
        case ScreenId::Space:     return make_space_screen(ctx);
        case ScreenId::Amp:       return make_amp_screen(ctx);
        case ScreenId::Skins:     return make_skins_screen(ctx);
        case ScreenId::Monster:   return make_monster_screen(ctx);
        case ScreenId::Radio:     return make_radio_screen(ctx);
        case ScreenId::Mailbox:   return make_mailbox_screen(ctx);
        case ScreenId::Map:       return make_map_screen(ctx);
        case ScreenId::Hood:      return make_hood_screen(ctx);
        case ScreenId::Ringtones: return make_ring_screen(ctx);
        case ScreenId::Snake:     return make_snake_screen(ctx);
        case ScreenId::Calc:      return make_calc_screen(ctx);
        case ScreenId::Mines:     return make_mines_screen(ctx);
        case ScreenId::Puzzle:    return make_puzzle_screen(ctx);
        case ScreenId::Savers:    return make_savers_screen(ctx);
        case ScreenId::Beats:     return make_beats_screen(ctx);
        default:                  return nullptr;
    }
}

const char* screen_slug(ScreenId id) {
    switch (id) {
        case ScreenId::Menu:      return "menu";
        case ScreenId::Party:     return "party";
        case ScreenId::Daycare:   return "daycare";
        case ScreenId::Breeding:  return "breeding";
        case ScreenId::GymSelect: return "gyms";
        case ScreenId::Battle:    return "battle";
        case ScreenId::Wifi:      return "wifi";
        case ScreenId::Buddies:   return "buddies";
        case ScreenId::Im:        return "im";
        case ScreenId::Space:     return "space";
        case ScreenId::Amp:       return "amp";
        case ScreenId::Skins:     return "skins";
        case ScreenId::Monster:   return "monstermesh";
        case ScreenId::Radio:     return "radio";
        case ScreenId::Mailbox:   return "mailbox";
        case ScreenId::Map:       return "map";
        case ScreenId::Hood:      return "neighborhood";
        case ScreenId::Ringtones: return "ringtones";
        case ScreenId::Snake:     return "snake";
        case ScreenId::Calc:      return "calc";
        case ScreenId::Mines:     return "mines";
        case ScreenId::Puzzle:    return "puzzle";
        case ScreenId::Savers:    return "screensavers";
        case ScreenId::Beats:     return "beats";
        default:                  return "unknown";
    }
}

namespace {

class PlaceholderScreen final : public Screen {
public:
    PlaceholderScreen(Ctx& ctx, const char* title, const char* blurb)
        : Screen(ctx), title_(title), blurb_(blurb) {}
    const char* title() const override { return title_; }
    void build(lv_obj_t* body) override {
        lv_obj_t* p = panel(body, 8, 8, kBodyW - 16, kBodyH - 16);
        lv_obj_t* l = label(p, blurb_, 8, 8, font_body(), color::kDim);
        lv_obj_set_width(l, kBodyW - 32);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
    std::string hints() const override { return "Esc:back"; }

private:
    const char* title_;
    const char* blurb_;
};

class WindowPlaceholder final : public Screen {
public:
    WindowPlaceholder(Ctx& ctx, const char* title, const char* blurb)
        : Screen(ctx), title_(title), blurb_(blurb) {}
    const char* title() const override { return title_; }
    bool windowed() const override { return true; }
    void build(lv_obj_t* body) override {
        lv_obj_t* f = win::field(body, 0, 0, kClientW, kClientH - 20);
        win::text_wrap(f, blurb_, 6, 6, kClientW - 12, skin().field_text);
        win::button(body, "OK", kClientW - 60, kClientH - 17, 60, true);
    }
    bool on_key(const KeyEvent& e) override {
        if (e.key == Key::A) { ctx_.router.pop(); return true; }
        return false;
    }
    std::string hints() const override { return "Enter/Esc:back"; }

private:
    const char* title_;
    const char* blurb_;
};

} // namespace

std::unique_ptr<Screen> make_window_placeholder_screen(Ctx& ctx, const char* title, const char* blurb) {
    return std::make_unique<WindowPlaceholder>(ctx, title, blurb);
}

std::unique_ptr<Screen> make_placeholder_screen(Ctx& ctx, const char* title, const char* blurb) {
    return std::make_unique<PlaceholderScreen>(ctx, title, blurb);
}

} // namespace mm::ui
