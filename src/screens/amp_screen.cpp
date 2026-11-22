// amp_screen.cpp — MeshAmp: a compact Winamp-style player for local music
// files and NTS Radio streams (backend/audio_player.h does the playing).
//
// Left: display panel (big readout, decorative spectrum bars, status line),
// title marquee, transport buttons, volume, "Set as away message".
// Right: "Files | NTS Radio | Retro" tabs over a list.
//
// V switches the whole window to the visualizer (ui/viz.h): built-in presets
// plus any MilkDrop 1 preset files found under assets/milkdrop (ui/milk.h),
// driven by the player's PCM tap when ffmpeg is installed, otherwise by a
// simulated beat.
//
// The screen draws from a View snapshot. Normally that is read from the
// AudioPlayer; in demo states (--shots) it comes from canned data and no
// process, network or music directory is touched.
#include "screens.h"

#include "audio_player.h"
#include "milk.h"
#include "viz.h"
#include "game_state.h"
#include "router.h"
#include "settings.h"
#include "widgets.h"
#include "win.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace mm::ui {
namespace {

constexpr int kLeftW = 150;
constexpr int kRightX = 156;
constexpr int kRightW = kClientW - kRightX;      // 156
constexpr int kBars = 10;
constexpr int kBarMax = 20;
constexpr int kMarqueeW = kLeftW - 6;
constexpr const char* kNoPlayer = "No audio player found (install mpv)";
constexpr int kVizW = kClientW / 2;                // rendered at half size, shown 2x
constexpr int kVizH = kClientH / 2;
constexpr const char* kMarqueeGap = "   +++   ";

// Flip to 1 once viz.h has Visualizer::load_milk (preset files).
#ifndef PC1999_MILK_READY
#define PC1999_MILK_READY 1
#endif
bool load_milk_into(viz::Visualizer& v, const milk::Preset& p) {
#if PC1999_MILK_READY
    return v.load_milk(p);
#else
    (void)v; (void)p;
    return false;
#endif
}

enum Tab { kTabFiles = 0, kTabStations = 1, kTabRetro = 2, kTabCount = 3 };
enum class Kind : uint8_t { None, File, Station, Retro };

const char* const kDemoTracks[] = {
    "Road Song", "Gravel Grinder", "Summit Song", "Switchback Blues",
    "Tent Static", "Last Bar of Signal", "Downhill", "Trailhead Reprise",
};
constexpr int kDemoTrackCount = sizeof(kDemoTracks) / sizeof(kDemoTracks[0]);
constexpr int kDemoBars[kBars] = {8, 14, 19, 11, 16, 9, 13, 6, 10, 4};

struct StationRow {
    std::string name;
    bool live = false;
    std::string country;
};

// Everything build() needs, from the real player or from a demo state.
struct View {
    bool playing = false;
    Kind kind = Kind::None;
    int index = -1;
    int elapsed = 0;
    int volume = 70;
    bool has_backend = true;
    std::string big;        // "02:17", "NTS 1", "--:--"
    std::string line2;      // "live stream, on air"
    std::string marquee;    // "*** DEEP MEDI *** now on NTS 1"
    std::string away;       // "DEEP MEDI on NTS 1"; empty: nothing to announce
    std::vector<std::string> files;
    std::vector<StationRow> stations;
    std::vector<StationRow> retro;
    std::string music_label = "~/Music";
};

std::string clock_text(int seconds) {
    seconds = std::max(0, seconds);
    return fmt("%02d:%02d", std::min(seconds / 60, 99), seconds % 60);
}

std::string upper(std::string s) {
    for (char& ch : s) {
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    }
    return s;
}

// Fills the derived text of a View whose state fields are set. `st` is the
// current station (File/None: ignored), `title` the track title, `ext` its type.
void describe(View& v, const AudioPlayer::Station* st, const std::string& title, const std::string& ext) {
    if (v.kind == Kind::Retro && st) {
        const std::string from = st->country.empty() ? "" : " (" + st->country + ")";
        v.marquee = "*** " + st->name + " *** retro radio" + from;
        v.away = st->name;
        if (v.playing) {
            v.big = st->short_name;
            v.line2 = "internet radio" + from;
        }
    } else if (v.kind == Kind::Station && st) {
        const std::string what = st->now.empty() ? st->name : st->now;
        if (st->live) {
            v.marquee = st->now.empty() ? "*** " + st->name + " *** live radio"
                                        : "*** " + st->now + " *** now on " + st->name;
            v.away = st->now.empty() ? st->name : st->now + " on " + st->name;
        } else {
            v.marquee = "*** " + what + " *** NTS infinite mixtape";
            v.away = what + " (NTS mixtape)";
        }
        if (v.playing) {
            v.big = st->short_name;
            v.line2 = st->live ? "live stream, on air" : "infinite mixtape";
        }
    } else if (v.kind == Kind::File && !title.empty()) {
        v.marquee = "*** " + title + " ***";
        v.away = title;
        if (v.playing) {
            v.big = clock_text(v.elapsed);
            v.line2 = ext.empty() ? "music file" : upper(ext) + " file";
        }
    }
    if (!v.playing) {
        v.big = "--:--";
        v.line2 = "stopped";
        v.away.clear();
        if (!v.has_backend) v.marquee = kNoPlayer;
        else if (v.marquee.empty()) v.marquee = "MeshAmp - pick something and press Enter";
    }
}

void pump_player(lv_timer_t*) {
    audio_player().tick();
}

// Keeps the player ticking (reaping, auto-advance) after the screen is closed.
void ensure_pump() {
    static lv_timer_t* timer = nullptr;
    if (!timer) timer = lv_timer_create(pump_player, 300, nullptr);
}

class AmpScreen final : public Screen {
public:
    explicit AmpScreen(Ctx& ctx) : Screen(ctx) {
        if (!ctx_.game.files_enabled()) {
            set_demo_state(0);                    // --shots: never the real player
            return;
        }
        AudioPlayer& p = audio_player();
        ensure_pump();
        static bool restored = false;             // once per run: the saved volume
        if (!restored) {
            restored = true;
            // The device's system volume remembers itself; only the desktop's
            // per-player volume comes from settings.
            if (!p.system_volume()) p.set_volume(static_cast<int>(settings().get_int("amp.volume", p.volume())));
        }
        vpreset_ = std::max(0, static_cast<int>(settings().get_int("amp.viz_preset", 0)));
        p.rescan_files();
        p.refresh_stations();
        last_refresh_ms_ = lv_tick_get();
        if (p.kind() == AudioPlayer::Kind::Station) tab_ = kTabStations;
        else if (p.kind() == AudioPlayer::Kind::Retro) tab_ = kTabRetro;
        else if (p.kind() == AudioPlayer::Kind::File) tab_ = kTabFiles;
        else tab_ = p.files().empty() ? kTabStations : kTabFiles;
        if (p.index() >= 0) sel_[tab_] = p.index();
    }

    ~AmpScreen() override {
        if (!demo()) audio_player().set_tap(false);
    }
    void on_hide() override {
        if (!demo()) audio_player().set_tap(false);
    }
    void on_show() override {
        if (!demo() && viz_) audio_player().set_tap(true);
    }

    const char* title() const override { return "MeshAmp"; }
    bool windowed() const override { return true; }
    const char* forced_skin() const override { return "winamp"; }   // MeshAmp always looks like itself
    bool fullscreen() const override { return viz_ && full_; }
    std::string hints() const override {
        return viz_ ? (full_ ? "V:player  Z/C:preset  Q:play  S/D:vol"
                             : "V:full screen  Z/C:preset  Q:play  S/D:vol")
                    : "Q:play  W/E:prev/next  S/D:vol  Z/C:tab  V:visuals";
    }

    int demo_state_count() const override { return 6; }
    const char* demo_state_name(int i) const override {
        static const char* const kNames[] = {"nts", "files", "stopped", "retro", "visuals", "visuals-full"};
        return (i >= 0 && i < 6) ? kNames[i] : "main";
    }
    void set_demo_state(int i) override {
        demo_ = std::clamp(i, 0, 5);
        fake_ = Fake{};
        sel_[0] = sel_[1] = sel_[2] = 0;
        viz_ = demo_ >= 4;
        full_ = demo_ == 5;
        vz_.reset();
        if (demo_ >= 4) {
            fake_.playing = true;
            fake_.kind = Kind::Station;
            fake_.index = 0;
            tab_ = kTabStations;
        } else if (demo_ == 3) {
            fake_.playing = true;
            fake_.kind = Kind::Retro;
            fake_.index = 2;
            tab_ = kTabRetro;
            sel_[kTabRetro] = 2;
        } else if (demo_ == 0) {
            fake_.playing = true;
            fake_.kind = Kind::Station;
            fake_.index = 0;
            tab_ = kTabStations;
        } else if (demo_ == 1) {
            fake_.playing = true;
            fake_.kind = Kind::File;
            fake_.index = 2;                      // Summit Song
            fake_.elapsed = 137;                  // 02:17
            tab_ = kTabFiles;
            sel_[kTabFiles] = 2;
        } else {
            tab_ = kTabFiles;
        }
        marquee_text_.clear();
    }

    void build(lv_obj_t* body) override {
        const Skin& s = skin();
        const View v = view();
        if (!demo()) seen_rev_ = audio_player().revision();
        shown_big_ = v.big;
        vimg_ = nullptr;
        vname_ = nullptr;
        if (viz_) {
            build_viz(body, v);
            return;
        }

        // ── Display panel ───────────────────────────────────────────────────
        lv_obj_t* disp = win::card(body, 0, 0, kLeftW, 50);
        big_ = label(disp, v.big, 6, 5, font_big(), s.card_text, 86);
        for (int i = 0; i < kBars; ++i) {
            bars_[i] = box(disp, 96 + i * 5, 0, 3, 1);
            lv_obj_set_style_bg_color(bars_[i], c(s.card_text), 0);
            lv_obj_set_style_bg_opa(bars_[i], LV_OPA_COVER, 0);
            bar_h_[i] = v.playing ? kDemoBars[i] : 1;
            place_bar(i);
        }
        win::text(disp, v.line2, 6, 33, s.card_text, kLeftW - 12);

        // ── Title marquee ───────────────────────────────────────────────────
        lv_obj_t* f = win::field(body, 0, 53, kLeftW, 15);
        lv_obj_t* clip = box(f, 3, 1, kMarqueeW, 13);
        const int ty = (13 - lv_font_get_line_height(font_ui())) / 2;
        marquee_ = win::text(clip, v.marquee, 0, ty, s.field_text);
        lv_obj_update_layout(marquee_);
        marquee_period_ = 0;
        if (lv_obj_get_width(marquee_) > kMarqueeW) {
            // Wrap-around ticker: "text +++ text", shifted by up to one period.
            lv_label_set_text(marquee_, (v.marquee + kMarqueeGap).c_str());
            lv_obj_update_layout(marquee_);
            marquee_period_ = lv_obj_get_width(marquee_);
            lv_label_set_text(marquee_, (v.marquee + kMarqueeGap + v.marquee).c_str());
        }
        if (v.marquee != marquee_text_) {
            marquee_text_ = v.marquee;
            marquee_off_ = 0;
            marquee_hold_until_ = lv_tick_get() + 1200;
        }
        if (marquee_period_ > 0) lv_obj_set_x(marquee_, -(marquee_off_ % marquee_period_));

        // ── Transport ───────────────────────────────────────────────────────
        // The UI font has no media glyphs: these four carry their own font.
        const char* const syms[4] = {LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_STOP, LV_SYMBOL_NEXT};
        for (int i = 0; i < 4; ++i) {
            const bool down = (i == 1 && v.playing) || (i == 2 && !v.playing);
            lv_obj_t* b = win::frame(body, i * 38, 71, 36, 16,
                                     down ? win::Edge::Sunken : win::Edge::Raised, s.face, s.radius);
            lv_obj_t* l = label(b, syms[i], 0, 0, &lv_font_montserrat_12, down ? s.dim : s.text);
            lv_obj_center(l);
        }

        // ── Volume, away message ────────────────────────────────────────────
        win::text(body, "Vol", 1, 89);
        win::meter(body, 24, 91, 98, 9, v.volume, 100, s.sel);
        lv_obj_t* vl = win::text(body, fmt("%d", v.volume), 0, 89);
        lv_obj_update_layout(vl);
        lv_obj_set_x(vl, kLeftW - 1 - lv_obj_get_width(vl));
        win::button(body, "Set as away message", 0, 104, kLeftW);

        // ── Library ─────────────────────────────────────────────────────────
        const int th = win::tabs(body, kRightX, 0, {"Files", "NTS Radio", "Retro"}, tab_);
        list_ = std::make_unique<win::ListBox>(body, kRightX, th, kRightW, kClientH - th);
        std::vector<win::Row> rows;
        if (tab_ == kTabFiles) {
            for (int i = 0; i < static_cast<int>(v.files.size()); ++i) {
                win::Row r;
                r.text = v.files[i];
                r.bold = v.playing && v.kind == Kind::File && v.index == i;
                rows.push_back(std::move(r));
            }
            if (rows.empty()) {
                win::Row r;
                r.text = "No music in " + v.music_label;
                r.dim = true;
                r.selectable = false;
                rows.push_back(std::move(r));
            }
        } else if (tab_ == kTabRetro) {
            for (int i = 0; i < static_cast<int>(v.retro.size()); ++i) {
                win::Row r;
                r.text = v.retro[i].name;
                r.right = v.retro[i].country + " ";
                r.bold = v.playing && v.kind == Kind::Retro && v.index == i;
                rows.push_back(std::move(r));
            }
        } else {
            for (int i = 0; i < static_cast<int>(v.stations.size()); ++i) {
                win::Row r;
                r.text = v.stations[i].name;
                r.right = v.stations[i].live ? "live " : "mix ";   // trailing space: clear of the scroll thumb
                r.bold = v.playing && v.kind == Kind::Station && v.index == i;
                rows.push_back(std::move(r));
            }
        }
        const int n = static_cast<int>(rows.size());
        list_->set_rows(std::move(rows));
        sel_[tab_] = std::clamp(sel_[tab_], 0, std::max(0, n - 1));
        list_->select(sel_[tab_]);
    }

    // MeshAmp takes no text, so the Cardputer Zero's Fn-layer keys work here
    // without Fn: F/Z/X/C are the arrows, Q/W/E play-pause/prev/next and
    // A/S/D mute/volume down/up, the same keys the Fn legends print.
    static KeyEvent bare_keys(KeyEvent e) {
        if (e.key != Key::Char) return e;
        switch (e.ch) {
            case 'f': e.key = Key::Up; break;
            case 'x': e.key = Key::Down; break;
            case 'z': e.key = Key::Left; break;
            case 'c': e.key = Key::Right; break;
            default: break;
        }
        return e;
    }

    bool on_key(const KeyEvent& raw) override {
        const KeyEvent e = bare_keys(raw);
        if (e.key == Key::Char) {
            switch (e.ch) {
                case 'v':                                   // visuals -> full screen -> player
                    if (!viz_) set_viz(true);
                    else if (!full_) set_full(true);
                    else set_viz(false);
                    return true;
                case 'q': toggle(); redraw(); return true;
                case 'w': case 'p': step(-1); redraw(); return true;
                case 'e': case 'n': step(+1); redraw(); return true;
                case 'a':
                    if (!demo()) {
                        audio_player().toggle_mute();
                        toast(audio_player().muted() ? "Muted" : "Sound on", 1200);
                    }
                    redraw();
                    return true;
                case 's': case '-': set_volume(-5); redraw(); return true;
                case 'd': case '+': case '=': set_volume(+5); redraw(); return true;
                default: break;
            }
        }
        if (viz_) {
            if (e.key == Key::B) {                                             // back to the player, not out of MeshAmp
                set_viz(false);
                return true;
            }
            if (e.key == Key::Right || e.key == Key::Down) { pick_preset(vpreset_ + 1); return true; }
            if (e.key == Key::Left || e.key == Key::Up) { pick_preset(vpreset_ - 1); return true; }
            if (e.key == Key::Tab) return true;
            if (e.key == Key::A) {
                if (e.raw == ' ') toggle();
                else pick_preset(vpreset_ + 1);
                redraw();
                return true;
            }
        }
        if (e.key == Key::Tab || e.key == Key::Right || e.key == Key::Left) {   // Z/C: Music / NTS / Retro
            tab_ = (tab_ + (e.key == Key::Left ? kTabCount - 1 : 1)) % kTabCount;
            redraw();
            return true;
        }
        if (list_ && list_->on_key(e)) {
            sel_[tab_] = std::max(0, list_->selected());
            return true;
        }
        if (e.key == Key::Up || e.key == Key::Down) return true;   // at the end of the list
        if (e.key == Key::A) {
            // Enter and Space both arrive as Key::A; Space is the play/stop toggle.
            if (e.raw == ' ') toggle();
            else play_selected();
            redraw();
            return true;
        }
        if (e.key == Key::Char && e.ch == 'm') { away(); return true; }
        return false;   // Esc: the router pops; playback carries on
    }

    void tick(uint32_t now_ms) override {
        if (!demo()) {
            AudioPlayer& p = audio_player();
            p.tick();
            if (now_ms - last_refresh_ms_ > 120000) {   // keep "now on NTS" fresh
                last_refresh_ms_ = now_ms;
                p.refresh_stations();
            }
            if (p.revision() != seen_rev_) {
                ctx_.router.rebuild();
                return;
            }
            if (big_ && p.playing() && p.kind() == AudioPlayer::Kind::File) {
                const std::string t = clock_text(p.elapsed_seconds());
                if (t != shown_big_) {
                    shown_big_ = t;
                    set_text(big_, t);
                }
            }
        }
        if (demo()) return;                         // screenshots stay still
        if (viz_) {
            viz_step(now_ms);
            return;
        }

        // Spectrum bars: decoration only, not derived from the audio.
        if (bars_[0] && now_ms - last_bars_ms_ >= 90) {
            last_bars_ms_ = now_ms;
            const bool on = audio_player().playing();
            for (int i = 0; i < kBars; ++i) {
                int target = 1;
                if (on) {
                    rng_ = rng_ * 1664525u + 1013904223u;
                    target = 2 + static_cast<int>((rng_ >> 16) % (kBarMax - 1));
                }
                const int h = on ? (bar_h_[i] + target) / 2 : std::max(1, bar_h_[i] - 3);
                if (h != bar_h_[i]) {
                    bar_h_[i] = h;
                    place_bar(i);
                }
            }
        }
        // Marquee: 1px per step, pausing at the start of each lap.
        if (marquee_ && marquee_period_ > 0 && now_ms - last_marquee_ms_ >= 45 &&
            static_cast<int32_t>(now_ms - marquee_hold_until_) >= 0) {
            last_marquee_ms_ = now_ms;
            marquee_off_ = (marquee_off_ + 1) % marquee_period_;
            if (marquee_off_ == 0) marquee_hold_until_ = now_ms + 1200;
            lv_obj_set_x(marquee_, -marquee_off_);
        }
    }

private:
    struct Fake {
        bool playing = false;
        Kind kind = Kind::None;
        int index = -1;
        int elapsed = 0;
        int volume = 70;
    };

    bool demo() const { return demo_ >= 0; }
    Kind tab_kind() const {
        return tab_ == kTabStations ? Kind::Station : tab_ == kTabRetro ? Kind::Retro : Kind::File;
    }

    void redraw() { ctx_.router.rebuild(); }

    // ── Visualizer ──────────────────────────────────────────────────────────
    static std::string milk_root() {
        const char* env = std::getenv("PC1999_MILK");
        if (env && *env) return env;
        // Your own presets folder, then an installed pack, then the source tree.
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::string mine = store::path("milkdrop");
        if (fs::is_directory(mine, ec)) return mine;
        if (fs::is_directory("/usr/share/1999online/milkdrop", ec)) return "/usr/share/1999online/milkdrop";
#ifdef APP_SOURCE_ASSETS_ROOT
        return std::string(APP_SOURCE_ASSETS_ROOT) + "/milkdrop";
#else
        return "assets/milkdrop";
#endif
    }

    // Preset files: every *.milk under the folder (two levels of packs), by name.
    void scan_milk() {
        if (milk_scanned_) return;
        milk_scanned_ = true;
        namespace fs = std::filesystem;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(milk_root(), fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end && milk_.size() < 2000; it.increment(ec)) {
            if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
            std::error_code ec2;
            if (!it->is_regular_file(ec2)) continue;
            const std::string ext = it->path().extension().string();
            if (ext == ".milk" || ext == ".MILK") milk_.push_back(it->path().string());
        }
        std::sort(milk_.begin(), milk_.end(), [](const std::string& a, const std::string& b) {
            return fs::path(a).filename().string() < fs::path(b).filename().string();
        });
    }

    int preset_total() const { return viz::Visualizer::preset_count() + static_cast<int>(milk_.size()); }

    // Built-in presets first, then the preset files. A file that cannot be
    // shown (MilkDrop 2 shaders, unreadable) is skipped in the direction of travel.
    void pick_preset(int index) {
        if (!vz_) return;
        const int n = preset_total();
        const int dir = index < vpreset_ ? -1 : 1;
        for (int tries = 0; tries < n; ++tries) {
            index = (index % n + n) % n;
            const int builtin = viz::Visualizer::preset_count();
            if (index < builtin) {
                vz_->set_preset(index);
                vname_text_ = viz::Visualizer::preset_name(index);
                break;
            }
            milk::Preset pr;
            if (milk::load_preset(milk_[static_cast<size_t>(index - builtin)], pr) && !pr.uses_shaders &&
                load_milk_into(*vz_, pr)) {
                vname_text_ = pr.name;
                break;
            }
            index += dir;
        }
        vpreset_ = index;
        if (!demo()) settings().set_int("amp.viz_preset", vpreset_);
        vname_until_ = lv_tick_get() + 3000;
        if (vname_) {
            set_text(vname_, vname_text_);
            lv_obj_remove_flag(vname_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void set_full(bool on) {
        full_ = on;
        vz_.reset();                              // the engine is rebuilt at the new size
        redraw();
    }

    void set_viz(bool on) {
        viz_ = on;
        if (!on && full_) { full_ = false; vz_.reset(); }
        if (!demo()) audio_player().set_tap(on);
        if (on) analyzer_.reset();
        redraw();
    }

    void build_viz(lv_obj_t* body, const View& v) {
        big_ = nullptr;
        marquee_ = nullptr;
        bars_.fill(nullptr);
        list_.reset();
        // Rendered at half size and shown 2x: the window's client area, or the
        // whole 320x170 display in full-screen mode.
        const int area_w = full_ ? 320 : kClientW, area_h = full_ ? 170 : kClientH;
        if (!vz_) {
            vw_ = area_w / 2;
            vh_ = area_h / 2;
            vz_ = std::make_unique<viz::Visualizer>(vw_, vh_);
            vbuf_.assign(static_cast<size_t>(vw_) * vh_, 0xff000000u);
            scan_milk();
            int first = vpreset_;
            if (demo()) {                           // screenshot: a preset file if the pack is there
                first = 0;
                for (size_t i = 0; i < milk_.size(); ++i) {
                    if (milk_[i].find("Geiss - Swirlie") != std::string::npos) {
                        first = viz::Visualizer::preset_count() + static_cast<int>(i);
                        break;
                    }
                }
            }
            pick_preset(first);
        }
        if (demo()) {                               // a settled frame for the screenshot
            for (int f = 0; f < 150; ++f) vz_->render(viz::fake_audio(static_cast<float>(f) / 30.f));
        }
        copy_frame();
        vdsc_ = lv_image_dsc_t{};
        vdsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        vdsc_.header.cf = LV_COLOR_FORMAT_XRGB8888;
        vdsc_.header.w = static_cast<uint32_t>(vw_);
        vdsc_.header.h = static_cast<uint32_t>(vh_);
        vdsc_.header.stride = static_cast<uint32_t>(vw_ * 4);
        vdsc_.data = reinterpret_cast<const uint8_t*>(vbuf_.data());
        vdsc_.data_size = static_cast<uint32_t>(vbuf_.size() * 4);

        lv_obj_t* back = box(body, 0, 0, area_w, area_h);
        lv_obj_set_style_bg_color(back, c(0x000000), 0);
        lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
        vimg_ = lv_image_create(back);
        lv_image_set_src(vimg_, &vdsc_);
        lv_image_set_antialias(vimg_, false);
        lv_image_set_inner_align(vimg_, LV_IMAGE_ALIGN_STRETCH);   // 2x, nearest neighbour
        lv_obj_set_pos(vimg_, (area_w - vw_ * 2) / 2, (area_h - vh_ * 2) / 2);
        lv_obj_set_size(vimg_, vw_ * 2, vh_ * 2);

        // Preset name for a moment after a change; the track along the bottom.
        vname_ = win::text(back, vname_text_, 3, 2, 0xffffff, area_w - 6);
        if (!demo() && static_cast<int32_t>(lv_tick_get() - vname_until_) >= 0) lv_obj_add_flag(vname_, LV_OBJ_FLAG_HIDDEN);
        const std::string what = v.away.empty() ? (v.playing ? "" : "stopped") : v.away;
        if (!full_) win::text(back, what, 3, area_h - 13, 0xc0c0c0, area_w - 6);
    }

    void copy_frame() {
        const uint32_t* src = vz_->pixels();
        for (size_t i = 0; i < vbuf_.size(); ++i) vbuf_[i] = src[i] | 0xff000000u;
    }

    void viz_step(uint32_t now_ms) {
        if (!vz_ || !vimg_ || now_ms - last_viz_ms_ < 40) return;     // 25 fps
        last_viz_ms_ = now_ms;
        AudioPlayer& p = audio_player();
        const float t = static_cast<float>(now_ms) / 1000.f;
        int16_t pcm[2048];
        size_t n;
        while ((n = p.read_pcm(pcm, 2048)) > 0) {
            analyzer_.push(pcm, n, AudioPlayer::kTapRate);
            if (n < 2048) break;
        }
        viz::AudioFrame frame;
        if (p.has_tap()) frame = analyzer_.frame(t);
        else if (p.playing()) frame = viz::fake_audio(t);            // no ffmpeg: a simulated beat
        else frame.time = t;
        vz_->render(frame);
        copy_frame();
        lv_obj_invalidate(vimg_);
        if (vname_ && static_cast<int32_t>(now_ms - vname_until_) >= 0) lv_obj_add_flag(vname_, LV_OBJ_FLAG_HIDDEN);
    }

    void place_bar(int i) {
        lv_obj_set_height(bars_[i], bar_h_[i]);
        lv_obj_set_y(bars_[i], 27 - bar_h_[i]);
    }

    View view() const {
        View v;
        AudioPlayer& p = audio_player();
        for (const AudioPlayer::Station& s : p.stations()) v.stations.push_back({s.name, s.live, ""});
        for (const AudioPlayer::Station& s : p.retro()) v.retro.push_back({s.name, true, s.country});
        if (demo()) {
            // Canned data. stations() is the built-in list: nothing is fetched,
            // scanned or spawned here.
            for (const char* t : kDemoTracks) v.files.emplace_back(t);
            v.playing = fake_.playing;
            v.kind = fake_.kind;
            v.index = fake_.index;
            v.elapsed = fake_.elapsed;
            v.volume = fake_.volume;
            AudioPlayer::Station st;
            std::string title;
            if (v.kind == Kind::Station && v.index >= 0 && v.index < static_cast<int>(p.stations().size())) {
                st = p.stations()[v.index];
                st.now = v.index == 0 ? "DEEP MEDI" : "";
            } else if (v.kind == Kind::Retro && v.index >= 0 && v.index < static_cast<int>(p.retro().size())) {
                st = p.retro()[v.index];
            } else if (v.kind == Kind::File && v.index >= 0 && v.index < kDemoTrackCount) {
                title = kDemoTracks[v.index];
            }
            describe(v, &st, title, "mp3");
            return v;
        }
        const auto& files = p.files();
        for (const AudioPlayer::Track& t : files) v.files.push_back(t.title);
        v.music_label = p.music_dir_label();
        v.playing = p.playing();
        v.kind = p.kind() == AudioPlayer::Kind::File ? Kind::File
               : p.kind() == AudioPlayer::Kind::Station ? Kind::Station
               : p.kind() == AudioPlayer::Kind::Retro ? Kind::Retro : Kind::None;
        v.index = p.index();
        v.elapsed = p.elapsed_seconds();
        v.volume = p.volume();
        v.has_backend = p.has_backend();
        const AudioPlayer::Station* st = nullptr;
        std::string title, ext;
        if (v.kind == Kind::Station && v.index >= 0 && v.index < static_cast<int>(p.stations().size())) {
            st = &p.stations()[v.index];
        } else if (v.kind == Kind::Retro && v.index >= 0 && v.index < static_cast<int>(p.retro().size())) {
            st = &p.retro()[v.index];
        } else if (v.kind == Kind::File && v.index >= 0 && v.index < static_cast<int>(files.size())) {
            title = files[v.index].title;
            ext = files[v.index].ext;
        }
        describe(v, st, title, ext);
        return v;
    }

    // ── Actions (demo states act on the fake player) ────────────────────────
    int list_size(Kind k) const {
        if (k == Kind::Station) return static_cast<int>(audio_player().stations().size());
        if (k == Kind::Retro) return static_cast<int>(audio_player().retro().size());
        return demo() ? kDemoTrackCount : static_cast<int>(audio_player().files().size());
    }

    void play(Kind kind, int index) {
        if (index < 0 || index >= list_size(kind)) return;
        if (demo()) {
            fake_.playing = true;
            fake_.kind = kind;
            fake_.index = index;
            fake_.elapsed = 0;
            return;
        }
        AudioPlayer& p = audio_player();
        const bool ok = kind == Kind::Station ? p.play_station(index)
                      : kind == Kind::Retro ? p.play_retro(index) : p.play_file(index);
        if (!ok) toast(p.has_backend() ? "Could not start the player" : kNoPlayer, 2600);
    }

    void play_selected() {
        const int i = list_ ? list_->selected() : -1;
        if (i >= 0) play(tab_kind(), i);
    }

    void stop() {
        if (demo()) fake_.playing = false;
        else audio_player().stop();
    }

    void toggle() {
        const View v = view();
        if (v.playing) stop();
        else if (v.kind != Kind::None && v.index >= 0) play(v.kind, v.index);   // what was on last
        else play_selected();
    }

    void step(int dir) {
        const View v = view();
        if (v.kind == Kind::None || v.index < 0) {
            play_selected();
            return;
        }
        const int n = list_size(v.kind);
        if (n <= 0) return;
        const int i = (v.index + dir + n) % n;
        play(v.kind, i);
        if (v.kind == tab_kind()) sel_[tab_] = i;
    }

    void set_volume(int delta) {
        if (demo()) fake_.volume = std::clamp(fake_.volume + delta, 0, 100);
        else {
            audio_player().set_volume(audio_player().volume() + delta);
            settings().set_int("amp.volume", audio_player().volume());
            toast("Volume: " + std::to_string(audio_player().volume()) + "%", 1000);
        }
    }

    void away() {
        const View v = view();
        if (v.away.empty()) {
            toast("Nothing is playing", 1600);
            return;
        }
        // TODO(messenger): hand "listening to <title>" to the messenger as the
        // away message once it has an API for that; for now this only toasts.
        toast("Away message: listening to " + v.away, 2600);
    }

    // Visualizer state (kept across rebuilds).
    bool viz_ = false;
    bool full_ = false;             // visualizer over the whole display (F)
    int vw_ = kVizW, vh_ = kVizH;   // engine size
    std::unique_ptr<viz::Visualizer> vz_;
    viz::Analyzer analyzer_;
    std::vector<uint32_t> vbuf_;
    lv_image_dsc_t vdsc_{};
    lv_obj_t* vimg_ = nullptr;
    lv_obj_t* vname_ = nullptr;
    std::string vname_text_;
    std::vector<std::string> milk_;
    bool milk_scanned_ = false;
    int vpreset_ = 0;
    uint32_t vname_until_ = 0;
    uint32_t last_viz_ms_ = 0;

    int demo_ = -1;                 // -1: the real player
    Fake fake_;
    int tab_ = kTabFiles;
    int sel_[kTabCount] = {0, 0, 0};
    uint32_t seen_rev_ = 0;
    uint32_t last_refresh_ms_ = 0;

    // Objects of the latest build().
    std::unique_ptr<win::ListBox> list_;
    lv_obj_t* big_ = nullptr;
    lv_obj_t* marquee_ = nullptr;
    std::array<lv_obj_t*, kBars> bars_{};
    std::array<int, kBars> bar_h_{};
    std::string shown_big_;

    std::string marquee_text_;
    int marquee_period_ = 0;        // px; 0: the title fits
    int marquee_off_ = 0;
    uint32_t marquee_hold_until_ = 0;
    uint32_t last_marquee_ms_ = 0;
    uint32_t last_bars_ms_ = 0;
    uint32_t rng_ = 0x1999;
};

} // namespace

std::unique_ptr<Screen> make_amp_screen(Ctx& ctx) {
    return std::make_unique<AmpScreen>(ctx);
}

} // namespace mm::ui
