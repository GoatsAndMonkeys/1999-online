// party_screen.cpp — the player's party (up to 6) from GameState::party().
//
// Two views:
//   List  : party list on the left (sprite, nickname, Lv, HP), detail pane on
//           the right for the selected mon (sprite, species, types, HP, Gen 1
//           stats, EXP to next level).
//   Moves : the selected mon's 4 moves as cards (type, power, accuracy, PP,
//           effect). Up/Down flips through the party.
// An empty party shows a friendly empty state. Data changes (revision bumps)
// rebuild the body when the party actually changed.
#include "screens.h"

#include "game_state.h"
#include "router.h"
#include "widgets.h"

// Read-only shared game data (monster_mesh_pi/src/{shared,battle}). These
// headers define file-static tables/functions we only partly use.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "DaycareSavPatcher.h"         // speciesBaseStats[] (pret), calcStat(), expForLevel()
#include "showdown_gen1_moves.h"       // gen1Move()
#include "showdown_gen3_basestats.h"   // GEN3_BASE_STATS (types; Gen 2/3 stats)
#include "showdown_gen3_moves.h"       // gen3Move() for Gen 2/3 move ids
#pragma GCC diagnostic pop

#include <algorithm>
#include <memory>
#include <vector>

namespace mm::ui {
namespace {

// ── Data helpers ────────────────────────────────────────────────────────────

constexpr int kTypeDark = 16, kTypeSteel = 17;   // Gen 3 type ids beyond Gen 1's 16

// Types for a dex number. GEN1_BASE_STATS is polluted with alternate formes
// (Alola/Galar/Mega...: e.g. Vulpix shows as Ice), so take the original-form
// types from the Gen 3 table and drop the Gen 2+ Steel/Dark for Gen 1 mons
// (only Magnemite/Magneton are affected).
void mon_types(int dex, int& t1, int& t2) {
    t1 = t2 = -1;
    if (dex < 1 || dex > 386) return;
    t1 = GEN3_BASE_STATS[dex].type1;
    t2 = GEN3_BASE_STATS[dex].type2;
    if (dex <= 151) {
        if (t1 >= 16) t1 = t2;
        if (t2 >= 16) t2 = t1;
    }
    if (t2 == t1) t2 = -1;
}

lv_color_t chip_color(int t) {
    if (t == kTypeDark) return c(0x705848);
    if (t == kTypeSteel) return c(0xb8b8d0);
    return type_color(t);
}

const char* chip_name(int t) {
    if (t == kTypeDark) return "DARK";
    if (t == kTypeSteel) return "STEEL";
    return type_name(t);
}

// Type pill with a readable text colour on dark type colours.
lv_obj_t* type_chip(lv_obj_t* parent, int t, int x, int y) {
    const lv_color_t bg = chip_color(t);
    const uint32_t fg = lv_color_luminance(bg) < 120 ? 0xffffff : 0x101418;
    return chip(parent, chip_name(t), x, y, bg, fg);
}

// Both type chips side by side; returns the x after the last chip.
int type_chips(lv_obj_t* parent, int dex, int x, int y) {
    int t1, t2;
    mon_types(dex, t1, t2);
    for (int t : {t1, t2}) {
        if (t < 0) continue;
        lv_obj_t* ch = type_chip(parent, t, x, y);
        lv_obj_update_layout(ch);
        x += lv_obj_get_width(ch) + 3;
    }
    return x;
}

struct Stats {
    int atk = 0, def = 0, spd = 0, spc = 0;
};

// Gen 1 stats from base stats + DVs + stat exp. HP comes from PartyMon
// (GameState derives it); the other four are computed here.
Stats mon_stats(const PartyMon& m) {
    Stats s;
    const uint8_t L = m.level ? m.level : 1;
    const uint8_t dv_atk = m.dvs[0] >> 4, dv_def = m.dvs[0] & 0xF;
    const uint8_t dv_spd = m.dvs[1] >> 4, dv_spc = m.dvs[1] & 0xF;
    uint8_t b_atk = 0, b_def = 0, b_spd = 0, b_spc = 0;
    if (m.dex >= 1 && m.dex <= 151) {
        const BaseStats& b = speciesBaseStats[m.dex];   // pret (correct Gen 1 values)
        b_atk = b.atk; b_def = b.def; b_spd = b.spd; b_spc = b.spc;
    } else if (m.dex >= 152 && m.dex <= 386) {
        const Gen3BaseStats& b = GEN3_BASE_STATS[m.dex];
        b_atk = b.atk; b_def = b.def; b_spd = b.spe; b_spc = b.spa;
    } else {
        return s;
    }
    s.atk = calcStat(b_atk, dv_atk, m.stat_exp[1], L);
    s.def = calcStat(b_def, dv_def, m.stat_exp[2], L);
    s.spd = calcStat(b_spd, dv_spd, m.stat_exp[3], L);
    s.spc = calcStat(b_spc, dv_spc, m.stat_exp[4], L);
    return s;
}

uint32_t exp_at(int dex, int level) {
    if (level <= 1) return 0;
    if (dex >= 1 && dex <= 151) return expForLevel((uint8_t)dex, (uint8_t)level);
    return (uint32_t)level * level * level;   // Medium Fast (matches GameState)
}

const Gen1MoveData* move_data(uint16_t id) {
    if (id == 0) return nullptr;
    if (const Gen1MoveData* d = gen1Move(id)) return d;
    return gen3Move(id);
}

// Short effect tag for a move card ("" if nothing worth saying).
std::string effect_tag(const Gen1MoveData& d) {
    const char* t = "";
    switch (d.effect) {
        case EFF_STATUS_PSN: t = "Poison"; break;
        case EFF_STATUS_BRN: t = "Burn"; break;
        case EFF_STATUS_PAR: t = "Paralyze"; break;
        case EFF_STATUS_SLP: t = "Sleep"; break;
        case EFF_STATUS_FRZ: t = "Freeze"; break;
        case EFF_STATUS_CONFUSE: t = "Confuse"; break;
        case EFF_BOOST_ATK: t = "Atk up"; break;
        case EFF_BOOST_DEF: t = "Def up"; break;
        case EFF_BOOST_SPD: t = "Spd up"; break;
        case EFF_BOOST_SPC: t = "Spc up"; break;
        case EFF_BOOST_ACC: t = "Acc up"; break;
        case EFF_BOOST_EVA: t = "Evasion up"; break;
        case EFF_DROP_ATK: t = "Atk down"; break;
        case EFF_DROP_DEF: t = "Def down"; break;
        case EFF_DROP_SPD: t = "Spd down"; break;
        case EFF_DROP_SPC: t = "Spc down"; break;
        case EFF_DROP_ACC: t = "Acc down"; break;
        case EFF_DROP_EVA: t = "Eva down"; break;
        case EFF_DRAIN_HP: t = "Drain"; break;
        case EFF_RECOIL: t = "Recoil"; break;
        case EFF_OHKO: t = "One-hit KO"; break;
        case EFF_MULTI_HIT: t = "2-5 hits"; break;
        case EFF_DOUBLE_HIT: t = "2 hits"; break;
        case EFF_FIXED_DMG: t = "Fixed dmg"; break;
        case EFF_LEVEL_DMG: t = "Dmg = Lv"; break;
        case EFF_PSYWAVE: t = "Random dmg"; break;
        case EFF_SUPER_FANG: t = "Halves HP"; break;
        case EFF_COUNTER: t = "Counter"; break;
        case EFF_CHARGE_TURN: t = "2 turns"; break;
        case EFF_RECHARGE: case EFF_HYPER_BEAM: t = "Recharge"; break;
        case EFF_RAGE: t = "Rage"; break;
        case EFF_TRAPPING: t = "Traps"; break;
        case EFF_FLINCH: t = "Flinch"; break;
        case EFF_SWIFT_NEVERMISS: t = "Never misses"; break;
        case EFF_DREAM_EATER: t = "Drain"; break;
        case EFF_EXPLODE: t = "User faints"; break;
        case EFF_REST: t = "Full heal+sleep"; break;
        case EFF_HEAL_HALF: t = "Heal 50%"; break;
        case EFF_SUBSTITUTE: t = "Substitute"; break;
        case EFF_REFLECT: t = "Reflect"; break;
        case EFF_LIGHT_SCREEN: t = "Light Screen"; break;
        case EFF_THRASH: t = "Thrash"; break;
        default: break;
    }
    if (!*t) return {};
    if (d.effectChance > 0 && d.effectChance < 100) {
        // Secondary effects: compact tag so it fits beside the stats line.
        const char* k = t;
        switch (d.effect) {
            case EFF_STATUS_PSN: k = "PSN"; break;
            case EFF_STATUS_BRN: k = "BRN"; break;
            case EFF_STATUS_PAR: k = "PAR"; break;
            case EFF_STATUS_SLP: k = "SLP"; break;
            case EFF_STATUS_FRZ: k = "FRZ"; break;
            case EFF_STATUS_CONFUSE: k = "CNF"; break;
            case EFF_DROP_ATK: k = "-ATK"; break;
            case EFF_DROP_DEF: k = "-DEF"; break;
            case EFF_DROP_SPD: k = "-SPD"; break;
            case EFF_DROP_SPC: k = "-SPC"; break;
            default: break;
        }
        return fmt("%d%% %s", d.effectChance, k);
    }
    return t;
}

std::string mon_name(const PartyMon& m) {
    if (!m.nickname.empty()) return m.nickname;
    if (!m.species.empty()) return m.species;
    return species_name(m.dex);
}

std::string mon_species(const PartyMon& m) {
    return title_case(m.species.empty() ? species_name(m.dex) : m.species);
}

// Cheap signature so a revision bump only rebuilds when the party changed.
uint64_t party_sig(const std::vector<PartyMon>& p) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(p.size());
    for (const auto& m : p) {
        mix(m.dex); mix(m.level); mix(m.hp); mix(m.max_hp); mix(m.exp);
        mix(m.dvs[0] | (m.dvs[1] << 8));
        for (uint16_t mv : m.moves) mix(mv);
        for (uint16_t se : m.stat_exp) mix(se);
        for (char ch : m.nickname) mix((uint8_t)ch);
    }
    return h;
}

lv_obj_t* right_label(lv_obj_t* parent, const std::string& text, int x, int y, int w,
                      const lv_font_t* font, uint32_t col) {
    lv_obj_t* l = label(parent, text, x, y, font, col, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    return l;
}

// Single-line label in `font`, dropping to font_small (and "..." clipping)
// when the text doesn't fit `avail` px. Long nicknames stay readable.
lv_obj_t* fit_label(lv_obj_t* parent, const std::string& text, int x, int y, int avail,
                    const lv_font_t* font, uint32_t col) {
    lv_obj_t* l = label(parent, text, x, y, font, col);
    lv_obj_update_layout(l);
    if (lv_obj_get_width(l) > avail) {
        const int dy = (lv_font_get_line_height(font) - lv_font_get_line_height(font_small())) / 2;
        lv_obj_delete(l);
        l = label(parent, text, x, y + dy, font_small(), col, avail);
    }
    return l;
}

// ── Layout ──────────────────────────────────────────────────────────────────

constexpr int kListX = 2, kListW = 156, kRowH = 22;
constexpr int kDetailX = 160, kDetailW = kBodyW - kDetailX - 2;   // 158

class PartyScreen final : public Screen {
public:
    explicit PartyScreen(Ctx& ctx) : Screen(ctx) {}

    const char* title() const override { return "Party"; }

    std::string hints() const override {
        if (party().empty()) return "Esc:back";
        if (mode_ == Mode::Moves) return "Up/Dn:prev/next mon  Enter/Esc:back";
        return "Up/Dn:select  Enter:moves  1-6:jump  Esc:back";
    }

    // Keys 4..6: previous / next monster, then the other page (moves/stats).
    SoftKeys softkeys() const override {
        SoftKeys k;
        k.left.text = MM_SOFT_BACK;
        if (party().empty()) return k;
        k.set(0, "PREV").set(1, "NEXT").set(2, mode_ == Mode::Moves ? "STATS" : "MOVES");
        return k;
    }

    void build(lv_obj_t* body) override {
        rev_ = ctx_.game.revision();
        sig_ = party_sig(party());
        list_.reset();
        detail_ = nullptr;
        const auto& p = party();
        if (p.empty()) {
            build_empty(body);
            return;
        }
        sel_ = std::clamp(sel_, 0, (int)p.size() - 1);
        if (mode_ == Mode::Moves) build_moves(body, p[sel_]);
        else build_list(body);
    }

    bool on_key(const KeyEvent& in) override {
        const int n = (int)party().size();
        if (n == 0) return false;   // B pops
        KeyEvent e = in;
        switch (softkey_slot(in)) {
            case 0: e.key = Key::Up; break;
            case 1: e.key = Key::Down; break;
            case 2: e.key = Key::Tab; break;
            case kSoftNone: break;
            default: return false;   // key 3: Router backs out; others unused
        }
        if (mode_ == Mode::Moves) {
            switch (e.key) {
                case Key::Up:
                case Key::Left:
                    sel_ = (sel_ + n - 1) % n;
                    ctx_.router.rebuild();
                    return true;
                case Key::Down:
                case Key::Right:
                    sel_ = (sel_ + 1) % n;
                    ctx_.router.rebuild();
                    return true;
                case Key::A:
                case Key::B:
                case Key::Tab:
                    if (e.repeat) return true;
                    mode_ = Mode::List;
                    ctx_.router.rebuild();
                    return true;
                default:
                    return false;
            }
        }
        // List mode.
        if (e.key == Key::Up || e.key == Key::Down) {
            if (list_ && list_->on_key(e)) {
                sel_ = list_->selected();
                fill_detail();
            }
            return true;
        }
        if ((e.key == Key::A || e.key == Key::Right || e.key == Key::Tab) && !e.repeat) {
            mode_ = Mode::Moves;
            ctx_.router.rebuild();
            return true;
        }
        return e.key == Key::Left;   // swallow (no paging in a <= 6 list)
    }

    void tick(uint32_t) override { maybe_refresh(); }
    void on_show() override { maybe_refresh(); }

    int demo_state_count() const override { return 3; }
    const char* demo_state_name(int i) const override {
        static constexpr const char* kNames[] = {"list", "moves", "empty"};
        return (i >= 0 && i < 3) ? kNames[i] : "?";
    }
    void set_demo_state(int i) override {
        force_empty_ = i == 2;
        mode_ = i == 1 ? Mode::Moves : Mode::List;
        sel_ = i == 1 ? 0 : 1;
    }

private:
    enum class Mode : uint8_t { List, Moves };

    const std::vector<PartyMon>& party() const {
        static const std::vector<PartyMon> kNone;
        return force_empty_ ? kNone : ctx_.game.party();
    }

    void maybe_refresh() {
        if (ctx_.game.revision() == rev_) return;
        rev_ = ctx_.game.revision();
        const uint64_t s = party_sig(party());
        if (s != sig_) ctx_.router.rebuild();   // build() stores the new sig
    }

    // ── List view ───────────────────────────────────────────────────────────
    void build_list(lv_obj_t* body) {
        const auto& p = party();
        list_ = std::make_unique<ListView>(body, kListX, 1, kListW, kBodyH - 1, kRowH,
            [this](lv_obj_t* row, int i, bool selected) { fill_row(row, i, selected); });
        list_->set_count((int)p.size());
        list_->select(sel_);

        detail_ = panel(body, kDetailX, 0, kDetailW, kBodyH);
        fill_detail();
    }

    void fill_row(lv_obj_t* row, int i, bool selected) {
        const auto& p = party();
        if (i < 0 || i >= (int)p.size()) return;
        const PartyMon& m = p[i];
        const int rw = kListW - 4;   // row width (ListView leaves a scroll gutter)
        sprite_box(row, m.dex, 0, 0, 28, kRowH - 1);   // half size; tall ones lose their feet
        fit_label(row, mon_name(m), 30, 0, rw - 30 - 44, font_body(),
                  selected ? color::kAccent : color::kText);
        hp_bar(row, 31, 15, rw - 31 - 44, m.hp, m.max_hp);
        right_label(row, fmt("Lv%d", m.level), rw - 42, -1, 39, font_small(), color::kText);
        right_label(row, fmt("%d/%d", m.hp, m.max_hp), rw - 42, 9, 39, font_small(), color::kDim);
    }

    void fill_detail() {
        if (!detail_) return;
        lv_obj_clean(detail_);
        const auto& p = party();
        if (sel_ < 0 || sel_ >= (int)p.size()) return;
        const PartyMon& m = p[sel_];
        lv_obj_t* d = detail_;
        constexpr int kW = kDetailW - 2;   // content width inside the 1px border

        sprite_box(d, m.dex, 0, 0, 64, 64);
        fit_label(d, mon_name(m), 66, 2, kW - 68, font_title(), color::kAccent);
        label(d, fmt("%s  #%03d", mon_species(m).c_str(), m.dex), 66, 20, font_small(),
              color::kDim, kW - 68);
        type_chips(d, m.dex, 66, 34);
        label(d, fmt("Lv %d", m.level), 66, 49, font_title(), color::kText);

        // HP
        label(d, "HP", 4, 66, font_small(), color::kDim);
        hp_bar(d, 22, 70, 76, m.hp, m.max_hp);
        right_label(d, fmt("%d/%d", m.hp, m.max_hp), 100, 64, kW - 104, font_body(), color::kText);

        // Gen 1 stats, 2 x 2
        const Stats s = mon_stats(m);
        const char* names[4] = {"ATK", "DEF", "SPD", "SPC"};
        const int vals[4] = {s.atk, s.def, s.spd, s.spc};
        for (int k = 0; k < 4; ++k) {
            const int x = 4 + (k % 2) * 78, y = 82 + (k / 2) * 15;
            label(d, names[k], x, y + 2, font_small(), color::kDim);
            label(d, fmt("%d", vals[k]), x + 28, y, font_body(), color::kText);
        }

        // EXP to next level
        if (m.level >= 100) {
            label(d, "EXP  max level", 4, 114, font_small(), color::kDim, kW - 8);
        } else {
            const uint32_t lo = exp_at(m.dex, m.level), hi = exp_at(m.dex, m.level + 1);
            const uint32_t cur = std::max(m.exp, lo);
            const uint32_t need = hi > cur ? hi - cur : 0;
            label(d, fmt("EXP to Lv%d: %u", m.level + 1, (unsigned)need), 4, 114, font_small(),
                  color::kDim, kW - 8);
            lv_obj_t* b = bar(d, 4, 128, kW - 8, 3, c(color::kInfo));
            bar_set(b, (int)(cur - lo), (int)std::max<uint32_t>(1, hi - lo));
        }
    }

    // ── Moves view ──────────────────────────────────────────────────────────
    void build_moves(lv_obj_t* body, const PartyMon& m) {
        constexpr int kLeftW = 104;
        lv_obj_t* l = panel(body, 2, 0, kLeftW, kBodyH);
        constexpr int kLW = kLeftW - 2;
        label(l, fmt("%d/%d", sel_ + 1, (int)party().size()), 3, 1, font_small(), color::kDim);
        sprite_box(l, m.dex, (kLW - 64) / 2, 4, 64, 64);
        lv_obj_t* nm = label(l, mon_name(m), 2, 66, font_body(), color::kAccent, kLW - 4);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t* sp = label(l, fmt("Lv%d %s", m.level, mon_species(m).c_str()), 2, 81,
                             font_small(), color::kText, kLW - 4);
        lv_obj_set_style_text_align(sp, LV_TEXT_ALIGN_CENTER, 0);
        // Centre the type chips: build in a temp box, then offset.
        lv_obj_t* tb = box(l, 0, 95, kLW, 13);
        const int tw = type_chips(tb, m.dex, 0, 0) - 3;
        lv_obj_set_x(tb, std::max(0, (kLW - tw) / 2));
        hp_bar(l, 6, 114, kLW - 12, m.hp, m.max_hp);
        lv_obj_t* hp = label(l, fmt("HP %d/%d", m.hp, m.max_hp), 2, 120, font_small(),
                             color::kDim, kLW - 4);
        lv_obj_set_style_text_align(hp, LV_TEXT_ALIGN_CENTER, 0);

        constexpr int kCardX = kLeftW + 6, kCardW = kBodyW - kCardX - 2, kCardH = 32;
        for (int i = 0; i < 4; ++i) {
            lv_obj_t* card = panel(body, kCardX, 2 + i * (kCardH + 2), kCardW, kCardH);
            fill_move(card, kCardW - 2, m.moves[i]);
        }
    }

    void fill_move(lv_obj_t* card, int w, uint16_t id) {
        if (id == 0) {
            label(card, "-", 8, 8, font_body(), color::kDim);
            return;
        }
        const Gen1MoveData* d = move_data(id);
        if (!d) {
            label(card, fmt("Move #%u", (unsigned)id), 8, 1, font_body(), color::kText, w - 12);
            return;
        }
        lv_obj_t* stripe = box(card, 0, 0, 3, kBodyH);
        lv_obj_set_style_bg_color(stripe, chip_color(d->type), 0);
        lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);

        lv_obj_t* ch = type_chip(card, d->type, 0, 0);
        lv_obj_align(ch, LV_ALIGN_TOP_RIGHT, -4, 3);
        label(card, d->name, 8, 1, font_body(), color::kText, w - 66);

        const std::string pow = d->power > 1 ? fmt("%d", d->power) : std::string("--");
        const std::string acc = d->accuracy ? fmt("%d", d->accuracy) : std::string("--");
        lv_obj_t* st = label(card, fmt("PWR %s  ACC %s  PP %d", pow.c_str(), acc.c_str(), d->pp),
                             8, 17, font_small(), color::kDim);
        const std::string eff = effect_tag(*d);
        if (!eff.empty()) {
            lv_obj_update_layout(st);
            const int x = 8 + lv_obj_get_width(st) + 6;
            if (w - 4 - x > 16) right_label(card, eff, x, 17, w - 4 - x, font_small(), color::kInfo);
        }
    }

    // ── Empty state ─────────────────────────────────────────────────────────
    void build_empty(lv_obj_t* body) {
        sprite_box(body, 143, (kBodyW - 64) / 2, 2, 64, 64);   // Snorlax, dozing
        lv_obj_t* t = label(body, "No Pokemon in your party", 0, 70, font_title(),
                            color::kAccent, kBodyW);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        const bool live = ctx_.game.daemon_connected();
        const char* msg = live
            ? "The daemon has no party yet. Check a save in to the daycare and it will show up here."
            : "Not connected to the MonsterMesh daemon. Start mmd with your save and your party will appear here.";
        lv_obj_t* b = text_block(body, msg, 24, 92, kBodyW - 48, font_small(), color::kDim);
        lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    }

    Mode mode_ = Mode::List;
    int sel_ = 0;
    bool force_empty_ = false;
    uint32_t rev_ = 0;
    uint64_t sig_ = 0;
    std::unique_ptr<ListView> list_;
    lv_obj_t* detail_ = nullptr;
};

} // namespace

std::unique_ptr<Screen> make_party_screen(Ctx& ctx) {
    return std::make_unique<PartyScreen>(ctx);
}

} // namespace mm::ui
