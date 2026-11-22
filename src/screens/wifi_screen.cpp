// wifi_screen.cpp — WiFi Encounters: the auto journey (port of mmterm's
// WiFi Encounters mode, src/terminal/TerminalUI.cpp wifiEnc*).
//
// You start with a Lv 5 PIKACHU. It battles on its own, one fight after
// another: each fight is set at a nearby network (wifi_networks.h; the name is
// flavour text, its 6-byte id seeds the foe's colour). 85% are wild mons from
// every Kanto zone unlocked so far, 15% are trainers of the next gym (the
// leader 40% of the time; only beating the leader earns the badge, which
// unlocks the next zones). KOs give 1/10 Gen 1 EXP: it levels, learns moves
// and evolves into RAICHU at Lv 30. A foe with a rare colour you don't own yet
// is worn down to 25% HP and then Poke Balls are thrown until it is caught;
// catches go to Bill's PC (GameState box) and rare_catches.dat. A loss replays
// the same spot. Any Bill's PC mon (Lv 5+) can take over as the battler, with
// its own level / badges / record ("independent journeys").
//
// Progress lives in wifi_encounters.dat next to the box file, byte-compatible
// with mmterm (same fields, same order, native little-endian). Nothing is
// written when GameState::files_enabled() is false (--shots).
//
// Enter opens the menu (the auto-fight pauses): Status, Bill's PC, Reset.
#include "screens.h"

#include "game_state.h"
#include "router.h"
#include "widgets.h"
#include "wifi_networks.h"

#include "Gen1BaseExp.h"        // monster_mesh_pi/src/shared
#include "Gen1BattleEngine.h"   // monster_mesh_pi/src/battle
#include "Gen1Evolution.h"
#include "Gen1Learnsets.h"
#include "KantoZones.h"
#include "LordGyms.h"
#include "WifiEncounters.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace mm::ui {
namespace {

using Eng = Gen1BattleEngine;

// ── Pacing (mmterm: 900 ms turns, 2.5 s linger, 6 s standby rescan) ─────────
constexpr uint32_t kMsgMs = 700;           // each battle message stays this long
constexpr uint32_t kEndMs = 2500;          // linger on the result before the next fight
constexpr uint32_t kStandbyScanMs = 6000;  // standby: look for networks again
constexpr uint32_t kSpotCacheMs = 30000;   // reuse the spot list this long (nmcli blocks)

constexpr uint8_t kPikachuEvolveLevel = 30;
constexpr uint8_t kNurseryGraduateLevel = 5;
constexpr uint32_t kSaveMagic = 0x504B4341u;   // 'PKCA'

// ── Geometry (same field as the Battle screen) ──────────────────────────────
constexpr int kStripY = 100, kStripH = 36;
constexpr int kFoeBoxX = 4, kFoeBoxY = 4, kBoxW = 152, kFoeBoxH = 34;
constexpr int kMeBoxX = 164, kMeBoxY = 60, kMeBoxH = 38;
constexpr int kFoeSpriteX = 238, kFoeSpriteY = -2;
constexpr int kMeSpriteX = 26, kMeSpriteY = 36;
constexpr int kZoneX = 96, kZoneY = 41, kZoneW = 140;

// Canonical Gen 1 Pikachu learnset (level-sorted).
struct PikaLearn {
    uint8_t level, move;
};
constexpr PikaLearn kPikaLearnset[] = {
    {1, 84}, {1, 45}, {6, 39}, {8, 86}, {11, 98}, {15, 104}, {20, 21}, {26, 85}, {33, 97}, {41, 87},
};

// The 4 most recently learned moves at `level`, Thunder Shock filling gaps.
void pika_moves_for_level(uint8_t level, uint8_t out[4]) {
    uint8_t w[4] = {0, 0, 0, 0};
    int n = 0;
    for (const PikaLearn& e : kPikaLearnset) {
        if (e.level > level) break;
        if (n < 4) {
            w[n++] = e.move;
        } else {
            w[0] = w[1]; w[1] = w[2]; w[2] = w[3]; w[3] = e.move;
        }
    }
    for (int i = 0; i < 4; ++i) out[i] = w[i] ? w[i] : 84;
}

std::string upper(std::string s) {
    for (char& ch : s)
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    return s;
}

std::string ascii(const char* s) {
    std::string out;
    for (; s && *s; ++s) {
        const unsigned char ch = static_cast<unsigned char>(*s);
        if (ch >= 32 && ch < 127) out.push_back(static_cast<char>(ch));
        else if (ch >= 0xC0) out.push_back('?');
    }
    return out;
}

bool ends_with(const std::string& s, const char* suf) {
    const size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

void set_nick(Eng::BattlePoke& bp, const std::string& name) {
    std::string n = upper(name);
    size_t w = 0;
    for (char ch : n) {
        if (w >= 10) break;
        if (ch >= 32 && ch < 127) bp.nickname[w++] = ch;
    }
    bp.nickname[w] = 0;
}

std::string move_name(uint16_t id) {
    const Gen1MoveData* mv = gen1Move(id);
    return mv ? upper(mv->name) : std::string("-");
}

uint16_t calc_stat(uint8_t base, uint8_t iv, uint8_t level, bool is_hp) {
    const uint32_t v = (uint32_t)(base + iv) * 2 * level / 100;
    return static_cast<uint16_t>(is_hp ? v + level + 10 : v + 5);
}

// mmterm builds foes from a Gen1Party with zeroed DVs; the engine's base init
// uses average DVs. Recompute so the foes are exactly as tough as in mmterm.
void zero_dvs(Eng::BattlePoke& bp) {
    if (bp.species < 1 || bp.species > 151) return;
    const Gen1BaseStats& b = GEN1_BASE_STATS[bp.species];
    bp.maxHp = calc_stat(b.hp, 0, bp.level, true);
    bp.atk = calc_stat(b.atk, 0, bp.level, false);
    bp.def = calc_stat(b.def, 0, bp.level, false);
    bp.spd = calc_stat(b.spd, 0, bp.level, false);
    bp.spc = calc_stat(b.spc, 0, bp.level, false);
    bp.hp = bp.maxHp;
}

int popcount(uint32_t v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

uint16_t clamp16(uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(v); }

// A "nursery day" rolls at 6 AM local time (noon anchor dodges DST edges).
long nursery_day_index(time_t t) {
    time_t shifted = t - 6 * 3600;
    struct tm lt = *localtime(&shifted);
    lt.tm_hour = 12; lt.tm_min = 0; lt.tm_sec = 0;
    lt.tm_isdst = -1;
    return static_cast<long>(mktime(&lt) / 86400);
}

std::string dir_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Rarity gate (accept per 1000): legendaries 5, pseudo-legendaries 20, rares 50.
uint16_t rarity_permille(uint8_t dex) {
    if (dex == 144 || dex == 145 || dex == 146 || dex == 150 || dex == 151) return 5;
    if (dex == 147 || dex == 148 || dex == 149) return 20;
    if (dex == 113 || dex == 130 || dex == 131 || dex == 137 || dex == 142 || dex == 143 ||
        dex == 134 || dex == 135 || dex == 136)
        return 50;
    return 0;
}
constexpr uint8_t kRarePool[] = {113, 130, 131, 134, 135, 136, 137, 142, 143,
                                 147, 148, 149, 144, 145, 146, 150, 151};

// ── The saved journey (wifi_encounters.dat, mmterm layout) ──────────────────
struct Journey {
    // Live set = the ACTIVE battler's progress.
    uint8_t level = 5;
    uint32_t xp = 0;
    uint8_t dex[19] = {};       // 151-bit "seen"
    uint8_t beaten[19] = {};    // 151-bit "beaten"
    uint16_t used_ssid = 0;     // mmterm field, kept as is
    uint32_t wins = 0, losses = 0;
    uint8_t gyms = 0;           // bit n = gym leader n beaten
    uint8_t active_dex = 0;     // 0 = the starter Pikachu
    uint8_t active_variant = 0; // its skin
    uint32_t active_id = 0;     // its BreedMon id (0 = starter)
    // The starter's own trip (independent journeys).
    uint8_t starter_level = 0;
    uint32_t starter_xp = 0;
    uint16_t starter_gym = 0, starter_wins = 0, starter_losses = 0;
    uint32_t nursery_grow = 0;  // last 6 AM nursery growth (epoch s)
};

bool bit_get(const uint8_t* bits, int dex) {
    return dex >= 1 && dex <= 151 && (bits[(dex - 1) >> 3] >> ((dex - 1) & 7) & 1);
}
void bit_set(uint8_t* bits, int dex) {
    if (dex >= 1 && dex <= 151) bits[(dex - 1) >> 3] |= static_cast<uint8_t>(1u << ((dex - 1) & 7));
}
int bit_count(const uint8_t* bits) {
    int n = 0;
    for (int d = 1; d <= 151; ++d) n += bit_get(bits, d);
    return n;
}

// ── View model ──────────────────────────────────────────────────────────────
struct SideSnap {
    bool shown = false;
    int dex = 0;
    std::string name;
    int level = 0, hp = 0, max_hp = 1;
};
struct Snap {
    SideSnap me, foe;
    int balls = 0;
};
struct Msg {
    std::string text;
    Snap snap;
};

enum class View : uint8_t { Battle, Menu, Status, Pc };
enum class Phase : uint8_t { Fight, Linger, Standby };

constexpr const char* kMenuItems[] = {"Back", "Status", "Bill's PC", "Reset Pikachu"};
constexpr int kMenuCount = 4;

class WifiScreen final : public Screen {
public:
    explicit WifiScreen(Ctx& ctx) : Screen(ctx) {
        rng_.seed(static_cast<uint32_t>(time(nullptr)) ^ (lv_tick_get() << 7));
        if (ctx_.game.files_enabled()) {
            path_ = dir_of(ctx_.game.box_path()) + "/wifi_encounters.dat";
            rare_path_ = dir_of(ctx_.game.box_path()) + "/rare_catches.dat";
            load_progress();
        }
        nursery_grow();
        sync_active();
        start_battle();
    }
    ~WifiScreen() override { save_progress(); }

    const char* title() const override { return "WiFi Encounters"; }

    std::string hints() const override {
        if (modal_ && modal_->open()) return "Up/Dn:choose  Enter:ok  Esc:cancel";
        switch (view_) {
            case View::Battle: return "Enter:menu  s:status  p:Bill's PC  Esc:leave";
            case View::Menu:   return "Up/Dn:choose  Enter:ok  Esc:resume";
            case View::Status: return "Enter/Esc:resume";
            case View::Pc:     return "Up/Dn:mon  Enter:set as battler  Esc:back";
        }
        return "";
    }

    void build(lv_obj_t* body) override {
        built_ = true;
        refs_ = Refs{};
        pc_list_.reset();
        switch (view_) {
            case View::Battle: build_battle(body); break;
            case View::Menu:   build_battle(body); build_menu(body); break;
            case View::Status: build_status(body); break;
            case View::Pc:     build_pc(body); break;
        }
    }

    bool on_key(const KeyEvent& e) override {
        if (modal_ && modal_->open()) return modal_->on_key(e);
        switch (view_) {
            case View::Battle:
                if (e.key == Key::A) { show(View::Menu); menu_sel_ = 0; ctx_.router.rebuild(); return true; }
                if (e.key == Key::Char && e.ch == 's') { show(View::Status); return true; }
                if (e.key == Key::Char && e.ch == 'p') { show(View::Pc); return true; }
                return false;   // Esc leaves (the destructor saves)
            case View::Menu:
                if (e.key == Key::Up || e.key == Key::Down) {
                    menu_sel_ = (menu_sel_ + (e.key == Key::Up ? kMenuCount - 1 : 1)) % kMenuCount;
                    paint_menu();
                    return true;
                }
                if (e.key == Key::B) { show(View::Battle); return true; }
                if (e.key == Key::A) { menu_pick(); return true; }
                return true;
            case View::Status:
                if (e.key == Key::A || e.key == Key::B) { show(View::Battle); return true; }
                return true;
            case View::Pc:
                if (e.key == Key::B) { show(View::Battle); return true; }
                if (e.key == Key::A) { pc_pick(); return true; }
                if (pc_list_ && pc_list_->on_key(e)) { pc_sel_ = pc_list_->selected(); return true; }
                return true;
        }
        return false;
    }

    void tick(uint32_t now) override {
        if (view_ != View::Battle) return;   // the journey pauses under menus
        if (refs_.foe_img)
            sprite_animate(refs_.foe_img, 0, shown_.foe.dex, false, foe_skin_, now, foe_tritan_);
        if (refs_.me_img)
            sprite_animate(refs_.me_img, 1, shown_.me.dex, true, my_skin_, now, my_tritan_);
        if (resume_) {   // just came back from a menu: give the eye a moment
            resume_ = false;
            next_at_ = now + 300;
        }
        if (now < next_at_) return;

        if (!queue_.empty()) {
            pop_message();
            apply_snap();
            next_at_ = now + kMsgMs;
            return;
        }
        switch (phase_) {
            case Phase::Standby:
                if (now - scan_at_ >= kStandbyScanMs) {
                    scan_at_ = now;
                    refresh_spots(true);
                    if (!spots_.empty()) start_battle();
                }
                return;
            case Phase::Linger:
                if (now >= linger_until_) {
                    if (result_ == Eng::Result::P2_WIN) rematch_ = true;   // lost: same spot again
                    start_battle();
                }
                return;
            case Phase::Fight:
                if (result_ != Eng::Result::ONGOING) {
                    finish_battle();
                    phase_ = Phase::Linger;
                    linger_until_ = now + kEndMs;
                    return;
                }
                beat();
                next_at_ = now;   // first new message on the next tick
                return;
        }
    }

    void on_hide() override { save_progress(); }

    // ── --shots ─────────────────────────────────────────────────────────────
    int demo_state_count() const override { return 8; }
    const char* demo_state_name(int i) const override {
        static const char* const kNames[] = {"battle", "catching", "caught", "gym",
                                             "standby", "menu", "status", "pc"};
        return (i >= 0 && i < 8) ? kNames[i] : "battle";
    }
    void set_demo_state(int i) override {
        rng_.seed(1234u + static_cast<uint32_t>(i));
        modal_.reset();
        j_ = Journey{};
        j_.level = 18;
        j_.xp = 420;
        j_.wins = 41;
        j_.losses = 6;
        j_.gyms = 0x01;   // Boulder Badge
        for (int d : {1, 10, 13, 16, 19, 21, 25, 29, 32, 41, 43, 56, 74, 95, 100})
            bit_set(j_.dex, d);
        for (int d : {10, 13, 16, 19, 21, 29, 32, 41, 74})
            bit_set(j_.beaten, d);
        sync_active();
        view_ = View::Battle;
        spot_ = wifi::demo_spots().front();
        have_spot_ = true;
        breeding::Genotype regular{};
        breeding::Genotype shiny{};
        shiny.shiny = 2;
        switch (i) {
            case 1:   // weakened rare foe, Poke Balls flying
            case 2: {
                setup_battle(43, 15, nullptr, shiny, "Route 24", "", 255);
                drain();
                auto& foe = eng_.party(1).mons[0];
                foe.hp = std::max<uint16_t>(1, foe.maxHp / 5);
                eng_.party(0).mons[0].hp = eng_.party(0).mons[0].maxHp * 2 / 3;
                catching_ = true;
                balls_ = i == 1 ? 2 : 3;
                push(i == 1 ? "Aww! It broke free!"
                            : "Gotcha! The Shiny Oddish was caught!");
                finalize();
                drain();
                break;
            }
            case 3: {   // gym trainer
                const LordGym* g = lordGym(1);
                const LordGymTrainer& tr = g->trainers[0];
                setup_battle(tr.party[0].species, tr.party[0].level, tr.party[0].moves, regular,
                             std::string(g->city) + " Gym", tr.name, 255);
                drain();
                break;
            }
            case 4:
                phase_ = Phase::Standby;
                queue_.clear();
                break;
            default: {   // battle / menu / status / pc
                setup_battle(16, 14, nullptr, regular, "Route 2", "", 255);
                drain();
                run_turn();
                drain();
                break;
            }
        }
        if (i == 5) { view_ = View::Menu; menu_sel_ = 1; }
        if (i == 6) view_ = View::Status;
        if (i == 7) { view_ = View::Pc; pc_sel_ = 0; }
        next_at_ = UINT32_MAX;   // freeze for the screenshot
    }

private:
    struct Refs {
        lv_obj_t* foe_box = nullptr;
        lv_obj_t* foe_name = nullptr;
        lv_obj_t* foe_lv = nullptr;
        lv_obj_t* foe_bar = nullptr;
        lv_obj_t* foe_img = nullptr;
        lv_obj_t* me_box = nullptr;
        lv_obj_t* me_name = nullptr;
        lv_obj_t* me_lv = nullptr;
        lv_obj_t* me_bar = nullptr;
        lv_obj_t* me_nums = nullptr;
        lv_obj_t* me_img = nullptr;
        lv_obj_t* msg = nullptr;
        lv_obj_t* balls = nullptr;
        lv_obj_t* menu_rows[kMenuCount] = {};
    };

    // ── Persistence ─────────────────────────────────────────────────────────
    void load_progress() {
        FILE* f = std::fopen(path_.c_str(), "rb");
        if (!f) return;   // no save yet: fresh Lv 5 Pikachu
        auto rd = [&](auto& v) { return std::fread(&v, sizeof(v), 1, f) == 1; };
        uint32_t magic = 0, xp = 0;
        uint8_t lvl = 0;
        if (rd(magic) && rd(lvl) && rd(xp) && magic == kSaveMagic && lvl >= 1 && lvl <= 100) {
            Journey j;
            j.level = lvl;
            j.xp = xp;
            // Everything after is an optional tail (older saves stop early).
            rd(j.dex) && rd(j.beaten) && rd(j.used_ssid) && rd(j.wins) && rd(j.losses) &&
                rd(j.gyms) && rd(j.active_dex) && rd(j.active_variant) && rd(j.active_id);
            const bool starter = rd(j.starter_level) && rd(j.starter_xp) && rd(j.starter_gym) &&
                                 rd(j.starter_wins) && rd(j.starter_losses);
            if (!starter || j.starter_level == 0) {   // migrate: the old global run is the starter's
                j.starter_level = j.level;
                j.starter_xp = j.xp;
                j.starter_gym = j.gyms;
                j.starter_wins = clamp16(j.wins);
                j.starter_losses = clamp16(j.losses);
            }
            uint32_t grow = 0;
            if (rd(grow)) j.nursery_grow = grow;
            j_ = j;
        }
        std::fclose(f);
    }

    void save_progress() {
        store_live_to_active();
        if (!ctx_.game.files_enabled() || path_.empty()) return;
        if (j_.active_id != 0) ctx_.game.save_box();   // the active box mon's trip
        const std::string tmp = path_ + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return;
        auto wr = [&](const auto& v) { std::fwrite(&v, sizeof(v), 1, f); };
        wr(kSaveMagic); wr(j_.level); wr(j_.xp); wr(j_.dex); wr(j_.beaten); wr(j_.used_ssid);
        wr(j_.wins); wr(j_.losses); wr(j_.gyms); wr(j_.active_dex); wr(j_.active_variant);
        wr(j_.active_id); wr(j_.starter_level); wr(j_.starter_xp); wr(j_.starter_gym);
        wr(j_.starter_wins); wr(j_.starter_losses); wr(j_.nursery_grow);
        const bool ok = std::fflush(f) == 0;
        std::fclose(f);
        if (ok) std::rename(tmp.c_str(), path_.c_str());
        else std::remove(tmp.c_str());
    }

    void save_rare_catch(uint8_t dex, uint8_t variant, uint8_t level) {
        if (!ctx_.game.files_enabled() || rare_path_.empty()) return;
        FILE* f = std::fopen(rare_path_.c_str(), "ab");
        if (!f) return;
        const uint8_t rec[3] = {dex, variant, level};
        std::fwrite(rec, 1, sizeof rec, f);
        std::fclose(f);
    }

    // Box mons below Lv 5 grow one level per 6 AM since the last growth.
    void nursery_grow() {
        const time_t now = time(nullptr);
        if (j_.nursery_grow == 0) {   // first run: anchor, don't leap
            j_.nursery_grow = static_cast<uint32_t>(now);
            return;
        }
        const long days = nursery_day_index(now) - nursery_day_index(static_cast<time_t>(j_.nursery_grow));
        if (days <= 0) return;
        j_.nursery_grow = static_cast<uint32_t>(now);
        bool grew = false;
        for (auto& m : ctx_.game.breeding().mutableRoster()) {
            if (m.level >= kNurseryGraduateLevel) continue;
            m.level = static_cast<uint8_t>(std::min<long>(kNurseryGraduateLevel, m.level + days));
            grew = true;
        }
        if (grew && ctx_.game.files_enabled()) ctx_.game.save_box();
    }

    // ── Independent journeys: live set <-> the active mon's home ────────────
    breeding::BreedMon* active_mon() {
        return j_.active_id ? ctx_.game.breeding().findById(j_.active_id) : nullptr;
    }

    void store_live_to_active() {
        if (j_.active_id == 0) {
            j_.starter_level = j_.level;
            j_.starter_xp = j_.xp;
            j_.starter_gym = j_.gyms;
            j_.starter_wins = clamp16(j_.wins);
            j_.starter_losses = clamp16(j_.losses);
        } else if (breeding::BreedMon* m = active_mon()) {
            m->tripLevel = j_.level;
            m->tripXp = j_.xp;
            m->tripGymBeaten = j_.gyms;
            m->tripWins = clamp16(j_.wins);
            m->tripLosses = clamp16(j_.losses);
        }
    }

    void load_active_to_live() {
        if (j_.active_id == 0) {
            if (j_.starter_level == 0) {
                j_.starter_level = 5;
                j_.starter_xp = 0;
                j_.starter_gym = j_.starter_wins = j_.starter_losses = 0;
            }
            j_.level = j_.starter_level;
            j_.xp = j_.starter_xp;
            j_.gyms = static_cast<uint8_t>(j_.starter_gym);
            j_.wins = j_.starter_wins;
            j_.losses = j_.starter_losses;
        } else if (breeding::BreedMon* m = active_mon()) {
            if (m->tripLevel == 0) {   // first outing: start from its catch level
                m->tripLevel = std::max<uint8_t>(5, m->level);
                m->tripXp = 0;
                m->tripGymBeaten = m->tripWins = m->tripLosses = 0;
            }
            j_.level = m->tripLevel;
            j_.xp = m->tripXp;
            j_.gyms = static_cast<uint8_t>(m->tripGymBeaten);
            j_.wins = m->tripWins;
            j_.losses = m->tripLosses;
        }
    }

    void activate(uint32_t id) {
        store_live_to_active();
        if (id == 0) {
            j_.active_id = 0;
            j_.active_dex = 0;
            j_.active_variant = 0;
        } else if (breeding::BreedMon* m = ctx_.game.breeding().findById(id)) {
            j_.active_id = m->id;
            j_.active_dex = m->dex;
            j_.active_variant = static_cast<uint8_t>(breeding::skinOf(m->geno));
        } else {
            return;
        }
        load_active_to_live();
    }

    // On entry: box ids are handed out in load order, so make sure the saved
    // id still names the same mon; otherwise look for it, else the starter.
    void sync_active() {
        if (j_.active_id != 0) {
            breeding::BreedMon* m = active_mon();
            if (!m || m->dex != j_.active_dex) {
                m = nullptr;
                for (auto& o : ctx_.game.breeding().mutableRoster())
                    if (o.dex == j_.active_dex && static_cast<uint8_t>(breeding::skinOf(o.geno)) == j_.active_variant) {
                        m = &o;
                        break;
                    }
                j_.active_id = m ? m->id : 0;
                if (!m) { j_.active_dex = 0; j_.active_variant = 0; }
            }
        }
        store_live_to_active();
        load_active_to_live();
    }

    // ── Battler ─────────────────────────────────────────────────────────────
    bool starter() const { return j_.active_dex == 0; }
    int battler_dex() const {
        if (!starter()) return gen1FinalFormAtLevel(j_.active_dex, j_.level);
        return j_.level >= kPikachuEvolveLevel ? 26 : 25;
    }
    void battler_moves(uint8_t out[4]) const {
        if (starter()) {
            pika_moves_for_level(j_.level, out);
            return;
        }
        const uint8_t dex = static_cast<uint8_t>(battler_dex());
        uint8_t all[32];
        uint8_t n = gen1MovesLearnedBetween(dex, 0, j_.level, all, 32);
        if (n == 0 && dex != j_.active_dex) n = gen1MovesLearnedBetween(j_.active_dex, 0, j_.level, all, 32);
        out[0] = out[1] = out[2] = out[3] = 0;
        for (int i = n > 4 ? n - 4 : 0, k = 0; i < n; ++i) out[k++] = all[i];
        if (out[0] == 0) out[0] = 33;   // Tackle
    }
    std::string battler_name() const { return title_case(species_name(battler_dex())); }

    // ── Spots ───────────────────────────────────────────────────────────────
    void refresh_spots(bool force) {
        const uint32_t now = lv_tick_get();
        if (!force && spots_at_ != 0 && now - spots_at_ < kSpotCacheMs && !spots_.empty()) return;
        spots_ = wifi::load_spots(&spots_demo_);
        spots_at_ = now ? now : 1;
    }

    static void spot_id(const wifi::Spot& s, uint8_t out[6]) {
        bool zero = true;
        for (int i = 0; i < 6; ++i) zero = zero && s.bssid[i] == 0;
        if (!zero) {
            std::memcpy(out, s.bssid, 6);
            return;
        }
        uint32_t h = 2166136261u;   // no id: FNV-1a of the name, like mmterm's demo list
        for (char ch : s.ssid) { h ^= static_cast<uint8_t>(ch); h *= 16777619u; }
        const uint8_t b[6] = {uint8_t(h), uint8_t(h >> 8), uint8_t(h >> 16), uint8_t(h >> 24),
                              uint8_t(h >> 4), uint8_t(h >> 12)};
        std::memcpy(out, b, 6);
    }

    int rnd(int n) { return n > 0 ? static_cast<int>(rng_() % static_cast<uint32_t>(n)) : 0; }

    // ── Battle setup ────────────────────────────────────────────────────────
    void start_battle() {
        save_progress();
        catch_toast_.clear();
        if (rematch_ && have_spot_) {
            rematch_ = false;
        } else {
            rematch_ = false;
            refresh_spots(false);
            if (spots_.empty()) {
                enter_standby();
                return;
            }
            spot_ = spots_[static_cast<size_t>(rnd(static_cast<int>(spots_.size())))];
            have_spot_ = true;
        }

        const int gyms_beaten = popcount(j_.gyms);
        std::vector<int> zones;
        for (int i = 0; i < KANTO_ZONE_COUNT; ++i)
            if (KANTO_ZONES[i].unlockedAfter <= gyms_beaten && KANTO_ZONES[i].wildCount > 0) zones.push_back(i);
        if (zones.empty()) zones.push_back(0);

        uint8_t foe_dex = 25, foe_lv = j_.level;
        uint8_t moves[4] = {0, 0, 0, 0};
        std::string zone, trainer;
        int gym_idx = 255;
        if (rnd(100) < 85) {
            const KantoZone& z = KANTO_ZONES[zones[static_cast<size_t>(rnd(static_cast<int>(zones.size())))]];
            const KantoWildMon* wm = &z.wilds[rnd(z.wildCount)];
            const uint16_t perm = rarity_permille(wm->dex);
            if (perm != 0 && rnd(1000) >= perm) {
                for (int t = 0; t < 8; ++t) {
                    const KantoWildMon* c = &z.wilds[rnd(z.wildCount)];
                    if (rarity_permille(c->dex) == 0) { wm = c; break; }
                }
            }
            uint8_t overlay = 0;
            if (rnd(100) < 2) {
                const uint8_t d = kRarePool[rnd(static_cast<int>(sizeof kRarePool))];
                uint16_t op = rarity_permille(d);
                if (op == 0) op = 50;
                if (rnd(1000) < op) overlay = d;
            }
            foe_dex = overlay ? overlay : wm->dex;
            int lo = std::max(2, j_.level / 2), hi = std::min(100, j_.level + 1);
            if (hi < lo + 1) hi = std::min(100, lo + 1);
            foe_lv = static_cast<uint8_t>(std::clamp(lo + rnd(hi - lo + 1), 2, 100));
            moves[0] = 1;    // Pound
            moves[1] = 33;   // Tackle
            if (foe_lv >= 10) moves[2] = 45;   // Growl
            zone = z.name;
        } else {
            const int g_idx = std::min(gyms_beaten, LORD_GYM_COUNT - 1);
            const LordGym* g = lordGym(static_cast<uint8_t>(g_idx));
            const LordGymMon* mon = nullptr;
            bool leader = false;
            if (g) {
                const uint8_t li = lordGymLeaderIndex(g);
                uint8_t t = (li == 0 || rnd(100) < 40) ? li : static_cast<uint8_t>(rnd(li));
                if (!g->trainers[t].party || g->trainers[t].count == 0) t = li;
                const LordGymTrainer& tr = g->trainers[t];
                leader = t == li;
                trainer = tr.name ? tr.name : "TRAINER";
                if (tr.party && tr.count > 0) mon = &tr.party[rnd(tr.count)];
            }
            if (mon) {
                foe_dex = mon->species;
                foe_lv = mon->level ? mon->level : j_.level;
                std::memcpy(moves, mon->moves, 4);
            } else {
                moves[0] = 84;
            }
            zone = std::string(g ? g->city : "Kanto") + " Gym";
            gym_idx = leader ? g_idx : 255;
        }

        uint8_t id[6];
        spot_id(spot_, id);
        const breeding::Genotype geno = wifi_enc::rollWildGenotype(id, foe_dex);
        setup_battle(foe_dex, foe_lv, moves, geno, zone, trainer, gym_idx);
        if (built_ && view_ == View::Battle) ctx_.router.rebuild();
    }

    // Builds the fight and queues its opening lines. moves == nullptr picks
    // the wild default set.
    void setup_battle(int foe_dex, int foe_lv, const uint8_t* moves, const breeding::Genotype& geno,
                      const std::string& zone, const std::string& trainer, int gym_idx) {
        phase_ = Phase::Fight;
        result_ = Eng::Result::ONGOING;
        catching_ = false;
        tallied_ = false;
        balls_ = 0;
        queue_.clear();
        building_.clear();
        down_[0] = down_[1] = true;
        zone_ = zone;
        trainer_ = upper(trainer);
        battle_gym_ = gym_idx;
        foe_dex_ = static_cast<uint8_t>(foe_dex);
        foe_geno_ = geno;
        foe_skin_ = static_cast<int>(breeding::skinOf(geno));
        foe_tritan_ = breeding::isTritan(geno);
        want_catch_ = want_cosmetic();
        bit_set(j_.dex, foe_dex);

        my_skin_ = starter() ? 0 : j_.active_variant;
        breeding::BreedMon* am = active_mon();
        my_tritan_ = am && breeding::isTritan(am->geno);

        Gen1Party none;
        std::memset(&none, 0, sizeof none);
        eng_.start(none, none, static_cast<uint32_t>(rng_()), 1);

        auto& me = eng_.party(0);
        std::memset(&me, 0, sizeof me);
        me.count = 1;
        uint8_t mv[4];
        battler_moves(mv);
        Eng::initBattlePokeFromBase(me.mons[0], static_cast<uint16_t>(battler_dex()), j_.level, mv, 1);
        set_nick(me.mons[0], species_name(battler_dex()));

        auto& op = eng_.party(1);
        std::memset(&op, 0, sizeof op);
        op.count = 1;
        uint8_t fm[4] = {1, 33, 0, 0};
        if (moves) std::memcpy(fm, moves, 4);
        else if (foe_lv >= 10) fm[2] = 45;
        Eng::initBattlePokeFromBase(op.mons[0], static_cast<uint16_t>(foe_dex),
                                    static_cast<uint8_t>(foe_lv), fm, 1);
        zero_dvs(op.mons[0]);
        set_nick(op.mons[0], species_name(foe_dex));

        const std::string place = wifi::place_name(spot_);
        const std::string foe_name = op.mons[0].nickname;
        const char* skin = foe_skin_ != breeding::SKIN_REGULAR ? breeding::skinName(breeding::Skin(foe_skin_)) : nullptr;
        shown_ = take();
        if (!trainer_.empty()) {
            push(trainer_ + " wants to battle!");
            push(trainer_ + " sent out " + foe_name + "!", 1);
        } else {
            push("Near " + place + "...");
            push(skin ? std::string("A wild ") + skin + " " + foe_name + " appeared!"
                      : "A wild " + foe_name + " appeared!", 1);
        }
        push("Go! " + std::string(me.mons[0].nickname) + "!", 0);
        finalize();
    }

    void enter_standby() {
        phase_ = Phase::Standby;
        queue_.clear();
        building_.clear();
        scan_at_ = lv_tick_get();
        if (built_ && view_ == View::Battle) ctx_.router.rebuild();
    }

    // ── One beat of the auto-fight ──────────────────────────────────────────
    void beat() {
        if (try_catch()) return;
        run_turn();
    }

    void run_turn() {
        auto& pp = eng_.party(0);
        auto& ep = eng_.party(1);
        uint8_t a = 0, i = 0;
        eng_.cpuPickAction(0, a, i);
        eng_.submitAction(0, a, i);
        eng_.cpuPickAction(1, a, i);
        eng_.submitAction(1, a, i);
        const uint16_t pre = ep.mons[0].hp;
        eng_.executeTurn(&WifiScreen::log_cb, this);

        if (pre > 0 && ep.mons[0].hp == 0) {
            const uint8_t dex = static_cast<uint8_t>(ep.mons[0].species);
            uint32_t xp = gen1XpYield(dex, ep.mons[0].level, true) / 10;
            if (xp < 1) xp = 1;
            bit_set(j_.beaten, dex);
            bit_set(j_.dex, dex);
            Eng::BattlePoke& mon = pp.mons[0];
            push(fmt("%s gained %u EXP!", mon.nickname, static_cast<unsigned>(xp)));
            j_.xp += xp;
            while (mon.level < 100) {
                const uint32_t L = mon.level;
                const uint32_t need = (L + 1) * (L + 1) * (L + 1) - L * L * L;
                if (j_.xp < need) break;
                j_.xp -= need;
                const uint8_t nl = static_cast<uint8_t>(mon.level + 1);
                const uint16_t old_max = mon.maxHp;
                mon.maxHp = static_cast<uint16_t>(uint32_t(mon.maxHp) * nl / mon.level);
                mon.atk = static_cast<uint16_t>(uint32_t(mon.atk) * nl / mon.level);
                mon.def = static_cast<uint16_t>(uint32_t(mon.def) * nl / mon.level);
                mon.spd = static_cast<uint16_t>(uint32_t(mon.spd) * nl / mon.level);
                mon.spc = static_cast<uint16_t>(uint32_t(mon.spc) * nl / mon.level);
                if (mon.maxHp > old_max) mon.hp = static_cast<uint16_t>(mon.hp + (mon.maxHp - old_max));
                mon.level = nl;
                const int before = battler_dex();
                j_.level = nl;
                push(fmt("%s grew to Lv%u!", mon.nickname, static_cast<unsigned>(nl)));
                // New moves and evolutions take effect from the next fight.
                if (starter()) {
                    for (const PikaLearn& e : kPikaLearnset)
                        if (e.level == nl) push("Learned " + move_name(e.move) + "!");
                } else {
                    uint8_t got[8];
                    const uint8_t n = gen1MovesLearnedBetween(static_cast<uint8_t>(battler_dex()),
                                                              static_cast<uint8_t>(nl - 1), nl, got, 8);
                    for (uint8_t k = 0; k < n; ++k) push("Learned " + move_name(got[k]) + "!");
                }
                if (battler_dex() != before)
                    push(upper(species_name(before)) + " evolved into " + upper(species_name(battler_dex())) + "!");
            }
        }
        result_ = eng_.result();
        finalize();
    }

    // A wild mon that LOOKS different (a colour skin, or a Tritan's tint) and
    // isn't in the box yet. Only what shows counts: a male carrying Pink, or any
    // hidden defect, looks regular and is just battled. The genotype itself is
    // rolled with the usual rarities (WifiEncounters.h GD_*), so a male caught
    // for another look may still carry Pink by chance.
    bool want_cosmetic() const {
        if (!trainer_.empty()) return false;               // a trainer's mon is not up for grabs
        if (foe_skin_ == breeding::SKIN_REGULAR && !foe_tritan_) return false;
        for (const auto& owned : ctx_.game.breeding().roster())
            if (owned.dex == foe_dex_ && static_cast<int>(breeding::skinOf(owned.geno)) == foe_skin_ &&
                breeding::isTritan(owned.geno) == foe_tritan_)
                return false;                              // already have this look: just battle it
        return true;
    }

    // Would this beat's attacks knock the foe out? Runs the turn on a copy
    // (same RNG state), so when it says no, the real turn plays out the same.
    bool next_turn_faints_foe() const {
        Eng sim = eng_;
        uint8_t a = 0, i = 0;
        sim.cpuPickAction(0, a, i);
        sim.submitAction(0, a, i);
        sim.cpuPickAction(1, a, i);
        sim.submitAction(1, a, i);
        sim.executeTurn([](const char*, void*) {}, nullptr);
        return sim.party(1).mons[0].hp == 0;
    }

    // Cosmetic you don't own yet: wear it down to 25% HP, or until the next hit
    // would knock it out, then throw balls.
    bool try_catch() {
        if (!want_catch_) return false;
        Eng::BattlePoke& foe = eng_.party(1).mons[0];
        if (foe.hp == 0 || foe.maxHp == 0) return false;
        std::string what = title_case(species_name(foe_dex_));
        if (foe_skin_ != breeding::SKIN_REGULAR)
            what = std::string(breeding::skinName(breeding::Skin(foe_skin_))) + " " + what;
        if (foe_tritan_) what = "Tritan " + what;
        if (!catching_) {
            if (uint32_t(foe.hp) * 4 > foe.maxHp && !next_turn_faints_foe()) return false;
            catching_ = true;
            push("A " + what + "! " + battler_name() + " throws a Poke Ball!");
        } else {
            push(battler_name() + " throws a Poke Ball!");
        }
        ++balls_;
        const uint16_t p = wifi_enc::catchPermille(foe.hp, foe.maxHp, false, false);
        if (rnd(1000) < p) {
            push("Gotcha! The " + what + " was caught!");
            breeding::BreedMon m{};
            m.dex = foe_dex_;
            m.level = foe.level;
            m.geno = foe_geno_;
            m.prov = breeding::PROV_WILD;
            m.caughtSec = static_cast<uint32_t>(time(nullptr));
            std::strncpy(m.nick, breeding::BreedingApp::dexName(foe_dex_), sizeof(m.nick) - 1);
            ctx_.game.breeding().add(m);
            const bool saved = ctx_.game.files_enabled() && ctx_.game.save_box();
            save_rare_catch(foe_dex_, static_cast<uint8_t>(foe_skin_), foe.level);
            catch_toast_ = what + (saved ? " went to Bill's PC!" : " is in Bill's PC (not saved)");
            result_ = Eng::Result::P1_WIN;
        } else {
            push("Aww! It broke free!");
        }
        finalize();
        return true;
    }

    // Result shown and read: tally once, badge for a beaten leader, save.
    void finish_battle() {
        if (tallied_) return;
        tallied_ = true;
        if (result_ == Eng::Result::P1_WIN) {
            ++j_.wins;
            if (battle_gym_ < 8 && !(j_.gyms >> battle_gym_ & 1)) {
                j_.gyms |= static_cast<uint8_t>(1u << battle_gym_);
                const LordGym* g = lordGym(static_cast<uint8_t>(battle_gym_));
                toast(fmt("%s beat %s! Badges %d/8", battler_name().c_str(),
                          g ? g->leaderName : "the leader", popcount(j_.gyms)),
                      3000, color::kAccent);
            }
        } else if (result_ == Eng::Result::P2_WIN) {
            ++j_.losses;
        }
        if (!catch_toast_.empty()) toast(catch_toast_, 3000, color::kGood);
        save_progress();
    }

    // ── Messages ────────────────────────────────────────────────────────────
    SideSnap take_side(int s) const {
        SideSnap o;
        const auto& bp = eng_.party(static_cast<uint8_t>(s));
        if (bp.count == 0) return o;
        const auto& m = bp.mons[bp.active];
        o.shown = !down_[s];
        o.dex = m.species;
        o.name = m.nickname;
        o.level = m.level;
        o.hp = m.hp;
        o.max_hp = std::max<int>(1, m.maxHp);
        return o;
    }
    Snap take() const { return Snap{take_side(0), take_side(1), balls_}; }

    // Each message shows the state as of the next message (after its effect).
    void push(const std::string& text, int reveal = -1, int hide = -1) {
        if (!building_.empty()) building_.back().snap = take();
        if (reveal >= 0) down_[reveal] = false;
        if (hide >= 0) down_[hide] = true;
        building_.push_back({text, take()});
    }
    void finalize() {
        if (!building_.empty()) building_.back().snap = take();
        for (auto& m : building_) queue_.push_back(std::move(m));
        building_.clear();
    }
    void pop_message() {
        if (queue_.empty()) return;
        Msg m = std::move(queue_.front());
        queue_.pop_front();
        cur_text_ = m.text;
        shown_ = m.snap;
    }
    void drain() {
        while (!queue_.empty()) pop_message();
    }

    static void log_cb(const char* line, void* ctx) {
        auto* self = static_cast<WifiScreen*>(ctx);
        const std::string text = ascii(line);
        int hide = -1;
        if (ends_with(text, " fainted!"))
            for (int s = 0; s < 2; ++s)
                if (self->eng_.party(static_cast<uint8_t>(s)).mons[0].hp == 0) { hide = s; break; }
        self->push(text, -1, hide);
    }

    // ── Views ───────────────────────────────────────────────────────────────
    void show(View v) {
        view_ = v;
        if (v == View::Battle) resume_ = true;
        ctx_.router.rebuild();
    }

    void build_battle(lv_obj_t* body) {
        if (phase_ == Phase::Standby) {
            build_standby(body);
            return;
        }
        auto shadow = [&](int x, int y, int w) {
            lv_obj_t* s = box(body, x, y, w, 10);
            lv_obj_set_style_bg_color(s, c(0x232b35), 0);
            lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        };
        shadow(kFoeSpriteX - 8, 50, 80);
        shadow(kMeSpriteX - 10, 88, 84);
        refs_.foe_img = sprite_box(body, std::max(1, shown_.foe.dex), kFoeSpriteX, kFoeSpriteY, 64, 64, false,
                                   foe_skin_, foe_tritan_, SpriteFit::Frame);
        refs_.me_img = sprite_box(body, std::max(1, shown_.me.dex), kMeSpriteX, kMeSpriteY, 64, 64, true,
                                  my_skin_, my_tritan_, SpriteFit::Frame);

        refs_.foe_box = panel(body, kFoeBoxX, kFoeBoxY, kBoxW, kFoeBoxH);
        refs_.foe_name = label(refs_.foe_box, "", 6, 2, font_body(), color::kText, 100);
        refs_.foe_lv = label(refs_.foe_box, "", 112, 4, font_small(), color::kDim, 36);
        label(refs_.foe_box, "HP", 6, 18, font_small(), color::kAccent);
        refs_.foe_bar = hp_bar(refs_.foe_box, 24, 22, kBoxW - 32, 1, 1);

        refs_.me_box = panel(body, kMeBoxX, kMeBoxY, kBoxW, kMeBoxH);
        refs_.me_name = label(refs_.me_box, "", 6, 2, font_body(), color::kText, 100);
        refs_.me_lv = label(refs_.me_box, "", 112, 4, font_small(), color::kDim, 36);
        label(refs_.me_box, "HP", 6, 18, font_small(), color::kAccent);
        refs_.me_bar = hp_bar(refs_.me_box, 24, 22, kBoxW - 32, 1, 1);
        refs_.me_nums = label(refs_.me_box, "", 24, 25, font_small(), color::kText, kBoxW - 32);
        lv_obj_set_style_text_align(refs_.me_nums, LV_TEXT_ALIGN_RIGHT, 0);

        // Where: a skin pill for a rare foe, then the zone, under the foe box.
        int x = kZoneX;
        if (foe_skin_ != breeding::SKIN_REGULAR) {
            lv_obj_t* ch = chip(body, breeding::skinName(breeding::Skin(foe_skin_)), x, kZoneY,
                                skin_color(foe_skin_));
            lv_obj_update_layout(ch);
            x += lv_obj_get_width(ch) + 4;
        }
        lv_obj_t* zl = label(body, zone_, x, kZoneY + 1, font_small(), color::kDim, kZoneX + kZoneW - x);
        lv_obj_set_style_text_align(zl, LV_TEXT_ALIGN_LEFT, 0);
        refs_.balls = label(body, "", kZoneX, kZoneY + 13, font_small(), color::kWarn, 64);

        lv_obj_t* strip = panel(body, 4, kStripY, kBodyW - 8, kStripH);
        refs_.msg = text_block(strip, cur_text_, 6, 3, kBodyW - 22, font_body());
        apply_snap();
    }

    void apply_snap() {
        if (!refs_.msg) return;
        set_text(refs_.msg, cur_text_);
        auto side = [](lv_obj_t* img, lv_obj_t* bx, lv_obj_t* name, lv_obj_t* lv, lv_obj_t* bar,
                       const SideSnap& s) {
            if (s.shown) lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
            if (s.shown || (s.hp == 0 && s.dex > 0)) lv_obj_remove_flag(bx, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(bx, LV_OBJ_FLAG_HIDDEN);
            set_text(name, s.name);
            set_text(lv, fmt("Lv%d", s.level));
            hp_bar_set(bar, s.hp, s.max_hp, true);
        };
        side(refs_.foe_img, refs_.foe_box, refs_.foe_name, refs_.foe_lv, refs_.foe_bar, shown_.foe);
        side(refs_.me_img, refs_.me_box, refs_.me_name, refs_.me_lv, refs_.me_bar, shown_.me);
        set_text(refs_.me_nums, fmt("%d/%d", shown_.me.hp, shown_.me.max_hp));
        set_text(refs_.balls, shown_.balls > 0 ? fmt("Balls x%d", shown_.balls) : std::string());
    }

    void build_standby(lv_obj_t* body) {
        lv_obj_t* p = panel(body, 12, 14, kBodyW - 24, 110);
        sprite_box(p, battler_dex(), 8, 22, 64, 64, false, my_skin_, my_tritan_);
        label(p, battler_name() + fmt(" Lv%d", j_.level), 84, 22, font_title(), color::kText);
        label(p, "Looking for a network...", 84, 46, font_body(), color::kDim);
        label(p, "None nearby right now.", 84, 66, font_small(), color::kDim);
        label(p, "Trying again every few seconds.", 84, 80, font_small(), color::kDim);
    }

    void build_menu(lv_obj_t* body) {
        constexpr int kRowH = 22, kW = 124;
        lv_obj_t* p = panel(body, kBodyW - kW - 6, 6, kW, kMenuCount * kRowH + 8);
        lv_obj_set_style_border_color(p, c(color::kAccent), 0);
        for (int i = 0; i < kMenuCount; ++i) {
            lv_obj_t* r = box(p, 4, 4 + i * kRowH, kW - 10, kRowH - 2);
            lv_obj_set_style_radius(r, 4, 0);
            std::string text = kMenuItems[i];
            if (i == 3 && !starter()) text = "Reset " + title_case(species_name(j_.active_dex));
            label(r, text, 8, 3, font_body(), i == 3 ? color::kBad : color::kText, kW - 26);
            refs_.menu_rows[i] = r;
        }
        paint_menu();
    }
    void paint_menu() {
        for (int i = 0; i < kMenuCount; ++i) {
            lv_obj_t* r = refs_.menu_rows[i];
            if (!r) continue;
            lv_obj_set_style_bg_color(r, c(color::kPanelHi), 0);
            lv_obj_set_style_bg_opa(r, i == menu_sel_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }

    void menu_pick() {
        switch (menu_sel_) {
            case 0: show(View::Battle); break;
            case 1: show(View::Status); break;
            case 2: pc_sel_ = 0; show(View::Pc); break;
            case 3: {
                const std::string who = starter() ? "PIKACHU" : upper(species_name(j_.active_dex));
                modal_ = std::make_unique<Modal>(
                    "Reset " + title_case(who) + "?",
                    "Back to Lv 5 with no badges, an empty Pokedex and a 0-0 record. "
                    "Bill's PC is kept.",
                    std::vector<std::string>{"Yes, reset all", "Cancel"},
                    [this](int choice) {
                        if (choice == 0) reset_journey();
                        ctx_.router.rebuild();
                    });
                ctx_.router.rebuild();
                break;
            }
        }
    }

    void reset_journey() {
        // Same as mmterm: the run starts over with the default Pikachu.
        const uint32_t grow = j_.nursery_grow;
        j_ = Journey{};
        j_.nursery_grow = grow;
        store_live_to_active();   // starter = fresh Lv 5
        rematch_ = false;
        view_ = View::Battle;
        save_progress();
        toast("A fresh Lv 5 PIKACHU sets out!", 2200, color::kAccent);
        start_battle();
    }

    // Status: the journey so far and the next gym.
    void build_status(lv_obj_t* body) {
        const int gyms = popcount(j_.gyms);
        const int lvl = j_.level;
        lv_obj_t* l = panel(body, 4, 4, 156, kBodyH - 8);
        sprite_box(l, battler_dex(), 112, 2, 40, 40, false, my_skin_, my_tritan_);
        label(l, upper(battler_name()), 6, 4, font_title(), color::kText, 104);
        label(l, fmt("Lv %d", lvl), 6, 22, font_body(), color::kAccent);
        const uint32_t need = lvl >= 100 ? 1 : uint32_t((lvl + 1) * (lvl + 1) * (lvl + 1) - lvl * lvl * lvl);
        lv_obj_t* xb = bar(l, 6, 40, 100, 4, c(color::kInfo));
        bar_set(xb, static_cast<int>(std::min(j_.xp, need)), static_cast<int>(need));
        label(l, lvl >= 100 ? std::string("Max level") : fmt("EXP %u/%u", unsigned(j_.xp), unsigned(need)),
              6, 46, font_small(), color::kDim);

        const KantoZone* where = &KANTO_ZONES[0];
        for (int i = 0; i < KANTO_ZONE_COUNT; ++i)
            if (KANTO_ZONES[i].unlockedAfter <= gyms && KANTO_ZONES[i].minPikaLvl <= lvl) where = &KANTO_ZONES[i];
        const uint32_t games = j_.wins + j_.losses;
        int y = 62;
        auto row = [&](const char* k, const std::string& v) {
            label(l, k, 6, y, font_small(), color::kDim);
            label(l, v, 48, y, font_small(), color::kText, 104);
            y += 13;
        };
        row("Area", where->name);
        row("Record", fmt("%u-%u (%u%%)", unsigned(j_.wins), unsigned(j_.losses),
                          games ? unsigned(j_.wins * 100 / games) : 0u));
        row("Dex", fmt("%d seen, %d beaten", bit_count(j_.dex), bit_count(j_.beaten)));
        row("PC", fmt("%zu mons", ctx_.game.breeding().size()));

        lv_obj_t* r = panel(body, 164, 4, kBodyW - 168, kBodyH - 8);
        label(r, fmt("BADGES %d/8", gyms), 6, 4, font_small(), color::kDim);
        for (int g = 0; g < 8; ++g) {
            lv_obj_t* pip = box(r, 74 + g * 9, 6, 7, 7);
            lv_obj_set_style_radius(pip, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(pip, c((j_.gyms >> g & 1) ? color::kAccent : color::kBorder), 0);
        }
        const int next = std::min(gyms, LORD_GYM_COUNT - 1);
        const LordGym* g = lordGym(static_cast<uint8_t>(next));
        if (!g) return;
        label(r, gyms >= 8 ? "ALL BADGES! REMATCH" : "NEXT GYM", 6, 22, font_small(), color::kDim);
        label(r, fmt("%s - %s", g->city, g->leaderName), 6, 34, font_body(), color::kText, kBodyW - 180);
        const LordGymTrainer& ld = g->trainers[lordGymLeaderIndex(g)];
        const int n = std::min<int>(ld.count, 6);
        for (int i = 0; i < n; ++i) {
            const int cx = 6 + (i % 3) * 46, cy = 52 + (i / 3) * 38;
            sprite_box(r, ld.party[i].species, cx, cy, 28, 28);
            label(r, fmt("L%d", ld.party[i].level), cx + 29, cy + 16, font_small(), color::kDim);
        }
    }

    // Bill's PC: pick the battler. Row 0 is the starter Pikachu.
    void build_pc(lv_obj_t* body) {
        const auto& roster = ctx_.game.breeding().roster();
        label(body, fmt("BILL'S PC  -  %zu mons  -  pick who battles", roster.size()), 6, 2,
              font_small(), color::kDim);
        pc_list_ = std::make_unique<ListView>(
            body, 4, 16, kBodyW - 8, kBodyH - 18, 30, [this](lv_obj_t* row, int idx, bool) {
                const auto& ro = ctx_.game.breeding().roster();
                int dex = 25, skin = 0, lvl = 0, trip = 0;
                bool tritan = false, active = false;
                std::string name, note;
                if (idx == 0) {
                    const uint8_t sl = j_.active_id == 0 ? j_.level : j_.starter_level;
                    dex = sl >= kPikachuEvolveLevel ? 26 : 25;
                    name = title_case(species_name(dex)) + " (starter)";
                    trip = sl ? sl : 5;
                    active = j_.active_id == 0;
                } else {
                    const breeding::BreedMon& m = ro[static_cast<size_t>(idx - 1)];
                    dex = m.dex;
                    skin = static_cast<int>(breeding::skinOf(m.geno));
                    tritan = breeding::isTritan(m.geno);
                    lvl = m.level;
                    active = m.id == j_.active_id;
                    trip = active ? j_.level : m.tripLevel;
                    name = title_case(species_name(dex));
                    if (m.level < kNurseryGraduateLevel) note = fmt("nursery Lv%d/%d", m.level, kNurseryGraduateLevel);
                }
                sprite_box(row, dex, 2, 1, 28, 28, false, skin, tritan);
                label(row, name, 34, 2, font_body(), color::kText, 120);
                int x = 34;
                if (skin != 0) {
                    lv_obj_t* ch = chip(row, breeding::skinName(breeding::Skin(skin)), x, 16, skin_color(skin));
                    lv_obj_update_layout(ch);
                    x += lv_obj_get_width(ch) + 4;
                }
                std::string info = note;
                if (info.empty()) {
                    if (trip) info = fmt("journey Lv%d", trip);
                    else info = fmt("caught Lv%d, not out yet", lvl);
                }
                label(row, info, x, 17, font_small(), color::kDim, 150);
                if (active) chip(row, "BATTLER", kBodyW - 80, 8, c(color::kAccent));
            });
        pc_list_->set_count(static_cast<int>(roster.size()) + 1);
        pc_list_->select(std::min(pc_sel_, pc_list_->count() - 1));
    }

    void pc_pick() {
        if (!pc_list_) return;
        const int idx = pc_list_->selected();
        pc_sel_ = idx;
        uint32_t id = 0;
        std::string name = "PIKACHU";
        if (idx > 0) {
            const auto& ro = ctx_.game.breeding().roster();
            if (static_cast<size_t>(idx - 1) >= ro.size()) return;
            const breeding::BreedMon& m = ro[static_cast<size_t>(idx - 1)];
            name = upper(species_name(m.dex));
            if (m.dex < 1 || m.dex > 151) {
                toast(name + " can't battle here.", 2000, color::kWarn);
                return;
            }
            if (m.level < kNurseryGraduateLevel) {
                toast(fmt("%s is in the nursery (Lv%d/%d).", name.c_str(), m.level, kNurseryGraduateLevel),
                      2200, color::kWarn);
                return;
            }
            id = m.id;
        }
        if (id == j_.active_id) {
            toast(name + " is already your battler.", 1600, color::kDim);
            return;
        }
        activate(id);
        save_progress();
        toast(name + " is now your battler!", 2200, color::kGood);
        // The current fight is dropped (not counted); the new battler starts fresh.
        rematch_ = false;
        view_ = View::Battle;
        resume_ = true;
        start_battle();
        ctx_.router.rebuild();
    }

    // ── State ───────────────────────────────────────────────────────────────
    std::mt19937 rng_;
    std::string path_, rare_path_;
    Journey j_;

    Eng eng_;
    Eng::Result result_ = Eng::Result::ONGOING;
    Phase phase_ = Phase::Fight;
    View view_ = View::Battle;
    bool catching_ = false, tallied_ = false, rematch_ = false, resume_ = false;
    bool built_ = false;   // on the stack: rebuild() would hit another screen before
    int balls_ = 0;
    int battle_gym_ = 255;
    uint8_t foe_dex_ = 0;
    breeding::Genotype foe_geno_{};
    int foe_skin_ = 0, my_skin_ = 0;
    bool foe_tritan_ = false, my_tritan_ = false;
    bool want_catch_ = false;   // this foe is a cosmetic not yet in the box
    std::string zone_, trainer_, catch_toast_;
    bool down_[2] = {true, true};

    std::vector<wifi::Spot> spots_;
    bool spots_demo_ = true;
    uint32_t spots_at_ = 0;
    wifi::Spot spot_;
    bool have_spot_ = false;

    std::deque<Msg> queue_;
    std::vector<Msg> building_;
    Snap shown_;
    std::string cur_text_;
    uint32_t next_at_ = 0, linger_until_ = 0, scan_at_ = 0;

    int menu_sel_ = 0, pc_sel_ = 0;
    std::unique_ptr<Modal> modal_;
    std::unique_ptr<ListView> pc_list_;
    Refs refs_;
};

} // namespace

std::unique_ptr<Screen> make_wifi_screen(Ctx& ctx) { return std::make_unique<WifiScreen>(ctx); }

} // namespace mm::ui
