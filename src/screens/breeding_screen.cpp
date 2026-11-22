// breeding_screen.cpp — Bill's PC box + the timed breeder rooms (nursery).
//
// Views (mode_):
//   Box     5-column sprite grid of the BreedingApp roster (3 rows visible,
//           scrolls by row) + an info card for the selected mon. Mons in a
//           room are dimmed with an "R#" chip; mons on the 7-day breed
//           cooldown show the days left. S cycles the sort (box / dex / skin /
//           level). P picks parent 1, then P on a second mon opens the pair
//           preview. Tab opens the rooms.
//   Detail  big sprite, genotype, notes and an action list (pick / pair with
//           parent 1 / favourite / release behind a confirm Modal).
//   Pair    both parents side by side + exact Mendelian offspring odds
//           (per-locus d/2 gamete rule from BreedingGenetics.h). Enter places
//           the pair in the first free breeder room (GameState::place_pair).
//   Rooms   the 3 breeder rooms: empty / pair incubating ("Egg in 42m") /
//           egg ("Hatches in 1h 12m"). Enter on a busy room cancels it
//           behind a confirm Modal.
//   Result  a hatch from GameState::pop_hatch(): the new child (already added
//           to the box and saved by GameState) or the failed (hh) egg.
//
// There is no instant breeding: rooms run on the wall clock in GameState
// (egg 1 h after placing, hatch 2 h later), shared with mmterm via
// breeder_rooms.dat. Demo states use private copies + a fixed clock and never
// touch GameState or disk.
#include "screens.h"

#include "game_state.h"
#include "mm_sprites.h"
#include "router.h"
#include "widgets.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <optional>

namespace mm::ui {
namespace {

using breeding::BreedMon;
using breeding::BreederManager;
using breeding::BreederRoom;

// ── Layout ──────────────────────────────────────────────────────────────────
constexpr int kCols = 5;
constexpr int kCell = 38;
constexpr int kGap = 2;
constexpr int kGridX = 4;
constexpr int kGridY = 16;
constexpr int kVisRows = 3;
constexpr int kInfoX = kGridX + kCols * kCell + (kCols - 1) * kGap + 4;   // 206
constexpr int kInfoW = kBodyW - kInfoX - 4;                               // 110

constexpr uint32_t kFemale = 0xf890c8;
constexpr uint32_t kMale = 0x58a8f8;

constexpr int kRoomW = 102;    // 3 room cards: x 4 / 109 / 214
constexpr int kRoomH = 130;
constexpr uint32_t kEggShell = 0xf2ead2;
constexpr uint32_t kEggSpot = 0x60b858;

// Fixed clock for the demo states (2026-01-01 00:00 UTC).
constexpr long kDemoNow = 1767225600L;

enum class Mode { Box, Detail, Pair, Rooms, Result };
enum class Sort { Box, Dex, Skin, Level, Count };
const char* sort_name(Sort s) {
    switch (s) {
        case Sort::Dex: return "Dex";
        case Sort::Skin: return "Skin";
        case Sort::Level: return "Level";
        default: return "Box";
    }
}

// ── Helpers ─────────────────────────────────────────────────────────────────
int skin_of(const BreedMon& m) { return static_cast<int>(breeding::skinOf(m.geno)); }
bool tritan_of(const BreedMon& m) { return breeding::isTritan(m.geno); }

std::string name_of(const BreedMon& m) {
    if (m.nick[0]) return std::string(m.nick, strnlen(m.nick, sizeof(m.nick)));
    return species_name(m.dex);
}

// Engine messages contain UTF-8 dashes / symbols; the Montserrat fonts only
// have ASCII, so fold them down.
std::string ascii(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(s[i]);
        if (ch < 0x80) {
            out += static_cast<char>(ch);
            continue;
        }
        if (ch == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80) {
            out += '-';   // en/em dash, etc.
            i += 2;
            continue;
        }
        while (i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) ++i;
    }
    return out;
}

// Short human notes about the genes worth knowing (hidden carriers, defects).
struct Note {
    std::string text;
    uint32_t col;
};
std::vector<Note> gene_notes(const BreedMon& m) {
    const auto& g = m.geno;
    std::vector<Note> n;
    if (breeding::isSterile(g)) n.push_back({"STERILE (bb)", color::kBad});
    if (breeding::cantFight(g)) n.push_back({"Can't fight (ff)", color::kBad});
    if (breeding::neverHatches(g)) n.push_back({"No-hatch (hh)", color::kBad});
    if (!g.female && g.rainbow == 2) n.push_back({"Hidden rainbow (rr)", kFemale});
    if (!g.female && g.rainbow == 1) n.push_back({"Hidden pink (Rr)", kFemale});
    if (g.rainbow > 0 && g.shiny == 2) n.push_back({"Masked shiny (ss)", color::kAccent});
    if (g.shiny == 1) n.push_back({"Shiny carrier (Ss)", color::kAccent});
    if (g.tritan >= 1) n.push_back({g.tritan == 2 ? "Tritan (TT)" : "Tritan (Tn)", color::kInfo});
    if (g.sterile == 1) n.push_back({"Sterile carrier (Bb)", color::kWarn});
    if (g.cantFight == 1) n.push_back({"Can't-fight carrier", color::kWarn});
    if (g.noHatch == 1) n.push_back({"No-hatch carrier (Hh)", color::kWarn});
    if (n.empty() && breeding::isCleanStock(g)) n.push_back({"Clean stock", color::kGood});
    return n;
}

std::string geno_cosmetic(const breeding::Genotype& g) {
    static const char* R[3] = {"RR", "Rr", "rr"};
    static const char* S[3] = {"SS", "Ss", "ss"};
    static const char* D[3] = {"dd", "Dd", "DD"};
    auto ix = [](uint8_t d) { return d > 2 ? 2 : d; };
    return fmt("%s %s %s", R[ix(g.rainbow)], S[ix(g.shiny)], D[ix(g.dark)]);
}
std::string geno_health(const breeding::Genotype& g) {
    static const char* B[3] = {"BB", "Bb", "bb"};
    static const char* F[3] = {"FF", "Ff", "ff"};
    static const char* H[3] = {"HH", "Hh", "hh"};
    static const char* T[3] = {"", " Tn", " TT"};
    auto ix = [](uint8_t d) { return d > 2 ? 2 : d; };
    return fmt("%s %s %s%s", B[ix(g.sterile)], F[ix(g.cantFight)], H[ix(g.noHatch)],
               T[ix(g.tritan)]);
}

// Exact offspring distribution: each parent passes the rare allele with
// probability dosage/2 (BreedingGenetics gamete()), loci independent, sex 50/50.
struct Odds {
    double skin[12] = {};
    double sterile = 0, cant = 0, nohatch = 0, tritan = 0, hidden = 0;
};
Odds child_odds(const breeding::Genotype& a, const breeding::Genotype& b) {
    auto locus = [](uint8_t da, uint8_t db, double out[3]) {
        const double pa = da / 2.0, pb = db / 2.0;
        out[0] = (1 - pa) * (1 - pb);
        out[2] = pa * pb;
        out[1] = 1.0 - out[0] - out[2];
    };
    double R[3], S[3], D[3], B[3], F[3], H[3], T[3];
    locus(a.rainbow, b.rainbow, R);
    locus(a.shiny, b.shiny, S);
    locus(a.dark, b.dark, D);
    locus(a.sterile, b.sterile, B);
    locus(a.cantFight, b.cantFight, F);
    locus(a.noHatch, b.noHatch, H);
    locus(a.tritan, b.tritan, T);
    Odds o;
    for (int rb = 0; rb < 3; ++rb)
        for (int sh = 0; sh < 3; ++sh)
            for (int dk = 0; dk < 3; ++dk)
                for (int fem = 0; fem < 2; ++fem) {
                    breeding::Genotype g{};
                    g.rainbow = static_cast<uint8_t>(rb);
                    g.shiny = static_cast<uint8_t>(sh);
                    g.dark = static_cast<uint8_t>(dk);
                    g.female = static_cast<uint8_t>(fem);
                    const double p = R[rb] * S[sh] * D[dk] * 0.5;
                    o.skin[static_cast<int>(breeding::skinOf(g))] += p;
                    if (!fem && rb > 0) o.hidden += p;
                }
    o.sterile = B[2];
    o.cant = F[2];
    o.nohatch = H[2];
    o.tritan = T[1] + T[2];
    return o;
}

int pct(double p) { return static_cast<int>(p * 100.0 + 0.5); }

// A coloured square used as a skin marker.
lv_obj_t* dot(lv_obj_t* parent, int x, int y, int sz, lv_color_t col) {
    lv_obj_t* d = box(parent, x, y, sz, sz);
    lv_obj_set_style_bg_color(d, col, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d, 2, 0);
    return d;
}

// "1h 12m" / "42m" / "3d 5h" / "<1m" for a positive number of seconds.
std::string dur(long s) {
    if (s < 60) return "<1m";
    const long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
    if (d > 0) return h ? fmt("%ldd %ldh", d, h) : fmt("%ldd", d);
    if (h > 0) return m ? fmt("%ldh %ldm", h, m) : fmt("%ldh", h);
    return fmt("%ldm", m);
}

// Short cooldown tag for a 38 px box cell: "6d", "5h", "9m".
std::string dur_tag(long s) {
    if (s >= 86400) return fmt("%ldd", (s + 86399) / 86400);
    if (s >= 3600) return fmt("%ldh", (s + 3599) / 3600);
    return fmt("%ldm", std::max(1L, (s + 59) / 60));
}

// Horizontally centred single-line label across [0, w).
lv_obj_t* centered(lv_obj_t* parent, const std::string& text, int y, int w, const lv_font_t* font,
                   uint32_t col) {
    lv_obj_t* l = label(parent, text, 0, y, font, col, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

// A plain egg: a narrow top circle over a wider bottom one + three spots.
void egg(lv_obj_t* parent, int x, int y) {
    auto blob = [parent](int bx, int by, int w, int h, uint32_t col) {
        lv_obj_t* o = box(parent, bx, by, w, h);
        lv_obj_set_style_bg_color(o, c(col), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        return o;
    };
    blob(x + 4, y, 24, 30, kEggShell);
    blob(x, y + 10, 32, 30, kEggShell);
    blob(x + 7, y + 9, 8, 8, kEggSpot);
    blob(x + 19, y + 18, 7, 7, kEggSpot);
    blob(x + 8, y + 27, 6, 6, kEggSpot);
}

lv_obj_t* gender_label(lv_obj_t* parent, const BreedMon& m, int x, int y) {
    return label(parent, m.geno.female ? "F" : "M", x, y, font_small(),
                 m.geno.female ? kFemale : kMale);
}

// ── Screen ──────────────────────────────────────────────────────────────────
class BreedingScreen final : public Screen {
public:
    explicit BreedingScreen(Ctx& ctx) : Screen(ctx) { nav_.cols = kCols; }

    const char* title() const override { return mode_ == Mode::Rooms ? "Breeder Rooms" : "Breeding"; }

    std::string hints() const override {
        switch (mode_) {
            case Mode::Detail: return "Up/Dn:action  Enter:do  L/R:prev/next  Esc:box";
            case Mode::Pair: return "Enter:place in room  Esc:back";
            case Mode::Rooms: return "L/R:room  Enter:cancel pair  Tab/Esc:box";
            case Mode::Result: return "Enter:continue";
            default:
                return pick_id_ ? "Enter:info  P:pick mate  S:sort  Tab:rooms  Esc:unpick"
                                : "Enter:info  P:pick  S:sort  Tab:rooms  Esc:back";
        }
    }

    void build(lv_obj_t* body) override {
        body_ = body;
        anim_.clear();
        cells_.clear();
        info_ = nullptr;
        acts_.clear();
        room_cards_.clear();
        auto& ap = app();
        if (pick_id_ && !ap.findById(pick_id_)) pick_id_ = 0;
        rev_ = ctx_.game.revision();
        last_min_ = now() / 60;
        switch (mode_) {
            case Mode::Detail:
                if (ap.findById(detail_id_)) { build_detail(); break; }
                mode_ = Mode::Box;
                build_box();
                break;
            case Mode::Pair:
                if (ap.findById(pair_a_) && ap.findById(pair_b_)) { build_pair(); break; }
                mode_ = Mode::Box;
                build_box();
                break;
            case Mode::Rooms: build_rooms(); break;
            case Mode::Result: build_result(); break;
            default: build_box(); break;
        }
    }

    // Soft keys (keys 4..8 under the display); key 3 is back / unpick.
    SoftKeys softkeys() const override {
        SoftKeys k;
        k.left.text = MM_SOFT_BACK;
        switch (mode_) {
            case Mode::Detail: {
                const BreedMon* m = mon(detail_id_);
                if (!m) break;
                const auto acts = actions(*m);
                for (size_t i = 0; i < acts.size() && i < 3; ++i) k.set(static_cast<int>(i), act_key(acts[i], *m));
                k.set(3, "PREV").set(4, "NEXT");
                k.selected = act_sel_;
                break;
            }
            case Mode::Pair: k.set(0, "PLACE"); break;
            case Mode::Rooms: {
                const int n = mgr().roomCount();
                if (n <= 3) {
                    for (int i = 0; i < n; ++i) k.set(i, fmt("ROOM %d", i + 1));
                    k.selected = room_sel_;
                } else {
                    k.set(0, "PREV").set(1, "NEXT");
                }
                const bool busy = room_sel_ < n && mgr().room(room_sel_).state != breeding::ROOM_EMPTY;
                k.set(3, "CANCEL", busy).set(4, "BOX");
                break;
            }
            case Mode::Result: k.set(0, "OK"); k.left.text.clear(); break;
            default:
                k.set(0, "INFO").set(1, pick_id_ ? "MATE" : "PICK").set(2, "SORT").set(3, "ROOMS");
                break;
        }
        return k;
    }

    // Turns a soft key into the key that already does that job. Returns false
    // (with `out` untouched) when the key is not a soft key.
    bool soft_to_key(const KeyEvent& in, KeyEvent& out) const {
        const int slot = softkey_slot(in);
        if (slot == kSoftNone) return false;
        auto key = [&](Key k) { out.key = k; return true; };
        auto ch = [&](char c) { out.key = Key::Char; out.ch = static_cast<uint32_t>(c); return true; };
        if (slot == kSoftLeft) return mode_ == Mode::Result ? key(Key::None) : key(Key::B);
        switch (mode_) {
            case Mode::Detail:
                if (slot == 3) return key(Key::Left);
                if (slot == 4) return key(Key::Right);
                return key(Key::None);   // 0..2 handled in key_detail
            case Mode::Pair: return key(slot == 0 ? Key::A : Key::None);
            case Mode::Rooms:
                if (slot == 3) return key(Key::A);
                if (slot == 4) return key(Key::B);
                if (mgr().roomCount() > 3) {
                    if (slot == 0) return key(Key::Left);
                    if (slot == 1) return key(Key::Right);
                }
                return key(Key::None);   // room pick handled in key_rooms
            case Mode::Result: return key(slot == 0 ? Key::A : Key::None);
            default:
                switch (slot) {
                    case 0: return key(Key::A);
                    case 1: return ch('p');
                    case 2: return ch('s');
                    case 3: return key(Key::Tab);
                    default: return key(Key::None);
                }
        }
    }

    bool on_key(const KeyEvent& in) override {
        KeyEvent e = in;
        const int slot = softkey_slot(in);
        if (soft_to_key(in, e)) {
            if (mode_ == Mode::Detail && slot >= 0 && slot < 3) return pick_action(slot);
            if (mode_ == Mode::Rooms && slot >= 0 && slot < 3 && mgr().roomCount() <= 3) {
                if (slot < mgr().roomCount()) { room_sel_ = slot; paint_rooms(); }
                return true;
            }
            if (e.key == Key::None) return true;
            // Key 3 in the plain box (nothing picked): let the Router go back.
            if (slot == kSoftLeft && mode_ == Mode::Box && !pick_id_) return false;
        }
        switch (mode_) {
            case Mode::Detail: return key_detail(e);
            case Mode::Pair: return key_pair(e);
            case Mode::Rooms: return key_rooms(e);
            case Mode::Result: return key_result(e);
            default: return key_box(e);
        }
    }

    void tick(uint32_t now_ms) override {
        const bool browsing = mode_ == Mode::Box || mode_ == Mode::Rooms;
        // A room hatched (GameState::tick_rooms): show it in the result view.
        if (!demo_ && browsing) {
            if (auto h = ctx_.game.pop_hatch()) {
                show_hatch(*h);
                return;
            }
        }
        if (browsing && ctx_.game.revision() != rev_) {
            ctx_.router.rebuild();
            return;
        }
        // Countdowns are shown to the minute.
        if (browsing && now() / 60 != last_min_) {
            last_min_ = now() / 60;
            if (mode_ == Mode::Rooms) {
                ctx_.router.rebuild();
                return;
            }
            paint_box();
        }
        for (const auto& a : anim_)
            sprite_animate(a.img, a.slot, a.dex, false, a.skin, now_ms, a.tritan);
    }

    int demo_state_count() const override { return 6; }
    const char* demo_state_name(int i) const override {
        static const char* kNames[] = {"box", "pair", "rooms", "result", "detail", "cancel"};
        return (i >= 0 && i < 6) ? kNames[i] : "box";
    }
    void set_demo_state(int i) override {
        setup_demo();
        const uint32_t rosa = id_by_nick("Rosa", 1);
        const uint32_t prism = id_by_nick("Prism", 2);
        sort_ = Sort::Box;
        pick_id_ = rosa;
        sel_id_ = id_by_nick("Sparky", 0);   // on cooldown: info card shows days left
        top_row_ = 0;
        act_sel_ = 0;
        room_sel_ = 0;
        switch (i) {
            case 1:
                mode_ = Mode::Pair;
                pair_a_ = rosa;
                pair_b_ = prism;
                break;
            case 2:
                mode_ = Mode::Rooms;
                pick_id_ = 0;
                break;
            case 3: {
                // A Rosa x Prism room placed 3 h ago, hatched now on a scratch
                // copy (seed chosen for a Rainbow showcase child).
                mode_ = Mode::Result;
                pick_id_ = 0;
                const int a = index_of(rosa), b = index_of(prism);
                result_ = {};
                for (uint64_t seed = 1; seed < 400 && a >= 0 && b >= 0; ++seed) {
                    breeding::BreedingApp scratch = demo_app_;
                    BreederManager mgr = demo_mgr_;
                    std::string err;
                    const int room = mgr.placePair(scratch, static_cast<size_t>(a), static_cast<size_t>(b),
                                                   kDemoNow - 3 * 3600, err);
                    if (room < 0) break;
                    breeding::Rng rng(seed);
                    const auto evs = mgr.tick(scratch, kDemoNow, rng);
                    if (evs.empty()) break;
                    result_ = evs.front().result;
                    res_room_ = evs.front().room;
                    if (result_.status == breeding::BREED_OK && skin_of(result_.child) == 3) break;
                }
                res_pa_ = *mon(rosa);
                res_pb_ = *mon(prism);
                result_saved_ = true;
                ret_mode_ = Mode::Rooms;
                break;
            }
            case 5:   // last: the Modal outlives the state (it is on the top layer)
                mode_ = Mode::Rooms;
                pick_id_ = 0;
                room_sel_ = 1;
                confirm_cancel(1);   // Modal lives on the top layer; survives rebuild()
                break;
            case 4:
                mode_ = Mode::Detail;
                detail_id_ = prism;   // Rosa is picked: actions are Pair / Fav / Release
                break;
            default: mode_ = Mode::Box; break;
        }
    }

private:
    struct Anim {
        lv_obj_t* img;
        int slot, dex, skin;
        bool tritan;
    };

    // ── Data sources (live GameState, or the private demo copies) ───────────
    breeding::BreedingApp& app() { return demo_ ? demo_app_ : ctx_.game.breeding(); }
    const breeding::BreedingApp& app() const { return demo_ ? demo_app_ : ctx_.game.breeding(); }
    const BreederManager& mgr() const { return demo_ ? demo_mgr_ : ctx_.game.rooms(); }
    long now() const { return demo_ ? kDemoNow : static_cast<long>(time(nullptr)); }
    bool save_box() { return demo_ ? true : ctx_.game.save_box(); }

    int place_pair(size_t a, size_t b, std::string& err) {
        if (demo_) return demo_mgr_.placePair(demo_app_, a, b, now(), err);
        return ctx_.game.place_pair(a, b, now(), err);
    }
    bool cancel_room(int room) { return demo_ ? demo_mgr_.cancel(room) : ctx_.game.cancel_room(room); }

    // Deterministic demo world: the test roster, Sparky on cooldown, room 1
    // incubating (egg in 42m), room 2 with an egg (hatches in 1h 12m), room 3 free.
    void setup_demo() {
        demo_ = true;
        demo_app_ = breeding::BreedingApp{};
        demo_app_.seedTestRoster();
        demo_mgr_ = BreederManager{};
        auto& ros = demo_app_.mutableRoster();
        auto idx = [&ros](const char* nick) -> size_t {
            for (size_t i = 0; i < ros.size(); ++i)
                if (strncmp(ros[i].nick, nick, sizeof(ros[i].nick)) == 0) return i;
            return ros.size();
        };
        std::string err;
        const size_t sparky = idx("Sparky");
        if (sparky < ros.size()) ros[sparky].lastBredAt = kDemoNow - (3 * 86400 + 5 * 3600);
        demo_mgr_.placePair(demo_app_, idx("Sable"), idx("Cinder"), kDemoNow - 18 * 60, err);
        demo_mgr_.placePair(demo_app_, idx("Shade"), idx("Umbra"), kDemoNow - (60 + 48) * 60, err);
        breeding::Rng rng(7);
        demo_mgr_.tick(demo_app_, kDemoNow, rng);   // room 2's egg appears
    }

    const std::vector<BreedMon>& roster() const { return app().roster(); }

    int index_of(uint32_t id) const {
        const auto& r = roster();
        for (int i = 0; i < static_cast<int>(r.size()); ++i)
            if (r[static_cast<size_t>(i)].id == id) return i;
        return -1;
    }
    const BreedMon* mon(uint32_t id) const {
        const int i = index_of(id);
        return i >= 0 ? &roster()[static_cast<size_t>(i)] : nullptr;
    }
    uint32_t id_by_nick(const char* nick, int fallback) const {
        const auto& r = roster();
        for (const auto& m : r)
            if (strncmp(m.nick, nick, sizeof(m.nick)) == 0) return m.id;
        if (fallback >= 0 && fallback < static_cast<int>(r.size()))
            return r[static_cast<size_t>(fallback)].id;
        return 0;
    }

    // Room (0..2) a box mon is sitting in, or -1.
    int room_of(uint32_t id) const {
        if (id == 0) return -1;
        const auto& m = mgr();
        for (int i = 0; i < m.roomCount(); ++i) {
            const BreederRoom& r = m.room(i);
            if (r.state != breeding::ROOM_EMPTY && (r.idA == id || r.idB == id)) return i;
        }
        return -1;
    }
    long cooldown_of(const BreedMon& m) const { return BreederManager::cooldownRemaining(m, now()); }

    // One-line breeding status for the info card / detail view ("" if idle).
    Note status_note(const BreedMon& m) const {
        const int r = room_of(m.id);
        if (r >= 0) {
            const BreederRoom& rm = mgr().room(r);
            if (rm.state == breeding::ROOM_EGG)
                return {fmt("R%d hatch in %s", r + 1, dur(rm.eggHatchAt - now()).c_str()), color::kInfo};
            return {fmt("R%d egg in %s", r + 1, dur(rm.eggAppearAt - now()).c_str()), color::kInfo};
        }
        if (const long cd = cooldown_of(m)) return {"Cooldown " + dur(cd), color::kWarn};
        return {"", 0};
    }

    void compute_order() {
        const auto& r = roster();
        order_.resize(r.size());
        for (size_t i = 0; i < r.size(); ++i) order_[i] = static_cast<int>(i);
        auto key = [&](const BreedMon& m) -> int {
            switch (sort_) {
                case Sort::Dex: return m.dex;
                case Sort::Skin: return -(skin_of(m) * 1000) + m.dex;   // fanciest first
                case Sort::Level: return -m.level;
                default: return 0;
            }
        };
        if (sort_ != Sort::Box)
            std::stable_sort(order_.begin(), order_.end(), [&](int a, int b) {
                return key(r[static_cast<size_t>(a)]) < key(r[static_cast<size_t>(b)]);
            });
        nav_.count = static_cast<int>(order_.size());
        nav_.sel = 0;
        for (int i = 0; i < nav_.count; ++i)
            if (r[static_cast<size_t>(order_[static_cast<size_t>(i)])].id == sel_id_) nav_.sel = i;
        if (nav_.count) sel_id_ = r[static_cast<size_t>(order_[static_cast<size_t>(nav_.sel)])].id;
    }

    const BreedMon* selected() const {
        if (nav_.sel < 0 || nav_.sel >= static_cast<int>(order_.size())) return nullptr;
        const int idx = order_[static_cast<size_t>(nav_.sel)];
        return idx < static_cast<int>(roster().size()) ? &roster()[static_cast<size_t>(idx)] : nullptr;
    }

    lv_obj_t* add_sprite(lv_obj_t* parent, const BreedMon& m, int x, int y, int px) {
        const int sk = skin_of(m);
        lv_obj_t* img = sprite_box(parent, m.dex, x, y, px, px, false, sk, tritan_of(m));
        if (mm::sprites::is_animated(sk) && anim_.size() < 4)
            anim_.push_back({img, static_cast<int>(anim_.size()), m.dex, sk, tritan_of(m)});
        return img;
    }

    // ── Box view ────────────────────────────────────────────────────────────
    void build_box() {
        compute_order();
        const int rows = (nav_.count + kCols - 1) / kCols;
        const int sel_row = nav_.sel / kCols;
        if (sel_row < top_row_) top_row_ = sel_row;
        if (sel_row >= top_row_ + kVisRows) top_row_ = sel_row - kVisRows + 1;
        top_row_ = std::max(0, std::min(top_row_, std::max(0, rows - kVisRows)));

        // Top line: count / pick on the left; rooms, sort, scroll on the right.
        if (const BreedMon* p = mon(pick_id_)) {
            label(body_, "Mate 1:", kGridX, 2, font_small(), color::kDim);
            label(body_, name_of(*p), kGridX + 38, 2, font_small(), color::kAccent, 64);
        } else {
            label(body_, fmt("Bill's PC  %d mons", nav_.count), kGridX, 2, font_small(), color::kDim);
        }
        int busy = 0;
        for (int i = 0; i < mgr().roomCount(); ++i) busy += mgr().room(i).state != breeding::ROOM_EMPTY;
        std::string right = fmt("Sort:%s  Rooms %d/%d", sort_name(sort_), busy, mgr().roomCount());
        if (rows > kVisRows && !pick_id_)
            right += fmt(" %d/%d", std::min(rows, top_row_ + kVisRows), rows);
        lv_obj_t* r = label(body_, right, 0, 2, font_small(), color::kDim);
        lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -(kBodyW - kInfoX + 6), 2);

        if (nav_.count == 0) {
            text_block(body_, "The box is empty. Catch mons in WiFi Encounters, then transfer them here.",
                       kGridX + 8, 50, kInfoX - 20, font_body(), color::kDim);
        }

        const auto& ros = roster();
        for (int vr = 0; vr < kVisRows; ++vr) {
            for (int col = 0; col < kCols; ++col) {
                const int i = (top_row_ + vr) * kCols + col;
                if (i >= nav_.count) break;
                const BreedMon& m = ros[static_cast<size_t>(order_[static_cast<size_t>(i)])];
                const int x = kGridX + col * (kCell + kGap);
                const int y = kGridY + vr * (kCell + kGap);
                lv_obj_t* cell = panel(body_, x, y, kCell, kCell);
                lv_obj_t* img = add_sprite(cell, m, 2, 2, 32);
                const int sk = skin_of(m);
                if (sk != 0) dot(cell, kCell - 9, 2, 6, skin_color(sk));
                if (m.id == pick_id_) chip(cell, "1", 1, kCell - 14, c(color::kAccent));
                const int room = room_of(m.id);
                if (room >= 0) {
                    lv_obj_set_style_image_opa(img, LV_OPA_40, 0);
                    chip(cell, fmt("R%d", room + 1), kCell - 18, kCell - 14, c(color::kInfo));
                } else if (const long cd = cooldown_of(m)) {
                    lv_obj_t* t = chip(cell, dur_tag(cd), 0, 0, c(color::kWarn));
                    lv_obj_align(t, LV_ALIGN_BOTTOM_RIGHT, -1, -1);
                } else if (breeding::isSterile(m.geno)) {
                    label(cell, "x", kCell - 9, kCell - 15, font_small(), color::kBad);
                }
                cells_.push_back({cell, i});
            }
        }

        info_ = panel(body_, kInfoX, kGridY - 14, kInfoW, kBodyH - kGridY + 12);
        paint_box();
    }

    void paint_box() {
        for (auto& [cell, i] : cells_) {
            const bool s = i == nav_.sel;
            lv_obj_set_style_border_color(cell, c(s ? color::kAccent : color::kBorder), 0);
            lv_obj_set_style_border_width(cell, s ? 2 : 1, 0);
            lv_obj_set_style_bg_color(cell, c(s ? color::kPanelHi : color::kPanel), 0);
        }
        if (!info_) return;
        lv_obj_clean(info_);
        const BreedMon* m = selected();
        if (!m) return;
        const int w = kInfoW - 10;
        label(info_, name_of(*m), 5, 3, font_body(), color::kText, w);
        label(info_, species_name(m->dex), 5, 19, font_small(), color::kDim, w);
        label(info_, fmt("Lv %d", m->level), 5, 32, font_small(), color::kText);
        gender_label(info_, *m, 40, 32);
        label(info_, breeding::BreedingApp::provTag(*m), 54, 32, font_small(), color::kDim);
        if (m->favorite) label(info_, "Fav", w - 14, 32, font_small(), color::kAccent);
        const int sk = skin_of(*m);
        chip(info_, skin_name(sk), 5, 46, skin_color(sk));
        int y = 62;
        const Note st = status_note(*m);
        if (!st.text.empty()) {
            label(info_, st.text, 5, y, font_small(), st.col, w);
            y += 12;
        }
        for (const auto& n : gene_notes(*m)) {
            if (y > 86) break;
            label(info_, n.text, 5, y, font_small(), n.col, w);
            y += 12;
        }
        label(info_, geno_cosmetic(m->geno), 5, 104, font_small(), color::kDim, w);
        label(info_, geno_health(m->geno), 5, 116, font_small(), color::kDim, w);
    }

    bool key_box(const KeyEvent& e) {
        if (nav_.on_key(e)) {
            sel_id_ = selected() ? selected()->id : 0;
            const int row = nav_.sel / kCols;
            if (row < top_row_ || row >= top_row_ + kVisRows) ctx_.router.rebuild();
            else paint_box();
            return true;
        }
        const BreedMon* m = selected();
        switch (e.key) {
            case Key::A:
                if (!m) return true;
                detail_id_ = m->id;
                act_sel_ = 0;
                mode_ = Mode::Detail;
                ctx_.router.rebuild();
                return true;
            case Key::Tab: open_rooms(-1); return true;
            case Key::B:
                if (pick_id_) {
                    pick_id_ = 0;
                    ctx_.router.rebuild();
                    return true;
                }
                return false;
            case Key::Char:
                if ((e.ch == 'p' || e.ch == 'P') && m) {
                    pick(*m);
                    return true;
                }
                if (e.ch == 's' || e.ch == 'S') {
                    sort_ = static_cast<Sort>((static_cast<int>(sort_) + 1) % static_cast<int>(Sort::Count));
                    top_row_ = 0;
                    ctx_.router.rebuild();
                    return true;
                }
                return false;
            default: return false;
        }
    }

    // Pick as parent 1, or (with a parent 1 armed) open the pair preview.
    void pick(const BreedMon& m) {
        if (breeding::isSterile(m.geno)) {
            toast(name_of(m) + " is STERILE - can't breed.", 2200, color::kBad);
            return;
        }
        if (pick_id_ == m.id) {
            pick_id_ = 0;
            toast("Parent pick cleared.");
            mode_ = Mode::Box;
        } else if (pick_id_ == 0) {
            pick_id_ = m.id;
            toast(name_of(m) + " is mate 1 - pick a mate.");
            mode_ = Mode::Box;
        } else {
            pair_a_ = pick_id_;
            pair_b_ = m.id;
            mode_ = Mode::Pair;
        }
        ctx_.router.rebuild();
    }

    // ── Detail view ─────────────────────────────────────────────────────────
    enum class Act { Pick, Unpick, BreedWith, Favorite, Release };

    std::vector<Act> actions(const BreedMon& m) const {
        std::vector<Act> a;
        if (pick_id_ == 0) a.push_back(Act::Pick);
        else if (pick_id_ == m.id) a.push_back(Act::Unpick);
        else a.push_back(Act::BreedWith);
        a.push_back(Act::Favorite);
        a.push_back(Act::Release);
        return a;
    }

    // Short soft key label for an action.
    std::string act_key(Act a, const BreedMon& m) const {
        switch (a) {
            case Act::Pick: return "PICK";
            case Act::Unpick: return "UNPICK";
            case Act::BreedWith: return "PAIR";
            case Act::Favorite: return m.favorite ? "UNFAV" : "FAV";
            case Act::Release: return "RELEASE";
        }
        return "";
    }

    bool pick_action(int i) {
        const BreedMon* m = mon(detail_id_);
        if (!m) return true;
        const auto acts = actions(*m);
        if (i < 0 || i >= static_cast<int>(acts.size())) return true;
        act_sel_ = i;
        paint_detail();
        do_action(acts[static_cast<size_t>(i)], *m);
        return true;
    }

    std::string act_text(Act a, const BreedMon& m) const {
        switch (a) {
            case Act::Pick: return "Pick as parent";
            case Act::Unpick: return "Unpick";
            case Act::BreedWith: {
                const BreedMon* p = mon(pick_id_);
                return "Pair w/ " + (p ? name_of(*p) : std::string("?"));
            }
            case Act::Favorite: return m.favorite ? "Unfavorite" : "Favorite";
            case Act::Release: return "Release";
        }
        return "";
    }

    void build_detail() {
        sel_id_ = detail_id_;
        compute_order();   // for L/R prev/next
        const BreedMon& m = *mon(detail_id_);
        const int sk = skin_of(m);

        lv_obj_t* card = panel(body_, 4, 4, 102, 130);
        lv_obj_set_style_border_color(card, sk ? skin_color(sk) : c(color::kBorder), 0);
        add_sprite(card, m, 2, 2, 96);
        chip(card, skin_name(sk), 4, 102, skin_color(sk));
        label(card, breeding::BreedingApp::provTag(m), 4, 116, font_small(), color::kDim);
        if (m.favorite) label(card, "Fav", 70, 116, font_small(), color::kAccent);

        const int mx = 112, mw = 116;
        label(body_, name_of(m), mx, 4, font_title(), color::kText, mw);
        label(body_, species_name(m.dex), mx, 22, font_small(), color::kDim, mw);
        label(body_, fmt("Lv %d", m.level), mx, 35, font_small(), color::kText);
        label(body_, m.geno.female ? "Female" : "Male", mx + 36, 35, font_small(),
              m.geno.female ? kFemale : kMale);
        label(body_, "Look", mx, 50, font_small(), color::kDim);
        label(body_, geno_cosmetic(m.geno), mx + 40, 50, font_small(), color::kText);
        label(body_, "Health", mx, 62, font_small(), color::kDim);
        label(body_, geno_health(m.geno), mx + 40, 62, font_small(), color::kText);
        int y = 78;
        std::vector<Note> notes = gene_notes(m);
        const Note st = status_note(m);
        if (!st.text.empty()) notes.insert(notes.begin(), st);
        for (const auto& n : notes) {
            if (y > 122) break;
            label(body_, n.text, mx, y, font_small(), n.col, mw);
            y += 12;
        }

        const auto acts = actions(m);
        act_sel_ = std::min(act_sel_, static_cast<int>(acts.size()) - 1);
        for (size_t i = 0; i < acts.size(); ++i) {
            lv_obj_t* row = panel(body_, 232, 4 + static_cast<int>(i) * 22, 84, 20);
            label(row, act_text(acts[i], m), 5, 3, font_small(),
                  acts[i] == Act::Release ? color::kBad : color::kText, 74);
            acts_.push_back(row);
        }
        paint_detail();
    }

    void paint_detail() {
        for (size_t i = 0; i < acts_.size(); ++i) {
            const bool s = static_cast<int>(i) == act_sel_;
            lv_obj_set_style_border_color(acts_[i], c(s ? color::kAccent : color::kBorder), 0);
            lv_obj_set_style_bg_color(acts_[i], c(s ? color::kPanelHi : color::kPanel), 0);
        }
    }

    bool key_detail(const KeyEvent& e) {
        const BreedMon* m = mon(detail_id_);
        if (!m) {
            mode_ = Mode::Box;
            ctx_.router.rebuild();
            return true;
        }
        const auto acts = actions(*m);
        const int n = static_cast<int>(acts.size());
        switch (e.key) {
            case Key::Up: act_sel_ = (act_sel_ + n - 1) % n; paint_detail(); return true;
            case Key::Down: act_sel_ = (act_sel_ + 1) % n; paint_detail(); return true;
            case Key::Left:
            case Key::Right: {
                if (order_.empty()) return true;
                int pos = 0;
                for (size_t i = 0; i < order_.size(); ++i)
                    if (roster()[static_cast<size_t>(order_[i])].id == detail_id_) pos = static_cast<int>(i);
                const int cnt = static_cast<int>(order_.size());
                pos = (pos + (e.key == Key::Left ? cnt - 1 : 1)) % cnt;
                detail_id_ = sel_id_ = roster()[static_cast<size_t>(order_[static_cast<size_t>(pos)])].id;
                ctx_.router.rebuild();
                return true;
            }
            case Key::B:
                sel_id_ = detail_id_;
                mode_ = Mode::Box;
                ctx_.router.rebuild();
                return true;
            case Key::A: do_action(acts[static_cast<size_t>(act_sel_)], *m); return true;
            default: return false;
        }
    }

    void do_action(Act a, const BreedMon& m) {
        switch (a) {
            case Act::Pick:
            case Act::Unpick:
            case Act::BreedWith:
                sel_id_ = m.id;
                pick(m);
                return;
            case Act::Favorite: {
                if (BreedMon* bm = app().findById(m.id)) {
                    bm->favorite = !bm->favorite;
                    save_box();
                    toast(bm->favorite ? "Favorited " + name_of(*bm) : "Unfavorited " + name_of(*bm));
                }
                ctx_.router.rebuild();
                return;
            }
            case Act::Release: {
                const int room = room_of(m.id);
                if (room >= 0) {
                    toast(fmt("%s is in Room %d - cancel the room first.", name_of(m).c_str(), room + 1),
                          2600, color::kBad);
                    return;
                }
                const uint32_t id = m.id;
                const std::string nm = name_of(m);
                ctx_.router.show_modal(std::make_unique<Modal>(
                    "Release " + nm + "?",
                    nm + " leaves Bill's PC for good. This can't be undone.",
                    std::vector<std::string>{"Keep", "Release"}, [this, id, nm](int choice) {
                        if (choice != 1) return;
                        auto& ros = app().mutableRoster();
                        ros.erase(std::remove_if(ros.begin(), ros.end(),
                                                 [id](const BreedMon& b) { return b.id == id; }),
                                  ros.end());
                        save_box();
                        if (pick_id_ == id) pick_id_ = 0;
                        toast(nm + " was released.");
                        mode_ = Mode::Box;
                        ctx_.router.rebuild();
                    }));
                return;
            }
        }
    }

    // ── Pair preview ────────────────────────────────────────────────────────
    void parent_card(const BreedMon& m, int x) {
        const int sk = skin_of(m);
        lv_obj_t* card = panel(body_, x, 4, 74, 94);
        lv_obj_set_style_border_color(card, sk ? skin_color(sk) : c(color::kBorder), 0);
        add_sprite(card, m, 4, 0, 64);
        label(card, name_of(m), 4, 63, font_small(), color::kText, 54);
        gender_label(card, m, 62, 63);
        chip(card, skin_name(sk), 3, 77, skin_color(sk));
    }

    // Why this pair can't go in a room right now ("" = it can), checked in
    // the same order as BreederManager::placePair.
    std::string pair_blocker(const BreedMon& a, const BreedMon& b) const {
        if (!app().breedingUnlocked()) return "Locked: needs a Wild catch";
        for (const BreedMon* m : {&a, &b}) {
            if (breeding::isSterile(m->geno)) return name_of(*m) + " is sterile (bb)";
            if (breeding::neverHatches(m->geno)) return name_of(*m) + " lays dud eggs (hh)";
        }
        for (const BreedMon* m : {&a, &b})
            if (const int r = room_of(m->id); r >= 0) return fmt("%s is in Room %d", name_of(*m).c_str(), r + 1);
        for (const BreedMon* m : {&a, &b})
            if (const long cd = cooldown_of(*m)) return name_of(*m) + " cooldown " + dur(cd);
        if (mgr().firstFreeRoom() < 0) return "All 3 rooms are busy";
        return "";
    }

    void build_pair() {
        const BreedMon& a = *mon(pair_a_);
        const BreedMon& b = *mon(pair_b_);
        parent_card(a, 4);
        label(body_, "x", 81, 42, font_title(), color::kDim);
        parent_card(b, 92);

        const std::string why = pair_blocker(a, b);
        if (why.empty())
            label(body_, fmt("Ready - Room %d is free", mgr().firstFreeRoom() + 1), 6, 100, font_small(),
                  color::kGood, 158);
        else
            label(body_, why, 6, 100, font_small(), color::kBad, 158);
        const int da = breeding::BreedingApp::baseForm(a.dex);
        const int db = breeding::BreedingApp::baseForm(b.dex);
        const std::string baby = da == db ? species_name(da)
                                          : species_name(da) + " or " + species_name(db);
        label(body_, "Baby: " + baby, 6, 112, font_small(), color::kDim, 158);
        label(body_, "Egg in 1h, hatches 2h later", 6, 124, font_small(), color::kDim, 158);

        lv_obj_t* p = panel(body_, 170, 4, 146, 130);
        label(p, "Offspring odds", 6, 3, font_small(), color::kAccent);
        const Odds o = child_odds(a.geno, b.geno);
        int order[12];
        for (int i = 0; i < 12; ++i) order[i] = i;
        std::stable_sort(order, order + 12, [&](int x, int y) { return o.skin[x] > o.skin[y]; });
        int y = 18;
        for (int k = 0; k < 12 && y <= 66; ++k) {
            const int sk = order[k];
            if (o.skin[sk] < 0.005) break;
            dot(p, 6, y + 3, 6, skin_color(sk));
            label(p, skin_name(sk), 16, y, font_small(), color::kText, 74);
            lv_obj_t* bb = bar(p, 92, y + 3, 24, 6, skin_color(sk));
            bar_set(bb, pct(o.skin[sk]), 100);
            label(p, fmt("%d%%", pct(o.skin[sk])), 119, y, font_small(), color::kText);
            y += 12;
        }
        y += 8;
        auto line = [&](const std::string& k, double v, uint32_t hot) {
            label(p, k, 6, y, font_small(), color::kDim);
            label(p, fmt("%d%%", pct(v)), 116, y, font_small(), v > 0.0 ? hot : color::kText);
            y += 11;
        };
        if (o.hidden > 0.0) line("Male hidden carrier", o.hidden, kFemale);
        line("Defect (bb/ff/hh)", 1.0 - (1.0 - o.sterile) * (1.0 - o.cant) * (1.0 - o.nohatch),
             color::kBad);
        if (y <= 104 && o.tritan > 0.0) line("Tritan", o.tritan, color::kInfo);
        if (y <= 104 && o.nohatch > 0.0) line("Egg won't hatch", o.nohatch, color::kBad);
    }

    bool key_pair(const KeyEvent& e) {
        if (e.key == Key::B) {
            mode_ = Mode::Box;
            ctx_.router.rebuild();
            return true;
        }
        if (e.key != Key::A) return false;
        const int a = index_of(pair_a_), b = index_of(pair_b_);
        if (a < 0 || b < 0) {
            mode_ = Mode::Box;
            ctx_.router.rebuild();
            return true;
        }
        std::string err;
        const int room = place_pair(static_cast<size_t>(a), static_cast<size_t>(b), err);
        if (room < 0) {
            toast(ascii(err.empty() ? std::string("Couldn't place the pair.") : err), 2600, color::kBad);
            return true;
        }
        toast(fmt("Placed in Room %d - egg in 1h", room + 1), 2400, color::kGood);
        pick_id_ = 0;
        open_rooms(room);
        return true;
    }

    // ── Rooms view ──────────────────────────────────────────────────────────
    void open_rooms(int sel) {
        if (sel >= 0) room_sel_ = sel;
        mode_ = Mode::Rooms;
        ctx_.router.rebuild();
    }

    void build_rooms() {
        const auto& m = mgr();
        room_sel_ = std::max(0, std::min(room_sel_, m.roomCount() - 1));
        for (int i = 0; i < m.roomCount(); ++i) {
            lv_obj_t* card = panel(body_, 4 + i * (kRoomW + 3), 4, kRoomW, kRoomH);
            room_cards_.push_back(card);
            build_room_card(card, i, m.room(i));
        }
        paint_rooms();
    }

    void build_room_card(lv_obj_t* card, int i, const BreederRoom& r) {
        const int w = kRoomW;
        label(card, fmt("Room %d", i + 1), 6, 4, font_small(), color::kText);
        const char* tag = r.state == breeding::ROOM_EGG ? "EGG" : r.state == breeding::ROOM_INCUBATING ? "PAIRED" : "FREE";
        const uint32_t tcol = r.state == breeding::ROOM_EGG ? color::kGood
                              : r.state == breeding::ROOM_INCUBATING ? color::kInfo : color::kBorder;
        lv_obj_t* t = chip(card, tag, 0, 4, c(tcol), r.state == breeding::ROOM_EMPTY ? color::kDim : 0x101418);
        lv_obj_align(t, LV_ALIGN_TOP_RIGHT, -5, 4);

        if (r.state == breeding::ROOM_EMPTY) {
            centered(card, "Empty", 38, w, font_title(), color::kDim);
            lv_obj_t* tb = text_block(card, "PICK two mons in the box to pair them here.", 8, 62, w - 16,
                                      font_small(), color::kDim);
            lv_obj_set_style_text_align(tb, LV_TEXT_ALIGN_CENTER, 0);
            return;
        }
        const long t_now = now();
        const std::string na = name_of(r.parentA), nb = name_of(r.parentB);
        long from, to;
        if (r.state == breeding::ROOM_INCUBATING) {
            add_sprite(card, r.parentA, 3, 16, 44);
            add_sprite(card, r.parentB, w - 3 - 44, 16, 44);
            label(card, na, 4, 61, font_small(), color::kText, 46);
            lv_obj_t* lb = label(card, nb, w - 50, 61, font_small(), color::kText, 46);
            lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_RIGHT, 0);
            centered(card, "Egg in", 76, w, font_small(), color::kDim);
            centered(card, dur(r.eggAppearAt - t_now), 88, w, font_title(), color::kText);
            from = r.placedAt;
            to = r.eggAppearAt;
            centered(card, "then hatches +2h", 114, w, font_small(), color::kDim);
        } else {
            add_sprite(card, r.parentA, 1, 26, 28);
            add_sprite(card, r.parentB, w - 1 - 28, 26, 28);
            egg(card, (w - 32) / 2, 17);
            centered(card, na + " x " + nb, 61, w, font_small(), color::kDim);
            centered(card, "Hatches in", 76, w, font_small(), color::kDim);
            centered(card, dur(r.eggHatchAt - t_now), 88, w, font_title(), color::kGood);
            from = r.eggAppearAt;
            to = r.eggHatchAt;
            centered(card, "egg laid " + dur(t_now - r.eggAppearAt) + " ago", 114, w, font_small(), color::kDim);
        }
        lv_obj_t* b = bar(card, 10, 107, w - 20, 4, c(r.state == breeding::ROOM_EGG ? color::kGood : color::kInfo));
        const long span = std::max(1L, to - from);
        bar_set(b, static_cast<int>(std::max(0L, std::min(span, t_now - from)) * 100 / span), 100);
    }

    void paint_rooms() {
        for (size_t i = 0; i < room_cards_.size(); ++i) {
            const bool s = static_cast<int>(i) == room_sel_;
            lv_obj_set_style_border_color(room_cards_[i], c(s ? color::kAccent : color::kBorder), 0);
            lv_obj_set_style_border_width(room_cards_[i], s ? 2 : 1, 0);
            lv_obj_set_style_bg_color(room_cards_[i], c(s ? color::kPanelHi : color::kPanel), 0);
        }
    }

    void confirm_cancel(int i) {
        const BreederRoom& r = mgr().room(i);
        if (r.state == breeding::ROOM_EMPTY) return;
        const std::string na = name_of(r.parentA), nb = name_of(r.parentB);
        const std::string body =
            r.state == breeding::ROOM_EGG
                ? "The egg is lost and " + na + " and " + nb + " return to the box. Cooldowns stay."
                : na + " and " + nb + " return to the box with no egg. Their 7-day cooldown stays.";
        ctx_.router.show_modal(std::make_unique<Modal>(
            fmt("Cancel Room %d?", i + 1), body, std::vector<std::string>{"Keep", "Cancel pair"},
            [this, i](int choice) {
                if (choice != 1) return;
                if (cancel_room(i)) toast(fmt("Room %d is free again.", i + 1), 2200, color::kWarn);
                ctx_.router.rebuild();
            }));
    }

    bool key_rooms(const KeyEvent& e) {
        const int n = mgr().roomCount();
        switch (e.key) {
            case Key::Left:
            case Key::Up: room_sel_ = (room_sel_ + n - 1) % n; paint_rooms(); return true;
            case Key::Right:
            case Key::Down: room_sel_ = (room_sel_ + 1) % n; paint_rooms(); return true;
            case Key::A:
                if (mgr().room(room_sel_).state == breeding::ROOM_EMPTY)
                    toast("Empty - PICK two mons in the box.");
                else
                    confirm_cancel(room_sel_);
                return true;
            case Key::Tab:
            case Key::B:
                mode_ = Mode::Box;
                ctx_.router.rebuild();
                return true;
            default: return false;
        }
    }

    // ── Result (a room hatched) ─────────────────────────────────────────────
    void show_hatch(const GameState::Hatch& h) {
        ret_mode_ = mode_;
        result_ = h.result;
        res_pa_ = h.parent_a;
        res_pb_ = h.parent_b;
        res_room_ = h.room;
        result_saved_ = true;   // GameState saved the box when the child hatched
        if (result_.status == breeding::BREED_OK) {
            sel_id_ = result_.child.id;
            toast(fmt("Room %d: the egg hatched!", h.room + 1), 2400, color::kGood);
        } else {
            toast(fmt("Room %d: the egg failed.", h.room + 1), 2400, color::kBad);
        }
        mode_ = Mode::Result;
        ctx_.router.rebuild();
    }

    void build_result() {
        const bool ok = result_.status == breeding::BREED_OK;
        lv_obj_t* card = panel(body_, 4, 4, 102, 130);
        const std::string parents =
            fmt("Room %d: ", res_room_ + 1) + name_of(res_pa_) + " x " + name_of(res_pb_);
        if (ok) {
            const BreedMon& m = result_.child;
            const int sk = skin_of(m);
            lv_obj_set_style_border_color(card, sk ? skin_color(sk) : c(color::kAccent), 0);
            lv_obj_set_style_border_width(card, 2, 0);
            add_sprite(card, m, 1, 4, 96);
            chip(card, "NEW", 4, 108, c(color::kAccent));

            label(body_, "It hatched!", 112, 4, font_big(), color::kAccent);
            label(body_, species_name(m.dex), 112, 28, font_title(), color::kText, 200);
            // One row: skin chip, sex, level + provenance, laid out by width.
            int x = 112;
            auto place = [&](lv_obj_t* o) {
                lv_obj_update_layout(o);
                x += static_cast<int>(lv_obj_get_width(o)) + 8;
            };
            place(chip(body_, skin_name(sk), x, 48, skin_color(sk)));
            place(label(body_, m.geno.female ? "Female" : "Male", x, 48, font_small(),
                        m.geno.female ? kFemale : kMale));
            label(body_, fmt("Lv %d   %s", m.level, breeding::BreedingApp::provTag(m).c_str()), x, 48,
                  font_small(), color::kDim);
            label(body_, "Look", 112, 64, font_small(), color::kDim);
            label(body_, geno_cosmetic(m.geno), 152, 64, font_small(), color::kText);
            label(body_, "Health", 112, 76, font_small(), color::kDim);
            label(body_, geno_health(m.geno), 152, 76, font_small(), color::kText);
            int y = 90;
            for (const auto& n : gene_notes(m)) {
                if (y > 92) break;
                label(body_, n.text, 112, y, font_small(), n.col, 200);
                y += 12;
            }
            label(body_, parents, 112, 106, font_small(), color::kDim, 200);
            label(body_, result_saved_ ? "Added to Bill's PC." : "Added to the box (save failed).",
                  112, 120, font_small(), result_saved_ ? color::kGood : color::kWarn, 200);
        } else {
            // The egg failed (hh): show the parents small, and the engine's reason.
            add_sprite(card, res_pa_, 2, 20, 48);
            add_sprite(card, res_pb_, 36, 64, 48);
            label(body_, "The egg didn't hatch", 112, 4, font_title(), color::kBad, 204);
            label(body_, parents, 112, 24, font_small(), color::kDim, 200);
            text_block(body_, ascii(result_.message), 112, 40, 200, font_small(), color::kText);
        }
    }

    bool key_result(const KeyEvent& e) {
        if (e.key != Key::A && e.key != Key::B) return false;
        mode_ = ret_mode_ == Mode::Rooms ? Mode::Rooms : Mode::Box;
        top_row_ = 0x7fff;   // build_box clamps; scroll so the selection is visible
        ctx_.router.rebuild();
        return true;
    }

    // ── State ───────────────────────────────────────────────────────────────
    Mode mode_ = Mode::Box;
    Mode ret_mode_ = Mode::Box;
    Sort sort_ = Sort::Box;
    GridNav nav_;
    std::vector<int> order_;   // roster indices in display order
    int top_row_ = 0;
    uint32_t sel_id_ = 0, pick_id_ = 0, detail_id_ = 0, pair_a_ = 0, pair_b_ = 0;
    int act_sel_ = 0;
    int room_sel_ = 0;
    breeding::BreedResult result_;
    BreedMon res_pa_ = {}, res_pb_ = {};
    int res_room_ = 0;
    bool result_saved_ = false;
    uint32_t rev_ = 0;
    long last_min_ = 0;

    // Demo states only: private world + fixed clock (never saved).
    bool demo_ = false;
    breeding::BreedingApp demo_app_;
    BreederManager demo_mgr_;

    lv_obj_t* body_ = nullptr;
    lv_obj_t* info_ = nullptr;
    std::vector<std::pair<lv_obj_t*, int>> cells_;
    std::vector<lv_obj_t*> acts_;
    std::vector<lv_obj_t*> room_cards_;
    std::vector<Anim> anim_;
};

} // namespace

std::unique_ptr<Screen> make_breeding_screen(Ctx& ctx) {
    return std::make_unique<BreedingScreen>(ctx);
}

} // namespace mm::ui
