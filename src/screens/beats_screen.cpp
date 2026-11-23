// beats_screen.cpp — MeshBeat: a pocket drum machine, the kind of beat maker
// a 2002 phone or a shareware CD shipped with.
//
// One window, several views (backend/beats.h does the pattern, the synth and
// the text form):
//
//   GRID   an amber step-sequencer readout on top (pattern name, tempo, swing,
//          play state and step), then 8 tracks x 16 steps of step LEDs in four
//          groups of four, a playhead LED row under them and each track's
//          volume on the right. Arrows move the cursor, Enter / Space set a
//          step, A accents it. Playing renders a few bars to a .wav and loops
//          it (beat::LoopPlayer); the playhead follows the clock of the
//          running loop. Edits made while playing are heard from the next bar
//          line (the loop is re-rendered and restarted there).
//   BANK   presets (original), the user's four slots (beats.json) and beats
//          buddies sent (found in the messenger history), with a preview.
//   SAVE   name the pattern and pick a slot.
//   SEND   pick a buddy or the channel; the pattern goes out as ONE text
//          message (beat::share_text) only when SEND / Enter is pressed here.
//   HELP   the key card.
//
// The grid is built once per rebuild(); key presses and the playhead restyle
// the existing objects, so neither typing nor playback rebuilds the window.
// Demo states (--shots) never play, save or send.
#include "screens.h"

#include "beats.h"
#include "game_state.h"
#include "messenger.h"
#include "router.h"
#include "settings.h"
#include "widgets.h"
#include "win.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace mm::ui {
namespace {

// The readout: a dark glass with amber segments whatever the skin (like the
// Ringtones LCD), framed by the skin's sunken edge.
constexpr uint32_t kGlassTop = 0x231a12;
constexpr uint32_t kGlassBottom = 0x120d09;
constexpr uint32_t kAmber = 0xffb43c;
constexpr uint32_t kAmberDim = 0x9a6a2c;
// Step LEDs: fixed colours, chosen to read on light and dark fields alike.
constexpr uint32_t kLedOn = 0xe0401c;
constexpr uint32_t kLedAccent = 0xffc020;
constexpr uint32_t kLedPlay = 0x38d848;

// Geometry inside the client area (312 x 121).
constexpr int kLcdH = 20;
constexpr int kGridY = kLcdH + 2;                  // 22
constexpr int kGridH = kClientH - kGridY;          // 99
constexpr int kRowY0 = 3;
constexpr int kRowPitch = 11;
constexpr int kPadH = 8;
constexpr int kPadW = 13;
constexpr int kPadX0 = 46;
constexpr int kLabelX = 3;
constexpr int kLabelW = 40;
constexpr int kHeadY = kRowY0 + beat::kTracks * kRowPitch;   // 91
constexpr int kVolX = 296;
constexpr int kStartLatencyMs = 40;                // player start-up, roughly

int pad_x(int s) { return kPadX0 + s * 15 + (s / 4) * 3; }
int row_y(int t) { return kRowY0 + t * kRowPitch; }

uint32_t mix(uint32_t a, uint32_t b, int a_pct) {
    auto ch = [&](int sh) {
        const int x = (a >> sh) & 0xff, y = (b >> sh) & 0xff;
        return static_cast<uint32_t>((x * a_pct + y * (100 - a_pct)) / 100) << sh;
    };
    return ch(16) | ch(8) | ch(0);
}

lv_obj_t* solid(lv_obj_t* parent, int x, int y, int w, int h, uint32_t col) {
    lv_obj_t* o = box(parent, x, y, w, h);
    lv_obj_set_style_bg_color(o, c(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

void set_bg(lv_obj_t* o, uint32_t col) { lv_obj_set_style_bg_color(o, c(col), 0); }

int text_w(const std::string& s, const lv_font_t* f) {
    int w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const uint32_t next = i + 1 < s.size() ? static_cast<uint8_t>(s[i + 1]) : 0;
        w += lv_font_get_glyph_width(f, static_cast<uint8_t>(s[i]), next);
    }
    return w;
}

lv_obj_t* glass(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* f = win::frame(parent, x, y, w, h, win::Edge::Sunken, kGlassBottom);
    lv_obj_t* g = box(f, 2, 2, w - 4, h - 4);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    win::fill(g, kGlassTop, kGlassBottom);
    return g;
}

lv_obj_t* amber(lv_obj_t* parent, const std::string& s, int x, int y, int width = 0, bool bold = false,
                uint32_t col = kAmber) {
    return label(parent, s, x, y, bold ? font_ui_bold() : font_ui(), col, width);
}

// A small picture of a pattern: one dot per step.
void mini_grid(lv_obj_t* parent, int x, int y, const beat::Pattern& p, uint32_t bg, uint32_t fg, int cw = 7,
               int ch = 4) {
    const uint32_t off = mix(fg, bg, 18), off2 = mix(fg, bg, 28);
    for (int t = 0; t < beat::kTracks; ++t) {
        for (int s = 0; s < beat::kSteps; ++s) {
            const uint8_t v = p.steps[t][s];
            uint32_t col = v == beat::Accent ? kLedAccent : v ? kLedOn : ((s / 4) % 2 ? off2 : off);
            if (v && p.muted(t)) col = mix(col, bg, 45);
            solid(parent, x + s * (cw + 2) + (s / 4) * 2, y + t * (ch + 2), cw, ch, col);
        }
    }
}

std::string swing_text(int sw) { return sw <= beat::kMinSwing ? std::string("straight") : fmt("%d%%", sw); }

enum class Mode : uint8_t { Grid, Bank, Save, Send, Help };

class BeatsScreen final : public Screen {
public:
    explicit BeatsScreen(Ctx& ctx) : Screen(ctx) {
        if (!ctx_.game.files_enabled()) {
            set_demo_state(1);
            return;
        }
        lib_.load();
        const std::string last = settings().get_string("beats.last");
        if (last.empty() || !beat::from_text(last, pat_)) pat_ = beat::presets().front();
        if (pat_.name.empty()) pat_.name = "New Beat";
    }
    ~BeatsScreen() override { stop_play(); }

    const char* title() const override { return "MeshBeat"; }
    bool windowed() const override { return true; }
    bool typing() const override { return mode_ == Mode::Save; }

    void on_hide() override {
        stop_play();
        remember();
    }

    std::string hints() const override {
        switch (mode_) {
            case Mode::Save: return "type a name  Tab:slot  Enter:save  Esc:cancel";
            case Mode::Help: return "any key: back to the grid";
            default: return "Enter:hit A:accent M:mute -/=:BPM P:play";
        }
    }

    SoftKeys softkeys() const override {
        SoftKeys k;
        if (mode_ == Mode::Save || mode_ == Mode::Help) return k;   // the name field takes digits
        k.left.text = MM_SOFT_BACK;
        if (mode_ == Mode::Bank) {
            const Entry* e = cur_entry();
            k.set(0, "LOAD", e && e->usable).set(1, "DEL", e && e->kind == Kind::User && e->usable).set(4, "CANCEL");
            return k;
        }
        if (mode_ == Mode::Send) {
            k.set(0, "SEND", !targets_.empty()).set(4, "CANCEL");
            return k;
        }
        k.set(0, playing() ? "STOP" : "PLAY")
            .set(1, "BANK")
            .set(2, "SAVE")
            .set(3, "SEND", !pat_.empty())
            .set(4, "CLEAR", !pat_.empty());
        k.right.text = "?";
        return k;
    }

    // ── Demo states (--shots) ───────────────────────────────────────────────
    int demo_state_count() const override { return 7; }
    const char* demo_state_name(int i) const override {
        static const char* const kNames[] = {"empty", "preset", "playing", "bank", "save", "send", "help"};
        return (i >= 0 && i < 7) ? kNames[i] : "main";
    }
    void set_demo_state(int i) override {
        demo_ = std::clamp(i, 0, 6);
        demo_play_step_ = -1;
        mode_ = Mode::Grid;
        lib_ = beat::Library();
        beat::Pattern mine = beat::presets()[3];
        mine.name = "Late Bus";
        mine.bpm = 88;
        mine.steps[beat::Bell].fill(beat::Off);
        mine.steps[beat::Tom][14] = beat::On;
        lib_.set(0, mine);
        received_.clear();
        beat::Pattern rx;
        if (beat::parse_shared(beat::share_text(beat::presets()[6]), rx)) {
            rx.name = "Trail Stomp";
            rx.bpm = 128;
            received_.push_back({rx, "KD2ABC-m"});
        }
        cur_t_ = cur_s_ = 0;
        switch (demo_) {
            case 0:
                pat_ = beat::Pattern();
                pat_.name = "New Beat";
                break;
            case 1:
                pat_ = beat::presets()[2];                 // Dialup Funk
                cur_t_ = 2;
                cur_s_ = 6;
                break;
            case 2:
                pat_ = beat::presets()[1];                 // Y2K Breaks
                pat_.mute = 1 << beat::Bell;
                pat_.vol[beat::Hat] = 5;
                cur_t_ = 1;
                cur_s_ = 9;
                demo_play_step_ = 6;
                break;
            case 3:
                pat_ = beat::presets()[0];
                open_bank();
                bank_row_ = 3;                             // Dialup Funk
                break;
            case 4:
                pat_ = beat::presets()[4];
                pat_.name = "Mall Rat 2";
                open_save();
                save_slot_ = 1;
                break;
            case 5:
                pat_ = beat::presets()[3];
                open_send();
                send_sel_ = std::min<int>(1, static_cast<int>(targets_.size()) - 1);
                break;
            case 6:
                pat_ = beat::presets()[0];
                mode_ = Mode::Help;
                break;
        }
    }

    void build(lv_obj_t* body) override {
        list_.reset();
        name_in_.reset();
        clear_grid_refs();
        switch (mode_) {
            case Mode::Grid: build_grid(body); break;
            case Mode::Bank: build_bank(body); break;
            case Mode::Save: build_save(body); break;
            case Mode::Send: build_send(body); break;
            case Mode::Help: build_help(body); break;
        }
    }

    bool on_key(const KeyEvent& e) override {
        switch (mode_) {
            case Mode::Grid: return grid_key(e);
            case Mode::Bank: return bank_key(e);
            case Mode::Save: return save_key(e);
            case Mode::Send: return send_key(e);
            case Mode::Help:
                mode_ = Mode::Grid;
                ctx_.router.rebuild();
                return true;
        }
        return false;
    }

    void tick(uint32_t now) override {
        if (demo()) return;
        beat::LoopPlayer& pl = beat::player();
        pl.tick();                                       // reaps; WNOHANG only
        if (!playing_) return;

        const double bar = beat::bar_ms(live_.bpm);
        const uint32_t loop_ms = static_cast<uint32_t>(bar * bars_);
        if (audio_) {
            if (!pl.playing()) {                         // the loop file ran out (or failed)
                if (now - loop_start_ < 300) {
                    if (++quick_fails_ >= 2) {
                        audio_ = false;
                        toast(pl.error().empty() ? std::string("Audio player keeps stopping") : pl.error(), 2400);
                    }
                } else {
                    quick_fails_ = 0;
                }
                if (audio_) relaunch(now, live_rev_ != edit_rev_);
                else loop_start_ = now;
            }
        } else if (now - loop_start_ >= loop_ms) {
            loop_start_ += loop_ms;                      // silent: keep the visual clock looping
            if (live_rev_ != edit_rev_) relaunch(now, true);
        }

        const int32_t el = static_cast<int32_t>(now - loop_start_) - (audio_ ? kStartLatencyMs : 0);
        const double t = std::max(0, el);
        const long bar_idx = static_cast<long>(t / bar);
        // An edit is heard from the next bar line: re-render and restart there.
        if (bar_idx != last_bar_ && last_bar_ >= 0 && live_rev_ != edit_rev_) {
            relaunch(now, true);
            return;
        }
        last_bar_ = bar_idx;
        const int step = beat::step_at(live_, std::fmod(t, bar));
        if (step != shown_step_) show_step(step);
    }

private:
    enum class Kind : uint8_t { Preset, User, Received };
    struct Entry {
        Kind kind;
        int index;               // preset / slot / received index
        bool usable;             // false: an empty slot
    };
    struct Rx {
        beat::Pattern pat;
        std::string from;
    };
    struct Target {
        uint32_t node = 0;
        std::string name;
        std::string right;
    };

    bool demo() const { return demo_ >= 0; }
    bool playing() const { return demo() ? demo_play_step_ >= 0 : playing_; }

    void edited() {
        ++edit_rev_;
        refresh_grid();
    }

    void remember() {
        if (demo() || !store::enabled()) return;
        settings().set_string("beats.last", beat::to_text(pat_));
    }

    // ── Playback ────────────────────────────────────────────────────────────
    void start_play() {
        if (demo()) {
            demo_play_step_ = demo_play_step_ >= 0 ? -1 : 0;
            return;
        }
        playing_ = true;
        quick_fails_ = 0;
        audio_ = true;
        relaunch(lv_tick_get(), true);
        if (!audio_) toast(beat::player().error(), 2400);
    }

    void stop_play() {
        if (demo()) return;
        beat::player().stop();
        playing_ = false;
        last_bar_ = -1;
        show_step(-1);
    }

    // (Re)starts the loop from its first bar. `render`: the pattern changed.
    void relaunch(uint32_t now, bool render) {
        beat::LoopPlayer& pl = beat::player();
        if (render || !audio_) {
            live_ = pat_;
            live_rev_ = edit_rev_;
            bars_ = beat::loop_bars(live_.bpm);
        }
        if (audio_) {
            const bool ok = render ? pl.play(beat::render(live_, bars_)) : pl.replay();
            if (!ok) audio_ = false;                     // keep the playhead going, silently
        }
        loop_start_ = now;
        last_bar_ = -1;
    }

    void audition(int track) {
        if (demo() || playing_) return;
        beat::player().play(beat::render_hit(track, pat_.steps[track][cur_s_] == beat::Accent, pat_.vol[track]));
    }

    // ── Grid ────────────────────────────────────────────────────────────────
    void clear_grid_refs() {
        for (auto& row : pads_) row.fill(nullptr);
        labels_.fill(nullptr);
        label_bg_.fill(nullptr);
        vols_.fill(nullptr);
        heads_.fill(nullptr);
        cursor_ = column_ = lcd_name_ = lcd_bpm_ = lcd_swing_ = lcd_state_ = lcd_step_ = nullptr;
        shown_step_ = -2;
    }

    void build_grid(lv_obj_t* body) {
        const Skin& sk = skin();
        lv_obj_t* g = glass(body, 0, 0, kClientW, kLcdH);
        lcd_name_ = amber(g, "", 4, 1, 128, true);
        lcd_bpm_ = amber(g, "", 136, 1, 58);
        lcd_swing_ = amber(g, "", 196, 1, 50);
        lcd_state_ = label(g, "", 254, 2, &lv_font_montserrat_12, kAmber);
        lcd_step_ = amber(g, "", 270, 1, 36);

        lv_obj_t* f = win::field(body, 0, kGridY, kClientW, kGridH);
        grid_ = f;
        off_a_ = mix(sk.field_text, sk.field, 16);
        off_b_ = mix(sk.field_text, sk.field, 27);
        for (int t = 0; t < beat::kTracks; ++t) {
            label_bg_[t] = solid(f, kLabelX - 1, row_y(t) - 1, kLabelW, kRowPitch - 1, sk.sel);
            labels_[t] = label(f, beat::track_name(t), kLabelX + 2, row_y(t) - 3, font_ui(), sk.field_text, kLabelW - 4);
            for (int s = 0; s < beat::kSteps; ++s) {
                lv_obj_t* p = solid(f, pad_x(s), row_y(t), kPadW, kPadH, off_a_);
                lv_obj_set_style_radius(p, 1, 0);
                pads_[t][s] = p;
            }
            vols_[t] = label(f, "", kVolX, row_y(t) - 3, font_ui(), sk.dim);
        }
        for (int s = 0; s < beat::kSteps; ++s) {
            heads_[s] = solid(f, pad_x(s) + 3, kHeadY + 1, kPadW - 6, 3, s % 4 ? off_a_ : off_b_);
        }
        // Playhead column: a tint over the step being heard.
        column_ = solid(f, pad_x(0) - 1, kRowY0 - 1, kPadW + 2, beat::kTracks * kRowPitch, kLedPlay);
        lv_obj_set_style_bg_opa(column_, LV_OPA_30, 0);
        lv_obj_add_flag(column_, LV_OBJ_FLAG_HIDDEN);
        // Cursor: a 1px box round the pad.
        cursor_ = box(f, 0, 0, kPadW + 4, kPadH + 4);
        lv_obj_set_style_border_width(cursor_, 1, 0);
        lv_obj_set_style_border_color(cursor_, c(sk.field_text), 0);
        lv_obj_set_style_border_opa(cursor_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(cursor_, 2, 0);
        refresh_grid();
        show_step(playing() ? (demo() ? demo_play_step_ : std::max(0, shown_step_)) : -1);
    }

    // Restyles the grid objects from pat_ (no objects are created).
    void refresh_grid() {
        if (!grid_ || !pads_[0][0]) return;
        const Skin& sk = skin();
        for (int t = 0; t < beat::kTracks; ++t) {
            const bool sel = t == cur_t_;
            const bool muted = pat_.muted(t);
            if (sel) lv_obj_clear_flag(label_bg_[t], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(label_bg_[t], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(labels_[t], c(sel ? sk.sel_text : muted ? sk.dim : sk.field_text), 0);
            for (int s = 0; s < beat::kSteps; ++s) {
                const uint8_t v = pat_.steps[t][s];
                const uint32_t off = (s / 4) % 2 ? off_b_ : off_a_;
                uint32_t col = v == beat::Accent ? kLedAccent : v ? kLedOn : off;
                if (v && muted) col = mix(col, sk.field, 40);
                set_bg(pads_[t][s], col);
                // An accent is a full pad; a plain hit is a slightly shorter one.
                lv_obj_set_height(pads_[t][s], v == beat::On ? kPadH - 2 : kPadH);
                lv_obj_set_y(pads_[t][s], row_y(t) + (v == beat::On ? 1 : 0));
            }
            if (muted) {
                set_text(vols_[t], "M");
                lv_obj_set_style_text_color(vols_[t], c(kLedOn), 0);
            } else {
                set_text(vols_[t], std::to_string(pat_.vol[t]));
                lv_obj_set_style_text_color(vols_[t], c(sel ? sk.field_text : sk.dim), 0);
            }
        }
        lv_obj_set_pos(cursor_, pad_x(cur_s_) - 2, row_y(cur_t_) - 2);
        set_text(lcd_name_, pat_.name.empty() ? "(untitled)" : pat_.name);
        set_text(lcd_bpm_, fmt("BPM %d", pat_.bpm));
        set_text(lcd_swing_, pat_.swing <= beat::kMinSwing ? std::string("SW --") : fmt("SW %d", pat_.swing));
        refresh_state();
    }

    void refresh_state() {
        if (!lcd_state_) return;
        const bool play = playing();
        set_text(lcd_state_, play ? LV_SYMBOL_PLAY : LV_SYMBOL_STOP);
        lv_obj_set_style_text_color(lcd_state_, c(play ? kLedPlay : kAmberDim), 0);
        const int st = demo() ? demo_play_step_ : shown_step_;
        set_text(lcd_step_, play && st >= 0 ? fmt("%d.%d", st / 4 + 1, st % 4 + 1) : fmt("%d.%d", cur_s_ / 4 + 1, cur_s_ % 4 + 1));
        lv_obj_set_style_text_color(lcd_step_, c(play ? kAmber : kAmberDim), 0);
    }

    // Moves the playhead (-1 hides it). Touches three objects and a label.
    void show_step(int step) {
        const int prev = shown_step_;
        shown_step_ = step;
        if (!column_) return;
        if (prev >= 0 && prev < beat::kSteps && heads_[prev]) set_bg(heads_[prev], prev % 4 ? off_a_ : off_b_);
        if (step < 0) {
            lv_obj_add_flag(column_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(column_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_x(column_, pad_x(step) - 1);
            set_bg(heads_[step], kLedPlay);
        }
        refresh_state();
    }

    bool grid_key(const KeyEvent& e) {
        const int slot = softkey_slot(e);
        switch (slot) {
            case 0: toggle_play(); return true;
            case 1: open_bank(); ctx_.router.rebuild(); return true;
            case 2: open_save(); ctx_.router.rebuild(); return true;
            case 3:
                if (!pat_.empty()) {
                    open_send();
                    ctx_.router.rebuild();
                }
                return true;
            case 4: ask_clear(); return true;
            case kSoftRight: mode_ = Mode::Help; ctx_.router.rebuild(); return true;
            case kSoftLeft: return false;                // Router: back
            default: break;
        }
        switch (e.key) {
            case Key::Left: cur_s_ = (cur_s_ + beat::kSteps - 1) % beat::kSteps; refresh_grid(); return true;
            case Key::Right: cur_s_ = (cur_s_ + 1) % beat::kSteps; refresh_grid(); return true;
            case Key::Up: cur_t_ = (cur_t_ + beat::kTracks - 1) % beat::kTracks; refresh_grid(); return true;
            case Key::Down: cur_t_ = (cur_t_ + 1) % beat::kTracks; refresh_grid(); return true;
            case Key::A: {
                uint8_t& v = pat_.steps[cur_t_][cur_s_];
                v = v ? beat::Off : beat::On;
                edited();
                if (v) audition(cur_t_);
                return true;
            }
            case Key::Tab: toggle_play(); return true;
            case Key::Char: break;
            default: return false;                       // Esc: the Router pops us
        }
        const char ch = static_cast<char>(e.ch);
        switch (ch) {
            case 'a': {
                uint8_t& v = pat_.steps[cur_t_][cur_s_];
                v = v == beat::Accent ? beat::On : beat::Accent;
                edited();
                audition(cur_t_);
                return true;
            }
            case 'm': pat_.mute ^= static_cast<uint8_t>(1u << cur_t_); edited(); return true;
            case 'p': toggle_play(); return true;
            case '-': case '_': bump_bpm(e.raw == '_' ? -10 : -1); return true;
            case '=': case '+': bump_bpm(e.raw == '+' ? 10 : 1); return true;
            case '[': pat_.swing = std::max(beat::kMinSwing, pat_.swing - 2); edited(); return true;
            case ']': pat_.swing = std::min(beat::kMaxSwing, pat_.swing + 2); edited(); return true;
            case 'v': {
                const int v = pat_.vol[cur_t_];
                const int nv = e.raw == 'V' ? (v + beat::kMaxVol) % (beat::kMaxVol + 1) : (v + 1) % (beat::kMaxVol + 1);
                pat_.vol[cur_t_] = static_cast<uint8_t>(nv);
                edited();
                audition(cur_t_);
                return true;
            }
            case 'e': pat_.clear_track(cur_t_); edited(); return true;
            case 'n': next_pattern(); return true;
            case 'h': case '?': mode_ = Mode::Help; ctx_.router.rebuild(); return true;
            default: return true;                        // stray letters do nothing
        }
    }

    void bump_bpm(int d) {
        pat_.bpm = std::clamp(pat_.bpm + d, beat::kMinBpm, beat::kMaxBpm);
        edited();
    }

    void toggle_play() {
        if (playing()) stop_play();
        else start_play();
        if (demo()) refresh_state();
        ctx_.router.refresh_chrome();
    }

    void ask_clear() {
        if (pat_.empty()) return;
        const int t = cur_t_;
        ctx_.router.show_modal(std::make_unique<Modal>(
            "Clear", std::string("Clear the ") + beat::track_long_name(t) + " track, or the whole pattern?",
            std::vector<std::string>{"Track", "All", "Cancel"}, [this, t](int choice) {
                if (choice == 0) pat_.clear_track(t);
                else if (choice == 1) pat_.clear();
                else return;
                edited();
                ctx_.router.refresh_chrome();
            }));
    }

    // 'n': the next preset / used slot after the one loaded, round the bank.
    void next_pattern() {
        std::vector<beat::Pattern> all(beat::presets().begin(), beat::presets().end());
        for (int i = 0; i < beat::kUserSlots; ++i)
            if (lib_.used(i)) all.push_back(lib_.slot(i));
        int at = -1;
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i].name == pat_.name) at = static_cast<int>(i);
        load(all[static_cast<size_t>((at + 1) % static_cast<int>(all.size()))]);
    }

    void load(const beat::Pattern& p) {
        pat_ = p;
        if (pat_.name.empty()) pat_.name = "Untitled";
        edited();
        remember();
        if (lcd_name_) toast("Loaded \"" + pat_.name + "\"", 1200);
    }

    // ── Bank ────────────────────────────────────────────────────────────────
    void open_bank() {
        if (!demo()) {
            lib_.load();
            scan_received();
        }
        mode_ = Mode::Bank;
        // Start on the loaded pattern if it is in the bank.
        build_entries();
        bank_row_ = 1;
        for (size_t r = 0; r < rows_.size(); ++r) {
            const int ei = rows_[r].tag;
            if (ei >= 0 && entries_[ei].usable && entry_pattern(entries_[ei]).name == pat_.name) {
                bank_row_ = static_cast<int>(r);
                break;
            }
        }
    }

    // Beats buddies sent: incoming DMs and channel messages that parse as one.
    void scan_received() {
        received_.clear();
        const Messenger& m = messenger();
        auto scan = [&](uint32_t node) {
            for (const Message& msg : m.messages(node)) {
                if (msg.from_me) continue;
                Rx rx;
                if (!beat::parse_shared(msg.text, rx.pat)) continue;
                const uint32_t who = Messenger::is_channel(node) && msg.from_node ? msg.from_node : node;
                rx.from = m.name_of(who);
                received_.insert(received_.begin(), std::move(rx));   // newest first, roughly
                if (received_.size() > 12) received_.pop_back();
            }
        };
        for (const Contact& ct : m.contacts()) scan(ct.node_id);
        if (m.has_chat(Messenger::kChannelNode)) scan(Messenger::kChannelNode);
    }

    void build_entries() {
        entries_.clear();
        rows_.clear();
        auto header = [&](const std::string& s) {
            win::Row h;
            h.text = s;
            h.header = true;
            h.selectable = false;
            h.tag = -1;
            rows_.push_back(h);
        };
        auto add = [&](Kind k, int idx, bool usable, const std::string& text, const std::string& right) {
            win::Row r;
            r.text = text;
            r.right = right;
            r.indent = 6;
            r.dim = !usable;
            r.tag = static_cast<int>(entries_.size());
            rows_.push_back(r);
            entries_.push_back({k, idx, usable});
        };
        header("Presets");
        for (size_t i = 0; i < beat::presets().size(); ++i)
            add(Kind::Preset, static_cast<int>(i), true, beat::presets()[i].name, std::to_string(beat::presets()[i].bpm) + " ");
        header("My beats");
        for (int i = 0; i < beat::kUserSlots; ++i) {
            add(Kind::User, i, lib_.used(i), fmt("%d: ", i + 1) + (lib_.used(i) ? lib_.slot(i).name : "(empty)"),
                lib_.used(i) ? std::to_string(lib_.slot(i).bpm) + " " : "");
        }
        if (!received_.empty()) {
            header("Received");
            for (size_t i = 0; i < received_.size(); ++i)
                add(Kind::Received, static_cast<int>(i), true, received_[i].pat.name, std::to_string(received_[i].pat.bpm) + " ");
        }
    }

    const beat::Pattern& entry_pattern(const Entry& e) const {
        switch (e.kind) {
            case Kind::Preset: return beat::presets()[static_cast<size_t>(e.index)];
            case Kind::User: return lib_.slot(e.index);
            default: return received_[static_cast<size_t>(e.index)].pat;
        }
    }

    const Entry* cur_entry() const {
        if (bank_row_ < 0 || bank_row_ >= static_cast<int>(rows_.size())) return nullptr;
        const int ei = rows_[static_cast<size_t>(bank_row_)].tag;
        return ei >= 0 && ei < static_cast<int>(entries_.size()) ? &entries_[static_cast<size_t>(ei)] : nullptr;
    }

    void build_bank(lv_obj_t* body) {
        const Skin& sk = skin();
        build_entries();
        constexpr int kListW = 150;
        list_ = std::make_unique<win::ListBox>(body, 0, 0, kListW, kClientH);
        list_->set_rows(rows_);
        list_->select(bank_row_);
        bank_row_ = list_->selected();

        const int px = kListW + 6, pw = kClientW - px;
        lv_obj_t* cd = win::card(body, px, 0, pw, kClientH);
        const Entry* e = cur_entry();
        if (!e || !e->usable) {
            win::text(cd, "Empty slot", 5, 4, sk.card_text, pw - 10, true);
            win::text_wrap(cd, "SAVE on the grid puts the pattern you are making here.", 5, 20, pw - 12, sk.card_text);
            return;
        }
        const beat::Pattern& p = entry_pattern(*e);
        win::text(cd, p.name, 5, 2, sk.card_text, pw - 10, true);
        std::string sub;
        if (e->kind == Kind::Preset) sub = beat::preset_list()[static_cast<size_t>(e->index)].blurb;
        else if (e->kind == Kind::User) sub = fmt("my slot %d", e->index + 1);
        else sub = "from " + received_[static_cast<size_t>(e->index)].from;
        win::text(cd, sub, 5, 15, sk.dim == sk.card ? sk.card_text : mix(sk.card_text, sk.card, 70), pw - 10);
        win::text(cd, fmt("%d BPM   swing %s", p.bpm, swing_text(p.swing).c_str()), 5, 29, sk.card_text, pw - 10);
        mini_grid(cd, 6, 47, p, sk.card, sk.card_text);
        win::text(cd, fmt("%d hits", p.hits()), 5, kClientH - 18, mix(sk.card_text, sk.card, 70), pw - 10);
    }

    bool bank_key(const KeyEvent& e) {
        const int slot = softkey_slot(e);
        if (slot == 4 || slot == kSoftLeft || e.key == Key::B) {
            mode_ = Mode::Grid;
            ctx_.router.rebuild();
            return true;
        }
        if (slot == 0 || e.key == Key::A) {
            const Entry* en = cur_entry();
            if (!en || !en->usable) return true;
            const beat::Pattern p = entry_pattern(*en);
            mode_ = Mode::Grid;
            ctx_.router.rebuild();
            load(p);
            return true;
        }
        if (slot == 1) {
            delete_slot();
            return true;
        }
        if (list_ && list_->on_key(e)) {
            bank_row_ = list_->selected();
            ctx_.router.rebuild();                       // the preview follows
            return true;
        }
        return true;
    }

    void delete_slot() {
        const Entry* en = cur_entry();
        if (!en || en->kind != Kind::User || !en->usable) return;
        const int idx = en->index;
        ctx_.router.show_modal(std::make_unique<Modal>(
            "Delete beat?", "Delete \"" + lib_.slot(idx).name + "\" from slot " + std::to_string(idx + 1) + "?",
            std::vector<std::string>{"Delete", "Keep"}, [this, idx](int choice) {
                if (choice != 0) return;
                lib_.erase(idx);
                if (!demo() && !lib_.save() && store::enabled()) toast("Could not write beats.json", 2400);
                ctx_.router.rebuild();
            }));
    }

    // ── Save ────────────────────────────────────────────────────────────────
    void open_save() {
        if (!demo()) lib_.load();
        save_name_ = pat_.name;
        save_slot_ = 0;
        for (int i = 0; i < beat::kUserSlots; ++i) {          // the slot it came from, else the first free one
            if (lib_.used(i) && lib_.slot(i).name == pat_.name) { save_slot_ = i; break; }
        }
        if (!lib_.used(save_slot_) || lib_.slot(save_slot_).name != pat_.name) {
            for (int i = 0; i < beat::kUserSlots; ++i)
                if (!lib_.used(i)) { save_slot_ = i; break; }
        }
        mode_ = Mode::Save;
    }

    void build_save(lv_obj_t* body) {
        const Skin& sk = skin();
        constexpr int kLeftW = 150;
        lv_obj_t* cd = win::card(body, 0, 0, kLeftW, kClientH);
        win::text(cd, "Save pattern", 5, 2, sk.card_text, kLeftW - 10, true);
        mini_grid(cd, 4, 19, pat_, sk.card, sk.card_text, 7, 4);
        win::text(cd, fmt("%d BPM  swing %s", pat_.bpm, swing_text(pat_.swing).c_str()), 5, 70, sk.card_text,
                  kLeftW - 10);
        win::text(cd, fmt("%d hits  beats.json", pat_.hits()), 5, 84, mix(sk.card_text, sk.card, 70), kLeftW - 10);

        const int px = kLeftW + 6, pw = kClientW - px;
        win::text(body, "Name:", px, 1);
        name_in_ = std::make_unique<win::TextInput>(body, px + 38, 0, pw - 38, 15, static_cast<int>(beat::kMaxNameBytes));
        name_in_->set_value(save_name_);
        name_in_->set_placeholder("name this beat");
        win::text(body, "Slot:", px, 20);
        list_ = std::make_unique<win::ListBox>(body, px, 34, pw, kClientH - 34 - 20);
        std::vector<win::Row> rows;
        for (int i = 0; i < beat::kUserSlots; ++i) {
            win::Row r;
            r.text = fmt("%d: ", i + 1) + (lib_.used(i) ? lib_.slot(i).name : "(empty)");
            r.right = lib_.used(i) ? "replace " : "";
            r.dim = !lib_.used(i);
            rows.push_back(r);
        }
        list_->set_rows(std::move(rows));
        list_->select(save_slot_);
        win::button(body, "Save", px, kClientH - 17, 60, true);
        win::button(body, "Cancel", px + 66, kClientH - 17, 60, false);
    }

    bool save_key(const KeyEvent& e) {
        const bool real_up = e.raw == LV_KEY_UP, real_down = e.raw == LV_KEY_DOWN;
        if (e.key == Key::A && e.raw != ' ') {
            save_now();
            return true;
        }
        if (e.raw == LV_KEY_ESC || (e.key == Key::B && e.raw != LV_KEY_BACKSPACE) ||
            (e.raw == LV_KEY_BACKSPACE && save_name_.empty())) {
            mode_ = Mode::Grid;
            ctx_.router.rebuild();
            return true;
        }
        if (e.key == Key::Tab || real_down) {
            save_slot_ = (save_slot_ + 1) % beat::kUserSlots;
            if (list_) list_->select(save_slot_);
            return true;
        }
        if (real_up) {
            save_slot_ = (save_slot_ + beat::kUserSlots - 1) % beat::kUserSlots;
            if (list_) list_->select(save_slot_);
            return true;
        }
        if (name_in_ && e.raw != ':' && name_in_->on_key(e)) {
            save_name_ = name_in_->value();
            return true;
        }
        return true;                                     // modal: swallow the rest
    }

    void save_now() {
        std::string name = beat::clean_name(save_name_);
        if (name.empty()) name = "Untitled";
        pat_.name = name;
        const int slot = save_slot_;
        lib_.set(slot, pat_);
        mode_ = Mode::Grid;
        ctx_.router.rebuild();
        if (demo()) return;
        remember();
        if (!lib_.save() && store::enabled()) toast("Could not write beats.json", 2400);
        else toast(fmt("Saved \"%s\" to slot %d", name.c_str(), slot + 1), 1600);
    }

    // ── Send (one text message, only from send_now()) ───────────────────────
    void open_send() {
        targets_.clear();
        const Messenger& m = messenger();
        targets_.push_back({Messenger::kChannelNode, "Channel: " + Messenger::channel_name(), "all "});
        std::vector<Target> later;
        for (const Contact& ct : m.contacts()) {
            Target tg{ct.node_id, ct.name, ct.online() ? "online " : ct.presence == Presence::Away ? "away " : "off "};
            (ct.online() ? targets_ : later).push_back(tg);
        }
        targets_.insert(targets_.end(), later.begin(), later.end());
        send_sel_ = targets_.size() > 1 ? 1 : 0;
        mode_ = Mode::Send;
    }

    void build_send(lv_obj_t* body) {
        const Skin& sk = skin();
        constexpr int kLeftW = 150;
        lv_obj_t* cd = win::card(body, 0, 0, kLeftW, kClientH);
        win::text(cd, "Send \"" + pat_.name + "\"", 5, 2, sk.card_text, kLeftW - 10, true);
        mini_grid(cd, 4, 19, pat_, sk.card, sk.card_text, 7, 4);
        const int bytes = static_cast<int>(beat::share_text(pat_).size());
        win::text(cd, fmt("%d BPM  swing %s", pat_.bpm, swing_text(pat_.swing).c_str()), 5, 70, sk.card_text,
                  kLeftW - 10);
        win::text(cd, fmt("1 message, %d bytes", bytes), 5, 84, mix(sk.card_text, sk.card, 70), kLeftW - 10);
        win::text(cd, "Their MeshBeat opens it.", 5, 98, mix(sk.card_text, sk.card, 70), kLeftW - 10);

        const int px = kLeftW + 6, pw = kClientW - px;
        win::text(body, "Send to:", px, 1);
        list_ = std::make_unique<win::ListBox>(body, px, 16, pw, kClientH - 16);
        std::vector<win::Row> rows;
        for (const Target& t : targets_) {
            win::Row r;
            r.text = t.name;
            r.right = t.right;
            r.dim = t.right == "off ";
            rows.push_back(r);
        }
        list_->set_rows(std::move(rows));
        list_->select(send_sel_);
    }

    bool send_key(const KeyEvent& e) {
        const int slot = softkey_slot(e);
        if (slot == 4 || slot == kSoftLeft || e.key == Key::B) {
            mode_ = Mode::Grid;
            ctx_.router.rebuild();
            return true;
        }
        if (slot == 0 || e.key == Key::A) {
            send_now();
            return true;
        }
        if (list_ && list_->on_key(e)) {
            send_sel_ = list_->selected();
            return true;
        }
        return true;
    }

    void send_now() {
        if (send_sel_ < 0 || send_sel_ >= static_cast<int>(targets_.size())) return;
        const Target t = targets_[static_cast<size_t>(send_sel_)];
        mode_ = Mode::Grid;
        ctx_.router.rebuild();
        if (demo()) return;                              // --shots: never touches the radio
        const std::string text = beat::share_text(pat_);
        if (text.size() > beat::kMaxShareBytes || !messenger().send(t.node, text)) {
            toast("Could not send the beat", 2400);
        } else {
            toast("Sent \"" + pat_.name + "\" to " + t.name, 2000);
        }
    }

    // ── Help ────────────────────────────────────────────────────────────────
    void build_help(lv_obj_t* body) {
        const Skin& sk = skin();
        lv_obj_t* cd = win::card(body, 0, 0, kClientW, kClientH);
        win::text(cd, "MeshBeat keys", 6, 2, sk.card_text, 200, true);
        static const char* const kLeft[][2] = {
            {"arrows", "move cursor"}, {"Enter/Spc", "step on / off"}, {"A", "accent step"},
            {"M", "mute track"},          {"V / shift-V", "track volume"},  {"E", "erase track"},
            {"P or Tab", "play / stop"},
        };
        static const char* const kRight[][2] = {
            {"- / =", "BPM -1 / +1"}, {"_ / +", "BPM -10 / +10"}, {"[ / ]", "swing"},
            {"N", "next pattern"}, {"keys 4-8", "the buttons"},   {"Esc", "close MeshBeat"},
        };
        const uint32_t dimc = mix(sk.card_text, sk.card, 65);
        int y = 18;
        for (const auto& r : kLeft) {
            win::text(cd, r[0], 6, y, sk.card_text, 66, true);
            win::text(cd, r[1], 72, y, dimc, 82);
            y += 14;
        }
        y = 18;
        for (const auto& r : kRight) {
            win::text(cd, r[0], 160, y, sk.card_text, 54, true);
            win::text(cd, r[1], 214, y, dimc, 92);
            y += 14;
        }
    }

    // ── State ───────────────────────────────────────────────────────────────
    int demo_ = -1;
    int demo_play_step_ = -1;
    Mode mode_ = Mode::Grid;
    beat::Pattern pat_;
    beat::Library lib_;
    int cur_t_ = 0, cur_s_ = 0;

    // Playback.
    bool playing_ = false;
    bool audio_ = true;
    int quick_fails_ = 0;
    beat::Pattern live_;                 // what the running loop file holds
    uint32_t edit_rev_ = 0, live_rev_ = 0;
    int bars_ = 1;
    uint32_t loop_start_ = 0;
    long last_bar_ = -1;
    int shown_step_ = -1;

    // Bank / save / send.
    std::vector<Entry> entries_;
    std::vector<win::Row> rows_;
    std::vector<Rx> received_;
    int bank_row_ = 1;
    std::string save_name_;
    int save_slot_ = 0;
    std::vector<Target> targets_;
    int send_sel_ = 0;

    // Objects of the latest build().
    lv_obj_t* grid_ = nullptr;
    std::array<std::array<lv_obj_t*, beat::kSteps>, beat::kTracks> pads_{};
    std::array<lv_obj_t*, beat::kTracks> labels_{}, label_bg_{}, vols_{};
    std::array<lv_obj_t*, beat::kSteps> heads_{};
    lv_obj_t* cursor_ = nullptr;
    lv_obj_t* column_ = nullptr;
    lv_obj_t* lcd_name_ = nullptr;
    lv_obj_t* lcd_bpm_ = nullptr;
    lv_obj_t* lcd_swing_ = nullptr;
    lv_obj_t* lcd_state_ = nullptr;
    lv_obj_t* lcd_step_ = nullptr;
    uint32_t off_a_ = 0, off_b_ = 0;
    std::unique_ptr<win::ListBox> list_;
    std::unique_ptr<win::TextInput> name_in_;
};

} // namespace

std::unique_ptr<Screen> make_beats_screen(Ctx& ctx) {
    return std::make_unique<BeatsScreen>(ctx);
}

} // namespace mm::ui
