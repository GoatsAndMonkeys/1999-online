// game_state.h — the one data model the MonsterMesh Cardputer UI reads.
//
// Owned by the app (one instance). Call tick() every ~50–100 ms from the UI
// loop; it pumps the mmd IPC socket (never blocks more than a few ms) and
// applies daemon pushes. Screens cache revision() and redraw when it changes.
//
// DEMO MODE: until the daemon has been reached at least once this session,
// GameState serves a plausible built-in party / daycare / neighbour / event
// set so every screen has data (always the case on the dev Mac). The first
// successful connection replaces all of it with live data. If the daemon later
// drops, the last live data is KEPT (daemon_connected() == false, demo_mode()
// stays false) so a daemon restart doesn't flash fake mons; reconnection is
// automatic. MM_FORCE_DEMO=1 pins demo mode and disables IPC.
//
// Persistence (same files mmterm uses; env overrides for dev):
//   Bill's PC box  : MM_BOX_PATH  || $MM_STATE_DIR/breeding_box.dat
//                    || MMD_STATE_DIR/breeding_box.dat   (IpcProtocol.h:
//                       /tmp/monstermesh on macOS, /var/lib/monstermesh on Linux)
//   Gym badges     : MM_LORD_PATH || $MM_STATE_DIR/lord.dat || LORD_SAVE_PATH (LordSave.h)
//   Breeder rooms  : MM_ROOMS_PATH || <box dir>/breeder_rooms.dat  (mmterm's file:
//                    "RRMB" magic, v1, uint16 sizeof(BreederRoom), raw room array)
//   Breed cooldowns: <rooms dir>/breeder_cooldowns.dat  (cardputer-only sidecar;
//                    the box format has no lastBredAt slot — see save_rooms())
//   Daemon socket  : MM_SOCK_PATH || MMD_SOCK_PATH (/tmp/monstermesh.sock)
//
// LVGL-free, SDL-free. Not thread-safe: call everything from the UI thread.
#pragma once

#include "ipc_client.h"
#include "mm_types.h"

#include "BreedingApp.h"   // monster_mesh_pi/src/shared
#include "BreederRoom.h"   // monster_mesh_pi/src/shared (timed breeder rooms)

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mm {

class GameState {
public:
    struct Options {
        std::string sock_path;   // "" => env MM_SOCK_PATH or MMD_SOCK_PATH
        std::string box_path;    // "" => env / default (see header comment)
        std::string lord_path;   // "" => env / default
        std::string rooms_path;  // "" => env MM_ROOMS_PATH or next to the box file
        bool force_demo = false; // also enabled by env MM_FORCE_DEMO=1
        bool enable_ipc = true;  // false => never try to reach the daemon
        bool load_files = true;  // false => skip box/lord/rooms load AND never write
                                 //   rooms/cooldowns or the hatch box save (shots/tests)
    };

    static constexpr size_t kMaxEvents = 50;
    static constexpr uint32_t kStatusPollMs = 5000;   // GET_STATUS cadence while live
    static constexpr uint32_t kDemoEventMs = 45000;   // ambient demo event cadence
    static constexpr uint32_t kRoomTickMs = 1000;     // breeder-room clock cadence
    static constexpr size_t kMaxHatches = 8;          // queued hatch results (oldest dropped)
    static constexpr uint32_t kFileWatchMs = 3000;    // box/rooms file change check

    GameState();
    explicit GameState(const Options &opt);
    ~GameState();
    GameState(const GameState &) = delete;
    GameState &operator=(const GameState &) = delete;

    // ── Pump ────────────────────────────────────────────────────────────────
    void tick();                       // call every ~50–100 ms
    uint32_t revision() const { return revision_; }   // bumps on any change

    bool daemon_connected() const;
    bool demo_mode() const { return demo_; }

    // ── Data ────────────────────────────────────────────────────────────────
    const std::vector<PartyMon> &party() const { return party_; }
    const DaycareStatus &daycare() const { return status_; }
    // Newest event FIRST (front); capped at kMaxEvents.
    const std::deque<DaycareEvent> &events() const { return events_; }
    // Current queued incoming PvP challenge (not consumed; cleared by
    // accept_challenge()/decline_challenge()).
    std::optional<IncomingChallenge> pending_challenge();
    // Pops the oldest unseen achievement toast, if any.
    std::optional<Achievement> pop_achievement();
    // Party of a neighbour fetched via request_neighbor_party() (nullptr if none yet).
    const std::vector<PartyMon> *neighbor_party(uint32_t node_id) const;
    // Last PvP / beacon outcome line for a status toast ("" if none).
    const std::string &last_pvp_status() const { return pvp_status_; }
    const std::string &last_error() const { return last_error_; }

    // ── Bill's PC (breeding box) ────────────────────────────────────────────
    breeding::BreedingApp &breeding() { return breed_; }
    const breeding::BreedingApp &breeding() const { return breed_; }
    bool save_box();                   // atomic rewrite in mmterm's v3 box format
    bool reload_box();                 // re-read the box file (e.g. mmterm changed it)
    bool box_is_test_roster() const { return box_test_; }   // file absent => seedTestRoster()
    // false in --shots / tests (Options::load_files): screens must not write any file.
    bool files_enabled() const { return opt_.load_files; }
    const std::string &box_path() const { return box_path_; }

    // ── Breeder rooms (timed nursery, shared engine + file with mmterm) ──────
    // 3 rooms (breeding::BreederManager). A pair placed at T lays an egg at
    // T + 1 h, which hatches at T + 3 h into a new box mon (BREED_OK) or fails
    // (hh egg, BREED_NO_HATCH). Placing starts both parents' 7-day cooldown.
    // tick() advances the rooms on the wall clock once a second; any change is
    // saved (rooms file), a hatch also saves the box, and every hatch/fail is
    // queued for the UI (pop_hatch()). Room parent ids are re-matched to the
    // live roster after every box/rooms (re)load, since box ids aren't stable.
    // Every kFileWatchMs, tick() also reloads the box/rooms files when another
    // process (mmterm) has replaced them (inode/size/mtime differ from ours).
    struct Hatch {
        int room = -1;
        breeding::BreedResult result;      // result.child.id = its id in the box (BREED_OK)
        breeding::BreedMon parent_a, parent_b;
    };
    const breeding::BreederManager &rooms() const { return rooms_; }
    // Place roster indices a, b in the first free room at `now` (epoch s).
    // Returns the room index, or -1 with `err` set (engine message, may hold UTF-8).
    int place_pair(size_t a, size_t b, long now, std::string &err);
    bool cancel_room(int room);        // empties it (an egg is lost; cooldown stands)
    int tick_rooms(long now);          // advance to `now`; returns # of hatch events
    std::optional<Hatch> pop_hatch();  // oldest unseen hatch/fail result
    // One-line announcement per hatch/fail, for a global toast (the router);
    // separate from pop_hatch() so the breeding screen still gets the result.
    std::optional<std::string> pop_hatch_notice();
    size_t pending_hatches() const { return hatches_.size(); }
    bool save_rooms();                 // atomic; false (no write) when !load_files
    bool reload_rooms();               // re-read rooms + cooldowns (e.g. mmterm changed them)
    const std::string &rooms_path() const { return rooms_path_; }

    // ── Gym progress (LORD save, 8 Kanto gyms, bit n = gym n) ──────────────
    uint8_t badges() const { return badges_; }
    bool has_badge(int gym) const { return gym >= 0 && gym < 8 && ((badges_ >> gym) & 1); }
    int badge_count() const;
    void set_badge(int gym);           // gym 0..7 (Brock..Giovanni); persists to lord.dat
    bool reload_badges();
    const std::string &lord_path() const { return lord_path_; }

    // ── Actions (forward to IPC; simulated in demo mode) ───────────────────
    void request_party();
    void request_status();
    void force_event();
    void force_beacon();
    void hollaback();                  // MQTT "HB!" roster refresh (daemon HOLLABACK)
    void challenge(uint32_t node_id);
    void accept_challenge();
    void decline_challenge();
    void request_neighbor_party(uint32_t node_id);

    // Demo helper: fake an incoming challenge from the first demo neighbour
    // (no-op when live). Also triggered by env MM_DEMO_CHALLENGE_SEC=<n>.
    void simulate_incoming_challenge();

    // Every parsed daemon message is also passed here (type, object) so other
    // layers (e.g. a PvP battle screen) can consume BATTLE_UPDATE,
    // PVP_ACCEPT_RECEIVED, MY_WIRE_PARTY, SAV_WRITEBACK, ... without changes here.
    using MessageHook = std::function<void(const std::string &type, const JsonValue &msg)>;
    void set_message_hook(MessageHook hook) { hook_ = std::move(hook); }

    IpcClient &ipc() { return ipc_; }

    // Exposed for tests / screens that build their own PartyMon:
    static void derive_stats(PartyMon &m);   // fills max_hp/hp/exp from dex+level+dvs+stat_exp
    static PartyMon party_mon_from_json(const JsonValue &slot);

private:
    Options opt_;
    IpcClient ipc_;
    uint32_t revision_ = 1;
    bool demo_ = true;
    bool ever_live_ = false;

    std::vector<PartyMon> party_;
    DaycareStatus status_;
    std::deque<DaycareEvent> events_;
    std::optional<IncomingChallenge> challenge_;
    std::deque<Achievement> achievements_;
    std::map<uint32_t, std::vector<PartyMon>> neighbor_parties_;
    std::string pvp_status_;
    std::string last_error_;

    breeding::BreedingApp breed_;
    bool box_test_ = false;
    std::string box_path_;

    uint8_t badges_ = 0;
    std::string lord_path_;

    breeding::BreederManager rooms_;
    std::deque<Hatch> hatches_;
    std::map<uint64_t, long> cooldowns_;   // mon fingerprint -> lastBredAt (still active)
    std::string rooms_path_, cooldown_path_;
    uint64_t last_room_tick_ms_ = 0;
    std::deque<std::string> hatch_notices_;

    // Box/rooms file identity as last read or written by us; a different one
    // means another process (mmterm) replaced the file -> reload it.
    struct FileStamp {
        uint64_t ino = 0, size = 0;
        int64_t mtime = 0;
        bool exists = false;
        bool operator==(const FileStamp &o) const {
            return ino == o.ino && size == o.size && mtime == o.mtime && exists == o.exists;
        }
        bool operator!=(const FileStamp &o) const { return !(*this == o); }
    };
    FileStamp box_stamp_, rooms_stamp_;
    uint64_t last_file_watch_ms_ = 0;
    uint64_t room_rng_ctr_ = 0;

    uint64_t last_status_poll_ms_ = 0;
    uint64_t demo_next_event_ms_ = 0;
    uint64_t demo_challenge_at_ms_ = 0;
    size_t demo_event_idx_ = 0;
    int demo_forced_events_ = 0;

    MessageHook hook_;

    void bump() { ++revision_; }
    void resolve_paths();
    bool load_box_file();
    void sync_rooms_with_box();        // re-match room parent ids + apply cooldowns
    void refresh_cooldowns(long now);  // roster lastBredAt -> cooldowns_ (drops expired)
    static FileStamp stamp_of(const std::string &path);
    void watch_files();                // reload box/rooms if another process changed them
    void load_demo();
    void go_live();
    void demo_tick(uint64_t now);
    void push_event(DaycareEvent e);
    void on_message(const JsonValue &msg, const std::string &raw, bool parsed);
    void on_connection(bool up);
    static uint64_t now_ms();
};

}  // namespace mm
