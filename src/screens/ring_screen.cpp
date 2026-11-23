// ring_screen.cpp — Ringtones: a 1999 cell-phone ringtone composer.
//
// Three views in one window (backend/ringtone.h does the notation, sound and
// sharing):
//
//   LIBRARY   "Ringtones | Received" tabs over a list; on the right a green
//             phone LCD previews the selected tune (piano roll, length, the
//             RTTTL text). Built-in tunes are public-domain classics plus
//             original beeps; "My tunes" are the user's, kept in
//             ringtones.json. Received = tunes buddies sent, found in the
//             messenger's history (read only).
//   COMPOSER  the LCD fills the window: a piano roll and the note strip with
//             a cursor, Nokia style. Letters type notes (typing() is on, so
//             c and f are notes here, not arrows).
//   SEND TO   pick a buddy or the channel; the tune goes out as one text
//             message (ring::share_text) through Messenger::send().
//
// Demo states (--shots) use canned tunes and never play, save or send.
#include "screens.h"

#include "game_state.h"
#include "messenger.h"
#include "ringtone.h"
#include "router.h"
#include "settings.h"
#include "widgets.h"
#include "win.h"

#include <algorithm>
#include <ctime>

namespace mm::ui {
namespace {

// The phone LCD: its own colours whatever the skin (like MeshAmp's display),
// framed by the skin's sunken edge.
constexpr uint32_t kLcdTop = 0xbcd49c;
constexpr uint32_t kLcdBottom = 0xa2bf82;
constexpr uint32_t kLcdInk = 0x1f2d18;
constexpr uint32_t kLcdDim = 0x6f8a5a;
constexpr uint32_t kLcdFaint = 0x93ad76;

constexpr int kListW = 150;
constexpr int kPaneX = 156;
constexpr int kPaneW = kClientW - kPaneX;     // 156

enum class Mode : uint8_t { Library, Composer, SendTo };
enum Tab { kTabTunes = 0, kTabReceived = 1 };
enum class Focus : uint8_t { Notes, Name, Tempo };

struct Entry {
    ring::Tune tune;
    std::string credit;      // built-in: composer / "original"; mine: ""
    int mine = -1;           // index into Library::tunes(), -1 for built-in
};

struct Received {
    ring::Tune tune;
    std::string from;
    std::string when;        // "2h ago"
    uint32_t ts = 0;
};

// Canned data for --shots (originals, nothing copyrighted).
const char* const kDemoMine[] = {
    "Summit Ring:d=8,o=6,b=160:c,g5,c,e,4g,p,e,g,4c7,p,16b,16a,g,4e",
    "Low Batt:d=16,o=5,b=100:4e6,p,4c6,p,2g",
};
struct DemoRx { const char* from; const char* when; const char* text; };
const DemoRx kDemoRx[] = {
    {"KD2ABC-m", "4m ago", "~RING~ Trail Mix:d=8,o=6,b=170:e,g,a,4b,a,g,e,4d,p,e,g,a,b,d7,2e7"},
    {"Ridge_Gym", "1h ago", "~RING~ Gym Win:d=16,o=6,b=180:g,g,g,4c7,p,a#,8c7,2d7"},
    {"Trailhead-R", "3h ago", "~RING~ Beep Beep:d=32,o=7,b=120:c,p,c,8p,c,p,c,4p,c,p,c"},
    {"ash_k3tchup", "yesterday", "~RING~ Fur Elise:d=16,o=5,b=90:e,d#,e,d#,e,b4,d,c,8a4,p,c4,e4,a4,8b4"},
};

std::string secs_text(uint32_t ms) {
    return fmt("%u.%us", ms / 1000, (ms % 1000) / 100);
}

std::string ago(uint32_t ts) {
    const long now = static_cast<long>(std::time(nullptr));
    const long d = std::max(0L, now - static_cast<long>(ts));
    if (ts == 0) return "";
    if (d < 60) return "just now";
    if (d < 3600) return fmt("%ldm ago", d / 60);
    if (d < 86400) return fmt("%ldh ago", d / 3600);
    if (d < 2 * 86400) return "yesterday";
    return fmt("%ldd ago", d / 86400);
}

int text_w(const std::string& s, const lv_font_t* f) {
    int w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const uint32_t next = i + 1 < s.size() ? static_cast<uint8_t>(s[i + 1]) : 0;
        w += lv_font_get_glyph_width(f, static_cast<uint8_t>(s[i]), next);
    }
    return w;
}

lv_obj_t* solid(lv_obj_t* parent, int x, int y, int w, int h, uint32_t col) {
    lv_obj_t* o = box(parent, x, y, w, h);
    lv_obj_set_style_bg_color(o, c(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

lv_obj_t* lcd_text(lv_obj_t* parent, const std::string& s, int x, int y, int width = 0, bool bold = false,
                   uint32_t col = kLcdInk) {
    return label(parent, s, x, y, bold ? font_ui_bold() : font_ui(), col, width);
}

// The phone LCD: a sunken skinned frame holding a lit green panel.
lv_obj_t* lcd(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* f = win::frame(parent, x, y, w, h, win::Edge::Sunken, kLcdBottom);
    lv_obj_t* glass = box(f, 2, 2, w - 4, h - 4);
    lv_obj_set_style_bg_opa(glass, LV_OPA_COVER, 0);
    win::fill(glass, kLcdTop, kLcdBottom);
    return glass;
}

int note_px(const ring::Note& n) {
    int w;
    switch (n.dur) {
        case 1: w = 28; break;
        case 2: w = 20; break;
        case 4: w = 13; break;
        case 8: w = 9; break;
        case 16: w = 6; break;
        default: w = 4; break;
    }
    if (n.dotted) w = w * 3 / 2;
    return w;
}

// Piano roll: x = time, y = pitch. `first`: first note shown (scrolling mode,
// each note at its own width). fit: the whole tune squeezed into `w`.
void draw_roll(lv_obj_t* parent, int x, int y, int w, int h, const ring::Tune& t, int first, int cursor,
               int playing, bool fit) {
    lv_obj_t* area = box(parent, x, y, w, h);
    for (int i = 0; i < 5; ++i) solid(area, 0, 3 + i * (h - 6) / 4, w, 1, kLcdFaint);   // staff
    if (t.notes.empty()) {
        lcd_text(area, "(empty - type a note)", 4, h / 2 - 7, w - 8, false, kLcdDim);
        return;
    }
    int lo = 127, hi = 0;
    for (const ring::Note& n : t.notes) {
        if (n.rest()) continue;
        lo = std::min(lo, n.midi());
        hi = std::max(hi, n.midi());
    }
    if (lo > hi) lo = hi = 72;
    const int bar_h = 3;
    const int span = std::max(12, hi - lo);
    const int mid = (lo + hi) / 2;
    lo = mid - span / 2;
    auto ypos = [&](int midi) {
        const int yy = (h - bar_h - 2) - (midi - lo) * (h - bar_h - 4) / span;
        return std::clamp(yy, 1, h - bar_h - 1);
    };
    const uint32_t total = std::max<uint32_t>(1, ring::duration_ms(t));
    uint32_t t_ms = 0;
    int px = 0;
    const int n = static_cast<int>(t.notes.size());
    for (int i = fit ? 0 : first; i < n; ++i) {
        const ring::Note& note = t.notes[i];
        int nx, nw;
        if (fit) {
            nx = static_cast<int>(static_cast<uint64_t>(t_ms) * w / total);
            t_ms += ring::note_ms(note, t.bpm);
            nw = std::max(1, static_cast<int>(static_cast<uint64_t>(t_ms) * w / total) - nx - 1);
        } else {
            nx = px;
            nw = note_px(note);
            px += nw + 1;
            if (nx >= w) break;
            nw = std::min(nw, w - nx);
        }
        const bool cur = i == cursor;
        const bool lit = i == playing;
        if (cur && !fit) solid(area, nx, 0, nw, h, kLcdFaint);
        if (note.rest()) {
            solid(area, nx + std::max(0, nw / 2 - 1), h - 5, std::min(3, nw), 2, cur || lit ? kLcdInk : kLcdDim);
        } else {
            const int yy = ypos(note.midi());
            solid(area, nx, yy, nw, lit ? bar_h + 2 : bar_h, kLcdInk);
            if (note.dotted && nw > 3) solid(area, nx + nw - 2, yy - 2, 2, 1, kLcdInk);
        }
        if (cur && !fit) {
            solid(area, nx, h - 1, nw, 1, kLcdInk);
            solid(area, nx, 0, nw, 1, kLcdInk);
        }
    }
}

class RingScreen final : public Screen {
public:
    explicit RingScreen(Ctx& ctx) : Screen(ctx) {
        if (!ctx_.game.files_enabled()) {
            set_demo_state(0);
            return;
        }
        lib_.load();
        refresh_entries();
        refresh_received();
    }
    ~RingScreen() override {
        if (!demo()) ring::player().stop();
    }

    const char* title() const override { return "Ringtones"; }
    bool windowed() const override { return true; }
    bool typing() const override { return mode_ == Mode::Composer; }

    void on_hide() override {
        if (!demo()) ring::player().stop();
    }

    std::string hints() const override {
        if (mode_ == Mode::Composer) {
            if (focus_ == Focus::Name) return "type a name  Enter/Tab:done";
            if (focus_ == Focus::Tempo) return "Up/Dn:tempo +-1  L/R:+-10  Tab:notes";
            return "a-g:note p:rest 1-8:length Spc:play W:save Tab:name/tempo";
        }
        return "Up/Dn:pick  Z/C:tab  Enter:play";
    }

    SoftKeys softkeys() const override {
        SoftKeys k;
        if (mode_ == Mode::Composer) return k;                // digits are note lengths here
        k.left.text = MM_SOFT_BACK;
        if (mode_ == Mode::SendTo) {
            k.set(0, "SEND", !targets_.empty()).set(4, "CANCEL");
            return k;
        }
        const bool play = playing();
        if (tab_ == kTabReceived) {
            const bool any = cur_rx() != nullptr;
            k.set(0, play ? "STOP" : "PLAY", any).set(1, "SAVE", any).set(2, "EDIT", any);
            return k;
        }
        const Entry* e = cur_entry();
        k.set(0, play ? "STOP" : "PLAY", e != nullptr)
            .set(1, "EDIT", e != nullptr)
            .set(2, "NEW")
            .set(3, "SEND", e != nullptr)
            .set(4, "DEL", e && e->mine >= 0);
        return k;
    }

    int demo_state_count() const override { return 5; }
    const char* demo_state_name(int i) const override {
        static const char* const kNames[] = {"library", "composer", "received", "send", "compose-name"};
        return (i >= 0 && i < 5) ? kNames[i] : "main";
    }
    void set_demo_state(int i) override {
        demo_ = std::clamp(i, 0, 4);
        lib_.tunes().clear();
        for (const char* s : kDemoMine) {
            ring::Tune t;
            if (ring::parse_rtttl(s, t)) lib_.tunes().push_back(t);
        }
        refresh_entries();
        received_.clear();
        for (const DemoRx& r : kDemoRx) {
            Received rx;
            if (!ring::parse_shared(r.text, rx.tune)) continue;
            rx.from = r.from;
            rx.when = r.when;
            received_.push_back(std::move(rx));
        }
        mode_ = Mode::Library;
        tab_ = kTabTunes;
        sel_[0] = sel_[1] = 0;
        focus_ = Focus::Notes;
        demo_play_ = -1;
        switch (demo_) {
            case 0:                                                // Gran Vals, playing
                sel_[kTabTunes] = 0;
                demo_play_ = 0;
                play_tab_ = kTabTunes;
                play_entry_ = 0;
                break;
            case 1:
            case 4:
                open_composer(entries_[1].tune, -1);               // Fur Elise
                cursor_ = 8;
                if (demo_ == 4) {
                    focus_ = Focus::Name;
                    edit_.name = "Fur Elise 2";
                }
                break;
            case 2: tab_ = kTabReceived; sel_[kTabReceived] = 0; break;
            case 3:
                sel_[kTabTunes] = 0;
                open_send(entries_[0].tune);
                send_sel_ = 2;
                break;
        }
    }

    void build(lv_obj_t* body) override {
        list_.reset();
        send_list_.reset();
        if (mode_ == Mode::Composer) build_composer(body);
        else build_library(body);
    }

    bool on_key(const KeyEvent& e) override {
        if (mode_ == Mode::Composer) return composer_key(e);
        if (mode_ == Mode::SendTo) return send_key(e);
        return library_key(e);
    }

    void tick(uint32_t now_ms) override {
        if (pending_close_) {
            pending_close_ = false;
            close_composer();
            return;
        }
        if (demo()) return;
        ring::Player& p = ring::player();
        const bool was = p.playing();
        p.tick();
        if (was && !p.playing()) {
            if (!p.error().empty()) toast(p.error(), 2400);
            play_idx_ = -1;
            ctx_.router.rebuild();
            return;
        }
        if (p.playing() && mode_ == Mode::Composer && play_from_ >= 0) {
            // Light the note being heard (the player's start-up latency aside).
            uint32_t t = p.elapsed_ms(), acc = 0;
            int idx = -1;
            for (int i = play_from_; i < static_cast<int>(edit_.notes.size()); ++i) {
                acc += ring::note_ms(edit_.notes[i], edit_.bpm);
                if (t < acc) { idx = i; break; }
            }
            if (idx != play_idx_) {
                play_idx_ = idx;
                ctx_.router.rebuild();
            }
        }
        if (mode_ == Mode::Library && tab_ == kTabReceived && messenger().revision() != seen_msg_rev_) {
            refresh_received();
            ctx_.router.rebuild();
        }
        (void)now_ms;
    }

private:
    bool demo() const { return demo_ >= 0; }
    bool playing() const { return demo() ? demo_play_ >= 0 : ring::player().playing(); }

    // ── Data ────────────────────────────────────────────────────────────────
    void refresh_entries() {
        entries_.clear();
        for (size_t i = 0; i < ring::builtin().size(); ++i) {
            entries_.push_back({ring::builtin()[i], ring::builtin_list()[i].credit, -1});
        }
        for (size_t i = 0; i < lib_.tunes().size(); ++i) entries_.push_back({lib_.tunes()[i], "", static_cast<int>(i)});
    }

    // Tunes buddies sent: every incoming message (DMs and the channel) that
    // parses as one, newest first.
    void refresh_received() {
        if (demo()) return;
        const Messenger& m = messenger();
        seen_msg_rev_ = m.revision();
        received_.clear();
        auto scan = [&](uint32_t node) {
            for (const Message& msg : m.messages(node)) {
                if (msg.from_me) continue;
                Received rx;
                if (!ring::parse_shared(msg.text, rx.tune)) continue;
                const uint32_t who = Messenger::is_channel(node) && msg.from_node ? msg.from_node : node;
                rx.from = m.name_of(who);
                if (Messenger::is_channel(node)) rx.from += " (channel)";
                rx.ts = msg.timestamp;
                rx.when = ago(msg.timestamp);
                received_.push_back(std::move(rx));
            }
        };
        for (const Contact& ct : m.contacts()) scan(ct.node_id);
        if (m.has_chat(Messenger::kChannelNode)) scan(Messenger::kChannelNode);
        std::stable_sort(received_.begin(), received_.end(),
                         [](const Received& a, const Received& b) { return a.ts > b.ts; });
    }

    // List rows -> entries_ index (tag), headers not selectable.
    const Entry* cur_entry() const {
        if (tab_ != kTabTunes) return nullptr;
        const int i = entry_at(sel_[kTabTunes]);
        return i >= 0 ? &entries_[i] : nullptr;
    }
    const Received* cur_rx() const {
        if (tab_ != kTabReceived) return nullptr;
        const int i = sel_[kTabReceived];
        return i >= 0 && i < static_cast<int>(received_.size()) ? &received_[i] : nullptr;
    }
    const ring::Tune* cur_tune() const {
        if (const Entry* e = cur_entry()) return &e->tune;
        if (const Received* r = cur_rx()) return &r->tune;
        return nullptr;
    }
    // sel_[kTabTunes] counts entries (0..n-1); rows add two headers.
    int entry_at(int i) const { return i >= 0 && i < static_cast<int>(entries_.size()) ? i : -1; }
    int builtin_count() const { return static_cast<int>(ring::builtin().size()); }
    int row_of_entry(int i) const { return i < builtin_count() ? i + 1 : i + 2; }

    // ── Library ─────────────────────────────────────────────────────────────
    void build_library(lv_obj_t* body) {
        const Skin& s = skin();
        const int th = win::tabs(body, 0, 0, {"Ringtones", "Received"}, tab_);
        list_ = std::make_unique<win::ListBox>(body, 0, th, kListW, kClientH - th);
        std::vector<win::Row> rows;
        int select_row = 0;
        if (tab_ == kTabTunes) {
            win::Row h;
            h.text = "Built-in";
            h.header = true;
            h.selectable = false;
            rows.push_back(h);
            for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
                if (i == builtin_count()) {
                    win::Row mh;
                    mh.text = "My tunes";
                    mh.header = true;
                    mh.selectable = false;
                    rows.push_back(mh);
                }
                win::Row r;
                r.text = entries_[i].tune.name;
                r.indent = 6;
                r.right = secs_text(ring::duration_ms(entries_[i].tune)) + " ";
                r.tag = i;
                r.bold = playing() && play_entry_ == i && play_tab_ == kTabTunes;
                rows.push_back(r);
            }
            if (lib_.tunes().empty()) {
                win::Row mh;
                mh.text = "My tunes";
                mh.header = true;
                mh.selectable = false;
                rows.push_back(mh);
                win::Row r;
                r.text = "none yet: NEW makes one";
                r.indent = 6;
                r.dim = true;
                r.selectable = false;
                rows.push_back(r);
            }
            sel_[kTabTunes] = std::clamp(sel_[kTabTunes], 0, std::max(0, static_cast<int>(entries_.size()) - 1));
            select_row = row_of_entry(sel_[kTabTunes]);
        } else {
            for (int i = 0; i < static_cast<int>(received_.size()); ++i) {
                win::Row r;
                r.text = received_[i].tune.name.empty() ? "(untitled)" : received_[i].tune.name;
                r.tag = i;
                r.bold = playing() && play_entry_ == i && play_tab_ == kTabReceived;
                rows.push_back(r);
            }
            if (rows.empty()) {
                win::Row r;
                r.text = "No tunes from buddies yet";
                r.dim = true;
                r.selectable = false;
                rows.push_back(r);
            }
            sel_[kTabReceived] = std::clamp(sel_[kTabReceived], 0, std::max(0, static_cast<int>(received_.size()) - 1));
            select_row = sel_[kTabReceived];
        }
        list_->set_rows(std::move(rows));
        list_->select(select_row);
        list_->set_active(mode_ == Mode::Library);

        if (mode_ == Mode::SendTo) {
            build_send(body);
            return;
        }

        // ── Preview LCD ─────────────────────────────────────────────────────
        lv_obj_t* g = lcd(body, kPaneX, 0, kPaneW, kClientH);
        const int gw = kPaneW - 4;
        const ring::Tune* t = cur_tune();
        if (!t) {
            lcd_text(g, "No tune selected", 5, 40, gw - 10, false, kLcdDim);
            return;
        }
        // Status icons along the top, like a phone's.
        lcd_text(g, tab_ == kTabReceived ? "INBOX" : "TONES", 4, 1, 0, false, kLcdDim);
        lv_obj_t* sig = label(g, playing() ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_AUDIO, gw - 16, 1,
                              &lv_font_montserrat_12, kLcdInk);
        (void)sig;
        solid(g, 3, 14, gw - 6, 1, kLcdDim);
        lcd_text(g, t->name.empty() ? "(untitled)" : t->name, 4, 17, gw - 8, true);
        std::string sub;
        if (const Entry* e = cur_entry()) sub = e->mine >= 0 ? "my tune" : e->credit;
        else if (const Received* r = cur_rx()) sub = "from " + r->from + (r->when.empty() ? "" : ", " + r->when);
        lcd_text(g, sub, 4, 30, gw - 8, false, kLcdDim);
        draw_roll(g, 4, 44, gw - 8, 26, *t, 0, -1, -1, true);
        lcd_text(g, fmt("%d notes  %s  b=%d", static_cast<int>(t->notes.size()),
                        secs_text(ring::duration_ms(*t)).c_str(), t->bpm),
                 4, 72, gw - 8);
        const std::string rt = ring::to_rtttl(*t);
        const size_t colon = rt.find(':', rt.find(':') + 1);
        std::string notes = colon == std::string::npos ? rt : rt.substr(colon + 1);
        if (notes.size() > 64) notes = notes.substr(0, 61) + "...";
        lv_obj_t* tb = text_block(g, notes, 4, 85, gw - 8, font_ui(), kLcdDim);
        lv_obj_set_height(tb, 28);
        (void)s;
    }

    bool library_key(const KeyEvent& e) {
        switch (softkey_slot(e)) {
            case 0: toggle_play(); return true;
            case 1:
                if (tab_ == kTabReceived) save_received();
                else edit_selected();
                return true;
            case 2:
                if (tab_ == kTabReceived) edit_selected();
                else new_tune();
                return true;
            case 3:
                if (const Entry* en = cur_entry()) {
                    open_send(en->tune);
                    ctx_.router.rebuild();
                }
                return true;
            case 4: delete_selected(); return true;
            case kSoftLeft: return false;                  // Router: back
            case kSoftRight: return true;
            default: break;
        }
        if (e.key == Key::Tab || e.key == Key::Left || e.key == Key::Right) {
            tab_ = tab_ == kTabTunes ? kTabReceived : kTabTunes;
            if (tab_ == kTabReceived) refresh_received();
            ctx_.router.rebuild();
            return true;
        }
        if (list_ && list_->on_key(e)) {
            const win::Row* r = list_->selected_row();
            sel_[tab_] = r ? r->tag : 0;
            ctx_.router.rebuild();                         // the preview follows
            return true;
        }
        if (e.key == Key::Up || e.key == Key::Down) return true;
        if (e.key == Key::A) {
            toggle_play();
            return true;
        }
        if (e.key == Key::Char) {
            switch (e.ch) {
                case 'e': edit_selected(); return true;
                case 'n': if (tab_ == kTabTunes) new_tune(); return true;
                default: break;
            }
        }
        return false;
    }

    void toggle_play() {
        const ring::Tune* t = cur_tune();
        if (demo()) {
            demo_play_ = (demo_play_ >= 0 || !t) ? -1 : 0;
            ctx_.router.rebuild();
            return;
        }
        ring::Player& p = ring::player();
        if (p.playing()) {
            p.stop();
        } else if (t) {
            if (!p.play(*t)) toast(p.error(), 2400);
            play_tab_ = tab_;
            play_entry_ = sel_[tab_];
            play_from_ = -1;
        }
        ctx_.router.rebuild();
    }

    void edit_selected() {
        if (const Entry* e = cur_entry()) open_composer(e->tune, e->mine);
        else if (const Received* r = cur_rx()) open_composer(r->tune, -1);
        else return;
        ctx_.router.rebuild();
    }

    void new_tune() {
        ring::Tune t;
        t.bpm = 120;
        int n = 1;
        auto taken = [&](const std::string& name) {
            for (const ring::Tune& x : lib_.tunes()) if (x.name == name) return true;
            return false;
        };
        do t.name = fmt("My Tune %d", n++); while (taken(t.name));
        open_composer(t, -1);
        dirty_ = true;
        ctx_.router.rebuild();
    }

    void save_received() {
        const Received* r = cur_rx();
        if (!r) return;
        if (!demo()) {
            lib_.put(r->tune);
            if (!lib_.save() && store::enabled()) toast("Could not save ringtones.json", 2400);
            else toast("Saved to My tunes: " + r->tune.name, 1800);
            refresh_entries();
        }
        ctx_.router.rebuild();
    }

    void delete_selected() {
        const Entry* e = cur_entry();
        if (!e || e->mine < 0) return;
        const int idx = e->mine;
        const std::string name = e->tune.name;
        ctx_.router.show_modal(std::make_unique<Modal>(
            "Delete tune?", "Delete \"" + name + "\" from My tunes?", std::vector<std::string>{"Delete", "Keep"},
            [this, idx](int choice) {
                if (choice != 0 || idx >= static_cast<int>(lib_.tunes().size())) return;
                lib_.tunes().erase(lib_.tunes().begin() + idx);
                if (!demo()) lib_.save();
                refresh_entries();
                sel_[kTabTunes] = std::max(0, sel_[kTabTunes] - 1);
                ctx_.router.rebuild();
            }));
    }

    // ── Send to ─────────────────────────────────────────────────────────────
    struct Target {
        uint32_t node = 0;
        std::string name;
        std::string right;
    };

    void open_send(const ring::Tune& t) {
        send_tune_ = t;
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
        mode_ = Mode::SendTo;
    }

    void build_send(lv_obj_t* body) {
        const Skin& s = skin();
        lv_obj_t* cardo = win::card(body, kPaneX, 0, kPaneW, 30);
        win::text(cardo, "Send \"" + send_tune_.name + "\"", 4, 2, s.card_text, kPaneW - 8, true);
        win::text(cardo, fmt("as 1 message, %d bytes", static_cast<int>(ring::share_text(send_tune_).size())), 4,
                  15, s.card_text, kPaneW - 8);
        send_list_ = std::make_unique<win::ListBox>(body, kPaneX, 33, kPaneW, kClientH - 33);
        std::vector<win::Row> rows;
        for (const Target& t : targets_) {
            win::Row r;
            r.text = t.name;
            r.right = t.right;
            r.dim = t.right == "off ";
            rows.push_back(r);
        }
        send_list_->set_rows(std::move(rows));
        send_list_->select(send_sel_);
    }

    bool send_key(const KeyEvent& e) {
        const int slot = softkey_slot(e);
        if (slot == 4 || slot == kSoftLeft || e.key == Key::B) {
            mode_ = Mode::Library;
            ctx_.router.rebuild();
            return true;
        }
        if (slot == 0 || e.key == Key::A) {
            send_now();
            return true;
        }
        if (send_list_ && send_list_->on_key(e)) {
            send_sel_ = send_list_->selected();
            return true;
        }
        return true;                                       // modal-ish: swallow the rest
    }

    void send_now() {
        if (send_sel_ < 0 || send_sel_ >= static_cast<int>(targets_.size())) return;
        const Target& t = targets_[send_sel_];
        mode_ = Mode::Library;
        if (demo()) {                                      // --shots: never touches the radio
            ctx_.router.rebuild();
            return;
        }
        const std::string text = ring::share_text(send_tune_);
        if (text.size() > ring::kMaxShareBytes || !messenger().send(t.node, text)) {
            toast("Too long for one message", 2400);
        } else {
            toast("Sent \"" + send_tune_.name + "\" to " + t.name, 2000);
        }
        ctx_.router.rebuild();
    }

    // ── Composer ────────────────────────────────────────────────────────────
    void open_composer(const ring::Tune& t, int mine) {
        if (!demo()) ring::player().stop();
        edit_ = t;
        edit_mine_ = mine;
        cursor_ = 0;
        roll_first_ = tok_first_ = 0;
        focus_ = Focus::Notes;
        dirty_ = false;
        play_idx_ = -1;
        mode_ = Mode::Composer;
    }

    void close_composer() {
        if (!demo()) ring::player().stop();
        mode_ = Mode::Library;
        play_idx_ = -1;
        ctx_.router.rebuild();
    }

    const ring::Note* cur_note() const {
        return cursor_ >= 0 && cursor_ < static_cast<int>(edit_.notes.size()) ? &edit_.notes[cursor_] : nullptr;
    }

    void build_composer(lv_obj_t* body) {
        const Skin& s = skin();
        constexpr int kLcdH = 82;
        lv_obj_t* g = lcd(body, 0, 0, kClientW, kLcdH);
        const int gw = kClientW - 4;

        // Line 1: name (editable) and tempo.
        const std::string name = edit_.name.empty() ? "(untitled)" : edit_.name;
        if (focus_ == Focus::Name) {
            const std::string shown = edit_.name + "_";
            solid(g, 2, 1, text_w(shown, font_ui_bold()) + 6, 13, kLcdInk);
            lcd_text(g, shown, 5, 1, 0, true, kLcdTop);
        } else {
            lcd_text(g, name, 4, 1, 170, true);
        }
        const std::string tempo = fmt("b=%d", edit_.bpm);
        const int tw = text_w(tempo, font_ui()) + 6;
        const int tx = gw - tw - 2;
        if (focus_ == Focus::Tempo) {
            solid(g, tx, 1, tw, 13, kLcdInk);
            lcd_text(g, tempo, tx + 3, 1, 0, false, kLcdTop);
        } else {
            lcd_text(g, tempo, tx + 3, 1);
        }
        lcd_text(g, secs_text(ring::duration_ms(edit_)), tx - 36, 1, 34, false, kLcdDim);
        solid(g, 2, 15, gw - 4, 1, kLcdDim);

        // Piano roll, scrolled to keep the cursor in view.
        const int roll_w = gw - 8;
        const int n = static_cast<int>(edit_.notes.size());
        roll_first_ = std::clamp(roll_first_, 0, std::max(0, n - 1));
        if (cursor_ < roll_first_) roll_first_ = cursor_;
        auto span_px = [&](int a, int b) {             // notes a..b inclusive
            int w = 0;
            for (int i = a; i <= b && i < n; ++i) w += note_px(edit_.notes[i]) + 1;
            return w;
        };
        while (roll_first_ < cursor_ && span_px(roll_first_, cursor_) > roll_w - 20) ++roll_first_;
        draw_roll(g, 4, 18, roll_w, 34, edit_, roll_first_, cursor_, play_idx_, false);

        // Note strip: the RTTTL tokens, cursor inverted.
        const lv_font_t* f = font_ui();
        auto tok = [&](int i) { return ring::note_text(edit_.notes[i]); };
        const int strip_w = gw - 8;
        tok_first_ = std::clamp(tok_first_, 0, std::max(0, n - 1));
        if (cursor_ < tok_first_) tok_first_ = cursor_;
        auto tok_span = [&](int a, int b) {
            int w = 0;
            for (int i = a; i <= b && i < n; ++i) w += text_w(tok(i), f) + 7;
            return w;
        };
        while (tok_first_ < cursor_ && tok_span(tok_first_, cursor_) > strip_w - 30) ++tok_first_;
        lv_obj_t* strip = box(g, 4, 55, strip_w, 13);
        int x = 0;
        for (int i = tok_first_; i < n && x < strip_w; ++i) {
            const std::string t = tok(i);
            const int w = text_w(t, f) + 4;
            if (i == cursor_) {
                solid(strip, x, 0, w, 13, kLcdInk);
                lcd_text(strip, t, x + 2, 0, 0, false, kLcdTop);
            } else {
                lcd_text(strip, t, x + 2, 0, 0, false, i == play_idx_ ? kLcdInk : kLcdInk);
            }
            x += w + 3;
        }
        if (n == 0) lcd_text(strip, "a b c d e f g   p = rest", 2, 0, 0, false, kLcdDim);

        // Status line.
        const int bytes = static_cast<int>(ring::share_text(edit_).size());
        lcd_text(g, n ? fmt("%d/%d", cursor_ + 1, n) : "0/0", 4, 67, 60, false, kLcdDim);
        const std::string st = playing() ? std::string(LV_SYMBOL_VOLUME_MAX) : std::string();
        if (!st.empty()) label(g, st, gw / 2 - 6, 66, &lv_font_montserrat_12, kLcdInk);
        const std::string fits = fmt("%d/%d bytes", bytes, static_cast<int>(ring::kMaxShareBytes));
        lcd_text(g, fits, gw - 4 - text_w(fits, font_ui()), 67, 0, false, bytes > 180 ? kLcdInk : kLcdDim);

        // Under the LCD: the cursor note and the packet meter, skinned.
        lv_obj_t* nf = win::field(body, 0, 86, 176, 15);
        const ring::Note* cn = cur_note();
        win::text(nf, cn ? "Note: " + ring::note_label(*cn) : "Note: -", 3, 1, s.field_text, 168);
        win::text(body, "Packet", 182, 87);
        win::meter(body, 222, 89, kClientW - 222, 9, bytes, static_cast<int>(ring::kMaxShareBytes), s.sel);
        win::text(body, "s:sharp  -/=:octave  h:dot  Up/Dn:pitch  Del:erase  Esc:done", 1, 106, s.dim,
                  kClientW - 2);
    }

    // Changing a note: hear it (not in demo states).
    void preview(const ring::Note& n) {
        if (demo() || n.rest()) return;
        ring::Tune t;
        t.bpm = edit_.bpm;
        t.notes.push_back(n);
        t.notes.back().dur = std::max<uint8_t>(n.dur, 8);     // a short blip at most an eighth
        t.notes.back().dotted = false;
        play_from_ = -1;
        ring::player().play(t);
    }

    // Keeps the tune within one packet: undoes `edit` (a copy) if it no longer fits.
    bool commit(const ring::Tune& before) {
        if (!ring::fits_share(edit_)) {
            edit_ = before;
            toast("Full: a tune must fit one message", 1800);
            return false;
        }
        dirty_ = true;
        return true;
    }

    void insert_note(int pitch) {
        const ring::Tune before = edit_;
        ring::Note n;
        if (const ring::Note* c = cur_note()) {
            n.octave = c->rest() ? last_oct_ : c->octave;
            n.dur = c->dur;
        } else {
            n.octave = static_cast<int8_t>(last_oct_);
            n.dur = static_cast<uint8_t>(last_dur_);
        }
        n.pitch = static_cast<int8_t>(pitch);
        if (pitch < 0) n.octave = static_cast<int8_t>(last_oct_);
        const int at = edit_.notes.empty() ? 0 : cursor_ + 1;
        edit_.notes.insert(edit_.notes.begin() + at, n);
        cursor_ = at;
        if (commit(before)) {
            if (!n.rest()) last_oct_ = n.octave;
            preview(n);
        } else {
            cursor_ = std::min(cursor_, static_cast<int>(edit_.notes.size()) - 1);
        }
    }

    void change(const std::function<void(ring::Note&)>& fn) {
        if (!cur_note()) return;
        const ring::Tune before = edit_;
        fn(edit_.notes[cursor_]);
        ring::Note& n = edit_.notes[cursor_];
        n.octave = static_cast<int8_t>(std::clamp<int>(n.octave, ring::kMinOctave, ring::kMaxOctave));
        if (commit(before)) {
            const ring::Note& now = edit_.notes[cursor_];
            if (!now.rest()) last_oct_ = now.octave;
            last_dur_ = now.dur;
            preview(now);
        }
    }

    static void semitone(ring::Note& n, int d) {
        if (n.rest()) return;
        int m = n.octave * 12 + n.pitch + d;
        m = std::clamp(m, ring::kMinOctave * 12, ring::kMaxOctave * 12 + 11);
        n.octave = static_cast<int8_t>(m / 12);
        n.pitch = static_cast<int8_t>(m % 12);
    }

    static void step_dur(ring::Note& n, bool longer) {
        static const int kD[6] = {32, 16, 8, 4, 2, 1};   // short -> long
        int i = 0;
        while (i < 5 && kD[i] != n.dur) ++i;
        i = std::clamp(i + (longer ? 1 : -1), 0, 5);
        n.dur = static_cast<uint8_t>(kD[i]);
    }

    void play_from(int from) {
        if (demo()) return;
        ring::Player& p = ring::player();
        if (p.playing()) {
            p.stop();
            play_idx_ = -1;
            return;
        }
        ring::Tune t = edit_;
        from = std::clamp(from, 0, std::max(0, static_cast<int>(t.notes.size()) - 1));
        t.notes.erase(t.notes.begin(), t.notes.begin() + std::min<size_t>(from, t.notes.size()));
        if (!p.play(t)) {
            toast(p.error(), 2400);
            return;
        }
        play_from_ = from;
    }

    void save_tune() {
        edit_.name = ring::clean_name(edit_.name);
        if (edit_.name.empty()) edit_.name = "Untitled";
        if (edit_.notes.empty()) {
            toast("Nothing to save yet", 1600);
            return;
        }
        if (edit_mine_ >= 0 && edit_mine_ < static_cast<int>(lib_.tunes().size())) lib_.tunes()[edit_mine_] = edit_;
        else edit_mine_ = lib_.put(edit_);
        dirty_ = false;
        refresh_entries();
        sel_[kTabTunes] = builtin_count() + edit_mine_;
        tab_ = kTabTunes;
        if (demo()) return;
        if (!lib_.save() && store::enabled()) toast("Could not write ringtones.json", 2400);
        else toast("Saved \"" + edit_.name + "\"", 1400);
    }

    void leave_composer() {
        if (!dirty_ || demo()) {
            close_composer();
            return;
        }
        ctx_.router.show_modal(std::make_unique<Modal>(
            "Save tune?", "Save \"" + edit_.name + "\" to My tunes?",
            std::vector<std::string>{"Save", "Discard", "Keep editing"}, [this](int choice) {
                if (choice == 0) save_tune();
                if (choice == 0 || choice == 1) pending_close_ = true;   // after the modal is gone
            }));
    }

    bool composer_key(const KeyEvent& e) {
        const bool is_esc = e.key == Key::B && e.raw != LV_KEY_BACKSPACE;
        const bool is_bksp = e.raw == LV_KEY_BACKSPACE;

        if (focus_ == Focus::Name) {
            if (e.key == Key::A || e.key == Key::Tab || is_esc) {
                edit_.name = ring::clean_name(edit_.name);
                focus_ = e.key == Key::Tab ? Focus::Tempo : Focus::Notes;
            } else if (is_bksp) {
                if (!edit_.name.empty()) {
                    edit_.name.pop_back();
                    while (!edit_.name.empty() && (static_cast<uint8_t>(edit_.name.back()) & 0xc0) == 0x80) edit_.name.pop_back();
                    dirty_ = true;
                }
            } else if (e.raw >= 32 && e.raw < 127 && e.raw != ':' && e.raw != ',' &&
                       edit_.name.size() < ring::kMaxNameBytes) {
                const ring::Tune before = edit_;
                edit_.name += static_cast<char>(e.raw);
                commit(before);
            }
            ctx_.router.rebuild();
            return true;
        }

        if (focus_ == Focus::Tempo) {
            int d = 0;
            if (e.key == Key::Up) d = 1;
            else if (e.key == Key::Down) d = -1;
            else if (e.key == Key::Right) d = 10;
            else if (e.key == Key::Left) d = -10;
            if (d) {
                const ring::Tune before = edit_;
                edit_.bpm = std::clamp(edit_.bpm + d, 40, 300);
                commit(before);
            } else if (e.key == Key::Tab || e.key == Key::A || is_esc) {
                focus_ = Focus::Notes;
            } else if (e.key == Key::Char && e.raw == ' ') {
                play_from(0);
            }
            ctx_.router.rebuild();
            return true;
        }

        const int n = static_cast<int>(edit_.notes.size());
        bool handled = true;
        if (is_esc) {
            leave_composer();
            return true;
        } else if (is_bksp || (e.key == Key::Char && e.ch == 'x')) {
            if (n > 0) {
                edit_.notes.erase(edit_.notes.begin() + cursor_);
                cursor_ = std::clamp(cursor_ - (is_bksp ? 1 : 0), 0, std::max(0, n - 2));
                dirty_ = true;
            }
        } else if (e.key == Key::Tab) {
            focus_ = Focus::Name;
        } else if (e.key == Key::Left) {
            cursor_ = std::max(0, cursor_ - 1);
        } else if (e.key == Key::Right) {
            cursor_ = std::min(std::max(0, n - 1), cursor_ + 1);
        } else if (e.key == Key::Up || e.key == Key::Down) {
            change([&](ring::Note& nt) { semitone(nt, e.key == Key::Up ? 1 : -1); });
        } else if (e.key == Key::A) {
            play_from(e.raw == ' ' ? 0 : cursor_);          // Space: from the top, Enter: from the cursor
        } else if (e.key == Key::Char) {
            const char ch = static_cast<char>(e.ch);
            static const int kPitch[7] = {9, 11, 0, 2, 4, 5, 7};   // a..g
            if (ch >= 'a' && ch <= 'g') insert_note(kPitch[ch - 'a']);
            else if (ch == 'p' || ch == 'r') insert_note(-1);
            else if (ch == 's' || e.raw == '#') {
                change([](ring::Note& nt) {
                    if (nt.rest()) return;
                    static const bool kSharp[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
                    if (kSharp[nt.pitch]) semitone(nt, -1);   // C# -> C
                    else if (nt.pitch != 4 && nt.pitch != 11) semitone(nt, 1);   // no E# / B#
                });
            }
            else if (ch == '-' || ch == '_') change([](ring::Note& nt) { if (!nt.rest()) nt.octave--; });
            else if (ch == '=' || ch == '+') change([](ring::Note& nt) { if (!nt.rest()) nt.octave++; });
            else if (ch == 'h' || ch == '*') change([](ring::Note& nt) { nt.dotted = !nt.dotted; });
            else if (ch == '[') change([](ring::Note& nt) { step_dur(nt, false); });
            else if (ch == ']') change([](ring::Note& nt) { step_dur(nt, true); });
            else if (ch == '1' || ch == '2' || ch == '4' || ch == '8' || ch == '6' || ch == '3') {
                const int d = ch == '6' ? 16 : ch == '3' ? 32 : ch - '0';
                last_dur_ = d;
                change([d](ring::Note& nt) { nt.dur = static_cast<uint8_t>(d); });
            } else if (ch == 'w') {
                save_tune();
            } else if (ch == 't') {
                focus_ = Focus::Tempo;
            } else if (ch == 'n') {
                focus_ = Focus::Name;
            } else {
                handled = false;
            }
        } else {
            handled = false;
        }
        if (handled) ctx_.router.rebuild();
        return handled || true;                         // the composer owns every key
    }

    // ── State ───────────────────────────────────────────────────────────────
    int demo_ = -1;
    int demo_play_ = -1;
    Mode mode_ = Mode::Library;
    int tab_ = kTabTunes;
    int sel_[2] = {0, 0};
    ring::Library lib_;
    std::vector<Entry> entries_;
    std::vector<Received> received_;
    uint32_t seen_msg_rev_ = 0;

    int play_tab_ = -1, play_entry_ = -1;
    int play_from_ = -1;         // composer playback: first note played (-1: not the composer)
    int play_idx_ = -1;          // note being heard

    ring::Tune edit_;
    int edit_mine_ = -1;         // index in lib_.tunes() being edited; -1: a new tune
    int cursor_ = 0;
    int roll_first_ = 0, tok_first_ = 0;
    Focus focus_ = Focus::Notes;
    bool dirty_ = false;
    bool pending_close_ = false;
    int last_oct_ = 5, last_dur_ = 4;

    ring::Tune send_tune_;
    std::vector<Target> targets_;
    int send_sel_ = 0;

    std::unique_ptr<win::ListBox> list_;
    std::unique_ptr<win::ListBox> send_list_;
};

} // namespace

std::unique_ptr<Screen> make_ring_screen(Ctx& ctx) {
    return std::make_unique<RingScreen>(ctx);
}

} // namespace mm::ui
