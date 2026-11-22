#include "router.h"

#include "game_state.h"
#include "mesh_backend.h"
#include "mesh_space.h"
#include "messenger.h"
#include "style.h"
#include "settings.h"
#include "skin.h"
#include "softkeys.h"
#include "widgets.h"
#include "win.h"

#include "device_status.h"
#include "logger.h"

#include <algorithm>

#include <cctype>
#include <ctime>

namespace mm::ui {
namespace {

constexpr uint32_t kGameTickMs = 66;
constexpr uint32_t kRepeatDelayMs = 350;
constexpr uint32_t kRepeatRateMs = 90;

lv_obj_t* make_bar(lv_obj_t* parent, int y, int h, uint32_t a, uint32_t b) {
    lv_obj_t* o = box(parent, 0, y, kScreenW, h);
    win::fill(o, a, b);
    return o;
}

std::string clock_text() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    int h = tm.tm_hour % 12;
    if (h == 0) h = 12;
    return fmt("%d:%02d %s", h, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
}

std::string upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

} // namespace

Router::Router(GameState& game) : game_(game), ctx_{game, *this, {}} {}

Router::~Router() {
    if (timer_) lv_timer_delete(timer_);
    modal_.reset();
    stack_.clear();
}

void Router::start() {
    set_skin(skin_index());
    build_chrome();
    if (!timer_) timer_ = lv_timer_create(timer_cb, 33, this);
    reset_to_menu();
}

// (Re)creates every chrome object for the current skin. Screen bodies are kept.
void Router::build_chrome() {
    const Skin& s = skin();
    root_ = lv_screen_active();
    for (lv_obj_t** o : {&header_, &footer_, &strip_, &frame_}) {
        if (*o) lv_obj_delete(*o);
        *o = nullptr;
    }
    status_ = nullptr;   // children of the deleted bars
    softbar_ = nullptr;
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root_, c(s.desk), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);

    // Window frame + title bar (windowed screens). Sits under the bodies.
    frame_ = win::frame(root_, kWinX, kWinY, kWinW, kWinH, win::Edge::Raised, s.face);
    lv_obj_move_to_index(frame_, 0);
    if (!s.bevel && s.win_radius) {
        lv_obj_set_style_radius(frame_, s.win_radius, 0);
        lv_obj_set_style_clip_corner(frame_, true, 0);
    }
    const int tw = kWinW - 2;
    lv_obj_t* tbar = box(frame_, 1, 1, tw, kTitleH);
    if (s.title_tab) {
        win::fill(tbar, s.desk, s.desk);
        wtab_ = box(tbar, 0, 0, 120, kTitleH);
        win::fill(wtab_, s.title_a, s.title_b);
    } else {
        wtab_ = tbar;
        if (s.title_fill == TitleFill::Stripes) {
            win::fill(tbar, s.title_a, s.title_a);
            for (int y = 3; y < kTitleH - 3; y += 2) {
                lv_obj_t* l = box(tbar, 2, y, tw - 4, 1);
                win::fill(l, s.title_b, s.title_b);
            }
        } else {
            win::fill(tbar, s.title_a, s.title_fill == TitleFill::Solid ? s.title_a : s.title_b,
                      s.title_fill == TitleFill::GradH);
        }
    }
    // Close box.
    {
        const int cx = s.close_left ? 3 : (s.title_tab ? 0 : tw - 14);
        lv_obj_t* cb = win::frame(wtab_, s.close_left ? 3 : cx, 2, 11, 11, win::Edge::Raised, s.close_bg,
                                  s.close_radius);
        if (s.close_fg != s.close_bg) {
            lv_obj_t* x = label(cb, "x", 0, 0, font_small(), s.close_fg);
            lv_obj_align(x, LV_ALIGN_CENTER, 0, -1);
        }
    }
    wtitle_ = label(wtab_, "", 0, 0, font_ui_bold(), s.title_text);
    if (s.title_fill == TitleFill::Stripes) {
        lv_obj_set_style_bg_color(wtitle_, c(s.title_a), 0);
        lv_obj_set_style_bg_opa(wtitle_, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(wtitle_, 5, 0);
    }

    // Status strip (windowed screens).
    strip_ = make_bar(root_, 0, kStatusH, s.status_a, s.status_b);
    // Cells: radio | note | Wi-Fi | battery | clock.
    auto host_at = [&](int x, int w) {
        return s.status_panels ? win::frame(strip_, x, 0, w, kStatusH, win::Edge::Sunken, s.status_a)
                               : box(strip_, x, 0, w, kStatusH);
    };
    const int ty = (kStatusH - lv_font_get_line_height(font_ui())) / 2;
    auto cell = [&](int x, int w) {
        return win::text(host_at(x, w), "", 3, ty, s.status_text, w - 6, false);
    };
    st_left_ = cell(1, 76);
    st_mid_ = cell(78, 108);
    st_wifi_ = box(host_at(187, 26), 1, 1, 24, kStatusH - 2);
    st_batt_ = box(host_at(214, 52), 1, 1, 50, kStatusH - 2);
    st_right_ = cell(267, 52);
    dev_wifi_ = dev_batt_ = -2;
    last_clock_min_ = 0xffffffff;

    // Legacy header (game screens): one 18px title strip in the title colours.
    header_ = make_bar(root_, 0, kHeaderH, s.title_a,
                       s.title_fill == TitleFill::GradV ? s.title_b : s.title_a);
    title_ = label(header_, "", 5, 1, font_title(), s.title_fill == TitleFill::Stripes ? s.text : s.title_text, 200);
    status_dot_ = box(header_, 0, 0, 6, 6);
    lv_obj_set_style_radius(status_dot_, 3, 0);
    lv_obj_set_style_bg_opa(status_dot_, LV_OPA_COVER, 0);
    status_ = label(header_, "", 0, 3, font_small(), s.title_fill == TitleFill::Stripes ? s.text : s.title_text);

    footer_ = make_bar(root_, kScreenH - kFooterH, kFooterH, s.hint_a, s.hint_b);
    hints_ = label(footer_, "", 5, (kFooterH - lv_font_get_line_height(font_ui())) / 2, font_ui(),
                   s.hint_text, kScreenW - 10);
    update_device(true);
    layout_chrome();
}

// Shows the chrome that matches the top screen (windowed or legacy).
void Router::layout_chrome() {
    const Screen* s = top();
    const bool w = s && s->windowed();
    auto show = [](lv_obj_t* o, bool on) {
        if (!o) return;
        if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    const bool fs = s && s->fullscreen();
    show(strip_, w && !fs);
    show(frame_, w && !fs);
    show(header_, !w && !fs);
    show(footer_, !fs);
    if (!stack_.empty()) {
        lv_obj_t* body = stack_.back().body;
        if (fs) {
            lv_obj_set_pos(body, 0, 0);
            lv_obj_set_size(body, lv_obj_get_width(root_), lv_obj_get_height(root_));
            lv_obj_move_foreground(body);
        } else {
            if (w) { lv_obj_set_pos(body, kClientX, kClientY); lv_obj_set_size(body, kClientW, kClientH); }
            else { lv_obj_set_pos(body, 0, kHeaderH); lv_obj_set_size(body, kBodyW, kBodyH); }
            lv_obj_move_to_index(body, 1);
        }
    }
    lv_obj_set_style_bg_color(root_, c(w ? skin().desk : skin().face), 0);
}

lv_obj_t* Router::make_body(const Screen& s) {
    lv_obj_t* body = s.windowed() ? box(root_, kClientX, kClientY, kClientW, kClientH)
                                  : box(root_, 0, kHeaderH, kBodyW, kBodyH);
    lv_obj_move_to_index(body, 1);   // above the window frame, under the strips
    return body;
}

// Puts on the skin the top screen asks for (MeshAmp is always Winamp), or the
// user's own. True if the skin changed: the chrome is rebuilt, the caller
// rebuilds the screen body.
bool Router::sync_skin() {
    if (user_skin_ < 0) user_skin_ = skin_index();
    const Screen* s = top();
    const char* forced = s ? s->forced_skin() : nullptr;
    int want = forced ? skin_from_id(forced) : -1;
    if (want < 0) want = user_skin_;
    if (want == skin_index()) return false;
    set_skin(want);
    build_chrome();
    return true;
}

void Router::apply_skin(int index) {
    set_skin(index);
    user_skin_ = skin_index();
    mm::settings().set_string("ui.skin", skin().id);   // remembered for the next start
    build_chrome();
    for (auto& e : stack_) {
        lv_obj_clean(e.body);
        e.screen->build(e.body);
    }
    refresh_chrome();
}

// Battery, charging and Wi-Fi level. Icons are drawn from boxes so they work
// in any UI font and take the skin's status colours.
void Router::update_device(bool force) {
    const platform::DeviceStatus d = platform::read_device_status();
    const int wifi = !d.wifi_present ? -1 : (d.wifi_connected ? std::max(0, d.wifi_strength_percent) : 0);
    const int batt = d.battery_present ? d.battery_percent : -1;
    if (!force && wifi == dev_wifi_ && batt == dev_batt_ && d.battery_charging == dev_charging_) return;
    dev_wifi_ = wifi;
    dev_batt_ = batt;
    dev_charging_ = d.battery_charging;
    const Skin& k = skin();
    const uint32_t on = k.status_text;
    const uint32_t off = k.dim == k.status_text ? k.lo : k.dim;
    auto rect = [](lv_obj_t* p, int x, int y, int w, int h, uint32_t col) {
        lv_obj_t* o = box(p, x, y, w, h);
        lv_obj_set_style_bg_color(o, c(col), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        return o;
    };

    lv_obj_clean(st_wifi_);
    if (wifi < 0) {
        win::text(st_wifi_, "--", 5, 0, off, 0, false);
    } else {
        const int lit = wifi == 0 ? 0 : 1 + std::min(3, wifi / 26);   // 1..4 bars when connected
        for (int i = 0; i < 4; ++i) {
            const int h = 3 + i * 2;
            rect(st_wifi_, 4 + i * 4, 10 - h, 3, h, i < lit ? on : off);
        }
        if (wifi == 0) rect(st_wifi_, 3, 5, 18, 1, on);   // struck through: not connected
    }

    lv_obj_clean(st_batt_);
    if (batt < 0) {
        win::text(st_batt_, "AC", 4, 0, on, 0, false);
    } else {
        // Outline 15x8 with a 2px nub, filled in proportion.
        rect(st_batt_, 2, 2, 15, 8, on);
        rect(st_batt_, 3, 3, 13, 6, k.status_a);
        rect(st_batt_, 17, 4, 2, 4, on);
        const int fill = std::clamp(batt, 0, 100) * 11 / 100;
        if (fill > 0) rect(st_batt_, 4, 4, fill, 4, batt <= 15 ? color::kBad : on);
        lv_obj_t* pct = win::text(st_batt_, fmt("%d%%", batt), 21, (kStatusH - 2 - lv_font_get_line_height(font_ui())) / 2,
                                  on, 0, false);
        if (d.battery_charging) {
            // A small bolt after the number while charging.
            lv_obj_update_layout(pct);
            const int bx = 21 + lv_obj_get_width(pct) + 1;
            rect(st_batt_, bx + 2, 1, 2, 4, on);
            rect(st_batt_, bx, 4, 5, 2, on);
            rect(st_batt_, bx + 1, 6, 2, 4, on);
        }
    }

    // Legacy header: same facts as text, right of the title.
    update_status();
}

void Router::set_status_note(const std::string& note) {
    status_note_ = note;
    update_status();
}

Screen* Router::top() const {
    return stack_.empty() ? nullptr : stack_.back().screen.get();
}

ScreenId Router::top_id() const {
    return stack_.empty() ? ScreenId::Menu : stack_.back().id;
}

void Router::push(ScreenId id) {
    if (Screen* cur = top()) {
        cur->on_hide();
        lv_obj_add_flag(stack_.back().body, LV_OBJ_FLAG_HIDDEN);
    }
    auto screen = make_screen(id, ctx_);
    if (!screen) {
        LOG_WARN("no screen registered for id {}", static_cast<int>(id));
        if (!stack_.empty()) lv_obj_remove_flag(stack_.back().body, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_t* body = make_body(*screen);
    stack_.push_back({id, std::move(screen), body});
    sync_skin();
    stack_.back().screen->build(body);
    stack_.back().screen->on_show();
    refresh_chrome();
}

void Router::pop() {
    if (stack_.size() <= 1) return;
    stack_.back().screen->on_hide();
    lv_obj_t* body = stack_.back().body;
    stack_.pop_back();          // screen dtor runs while its objects still exist
    lv_obj_delete(body);
    lv_obj_remove_flag(stack_.back().body, LV_OBJ_FLAG_HIDDEN);
    if (sync_skin()) {
        lv_obj_clean(stack_.back().body);
        stack_.back().screen->build(stack_.back().body);
    }
    stack_.back().screen->on_show();
    refresh_chrome();
}

void Router::replace(ScreenId id) {
    if (stack_.size() > 1) {
        stack_.back().screen->on_hide();
        lv_obj_t* body = stack_.back().body;
        stack_.pop_back();
        lv_obj_delete(body);
    }
    push(id);
}

namespace {

// The app launcher on the soft keys while Fn is held.
constexpr ScreenId kAppKeys[] = {ScreenId::Buddies, ScreenId::Space, ScreenId::Amp,
                                 ScreenId::Monster, ScreenId::Radio};
constexpr const char* kAppLabels[] = {"Buddies", "Space", "Amp", "Monster", "Radio"};

SoftKeys app_softkeys(ScreenId current) {
    SoftKeys k;
    for (int i = 0; i < kSoftKeyCount; ++i) {
        k.set(i, kAppLabels[i]);
        if (kAppKeys[i] == current) k.selected = i;
    }
    k.left.text = LV_SYMBOL_HOME;
    k.right.text = LV_SYMBOL_TINT;   // Skins
    return k;
}

} // namespace

void Router::set_app_bar(bool on) {
    if (app_bar_ == on) return;
    app_bar_ = on;
    if (!modal_) update_softkeys();
}

bool Router::open_app_key(int digit) {
    ScreenId id;
    if (digit == 3) id = ScreenId::Menu;
    else if (digit >= 4 && digit <= 8) id = kAppKeys[digit - 4];
    else if (digit == 9) id = ScreenId::Skins;
    else return false;
    modal_.reset();
    // Already open somewhere in the stack: go back down to it.
    for (size_t i = 0; i < stack_.size(); ++i) {
        if (stack_[i].id != id) continue;
        while (stack_.size() > i + 1) pop();
        refresh_chrome();
        return true;
    }
    reset_to_menu();
    if (id == ScreenId::Space) ctx_.peer = 0;   // my own Mesh Space
    if (id != ScreenId::Menu) push(id);
    return true;
}

void Router::reset_to_menu() {
    modal_.reset();
    while (!stack_.empty()) {
        lv_obj_t* body = stack_.back().body;
        stack_.pop_back();
        lv_obj_delete(body);
    }
    push(ScreenId::Menu);
}

void Router::rebuild() {
    if (stack_.empty()) return;
    auto& e = stack_.back();
    lv_obj_clean(e.body);
    e.screen->build(e.body);
    refresh_chrome();
}

void Router::refresh_chrome() {
    layout_chrome();
    if (Screen* s = top()) {
        const Skin& k = skin();
        set_text(title_, s->title());
        set_text(hints_, s->hints());
        update_softkeys();
        set_text(wtitle_, k.title_upper ? upper(s->title()) : std::string(s->title()));
        lv_obj_update_layout(wtitle_);
        const int tw = lv_obj_get_width(wtitle_);
        const int bar_w = kWinW - 2;
        if (k.title_tab) {
            lv_obj_set_width(wtab_, tw + 30);
            lv_obj_set_pos(wtitle_, 19, 1);
        } else if (k.title_center) {
            lv_obj_set_pos(wtitle_, (bar_w - tw) / 2, 1);
        } else {
            lv_obj_set_pos(wtitle_, k.close_left ? 18 : 4, 1);
        }
    }
    update_status();
}

// Soft keys of the top screen in place of the hint text (same footer strip,
// so the bar sits on the bottom edge of the display, right over the keys).
void Router::update_softkeys() {
    if (!footer_) return;
    const Screen* s = top();
    const SoftKeys sk = app_bar_ ? app_softkeys(s ? top_id() : ScreenId::Menu)
                                 : s ? s->softkeys() : SoftKeys{};
    if (sk.empty()) {
        if (softbar_) lv_obj_delete(softbar_);
        softbar_ = nullptr;
        soft_ = {};
        lv_obj_remove_flag(hints_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(hints_, LV_OBJ_FLAG_HIDDEN);
    if (softbar_ && sk == soft_) return;
    if (softbar_) lv_obj_delete(softbar_);
    soft_ = sk;
    softbar_ = softkey_bar(footer_, sk, 0, 0, kFooterH, 0);
}

void Router::update_status() {
    if (!status_) return;
    const Skin& k = skin();
    uint32_t dot = color::kBad;
    std::string text;
    const mm::MeshBackend::Status mesh = mm::mesh_backend().status();
    if (mesh != mm::MeshBackend::Status::Off) {
        // The messenger's radio is on: say what it is doing.
        dot = mesh == mm::MeshBackend::Status::Ready ? color::kGood
            : mesh == mm::MeshBackend::Status::Sync  ? color::kWarn : color::kBad;
        text = mm::mesh_backend().status_text();
    } else if (game_.demo_mode()) {
        dot = color::kWarn;
        text = "DEMO";
    } else if (game_.daemon_connected()) {
        const auto& d = game_.daycare();
        dot = d.serial_connected ? color::kGood : color::kWarn;
        text = d.serial_connected ? fmt("MESH  %d near", d.neighbors) : "NO RADIO";
    } else {
        text = "OFFLINE";
    }
    // Windowed strip: radio | note | clock.
    set_text(st_left_, text);
    set_text(st_mid_, status_note_);
    set_text(st_right_, clock_text());
    // Legacy header: badges + radio on the right of the title strip.
    const int badges = game_.badge_count();
    if (badges > 0) text = fmt("%d/8 badges   ", badges) + text;
    if (dev_batt_ >= 0) text += fmt("   %d%%%s", dev_batt_, dev_charging_ ? " " LV_SYMBOL_CHARGE : "");
    if (dev_wifi_ > 0) text += "  " LV_SYMBOL_WIFI;
    text += "  " + clock_text();
    set_text(status_, text);
    lv_obj_update_layout(status_);
    const int w = lv_obj_get_width(status_);
    lv_obj_set_x(status_, kScreenW - 5 - w);
    lv_obj_set_pos(status_dot_, kScreenW - 5 - w - 9, 6);
    lv_obj_set_style_bg_color(status_dot_, c(k.dark ? dot : k.title_text), 0);
    last_status_rev_ = game_.revision();
    last_connected_ = game_.daemon_connected();
}

void Router::show_modal(std::unique_ptr<Modal> modal) {
    modal_ = std::move(modal);
}

void Router::dispatch(const KeyEvent& e) {
    if (modal_) {
        modal_->on_key(e);
        if (!modal_->open()) {
            modal_.reset();
            refresh_chrome();   // the choice may have changed the screen's soft keys
        }
        return;
    }
    Screen* s = top();
    if (!s) return;
    if (s->on_key(e)) {
        // The screen may have pushed/popped; refresh whatever is on top now.
        refresh_chrome();
        return;
    }
    // Key 3 under a "back" corner label: Esc.
    const bool soft_back = softkey_slot(e) == kSoftLeft && !s->softkeys().left.text.empty();
    if ((e.key == Key::B || soft_back) && !e.repeat) {
        if (stack_.size() > 1) {
            pop();
        } else {
            exit_armed_ = true;
            toast("Hold Esc to exit 1999 Online", 1500, color::kWarn);
        }
    }
}

uint32_t Router::nav_raw(uint32_t raw) const {
    const Screen* s = top();
    if (!modal_ && s && s->typing()) return raw;
    return arrow_letter(raw);
}

bool Router::handle_raw_key(uint32_t raw, bool long_pressed) {
    if (long_pressed) {
        if (exit_armed_ && (raw == LV_KEY_ESC) && stack_.size() <= 1 && !modal_) {
            LOG_INFO("exit requested");
            quit_ = true;
        }
        return true;
    }
    const KeyEvent e = translate_key(nav_raw(raw));
    if (e.key == Key::None) return false;
    exit_armed_ = false;
    if (is_direction(e.key)) {
        held_raw_ = raw;
        held_since_ = lv_tick_get();
        last_repeat_ = held_since_;
    } else {
        held_raw_ = 0;
    }
    dispatch(e);
    if (e.key == Key::B && stack_.size() <= 1 && !modal_) exit_armed_ = true;
    return true;
}

void Router::handle_raw_release(uint32_t raw) {
    if (raw == held_raw_) held_raw_ = 0;
}

void Router::timer_cb(lv_timer_t* t) {
    static_cast<Router*>(lv_timer_get_user_data(t))->tick();
}

void Router::tick() {
    const uint32_t now = lv_tick_get();

    // The radio is pumped on every tick (no-op when the backend is off).
    mm::mesh_backend().tick(now);
    if (mm::mesh_backend().on()) {
        const std::string mesh = mm::mesh_backend().status_text();
        if (mesh != last_mesh_status_) {
            last_mesh_status_ = mesh;
            update_status();
        }
    }

    if (now - last_game_tick_ >= kGameTickMs) {
        last_game_tick_ = now;
        game_.tick();

        // The messenger runs on every screen so replies and the unread note
        // keep arriving while you are in the game or the player.
        mm::messenger().tick(now);
        mm::settings().tick(now);   // debounced write of changed settings
        mm::mesh_space().tick(now); // same for my Mesh Space page
        if (mm::messenger().revision() != last_msg_rev_) {
            last_msg_rev_ = mm::messenger().revision();
            const int unread = mm::messenger().total_unread();
            set_status_note(unread == 0 ? std::string()
                            : fmt("%d new message%s", unread, unread == 1 ? "" : "s"));
        }

        if (auto a = game_.pop_achievement()) {
            toast(std::string(LV_SYMBOL_BELL " ") + a->name + "\n" + a->description, 3500, color::kAccent);
        }
        // Hatches anywhere in the app; the breeding screen shows its own result.
        if (auto n = game_.pop_hatch_notice()) {
            if (top_id() != ScreenId::Breeding) toast(std::string(LV_SYMBOL_BELL " ") + *n, 3500, color::kGood);
        }
        if (auto ch = game_.pending_challenge()) {
            if (ch->node_id != seen_challenge_node_) {
                seen_challenge_node_ = ch->node_id;
                const std::string who = ch->trainer.empty() ? fmt("!%08x", ch->node_id) : ch->trainer;
                toast(who + " wants to battle!  Open Battle to answer.", 4000, color::kInfo);
            }
        } else {
            seen_challenge_node_ = 0;
        }
        if (game_.revision() != last_status_rev_ || game_.daemon_connected() != last_connected_) {
            update_status();
        }
    }

    if (held_raw_ && now - held_since_ >= kRepeatDelayMs && now - last_repeat_ >= kRepeatRateMs) {
        last_repeat_ = now;
        KeyEvent e = translate_key(nav_raw(held_raw_));
        e.repeat = true;
        dispatch(e);
    }

    const uint32_t minute = static_cast<uint32_t>(std::time(nullptr) / 60);
    if (minute != last_clock_min_) {
        last_clock_min_ = minute;
        update_status();
    }
    if (now - last_dev_poll_ >= 5000) {
        last_dev_poll_ = now;
        update_device(false);
    }

    if (Screen* s = top()) s->tick(now);
    // State can change without a key (a challenge arrives, a room hatches):
    // keep the soft keys in step. Redraws only when they differ.
    if (!modal_) update_softkeys();
}

} // namespace mm::ui
