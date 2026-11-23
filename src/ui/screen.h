// screen.h — the contract every MonsterMesh screen implements.
//
// Layout: the Router owns a persistent header (kHeaderH: title on the left,
// daemon/demo status on the right) and footer (kFooterH: key hints). A screen
// only ever draws into its `body`, a kBodyW x kBodyH (320x138) container at
// y = kHeaderH. Build everything with mm::ui widgets; no LVGL groups/focus.
//
// Keys: the Router translates every key press into a KeyEvent and gives it to
// the top screen's on_key(). Return true if handled. An unhandled Key::B pops
// the screen (on the menu: shows "hold Esc to exit").
//
// Lifetime: one instance per push. build() may be called more than once
// (Router::rebuild(), demo states); it always gets an empty body. Keep
// lv_obj_t* members only for objects created in the latest build().
#pragma once

#include "keys.h"
#include "softkeys.h"

#include "lvgl.h"

#include <cstdint>
#include <memory>
#include <string>

namespace mm {
class GameState;
}

namespace mm::ui {

class Router;

enum class ScreenId : uint8_t {
    Menu,
    Party,
    Daycare,
    Breeding,
    GymSelect,
    Battle,
    Wifi,
    Buddies,      // 1999 Online: buddy list
    Im,           // instant message with Ctx::peer
    Space,        // Mesh Space (profile / wall / gym) of Ctx::peer (0 = mine)
    Amp,          // MeshAmp player
    Skins,        // skin picker
    Monster,      // MonsterMesh: the game's own menu (party, gyms, battle, ...)
    Radio,        // Meshtastic radio setup
    Mailbox,      // Mailbox: inbox + outbox that sends when the buddy comes back
    Map,          // MeshMap: buddies plotted from their shared positions
    Hood,         // Mesh Neighborhood: nodes in range, signal, hops, ping/traceroute
    Ringtones,    // Ringtones: compose / play / send one-packet tunes
    Snake,        // Snake
    Calc,         // Calculator
    Mines,        // MeshMines
    Puzzle,       // Bit Quest
    Savers,       // Screen Savers
    Beats,        // MeshBeat drum machine
    Count,
};

// What the Battle screen should start when it is pushed.
struct BattleRequest {
    // PlayerGym: a Mesh Space gym. The owner is not online: their posted team
    // (`team`, in order) is played by the AI like a gym leader; the result is
    // written to MeshSpace::record_challenge(node_id, ...).
    enum class Kind : uint8_t { Practice, Gym, Mesh, PlayerGym } kind = Kind::Practice;
    int gym = 0;              // 0..7 Brock..Giovanni (Kind::Gym)
    uint32_t node_id = 0;     // opponent (Kind::Mesh) / gym owner (Kind::PlayerGym)
    int wild_dex = 0;         // Practice vs. a specific species (0 = random)
    int wild_level = 0;

    // Kind::PlayerGym only.
    struct Mon {
        uint16_t dex = 0;     // national dex (only 1..151 can fight)
        uint8_t level = 0;
    };
    Mon team[6] = {};         // leader's team, lead first
    uint8_t team_count = 0;
    std::string trainer;      // owner's name ("KD2ABC-m")
    std::string place;        // gym name ("Summit Gym")
    std::string prize;        // badge name ("Summit Badge"; may be empty)
};

struct Ctx {
    GameState& game;
    Router& router;
    BattleRequest battle;     // set before router.push(ScreenId::Battle)
    uint32_t peer = 0;        // node id for ScreenId::Im / ScreenId::Space (0 = me)
};

class Screen {
public:
    explicit Screen(Ctx& ctx) : ctx_(ctx) {}
    virtual ~Screen() = default;
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    virtual const char* title() const = 0;
    // true: 1999 Online window chrome; build() gets a kClientW x kClientH
    // (312x121) body and draws with ui/win.h. false (legacy game screens): a
    // 320x138 body under an 18px title strip.
    virtual bool windowed() const { return false; }
    // A skin id ("winamp") this screen always wears, whatever the user picked;
    // nullptr: the user's skin.
    virtual const char* forced_skin() const { return nullptr; }
    // True while the screen wants the whole display: no status strip, window
    // frame or hint bar, and a body of the full screen size. May change at
    // run time; call Router::rebuild() after it does.
    virtual bool fullscreen() const { return false; }
    // Fill `body` (320x138, already empty). Called on push and on rebuild().
    virtual void build(lv_obj_t* body) = 0;
    virtual bool on_key(const KeyEvent& e) { (void)e; return false; }
    // True while a text field takes letters. Otherwise the Router turns the
    // keys printed with arrows (F up, Z left, X down, C right) into arrows.
    virtual bool typing() const { return false; }
    // ~30 Hz while on top. GameState::revision() tells you if data changed.
    virtual void tick(uint32_t now_ms) { (void)now_ms; }
    virtual void on_show() {}      // became top (after push, or pop of the screen above)
    virtual void on_hide() {}      // another screen pushed on top / about to be popped
    // Footer text, e.g. "Enter:pick  Tab:sort  Esc:back". Re-read after on_key().
    virtual std::string hints() const { return "Enter:select  Esc:back"; }
    // Choices on the number keys under the display (softkeys.h). Non-empty:
    // the Router draws them in place of the hint strip, each label right above
    // its key. The screen still handles the keys in on_key() (softkey_slot()).
    // Re-read after on_key() and rebuild(). Key 3 falls back to Esc (back) when
    // `left` is set and on_key() does not take it.
    virtual SoftKeys softkeys() const { return {}; }

    // Layout review / headless screenshots (--shots): a screen may expose
    // several canned states (e.g. battle: intro, move menu, faint). The router
    // calls set_demo_state(i) and then rebuild()s.
    virtual int demo_state_count() const { return 1; }
    virtual const char* demo_state_name(int i) const { (void)i; return "main"; }
    virtual void set_demo_state(int i) { (void)i; }

protected:
    Ctx& ctx_;
};

using ScreenFactory = std::unique_ptr<Screen> (*)(Ctx&);

} // namespace mm::ui
