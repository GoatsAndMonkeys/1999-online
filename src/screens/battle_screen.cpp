// battle_screen.cpp — Battle: Practice / Gym run locally on the shared Gen 1
// engine (src/battle/Gen1BattleEngine.h); Mesh PvP is daemon-driven
// (BATTLE_UPDATE via GameState::set_message_hook, BATTLE_ACTION via IpcClient).
//
// Layout (body 320x138): opponent info box top-left, opponent front sprite
// top-right; player back sprite bottom-left, player info box (with HP
// numbers) right; a 36px message / menu strip at the bottom. The party switch
// list and the result card are full-body overlays.
//
// Flow: every turn the engine runs synchronously; its log lines become a
// queue of messages, each carrying a snapshot of both active mons so the HP
// bars / sprites animate in step with the text as Enter advances it.
//
// Gym = full trainer gauntlet: every LordGym trainer in order, leader last,
// party fully healed between fights; a loss or leaving ends the run (nothing
// is kept), the badge is set only on beating the leader.
//
// PlayerGym = somebody's Mesh Space gym: one AI-played leader fight against
// the team the owner posted (up to 6, in order; the owner is not online). No
// running; the outcome (win / loss / draw / forfeit) goes to
// MeshSpace::record_challenge(), which feeds the ladder and the Updates tabs.
#include "screens.h"

#include "game_state.h"
#include "mesh_space.h"
#include "router.h"
#include "widgets.h"

#include "Gen1BattleEngine.h"
#include "Gen1Learnsets.h"
#include "LordGyms.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

namespace mm::ui {
namespace {

using Eng = Gen1BattleEngine;

// ── Geometry ────────────────────────────────────────────────────────────────
constexpr int kStripY = 100;               // bottom message / menu strip
constexpr int kStripH = 36;
constexpr int kFoeBoxX = 4, kFoeBoxY = 4, kBoxW = 152, kFoeBoxH = 34;
constexpr int kMeBoxX = 164, kMeBoxY = 60, kMeBoxH = 38;
constexpr int kFoeSpriteX = 238, kFoeSpriteY = -2;
constexpr int kMeSpriteX = 26, kMeSpriteY = 36;
// Gym gauntlet progress: in the gap between foe box (x<156) and foe sprite.
constexpr int kProgX = 154, kProgY = 6, kProgW = 80;
constexpr int kPip = 6, kLeaderPip = 8, kPipStep = 8;

// ── Small helpers ───────────────────────────────────────────────────────────
// A one-word move name too wide for a key-wide cell ("THUNDERSHOCK") gets a
// hyphenated break at a word-ish seam instead of LVGL's mid-letter wrap.
std::string wrap_move_name(const std::string& name, const lv_font_t* f, int width) {
    if (name.find(' ') != std::string::npos) return name;
    lv_point_t p{};
    lv_text_get_size(&p, name.c_str(), f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (p.x <= width || name.size() < 6) return name;
    static const char* const kSeams[] = {"THUNDER", "SOLAR", "HYPER", "PSYCH", "BUBBLE", "DRAGON",
                                         "SEISMIC", "SELF", "SKULL", "MEGA", "SUPER", "SLUDGE",
                                         "SPIKE", "DREAM", "HORN", "LEECH", "HYDRO", "FLAME",
                                         "BONE", "SONIC", "DOUBLE", "STUN", "SPORE", "TRI", "SAND"};
    size_t cut = 0;
    for (const char* seam : kSeams) {
        const size_t n = std::strlen(seam);
        if (name.size() > n + 2 && name.compare(0, n, seam) == 0) { cut = n; break; }
    }
    if (!cut) cut = (name.size() + 1) / 2;
    return name.substr(0, cut) + "-\n" + name.substr(cut);
}

std::string upper(std::string s) {
    for (char& ch : s)
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    return s;
}

// Engine text is ASCII except the odd typographic glyph; Montserrat here is
// ASCII-only, so anything else becomes '?'.
std::string ascii(const char* s) {
    std::string out;
    for (; s && *s; ++s) {
        const unsigned char ch = static_cast<unsigned char>(*s);
        if (ch >= 32 && ch < 127) out.push_back(static_cast<char>(ch));
        else if (ch >= 0xC0) out.push_back('?');   // lead byte of a UTF-8 sequence
    }
    return out;
}

bool ends_with(const std::string& s, const char* suf) {
    const size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
bool starts_with(const std::string& s, const char* pre) {
    return s.rfind(pre, 0) == 0;
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

// Gen 1 stat formula, identical to the engine's (static) calcStat().
uint16_t calc_stat(uint8_t base, uint8_t iv, uint16_t stat_exp, uint8_t level, bool is_hp) {
    uint32_t r = 0;
    while ((r + 1) * (r + 1) <= stat_exp) ++r;
    const uint32_t bonus = (r + 3) >> 2;
    const uint32_t v = ((uint32_t)(base + iv) * 2 + bonus) * level / 100;
    return static_cast<uint16_t>(is_hp ? v + level + 10 : v + 5);
}

// Up to 4 moves for a mon with no move list: the last level-up moves it knows
// at `level`, Tackle first as a fallback (learnsets omit level-1 moves).
void default_moves(int dex, int level, uint8_t out[4]) {
    std::vector<uint8_t> list{33};   // Tackle
    uint8_t buf[24];
    const uint8_t n = gen1MovesLearnedBetween(static_cast<uint8_t>(dex), 0,
                                              static_cast<uint8_t>(level), buf, sizeof(buf));
    for (uint8_t i = 0; i < n; ++i)
        if (std::find(list.begin(), list.end(), buf[i]) == list.end()) list.push_back(buf[i]);
    while (list.size() > 4) list.erase(list.begin());
    for (int i = 0; i < 4; ++i) out[i] = i < static_cast<int>(list.size()) ? list[i] : 0;
}

void poke_from_party(Eng::BattlePoke& bp, const PartyMon& m) {
    const int dex = m.dex;
    const uint8_t lvl = static_cast<uint8_t>(std::clamp<int>(m.level, 1, 100));
    uint8_t mv[4] = {0, 0, 0, 0};
    int n = 0;
    for (uint16_t id : m.moves)
        if (id > 0 && id <= GEN1_MOVE_COUNT && n < 4) mv[n++] = static_cast<uint8_t>(id);
    if (n == 0) default_moves(dex, lvl, mv);
    Eng::initBattlePokeFromBase(bp, static_cast<uint16_t>(dex), lvl, mv, 1);

    // Real DVs + stat exp instead of the wild-mon defaults.
    const Gen1BaseStats& b = GEN1_BASE_STATS[dex];
    const uint8_t atk = (m.dvs[0] >> 4) & 0xF, def = m.dvs[0] & 0xF;
    const uint8_t spd = (m.dvs[1] >> 4) & 0xF, spc = m.dvs[1] & 0xF;
    const uint8_t hpdv = static_cast<uint8_t>(((atk & 1) << 3) | ((def & 1) << 2) |
                                              ((spd & 1) << 1) | (spc & 1));
    bp.maxHp = calc_stat(b.hp, hpdv, m.stat_exp[0], lvl, true);
    bp.atk = calc_stat(b.atk, atk, m.stat_exp[1], lvl, false);
    bp.def = calc_stat(b.def, def, m.stat_exp[2], lvl, false);
    bp.spd = calc_stat(b.spd, spd, m.stat_exp[3], lvl, false);
    bp.spc = calc_stat(b.spc, spc, m.stat_exp[4], lvl, false);
    bp.hp = bp.maxHp;
    set_nick(bp, m.nickname.empty() ? species_name(dex) : m.nickname);
}

std::string move_name(uint16_t id) {
    const Gen1MoveData* mv = gen1Move(id);
    if (!mv) return "-";
    std::string n = upper(mv->name);
    // Long names lose their spaces Gen 1 style ("THUNDERSHOCK") to fit a cell.
    if (n.size() > 12) n.erase(std::remove(n.begin(), n.end(), ' '), n.end());
    return n;
}

const char* status_text(uint8_t st) {
    if (st & Eng::ST_PAR) return "PAR";
    if (st & Eng::ST_FRZ) return "FRZ";
    if (st & Eng::ST_BRN) return "BRN";
    if (st & Eng::ST_PSN) return "PSN";
    if (st & Eng::ST_SLP) return "SLP";
    return "";
}
uint32_t status_color(uint8_t st) {
    if (st & Eng::ST_PAR) return 0xf8d030;
    if (st & Eng::ST_FRZ) return 0x98d8d8;
    if (st & Eng::ST_BRN) return 0xf08030;
    if (st & Eng::ST_PSN) return 0xb060c0;
    return 0x8a94a0;
}

// Text colour that reads on a chip of colour `bg` (skins recolour kAccent).
uint32_t on_color(uint32_t bg) {
    const int lum = (((bg >> 16) & 0xFF) * 299 + ((bg >> 8) & 0xFF) * 587 + (bg & 0xFF) * 114) / 1000;
    return lum >= 140 ? 0x101418 : 0xffffff;
}

// ── View model ──────────────────────────────────────────────────────────────
struct SideSnap {
    bool shown = false;     // sprite on the field
    int dex = 0;
    std::string name;
    int level = 0;
    int hp = 0, max_hp = 1;
    uint8_t status = 0;
};
struct Snap {
    SideSnap me, foe;
    int trainer = 0;        // gym gauntlet: index of the trainer being fought
                            // (player gym: how many of the leader's team fainted)
};
struct Msg {
    std::string text;
    Snap snap;
};

enum class Phase : uint8_t { Messages, Action, Moves, Party, Wait, Result };
enum class Next : uint8_t { Action, ForcedParty, Result, Pop, Wait };
enum class Outcome : uint8_t { None, Win, Lose, Draw, Fled };

// One row of the party switch list.
struct TeamRow {
    int dex = 0;
    std::string name;
    int level = 0;
    int hp = 0, max_hp = 1;
    uint8_t status = 0;
};

// Parsed BATTLE_UPDATE (mesh).
struct MeshUpdate {
    int my_hp = 0, enemy_hp = 0, turn = 0, result = 0;
    int my_status = 0, enemy_status = 0;
    bool need_switch = false;
    bool has_pp = false;
    int pp[4] = {0, 0, 0, 0};
    std::vector<std::string> log;
};

constexpr int kDemoGym = 1;          // Misty: SPARKY's THUNDERSHOCK is super effective
constexpr uint32_t kDemoSeed = 0x5eed1234u;
constexpr int kDemoTurns = 27;       // player-gym demo: turn count on the result card

// Request for a fight at `node`'s Mesh Space gym as `ms` lists it (demo states;
// the Mesh Space screen builds the same thing for a real challenge).
BattleRequest player_gym_request(const MeshSpace& ms, uint32_t node) {
    BattleRequest r;
    r.kind = BattleRequest::Kind::PlayerGym;
    r.node_id = node;
    const SpaceGym& g = ms.gym(node);
    for (const SpaceMon& m : g.party) {
        if (r.team_count >= 6) break;
        r.team[r.team_count++] = {m.dex, m.level};
    }
    r.trainer = ms.profile(node).name;
    r.place = g.name;
    r.prize = g.badge;
    return r;
}

class BattleScreen final : public Screen {
public:
    explicit BattleScreen(Ctx& ctx) : Screen(ctx) {
        setup(ctx.battle, fresh_seed());
    }

    ~BattleScreen() override { ctx_.game.set_message_hook(nullptr); }

    const char* title() const override { return title_.c_str(); }

    std::string hints() const override {
        switch (phase_) {
            case Phase::Messages: return "Enter:next";
            case Phase::Action:   return "Arrows:move  Enter:select  F/P/R  Esc:leave";
            case Phase::Moves:    return "Arrows:move  Enter:use  1-4  Esc:back";
            case Phase::Party:
                return forced_ ? "Arrows:move  Enter:send out" : "Arrows:move  Enter:switch  Esc:back";
            case Phase::Wait:     return "Enter:menu  Esc:forfeit";
            case Phase::Result:   return "Enter:continue";
        }
        return "Enter:select";
    }

    // Choices sit on keys 4..8 under the display; key 3 (corner) backs out.
    SoftKeys softkeys() const override {
        SoftKeys k;
        switch (phase_) {
            case Phase::Messages: k.set(0, "NEXT"); break;
            case Phase::Action:
                k.set(0, "FIGHT").set(1, "PARTY").set(2, "RUN", !(trainer_battle() && !mesh_));
                k.selected = act_sel_;
                k.left.text = MM_SOFT_BACK;
                break;
            case Phase::Moves: {
                MoveSlot mv[4];
                active_moves(mv);
                for (int i = 0; i < 4; ++i) {
                    const Gen1MoveData* d = mv[i].id ? gen1Move(mv[i].id) : nullptr;
                    if (!d) { k.set(i, "-", false); continue; }
                    k.set(i, upper(type_name(d->type)), mv[i].pp > 0, type_color_hex(d->type));
                }
                k.set(4, "BACK");
                k.selected = move_sel_;
                k.left.text = MM_SOFT_BACK;
                break;
            }
            case Phase::Party:
                k.set(0, forced_ ? "SEND" : "SWITCH");
                if (!forced_) { k.set(4, "BACK"); k.left.text = MM_SOFT_BACK; }
                break;
            case Phase::Wait:
                k.set(0, "MENU").set(4, "FORFEIT");
                k.left.text = MM_SOFT_BACK;
                break;
            case Phase::Result: k.set(0, "OK"); break;
        }
        return k;
    }

    // ── Build ───────────────────────────────────────────────────────────────
    void build(lv_obj_t* body) override {
        clear_refs();
        if (trainer_battle() && !mesh_) title_ = "vs " + trainer_name(shown_.trainer);
        if (phase_ == Phase::Party) {
            build_party(body);
            return;
        }
        if (phase_ == Phase::Result) {
            build_result(body);
            return;
        }
        build_field(body);
        switch (phase_) {
            case Phase::Messages:
            case Phase::Wait:   build_message(body); break;
            case Phase::Action: build_action(body); break;
            case Phase::Moves:  build_moves(body); break;
            case Phase::Result: break;
            case Phase::Party:  break;
        }
        apply_snap(false);
    }

    // ── Keys ────────────────────────────────────────────────────────────────
    bool on_key(const KeyEvent& e) override {
        const int slot = softkey_slot(e);
        switch (phase_) {
            case Phase::Messages:
                if (e.key == Key::A || e.key == Key::B || slot == 0) { advance(); return true; }
                return true;   // swallow everything: never pop mid-text
            case Phase::Action:  return key_action(e);
            case Phase::Moves:   return key_moves(e);
            case Phase::Party:   return key_party(e);
            case Phase::Wait:
                if (e.key == Key::A || slot == 0) { set_phase(Phase::Action); return true; }
                if (e.key == Key::B || slot == 4 || slot == kSoftLeft) { confirm_leave(); return true; }
                return true;
            case Phase::Result:
                if (e.key == Key::A || e.key == Key::B || slot == 0) { ctx_.router.pop(); return true; }
                return true;
        }
        return false;
    }

    void tick(uint32_t) override {
        if (pending_pop_) {
            pending_pop_ = false;
            ctx_.router.pop();   // destroys *this: nothing after this line
            return;
        }
        if (!mesh_updates_.empty()) process_mesh_updates();
    }

    void on_show() override {
        if (!mesh_) return;
        ctx_.game.set_message_hook([this](const std::string& type, const JsonValue& msg) {
            on_daemon(type, msg);
        });
    }
    void on_hide() override { ctx_.game.set_message_hook(nullptr); }

    // ── Demo states ─────────────────────────────────────────────────────────
    // Misty's gym (3 trainers): fight vs trainer 1, its move menu, the
    // interstitial after beating trainer 1, the leader fight, the badge card.
    // 5-7: KD2ABC-m's Mesh Space gym (PlayerGym): action menu, win card, loss
    // card. Nothing is written to the Mesh Space model in demo states.
    int demo_state_count() const override { return 9; }
    const char* demo_state_name(int i) const override {
        static const char* const kNames[] = {"action", "moves", "next-trainer", "leader", "victory",
                                             "player-gym", "player-gym-win", "player-gym-loss", "party"};
        return (i >= 0 && i < 9) ? kNames[i] : "main";
    }
    void set_demo_state(int i) override {
        demo_ = true;
        if (i == 8) {                 // the switch menu, from the first action menu
            BattleRequest r;
            r.kind = BattleRequest::Kind::Gym;
            r.gym = kDemoGym;
            setup(r, kDemoSeed);
            drain_all();
            forced_ = false;
            party_sel_ = 1;
            phase_ = Phase::Party;
            return;
        }
        if (i >= 5) {
            setup(player_gym_request(mesh_space(), MeshSpace::kDemoNode), kDemoSeed);
            drain_all();              // skip the intro -> action menu vs the leader's lead
            if (i == 5) return;
            // Pretend one side's last mon just fainted, then end it the real way.
            auto& side = eng_.party(i == 6 ? 1 : 0);
            for (int k = 0; k < side.count; ++k) side.mons[k].hp = 0;
            side.active = static_cast<uint8_t>(side.count ? side.count - 1 : 0);
            if (i == 6) {              // the lead took a few hits on the way
                auto& me = eng_.party(0);
                me.mons[0].hp = static_cast<uint16_t>(me.mons[0].maxHp * 2 / 5);
            }
            end_battle(i == 6 ? Eng::Result::P1_WIN : Eng::Result::P2_WIN);
            finalize();
            drain_all();              // -> result card
            return;
        }
        BattleRequest r;
        r.kind = BattleRequest::Kind::Gym;
        r.gym = kDemoGym;
        setup(r, kDemoSeed);
        drain_all();                  // skip the intro -> action menu vs trainer 1
        if (i == 1) {
            phase_ = Phase::Moves;
            move_sel_ = 0;
        } else if (i == 2) {
            demo_beat_trainer();
            // Step to the "Next up: ..." line of the interstitial.
            while (!queue_.empty()) {
                pop_message();
                if (starts_with(cur_text_, "Next up")) break;
            }
            phase_ = Phase::Messages;
        } else if (i >= 3) {
            while (gym_ && !is_leader(trainer_idx_)) {
                demo_beat_trainer();
                drain_all();
            }
            if (i == 4) {
                // Pretend the leader's last mon just fainted (no lord.dat write in demo).
                auto& foe = eng_.party(1);
                for (int k = 0; k < foe.count; ++k) foe.mons[k].hp = 0;
                foe.active = static_cast<uint8_t>(foe.count ? foe.count - 1 : 0);
                outcome_ = Outcome::Win;
                badge_ = gym_badge_name();
                shown_ = take();
                phase_ = Phase::Result;
            }
        }
    }

private:
    // ── Setup ───────────────────────────────────────────────────────────────
    static uint32_t fresh_seed() {
        std::random_device rd;
        const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
        return rd() ^ static_cast<uint32_t>(t) ^ static_cast<uint32_t>(t >> 32);
    }

    void setup(const BattleRequest& req, uint32_t seed) {
        req_ = req;
        mesh_ = req.kind == BattleRequest::Kind::Mesh;
        queue_.clear();
        building_.clear();
        cur_text_.clear();
        outcome_ = Outcome::None;
        badge_.clear();
        forced_ = false;
        act_sel_ = move_sel_ = party_sel_ = 0;
        down_[0] = down_[1] = true;
        mesh_updates_.clear();
        leader_.clear();
        foe_name_.clear();
        gym_ = nullptr;
        pgym_ = req.kind == BattleRequest::Kind::PlayerGym;
        reported_ = false;
        challenge_ = GymChallenge{};
        place_.clear();
        error_ = false;
        trainer_idx_ = 0;
        title_ = "Battle";

        if (mesh_) {
            setup_mesh();
            return;
        }

        team_.clear();
        const auto& party = ctx_.game.party();
        for (size_t i = 0; i < party.size() && team_.size() < Eng::MAX_PARTY; ++i)
            if (party[i].dex >= 1 && party[i].dex <= 151 && party[i].level > 0)
                team_.push_back(party[i]);
        if (team_.empty()) {
            fail(party.empty() ? "You have no POKEMON! Load a party first."
                               : "No Gen 1 POKEMON in your party can battle here.");
            return;
        }

        if (pgym_) {
            // Checked before anything starts: nobody fights their own gym.
            if (req.node_id == MeshSpace::kMe || req.trainer == mesh_space().my_name()) {
                pgym_ = false;
                fail("You cannot challenge your own gym.");
                return;
            }
            leader_ = upper(ascii(req.trainer.c_str()));
            if (leader_.empty()) leader_ = "LEADER";
            place_ = upper(ascii(req.place.c_str()));
            if (place_.empty()) place_ = leader_ + "'S GYM";
            foe_name_ = leader_;
            title_ = "vs " + leader_;
        }

        Gen1Party none, foe;
        std::memset(&none, 0, sizeof(none));
        std::memset(&foe, 0, sizeof(foe));
        if (req.kind == BattleRequest::Kind::Gym) {
            // Full gauntlet: every gym trainer in order, the leader last.
            gym_ = lordGym(static_cast<uint8_t>(req.gym));
            if (!gym_ || gym_->trainerCount == 0 ||
                !lordBuildGymParty(static_cast<uint8_t>(req.gym), 0, foe)) {
                gym_ = nullptr;
                fail(fmt("Gym %d is not available.", req.gym + 1));
                return;
            }
            leader_ = upper(gym_->leaderName);
            trainer_idx_ = 0;
            foe_name_ = trainer_name(0);
            title_ = "vs " + foe_name_;
        }
        eng_.start(none, foe, seed, 1);

        auto& me = eng_.party(0);
        me.count = static_cast<uint8_t>(team_.size());
        me.active = 0;
        for (size_t i = 0; i < team_.size(); ++i) poke_from_party(me.mons[i], team_[i]);

        auto& op = eng_.party(1);
        if (gym_) {
            name_foe_party();
            const int before = trainer_count() - 1;   // trainers before the leader
            if (before > 0)
                push(fmt("%s GYM: %d trainer%s, then %s!", upper(gym_->city).c_str(), before,
                         before == 1 ? "" : "s", leader_.c_str()));
            announce_trainer();
        } else if (pgym_) {
            if (!build_player_gym_team()) {
                pgym_ = false;
                fail(leader_ + "'s gym has no Gen 1 POKEMON to battle.");
                return;
            }
            push(place_ + ": " + leader_ + " is away, but the team fights on!");
            announce_trainer();
        } else {
            std::mt19937 rng(seed);
            int dex = req.wild_dex, lvl = req.wild_level;
            if (dex < 1 || dex > 151) {
                static const int kSkip[] = {144, 145, 146, 150, 151};   // no legendaries
                do {
                    dex = 1 + static_cast<int>(rng() % 151);
                } while (std::find(std::begin(kSkip), std::end(kSkip), dex) != std::end(kSkip));
            }
            if (lvl < 1) lvl = me.mons[0].level - 3 + static_cast<int>(rng() % 6);
            lvl = std::clamp(lvl, 2, 100);
            uint8_t mv[4];
            default_moves(dex, lvl, mv);
            std::memset(&op, 0, sizeof(op));
            op.count = 1;
            op.active = 0;
            Eng::initBattlePokeFromBase(op.mons[0], static_cast<uint16_t>(dex),
                                        static_cast<uint8_t>(lvl), mv, 1);
            set_nick(op.mons[0], species_name(dex));
            title_ = "Wild " + std::string(op.mons[0].nickname);
            down_[1] = false;
            push(std::string("A wild ") + op.mons[0].nickname + " appeared!");
        }
        push(std::string("Go! ") + me.mons[0].nickname + "!", /*reveal=*/0);
        shown_ = take();
        shown_.me.shown = false;
        if (trainer_battle()) shown_.foe.shown = false;
        next_ = Next::Action;
        finalize();
        phase_ = Phase::Messages;
        pop_message();
    }

    // ── Gym gauntlet ────────────────────────────────────────────────────────
    bool trainer_battle() const { return gym_ || pgym_; }
    int trainer_count() const { return pgym_ ? 1 : gym_ ? gym_->trainerCount : 0; }
    bool is_leader(int t) const { return pgym_ || (gym_ && t >= lordGymLeaderIndex(gym_)); }
    std::string trainer_name(int t) const {
        if (pgym_) return leader_;
        if (!gym_) return {};
        if (is_leader(t)) return leader_;
        const char* n = gym_->trainers[std::clamp(t, 0, trainer_count() - 1)].name;
        return upper(n ? n : "TRAINER");
    }

    // lordBuildGymParty nicknames every mon "FOE": show species names instead.
    void name_foe_party() {
        auto& op = eng_.party(1);
        for (int k = 0; k < op.count; ++k) set_nick(op.mons[k], species_name(op.mons[k].species));
    }

    // "<TRAINER> wants to fight!" / "<TRAINER> sent out X!" for trainer_idx_.
    void announce_trainer() {
        const auto& op = eng_.party(1);
        if (is_leader(trainer_idx_)) push("Gym Leader " + leader_ + " wants to fight!");
        else push(foe_name_ + " wants to fight!");
        push(foe_name_ + " sent out " + op.mons[0].nickname + "!", /*reveal=*/1);
    }

    // Like walking out to the POKEMON CENTER between gym trainers: HP, PP,
    // status, stat stages and all volatile state are reset from the party.
    void heal_team() {
        auto& me = eng_.party(0);
        for (size_t i = 0; i < team_.size() && i < Eng::MAX_PARTY; ++i)
            poke_from_party(me.mons[i], team_[i]);
        me.reflect = me.lightScreen = me.mist = me.focused = false;
        me.reflectTurns = me.lightScreenTurns = 0;
    }

    // A gym trainer (not the leader) is down: heal and queue the next one.
    void next_gym_trainer() {
        push("You defeated " + foe_name_ + "!");
        push("Your POKEMON were fully healed!");
        heal_team();
        Gen1Party next;
        std::memset(&next, 0, sizeof(next));
        if (!lordBuildGymParty(static_cast<uint8_t>(req_.gym), static_cast<uint8_t>(trainer_idx_ + 1), next)) {
            push("Nobody else is in the gym...");
            next_ = Next::Pop;
            return;
        }
        ++trainer_idx_;
        foe_name_ = trainer_name(trainer_idx_);
        const int n = trainer_count();
        // Clear the field for the interstitial (the snap is taken post-swap).
        push(is_leader(trainer_idx_)
                 ? fmt("Next up: Gym Leader %s!  (%d/%d)", leader_.c_str(), trainer_idx_ + 1, n)
                 : fmt("Next up: %s  (%d/%d)", foe_name_.c_str(), trainer_idx_ + 1, n),
             -1, /*hide=*/0);
        eng_.replaceOpponent(next);
        name_foe_party();
        down_[1] = true;
        eng_.party(0).active = 0;   // lead with the first POKEMON again
        announce_trainer();
        push(std::string("Go! ") + eng_.party(0).mons[0].nickname + "!", /*reveal=*/0);
        next_ = Next::Action;
    }

    // Demo only: knock out the current trainer's team and queue the next one.
    void demo_beat_trainer() {
        if (!gym_ || is_leader(trainer_idx_)) return;
        auto& foe = eng_.party(1);
        for (int k = 0; k < foe.count; ++k) foe.mons[k].hp = 0;
        down_[1] = true;
        next_gym_trainer();
        finalize();
    }

    // ── Player gym (Mesh Space) ─────────────────────────────────────────────
    // The owner's posted team, in order, with level-up moves (the page lists
    // species + level only). Mons the Gen 1 engine cannot field are skipped.
    bool build_player_gym_team() {
        auto& op = eng_.party(1);
        std::memset(&op, 0, sizeof(op));
        int n = 0;
        for (int i = 0; i < req_.team_count && i < 6 && n < Eng::MAX_PARTY; ++i) {
            const int dex = req_.team[i].dex;
            if (dex < 1 || dex > 151 || req_.team[i].level == 0) continue;
            const int lvl = std::clamp<int>(req_.team[i].level, 1, 100);
            uint8_t mv[4];
            default_moves(dex, lvl, mv);
            Eng::initBattlePokeFromBase(op.mons[n], static_cast<uint16_t>(dex),
                                        static_cast<uint8_t>(lvl), mv, 1);
            set_nick(op.mons[n], species_name(dex));
            ++n;
        }
        op.count = static_cast<uint8_t>(n);
        op.active = 0;
        return n > 0;
    }

    int foe_fainted() const {
        const auto& op = eng_.party(1);
        int n = 0;
        for (int k = 0; k < op.count; ++k) n += op.mons[k].hp == 0;
        return n;
    }

    // The fight is over (won, or lost / drawn / forfeited): tell Mesh Space
    // once. The ladder, both Updates feeds and the Gym tab's result come from
    // record_challenge().
    void report_player_gym(bool won) {
        if (!pgym_ || reported_) return;
        reported_ = true;
        const int turns = demo_ ? kDemoTurns : std::max<int>(1, eng_.turn());
        std::vector<SpaceMon> party;
        for (const PartyMon& m : team_)
            party.push_back({m.dex, static_cast<uint8_t>(std::clamp<int>(m.level, 1, 100))});
        if (demo_) {
            // Screenshots: work out the result on a copy, leave the model alone.
            MeshSpace copy = mesh_space();
            challenge_ = copy.record_challenge(req_.node_id, copy.my_name(), won, turns, party);
            return;
        }
        // TODO(mesh): asynchronous play. The owner's node was not part of this
        // fight; record_challenge() updates only our local copy of their page.
        // Send the result (winner name, turns, party; ideally seed + inputs so
        // the owner can replay and verify it) to req_.node_id over the radio.
        MeshSpace& ms = mesh_space();
        challenge_ = ms.record_challenge(req_.node_id, ms.my_name(), won, turns, party);
    }

    void fail(const std::string& why) {
        pgym_ = false;               // nothing was fought: nothing to report
        error_ = true;
        shown_ = Snap{};
        cur_text_ = why;
        next_ = Next::Pop;
        phase_ = Phase::Messages;
    }

    // ── Snapshots / message queue ───────────────────────────────────────────
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
        o.status = m.status;
        return o;
    }
    Snap take() const {
        if (mesh_) return mesh_snap();
        return Snap{take_side(0), take_side(1), pgym_ ? foe_fainted() : trainer_idx_};
    }

    // Each message shows the state as of the NEXT message's emission (i.e.
    // after the effects of its own action), so "X used Y!" already shows the
    // damage. reveal/hide toggle a side's sprite from this message on.
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

    static void log_cb(const char* line, void* ctx) {
        static_cast<BattleScreen*>(ctx)->on_engine_line(ascii(line));
    }
    void on_engine_line(const std::string& line) {
        int reveal = -1, hide = -1;
        std::string text = line;
        if (ends_with(line, " fainted!")) {
            const std::string who = line.substr(0, line.size() - 9);
            for (int s = 0; s < 2; ++s) {
                const auto& bp = eng_.party(static_cast<uint8_t>(s));
                if (bp.mons[bp.active].hp == 0 && who == bp.mons[bp.active].nickname) { hide = s; break; }
            }
        } else if (starts_with(line, "Come back, ")) {
            hide = 0;
        } else if (starts_with(line, "Go! ")) {
            if (foe_sending_) {
                const std::string who = line.substr(4, line.size() > 5 ? line.size() - 5 : 0);
                text = (foe_name_.empty() ? std::string("Foe") : foe_name_) + " sent out " + who + "!";
                reveal = 1;
            } else {
                reveal = 0;
            }
        }
        push(text, reveal, hide);
    }

    void pop_message() {
        if (queue_.empty()) return;
        Msg m = std::move(queue_.front());
        queue_.pop_front();
        cur_text_ = m.text;
        shown_ = m.snap;
    }

    // Enter while a message is up.
    void advance() {
        if (queue_.empty()) {
            go_next();
            return;
        }
        pop_message();
        set_text(msg_lbl_, cur_text_);
        if (more_lbl_) {
            if (queue_.empty() && next_ != Next::Action) lv_obj_add_flag(more_lbl_, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(more_lbl_, LV_OBJ_FLAG_HIDDEN);
        }
        apply_snap(true);
    }

    void drain_all() {
        while (!queue_.empty()) pop_message();
        go_next(/*rebuild=*/false);
    }

    void start_messages() {
        // Rebuild with the pre-turn snapshot, then animate to the first line.
        phase_ = Phase::Messages;
        cur_text_.clear();
        ctx_.router.rebuild();
        advance();
    }

    void go_next(bool rebuild = true) {
        switch (next_) {
            case Next::Action: phase_ = Phase::Action; break;
            case Next::Wait:   phase_ = Phase::Wait; cur_text_ = "Waiting for " + mesh_name_ + "..."; break;
            case Next::ForcedParty:
                phase_ = Phase::Party;
                forced_ = true;
                party_sel_ = first_alive();
                break;
            case Next::Result:
                phase_ = Phase::Result;
                if (rebuild && outcome_ == Outcome::Win && !badge_.empty())
                    toast("Got the " + badge_ + "!", 3000, color::kAccent);
                break;
            case Next::Pop:
                if (rebuild) pending_pop_ = true;
                return;
        }
        if (rebuild) ctx_.router.rebuild();
    }

    void set_phase(Phase p) {
        phase_ = p;
        ctx_.router.rebuild();
    }

    // ── Turn logic (local engine) ───────────────────────────────────────────
    void run_turn(uint8_t act, uint8_t idx, bool show = true) {
        uint8_t ca = 0, ci = 0;
        eng_.cpuPickAction(1, ca, ci);
        eng_.submitAction(0, act, idx);
        eng_.submitAction(1, ca, ci);
        eng_.executeTurn(&BattleScreen::log_cb, this);
        after_turn();
        if (show) start_messages();
    }

    void after_turn() {
        const auto r = eng_.result();
        if (r == Eng::Result::ONGOING) {
            auto& op = eng_.party(1);
            if (op.mons[op.active].hp == 0) {
                foe_sending_ = true;
                eng_.autoReplaceIfFainted(1, &BattleScreen::log_cb, this);
                foe_sending_ = false;
            }
            const auto& me = eng_.party(0);
            next_ = me.mons[me.active].hp == 0 ? Next::ForcedParty : Next::Action;
        } else {
            end_battle(r);
        }
        finalize();
    }

    void end_battle(Eng::Result r) {
        if (r == Eng::Result::P1_WIN && gym_ && !is_leader(trainer_idx_)) {
            next_gym_trainer();   // the gauntlet goes on; no result card yet
            return;
        }
        next_ = Next::Result;
        if (r == Eng::Result::P1_WIN) {
            outcome_ = Outcome::Win;
            if (gym_) {
                push("Gym Leader " + leader_ + " was defeated!");
                badge_ = gym_badge_name();
                push("You earned the " + badge_ + "!");
                if (!demo_ && !ctx_.game.demo_mode()) ctx_.game.set_badge(req_.gym);
            } else if (pgym_) {
                push("Gym Leader " + leader_ + "'s team was defeated!");
                report_player_gym(true);
                if (challenge_.new_best)
                    push(fmt("%d turns: you are #%d on the champions ladder!", challenge_.turns,
                             challenge_.rank));
                else if (challenge_.rank > 0)
                    push(fmt("%d turns. Your %d-turn run stays #%d on the ladder.", challenge_.turns,
                             challenge_.best_turns, challenge_.rank));
                if (challenge_.badge && !req_.prize.empty())
                    push("You earned the " + req_.prize + "!");
            }
        } else if (r == Eng::Result::P2_WIN) {
            outcome_ = Outcome::Lose;
            push("You have no more POKEMON that can fight!");
            push("You blacked out!");
        } else {
            outcome_ = Outcome::Draw;
            push("Both sides are out of POKEMON. It's a draw!");
        }
        if (pgym_ && outcome_ != Outcome::Win) {
            report_player_gym(false);
            push(leader_ + "'s gym held you off.");
        }
    }

    std::string gym_badge_name() const {
        const LordGym* g = lordGym(static_cast<uint8_t>(req_.gym));
        return g ? upper(g->badgeName) + "BADGE" : std::string("BADGE");
    }

    int first_alive() const {
        const auto rows = team_rows();
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].hp > 0) return static_cast<int>(i);
        return 0;
    }

    // ── Moves / team data (local or mesh) ───────────────────────────────────
    struct MoveSlot {
        uint16_t id = 0;
        int pp = 0, max_pp = 0;
    };
    void active_moves(MoveSlot out[4]) const {
        if (mesh_) {
            const auto& party = ctx_.game.party();
            const PartyMon* m = mesh_active_ < static_cast<int>(party.size()) ? &party[static_cast<size_t>(mesh_active_)] : nullptr;
            for (int i = 0; i < 4; ++i) {
                out[i] = MoveSlot{};
                if (!m) continue;
                out[i].id = m->moves[i];
                const Gen1MoveData* mv = gen1Move(m->moves[i]);
                out[i].max_pp = mv ? mv->pp : 0;
                out[i].pp = mesh_has_pp_ ? mesh_pp_[i] : out[i].max_pp;
            }
            return;
        }
        const auto& bp = eng_.party(0);
        const auto& m = bp.mons[bp.active];
        for (int i = 0; i < 4; ++i) {
            out[i].id = m.moves[i];
            const Gen1MoveData* mv = gen1Move(m.moves[i]);
            out[i].max_pp = mv ? mv->pp : 0;
            out[i].pp = m.pp[i] & 0x3F;
        }
    }

    int active_index() const { return mesh_ ? mesh_active_ : eng_.party(0).active; }

    std::vector<TeamRow> team_rows() const {
        std::vector<TeamRow> rows;
        if (mesh_) {
            const auto& party = ctx_.game.party();
            for (size_t i = 0; i < party.size() && i < 6; ++i) {
                const auto& m = party[i];
                TeamRow r;
                r.dex = m.dex;
                r.name = upper(m.nickname.empty() ? species_name(m.dex) : m.nickname);
                r.level = m.level;
                r.max_hp = std::max<int>(1, m.max_hp);
                r.hp = m.max_hp;
                if (static_cast<int>(i) == mesh_active_) {
                    r.hp = mesh_my_hp_;
                    r.max_hp = std::max(1, mesh_my_max_);
                    r.status = static_cast<uint8_t>(mesh_my_status_);
                }
                rows.push_back(r);
            }
            return rows;
        }
        const auto& bp = eng_.party(0);
        for (int i = 0; i < bp.count; ++i) {
            const auto& m = bp.mons[i];
            rows.push_back({m.species, m.nickname, m.level, m.hp, std::max<int>(1, m.maxHp), m.status});
        }
        return rows;
    }

    // ── Key handlers ────────────────────────────────────────────────────────
    bool key_action(const KeyEvent& e) {
        int pick = -1;
        const int slot = softkey_slot(e);
        if (slot >= 0 && slot < 3) pick = slot;
        else if (slot == kSoftLeft) { confirm_leave(); return true; }
        else if (e.key == Key::Left || e.key == Key::Up) act_sel_ = (act_sel_ + 2) % 3;
        else if (e.key == Key::Right || e.key == Key::Down) act_sel_ = (act_sel_ + 1) % 3;
        else if (e.key == Key::A) pick = act_sel_;
        else if (e.key == Key::Char && e.ch == 'f') pick = 0;
        else if (e.key == Key::Char && e.ch == 'p') pick = 1;
        else if (e.key == Key::Char && e.ch == 'r') pick = 2;
        else if (e.key == Key::B) { confirm_leave(); return true; }
        else return true;

        if (pick < 0) {
            paint_action();
            return true;
        }
        act_sel_ = pick;
        if (pick == 0) {
            set_phase(Phase::Moves);
        } else if (pick == 1) {
            forced_ = false;
            party_sel_ = active_index();
            set_phase(Phase::Party);
        } else {
            do_run();
        }
        return true;
    }

    void do_run() {
        if (mesh_) {
            confirm_leave();
            return;
        }
        if (trainer_battle()) {
            push("No! There's no running from a trainer battle!");
            next_ = Next::Action;
        } else {
            push("Got away safely!");
            outcome_ = Outcome::Fled;
            next_ = Next::Pop;
        }
        finalize();
        start_messages();
    }

    bool key_moves(const KeyEvent& e) {
        const int slot = softkey_slot(e);
        if (e.key == Key::B || slot == 4 || slot == kSoftLeft) { set_phase(Phase::Action); return true; }
        GridNav nav{4, 4, move_sel_};
        if (nav.on_key(e)) {
            move_sel_ = nav.sel;
            paint_moves();
            return true;
        }
        int pick = -1;
        if (e.key == Key::A) pick = move_sel_;
        else if (slot >= 0 && slot < 4) pick = slot;
        if (pick < 0) return true;

        MoveSlot mv[4];
        active_moves(mv);
        bool any_pp = false;
        for (const auto& s : mv) any_pp = any_pp || (s.id && s.pp > 0);
        if (any_pp) {
            if (mv[pick].id == 0) return true;
            if (mv[pick].pp <= 0) {
                toast("No PP left for this move!", 1600, color::kWarn);
                return true;
            }
        }
        move_sel_ = pick;
        const uint8_t idx = any_pp ? static_cast<uint8_t>(pick) : 0xFE;   // 0xFE = Struggle
        if (mesh_) mesh_send(0, idx);
        else run_turn(0, idx);
        return true;
    }

    bool key_party(const KeyEvent& e) {
        const auto rows = team_rows();
        const int slot = softkey_slot(e);
        if (e.key == Key::B || slot == 4 || slot == kSoftLeft) {
            if (forced_) toast("Choose a POKEMON to send out!", 1600, color::kWarn);
            else set_phase(Phase::Action);
            return true;
        }
        GridNav nav{2, static_cast<int>(rows.size()), party_sel_};
        if (nav.on_key(e)) {
            party_sel_ = nav.sel;
            paint_party();
            return true;
        }
        if ((e.key != Key::A && slot != 0) || rows.empty()) return true;
        const int i = party_sel_;
        if (rows[static_cast<size_t>(i)].hp <= 0) {
            toast(rows[static_cast<size_t>(i)].name + " has no energy left!", 1600, color::kBad);
            return true;
        }
        if (i == active_index() && !forced_) {
            toast(rows[static_cast<size_t>(i)].name + " is already out!", 1600, color::kWarn);
            return true;
        }
        if (mesh_) {
            mesh_active_ = i;
            mesh_send(1, static_cast<uint8_t>(i));
            return true;
        }
        if (forced_) {
            // Replacing a fainted mon is free (no turn passes).
            forced_ = false;
            auto& me = eng_.party(0);
            me.active = static_cast<uint8_t>(i);
            push(std::string("Go! ") + me.mons[i].nickname + "!", /*reveal=*/0);
            next_ = Next::Action;
            finalize();
            start_messages();
        } else {
            run_turn(1, static_cast<uint8_t>(i));
        }
        return true;
    }

    void confirm_leave() {
        std::string body;
        if (mesh_) body = "Forfeit the battle vs " + mesh_name_ + "?";
        else if (pgym_) body = "Forfeit? It counts as a win for " + leader_ + "'s gym.";
        else if (gym_) body = "Forfeit the " + leader_ + " gym? No badge; next time you restart from trainer 1.";
        else body = "Run from the wild " + shown_.foe.name + "?";
        ctx_.router.show_modal(std::make_unique<Modal>(
            "Leave battle?", body, std::vector<std::string>{"Keep fighting", "Leave"},
            [this](int choice) {
                if (choice != 1) return;
                if (mesh_) ctx_.game.ipc().send_battle_action(2, 0, mesh_turn_);
                if (outcome_ == Outcome::None) report_player_gym(false);   // forfeit = defended
                pending_pop_ = true;
            }));
    }

    // ── Mesh (daemon-driven) ────────────────────────────────────────────────
    void setup_mesh() {
        const auto& d = ctx_.game.daycare();
        mesh_name_.clear();
        int dex = 0, lvl = 0;
        std::string lead;
        for (const auto& n : d.neighbor_list) {
            if (n.node_id != req_.node_id) continue;
            mesh_name_ = !n.game_name.empty() ? n.game_name : n.short_name;
            dex = n.lead_dex;
            lvl = n.lead_level;
            lead = n.lead_nick;
        }
        if (const auto* np = ctx_.game.neighbor_party(req_.node_id); np && !np->empty()) {
            dex = np->front().dex;
            lvl = np->front().level;
            lead = np->front().nickname.empty() ? species_name(dex) : np->front().nickname;
        }
        if (mesh_name_.empty()) mesh_name_ = fmt("!%08x", req_.node_id);
        mesh_name_ = upper(mesh_name_);
        title_ = "vs " + mesh_name_;
        mesh_foe_ = SideSnap{};
        mesh_foe_.dex = dex;
        mesh_foe_.level = lvl;
        mesh_foe_.name = upper(lead.empty() ? (dex ? species_name(dex) : std::string("???")) : lead);
        mesh_foe_.shown = dex > 0;
        mesh_foe_.hp = mesh_foe_.max_hp = 1;
        mesh_active_ = 0;
        mesh_turn_ = 0;
        mesh_has_pp_ = false;
        const auto& party = ctx_.game.party();
        mesh_my_hp_ = mesh_my_max_ = party.empty() ? 1 : std::max<int>(1, party.front().max_hp);
        mesh_my_status_ = 0;
        mesh_started_ = false;
        shown_ = mesh_snap();
        cur_text_ = "Waiting for " + mesh_name_ + "...";
        next_ = Next::Wait;
        phase_ = Phase::Wait;
    }

    Snap mesh_snap() const {
        Snap s;
        const auto& party = ctx_.game.party();
        if (mesh_active_ < static_cast<int>(party.size())) {
            const auto& m = party[static_cast<size_t>(mesh_active_)];
            s.me.shown = true;
            s.me.dex = m.dex;
            s.me.name = upper(m.nickname.empty() ? species_name(m.dex) : m.nickname);
            s.me.level = m.level;
        }
        s.me.hp = mesh_my_hp_;
        s.me.max_hp = std::max(1, mesh_my_max_);
        s.me.status = static_cast<uint8_t>(mesh_my_status_);
        s.foe = mesh_foe_;
        return s;
    }

    void on_daemon(const std::string& type, const JsonValue& msg) {
        if (type == "PVP_ACCEPT_RECEIVED") {
            MeshUpdate u;
            u.result = -1;   // marker: battle started, no state
            mesh_updates_.push_back(std::move(u));
            return;
        }
        if (type != "BATTLE_UPDATE") return;
        MeshUpdate u;
        u.my_hp = static_cast<int>(msg.get_int("my_hp"));
        u.enemy_hp = static_cast<int>(msg.get_int("enemy_hp"));
        u.turn = static_cast<int>(msg.get_int("turn"));
        u.result = static_cast<int>(msg.get_int("result"));
        u.my_status = static_cast<int>(msg.get_int("my_status"));
        u.enemy_status = static_cast<int>(msg.get_int("enemy_status"));
        u.need_switch = msg.get_int("need_switch") != 0;
        const JsonValue& pp = msg["my_pp"];
        if (pp.is_array()) {
            u.has_pp = true;
            for (int i = 0; i < 4; ++i) u.pp[i] = static_cast<int>(pp[i].as_int());
        }
        const JsonValue& log = msg["log"];
        if (log.is_array())
            for (const auto& l : log.items()) {
                const std::string s = ascii(l.as_string().c_str());
                if (!s.empty()) u.log.push_back(s);
            }
        mesh_updates_.push_back(std::move(u));
    }

    void process_mesh_updates() {
        bool any_log = false;
        while (!mesh_updates_.empty()) {
            MeshUpdate u = std::move(mesh_updates_.front());
            mesh_updates_.pop_front();
            if (u.result == -1) {
                mesh_started_ = true;
                next_ = Next::Action;
                continue;
            }
            mesh_started_ = true;
            if (mesh_foe_.max_hp <= 1 && u.enemy_hp > 0) mesh_foe_.max_hp = u.enemy_hp;
            if (u.my_hp > mesh_my_max_) mesh_my_max_ = u.my_hp;
            mesh_my_hp_ = u.my_hp;
            mesh_my_status_ = u.my_status;
            mesh_foe_.hp = u.enemy_hp;
            mesh_foe_.status = static_cast<uint8_t>(u.enemy_status);
            mesh_turn_ = u.turn;
            if (u.has_pp) {
                mesh_has_pp_ = true;
                for (int i = 0; i < 4; ++i) mesh_pp_[i] = u.pp[i];
            }
            for (const auto& l : u.log) push(l);
            any_log = any_log || !u.log.empty();
            if (u.result != 0) {
                next_ = Next::Result;
                outcome_ = u.result == 1 ? Outcome::Win : u.result == 2 ? Outcome::Lose
                         : u.result == 4 ? Outcome::Fled : Outcome::Draw;
            } else {
                next_ = u.need_switch ? Next::ForcedParty : Next::Action;
            }
        }
        finalize();
        if (phase_ == Phase::Messages) return;   // queued behind the current text
        if (any_log) start_messages();
        else if (phase_ == Phase::Wait) go_next();
    }

    void mesh_send(int action, uint8_t index) {
        ctx_.game.ipc().send_battle_action(action, index, mesh_turn_);
        forced_ = false;
        cur_text_ = "Waiting for " + mesh_name_ + "...";
        next_ = Next::Wait;
        set_phase(Phase::Wait);
    }

    // ── Drawing ─────────────────────────────────────────────────────────────
    struct InfoRefs {
        lv_obj_t* box = nullptr;
        lv_obj_t* name = nullptr;
        lv_obj_t* lv = nullptr;
        lv_obj_t* st = nullptr;
        lv_obj_t* bar = nullptr;
        lv_obj_t* nums = nullptr;
        lv_obj_t* sprite = nullptr;
        int dex = -1;
    };

    void clear_refs() {
        me_ = InfoRefs{};
        foe_ = InfoRefs{};
        msg_lbl_ = more_lbl_ = nullptr;
        for (auto& b : btn_) b = nullptr;
        for (auto& b : move_cells_) b = nullptr;
        for (auto& b : party_cells_) b = nullptr;
        move_type_lbl_ = nullptr;
        prog_lbl_ = nullptr;
        for (auto& p : pips_) p = nullptr;
        card_ = card_sub_ = card_name_ = nullptr;
    }

    void build_info(lv_obj_t* body, InfoRefs& r, int x, int y, int h, bool numbers) {
        r.box = panel(body, x, y, kBoxW, h);
        r.name = label(r.box, "", 6, 2, font_body(), color::kText, 88);
        r.st = chip(r.box, "PSN", 96, 4, c(0x8a94a0));
        r.lv = label(r.box, "", 120, 4, font_small(), color::kDim, 28);
        label(r.box, "HP", 6, numbers ? 18 : 18, font_small(), color::kAccent);
        r.bar = hp_bar(r.box, 24, 22, kBoxW - 32, 1, 1);
        if (numbers) {
            r.nums = label(r.box, "", 24, 25, font_small(), color::kText, kBoxW - 32);
            lv_obj_set_style_text_align(r.nums, LV_TEXT_ALIGN_RIGHT, 0);
        }
    }

    void build_field(lv_obj_t* body) {
        // Ground shadows under both sprites.
        auto shadow = [&](int x, int y, int w) {
            lv_obj_t* s = box(body, x, y, w, 10);
            lv_obj_set_style_bg_color(s, c(0x232b35), 0);
            lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        };
        shadow(kFoeSpriteX - 8, 50, 80);
        shadow(kMeSpriteX - 10, 88, 84);

        foe_.sprite = sprite_box(body, std::max(1, shown_.foe.dex), kFoeSpriteX, kFoeSpriteY, 64, 64, false,
                                 0, false, SpriteFit::Frame);
        foe_.dex = shown_.foe.dex;
        me_.sprite = sprite_box(body, std::max(1, shown_.me.dex), kMeSpriteX, kMeSpriteY, 64, 64, true,
                                0, false, SpriteFit::Frame);
        me_.dex = shown_.me.dex;
        build_info(body, foe_, kFoeBoxX, kFoeBoxY, kFoeBoxH, false);
        build_info(body, me_, kMeBoxX, kMeBoxY, kMeBoxH, true);
        if (trainer_battle() && !mesh_) {
            build_progress(body);
            // Between trainers (field empty): who's next, centred on the field.
            card_ = box(body, 0, 40, kBodyW, 52);
            card_sub_ = label(card_, pgym_ ? place_ : upper(gym_->city) + " GYM", 0, 2, font_small(), color::kDim);
            lv_obj_align(card_sub_, LV_ALIGN_TOP_MID, 0, 2);
            card_name_ = label(card_, "", 0, 18, font_big(), color::kText);
            lv_obj_align(card_name_, LV_ALIGN_TOP_MID, 0, 16);
        }
    }

    // Gauntlet progress between the foe's info box and sprite: "TRAINER 2/3"
    // over one pip per trainer (beaten / current / to come), leader pip last
    // and larger.
    // Player gym: one equal pip per mon of the leader's team instead.
    int pip_count() const {
        return pgym_ ? eng_.party(1).count : std::min<int>(trainer_count(), LORD_GYM_MAX_TRAINERS);
    }

    void build_progress(lv_obj_t* body) {
        const int n = pip_count();
        prog_lbl_ = label(body, "", kProgX, kProgY, font_small(), color::kDim, kProgW);
        lv_obj_set_style_text_align(prog_lbl_, LV_TEXT_ALIGN_CENTER, 0);
        const int total = (n - 1) * kPipStep + kLeaderPip;
        int x = kProgX + (kProgW - total) / 2;
        for (int i = 0; i < n; ++i) {
            const bool ldr = i == n - 1 && !pgym_;
            const int d = ldr ? kLeaderPip : kPip;
            lv_obj_t* p = box(body, x, kProgY + 16 + (kLeaderPip - d) / 2, d, d);
            lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(p, 1, 0);
            pips_[i] = p;
            x += kPipStep;
        }
    }

    void apply_progress() {
        if (!trainer_battle() || mesh_) return;
        const int cur = shown_.trainer;
        const int n = pip_count();
        title_ = "vs " + trainer_name(cur);
        if (card_) {
            const bool empty = !shown_.me.shown && !shown_.foe.shown &&
                               (shown_.foe.hp > 0 || shown_.foe.dex == 0);
            if (empty) lv_obj_remove_flag(card_, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(card_, LV_OBJ_FLAG_HIDDEN);
            set_text(card_name_, is_leader(cur) ? "Gym Leader " + leader_ : trainer_name(cur));
            lv_obj_set_style_text_color(card_name_, c(is_leader(cur) ? color::kAccent : color::kText), 0);
            lv_obj_align(card_name_, LV_ALIGN_TOP_MID, 0, 16);
        }
        if (prog_lbl_ && pgym_) {
            set_text(prog_lbl_, fmt("TEAM %d/%d", std::min(cur + 1, n), n));
        } else if (prog_lbl_) {
            set_text(prog_lbl_, fmt("%s %d/%d", is_leader(cur) ? "LEADER" : "TRAINER", cur + 1, n));
            lv_obj_set_style_text_color(prog_lbl_, c(is_leader(cur) ? color::kAccent : color::kDim), 0);
        }
        for (int i = 0; i < n; ++i) {
            if (!pips_[i]) continue;
            const bool ldr = i == n - 1 && !pgym_;
            uint32_t fill = color::kPanel, edge = ldr ? color::kAccent : color::kBorder;
            if (i < cur) fill = edge = color::kGood;
            else if (i == cur) fill = edge = color::kAccent;
            lv_obj_set_style_bg_color(pips_[i], c(fill), 0);
            lv_obj_set_style_border_color(pips_[i], c(edge), 0);
        }
    }

    void apply_side(InfoRefs& r, const SideSnap& s, bool animate, bool numbers) {
        if (!r.box) return;
        if (r.sprite) {
            if (s.dex != r.dex && s.dex > 0) {
                sprite_set(r.sprite, s.dex, &r == &me_);
                r.dex = s.dex;
            }
            if (s.shown && s.dex > 0) lv_obj_remove_flag(r.sprite, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(r.sprite, LV_OBJ_FLAG_HIDDEN);
        }
        // The info box is up while the mon is out, and stays (empty bar) on faint.
        if (s.shown || (s.hp == 0 && s.dex > 0)) lv_obj_remove_flag(r.box, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(r.box, LV_OBJ_FLAG_HIDDEN);
        set_text(r.name, s.name);
        set_text(r.lv, s.level > 0 ? fmt("Lv%d", s.level) : std::string());
        const char* st = s.hp == 0 && s.dex > 0 && !s.shown ? "FNT" : status_text(s.status);
        if (*st) {
            set_text(r.st, st);
            lv_obj_set_style_bg_color(r.st, c(s.hp == 0 ? color::kBad : status_color(s.status)), 0);
            lv_obj_remove_flag(r.st, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(r.st, LV_OBJ_FLAG_HIDDEN);
        }
        hp_bar_set(r.bar, s.hp, s.max_hp, animate);
        if (numbers) set_text(r.nums, fmt("%d / %d", s.hp, s.max_hp));
    }

    void apply_snap(bool animate) {
        apply_side(foe_, shown_.foe, animate, false);
        apply_side(me_, shown_.me, animate, true);
        apply_progress();
    }

    void build_message(lv_obj_t* body) {
        lv_obj_t* p = panel(body, 4, kStripY, kBodyW - 8, kStripH);
        msg_lbl_ = text_block(p, cur_text_, 6, 2, kBodyW - 30, font_body(), color::kText);
        more_lbl_ = label(p, LV_SYMBOL_DOWN, kBodyW - 24, 20, font_small(), color::kAccent);
        if (phase_ == Phase::Wait || (queue_.empty() && next_ != Next::Action))
            lv_obj_add_flag(more_lbl_, LV_OBJ_FLAG_HIDDEN);
    }

    void build_action(lv_obj_t* body) {
        // FIGHT / PARTY / RUN are the soft keys right under this strip.
        lv_obj_t* p = panel(body, 4, kStripY, kBodyW - 8, kStripH);
        text_block(p, "What will " + shown_.me.name + " do?", 6, 2, kBodyW - 24, font_body(), color::kText);
    }

    void paint_action() {
        for (int i = 0; i < 3; ++i) {
            if (!btn_[i]) continue;
            const bool s = i == act_sel_;
            lv_obj_set_style_bg_color(btn_[i], c(s ? color::kPanelHi : color::kPanel), 0);
            lv_obj_set_style_border_color(btn_[i], c(s ? color::kAccent : color::kBorder), 0);
            lv_obj_set_style_border_width(btn_[i], s ? 2 : 1, 0);
        }
    }

    void build_moves(lv_obj_t* body) {
        MoveSlot mv[4];
        active_moves(mv);
        // One column per move, each straight above its key (4..7); the soft
        // key under it shows the move's type in the type colour.
        for (int i = 0; i < 4; ++i) {
            constexpr int kCellW = kSoftKeyPitch - 3;
            lv_obj_t* cell = panel(body, softkey_center(i) - kCellW / 2, kStripY, kCellW, kStripH + 2);
            move_cells_[i] = cell;
            const Gen1MoveData* d = gen1Move(mv[i].id);
            if (!mv[i].id || !d) {
                lv_obj_t* l = label(cell, "-", 0, 0, font_body(), color::kDim);
                lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
                continue;
            }
            lv_obj_t* name = text_block(cell, title_case(wrap_move_name(move_name(mv[i].id), font_small(), kCellW - 4)),
                                        0, 0, kCellW - 4, font_small(), color::kText);
            lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
            const uint32_t ppc = mv[i].pp == 0 ? color::kBad
                               : mv[i].pp * 4 <= mv[i].max_pp ? color::kWarn : color::kDim;
            lv_obj_t* pp = label(cell, fmt("%d/%d", mv[i].pp, mv[i].max_pp), 0, 0, font_small(), ppc);
            lv_obj_align(pp, LV_ALIGN_BOTTOM_MID, 0, 0);
        }
        // Selected move's type, as a chip on the player's info box.
        if (me_.box) {
            move_type_lbl_ = chip(body, "", kMeBoxX + 4, kMeBoxY - 14, c(0x8a94a0));
        }
        paint_moves();
    }

    void paint_moves() {
        for (int i = 0; i < 4; ++i) {
            if (!move_cells_[i]) continue;
            const bool s = i == move_sel_;
            lv_obj_set_style_bg_color(move_cells_[i], c(s ? color::kPanelHi : color::kPanel), 0);
            lv_obj_set_style_border_color(move_cells_[i], c(s ? color::kAccent : color::kBorder), 0);
        }
        if (move_type_lbl_) {
            MoveSlot mv[4];
            active_moves(mv);
            const Gen1MoveData* d = gen1Move(mv[move_sel_].id);
            if (d && mv[move_sel_].id) {
                set_text(move_type_lbl_, fmt("%s  %d PWR", type_name(d->type), d->power));
                if (d->power == 0) set_text(move_type_lbl_, fmt("%s  STATUS", type_name(d->type)));
                lv_obj_set_style_bg_color(move_type_lbl_, type_color(d->type), 0);
                lv_obj_remove_flag(move_type_lbl_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(move_type_lbl_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    void build_party(lv_obj_t* body) {
        lv_obj_t* bg = box(body, 0, 0, kBodyW, kBodyH);
        lv_obj_set_style_bg_color(bg, c(color::kBg), 0);
        lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
        label(bg, forced_ ? "Bring out which POKEMON?" : "Switch to which POKEMON?", 6, 1,
              font_small(), color::kAccent);
        const auto rows = team_rows();
        const int act = active_index();
        for (size_t i = 0; i < rows.size() && i < 6; ++i) {
            const auto& r = rows[i];
            const int col = static_cast<int>(i) % 2, row = static_cast<int>(i) / 2;
            lv_obj_t* cell = panel(bg, 4 + col * 158, 16 + row * 40, 154, 38);
            party_cells_[i] = cell;
            const bool fainted = r.hp <= 0;
            lv_obj_t* sp = sprite_box(cell, r.dex, 1, 2, 32, 32);
            if (fainted && sp) lv_obj_set_style_image_opa(sp, LV_OPA_40, 0);
            label(cell, r.name, 36, 2, font_body(), fainted ? color::kDim : color::kText, 84);
            lv_obj_t* lvl = label(cell, fmt("Lv%d", r.level), 120, 4, font_small(), color::kDim, 30);
            lv_obj_set_style_text_align(lvl, LV_TEXT_ALIGN_RIGHT, 0);
            hp_bar(cell, 36, 22, 64, r.hp, r.max_hp);
            lv_obj_t* n = label(cell, fmt("%d/%d", r.hp, r.max_hp), 100, 18, font_small(),
                                fainted ? color::kBad : color::kText, 48);
            lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_RIGHT, 0);
            if (static_cast<int>(i) == act && !fainted)
                chip(cell, "OUT", 36, 26, c(color::kInfo));
            else if (fainted)
                chip(cell, "FNT", 36, 26, c(color::kBad));
            else if (*status_text(r.status))
                chip(cell, status_text(r.status), 36, 26, c(status_color(r.status)));
        }
        party_sel_ = std::clamp(party_sel_, 0, std::max(0, static_cast<int>(rows.size()) - 1));
        paint_party();
    }

    void paint_party() {
        for (int i = 0; i < 6; ++i) {
            if (!party_cells_[i]) continue;
            const bool s = i == party_sel_;
            lv_obj_set_style_bg_color(party_cells_[i], c(s ? color::kPanelHi : color::kPanel), 0);
            lv_obj_set_style_border_color(party_cells_[i], c(s ? color::kAccent : color::kBorder), 0);
            lv_obj_set_style_border_width(party_cells_[i], s ? 2 : 1, 0);
        }
    }

    std::string gym_win_line() const {
        const int before = trainer_count() - 1;
        if (before <= 0) return "You defeated Gym Leader " + leader_ + "!";
        return fmt("You beat %d trainer%s and Gym Leader %s!", before, before == 1 ? "" : "s",
                   leader_.c_str());
    }
    std::string gym_loss_line() const {
        const std::string who = is_leader(trainer_idx_) ? "Gym Leader " + leader_ : foe_name_;
        return fmt("%s won (%d/%d). Next time the gym starts over.", who.c_str(),
                   trainer_idx_ + 1, trainer_count());
    }

    void build_result(lv_obj_t* body) {
        lv_obj_t* p = panel(body, 4, 4, kBodyW - 8, kBodyH - 8);
        lv_obj_set_style_border_color(p, c(outcome_ == Outcome::Win ? color::kAccent
                                         : outcome_ == Outcome::Lose ? color::kBad : color::kBorder), 0);
        lv_obj_set_style_border_width(p, 2, 0);
        const char* head = "DRAW";
        uint32_t hc = color::kText;
        std::string line;
        const bool gym = req_.kind == BattleRequest::Kind::Gym && !mesh_;
        switch (outcome_) {
            case Outcome::Win:
                head = "VICTORY!";
                hc = color::kAccent;
                line = gym ? gym_win_line()
                     : pgym_ ? fmt("You beat %s's team in %d turns!", leader_.c_str(), challenge_.turns)
                     : mesh_ ? "You beat " + mesh_name_ + "!"
                     : "The wild " + shown_.foe.name + " fainted.";
                break;
            case Outcome::Lose:
                head = "DEFEAT";
                hc = color::kBad;
                line = gym ? gym_loss_line()
                     : pgym_ ? leader_ + "'s gym held you off. Only a win puts you on the ladder."
                     : "Your team fainted. You blacked out!";
                break;
            case Outcome::Fled:
                head = "GOT AWAY";
                line = "You got away safely.";
                break;
            default:
                line = pgym_ ? "Nobody is left standing. " + leader_ + "'s gym counts it as defended."
                             : std::string("Nobody is left standing.");
                break;
        }
        lv_obj_t* h = label(p, head, 0, 6, font_big(), hc);
        lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 4);
        const int dex = outcome_ == Outcome::Lose ? shown_.foe.dex : shown_.me.dex;
        sprite_box(p, std::max(1, dex), 16, 38, 64, 64);
        text_block(p, line, 92, 34, kBodyW - 8 - 104, font_body(), color::kText);
        if (outcome_ == Outcome::Win && !badge_.empty()) {
            chip(p, badge_, 92, 76, c(color::kAccent), on_color(color::kAccent));
            const int count = ctx_.game.badge_count() + (demo_ ? 1 : 0);
            label(p, fmt("Badges: %d/8", std::min(8, count)), 92, 98, font_small(), color::kDim);
        } else if (pgym_ && outcome_ == Outcome::Win) {
            chip(p, challenge_.rank > 0 ? fmt("#%d ON THE LADDER", challenge_.rank)
                                        : std::string("GYM BEATEN"), 92, 76, c(color::kAccent),
                 on_color(color::kAccent));
            label(p, challenge_.new_best ? place_
                                         : fmt("Your best here: %d turns", challenge_.best_turns),
                  92, 98, font_small(), color::kDim);
        } else if (!mesh_) {
            const auto rows = team_rows();
            int alive = 0;
            for (const auto& r : rows) alive += r.hp > 0;
            label(p, fmt("%d/%zu of your team still standing", alive, rows.size()), 92, 98,
                  font_small(), color::kDim);
        }
    }

    // ── State ───────────────────────────────────────────────────────────────
    BattleRequest req_;
    Eng eng_;
    bool mesh_ = false;
    bool demo_ = false;
    bool error_ = false;
    bool forced_ = false;
    bool foe_sending_ = false;
    bool pending_pop_ = false;
    bool down_[2] = {true, true};    // side's sprite currently off the field
    std::vector<PartyMon> team_;     // party copy in battle order (local; heal source)
    std::string title_ = "Battle";
    std::string leader_;             // "BROCK" (gym)
    const LordGym* gym_ = nullptr;   // gym gauntlet (Kind::Gym)
    bool pgym_ = false;              // Mesh Space gym (Kind::PlayerGym)
    bool reported_ = false;          // its result went to Mesh Space
    GymChallenge challenge_;         // ... and this came back
    std::string place_;              // "SUMMIT GYM"
    int trainer_idx_ = 0;            // current gauntlet trainer (leader = last)
    std::string foe_name_;           // "SWIMMER" / "MISTY": current trainer

    Phase phase_ = Phase::Messages;
    Next next_ = Next::Action;
    Outcome outcome_ = Outcome::None;
    std::string badge_;              // "BOULDERBADGE" once earned
    std::deque<Msg> queue_;
    std::vector<Msg> building_;
    std::string cur_text_;
    Snap shown_;
    int act_sel_ = 0, move_sel_ = 0, party_sel_ = 0;

    // Mesh
    std::string mesh_name_;
    SideSnap mesh_foe_;
    int mesh_active_ = 0, mesh_turn_ = 0;
    int mesh_my_hp_ = 1, mesh_my_max_ = 1, mesh_my_status_ = 0;
    bool mesh_has_pp_ = false, mesh_started_ = false;
    int mesh_pp_[4] = {0, 0, 0, 0};
    std::deque<MeshUpdate> mesh_updates_;

    // Widgets from the latest build()
    InfoRefs me_, foe_;
    lv_obj_t* msg_lbl_ = nullptr;
    lv_obj_t* more_lbl_ = nullptr;
    lv_obj_t* btn_[3] = {};
    lv_obj_t* move_cells_[4] = {};
    lv_obj_t* party_cells_[6] = {};
    lv_obj_t* move_type_lbl_ = nullptr;
    lv_obj_t* prog_lbl_ = nullptr;
    lv_obj_t* card_ = nullptr;       // between-trainers name card
    lv_obj_t* card_sub_ = nullptr;
    lv_obj_t* card_name_ = nullptr;
    lv_obj_t* pips_[LORD_GYM_MAX_TRAINERS] = {};
};

} // namespace

std::unique_ptr<Screen> make_battle_screen(Ctx& ctx) {
    return std::make_unique<BattleScreen>(ctx);
}

} // namespace mm::ui
