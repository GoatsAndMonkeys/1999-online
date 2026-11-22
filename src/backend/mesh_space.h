// mesh_space.h — data model behind the Mesh Space screen: one small personal
// page per mesh node (profile, wall, gym + champions ladder).
//
// Today it is seeded with demo pages. A mesh backend fills it later through
// the set_*/post/record_win calls below (they bump revision(), the screen
// redraws); nothing here knows about the radio, LVGL or the daemon.
//
// Node 0 is always "me". Unknown nodes get a sensible empty page.
// Not thread-safe: call everything from the UI thread.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mm {

struct SpaceMon {
    uint16_t dex = 0;      // national dex number
    uint8_t level = 0;
};

struct SpaceProfile {
    std::string name;
    std::string headline;
    std::string about;
    std::string mood;
    bool online = false;
    int friends = 0;
    std::vector<std::string> top8;    // up to kTop
    uint16_t icon_dex = 0;            // buddy icon (national dex); 0 = the gym lead
};

struct WallPost {
    std::string author;
    std::string text;
    std::string time;                 // "Nov 20 11:02 AM"
};

// One line of the Updates feed: what happened around a node's gym and page.
// Daycare events are deliberately not part of it (daycare has its own screen).
enum class UpdateKind : uint8_t {
    GymBeaten,              // "Trailhead-R beat {your} gym in 31 turns"
    LadderMove,             // "ash_k3tchup moved up to #2 on the champions ladder"
    GymDefended,            // "{Your} gym held off W2XYZ"
    WallPost,               // "base_stn wrote on {your} wall"
    NewChampionElsewhere,   // "{You are} now #1 on Ridge_Gym's ladder"
    BadgeEarned,            // "{You} earned the Trail Badge"
};

// `text` is written from the page owner's point of view with the tokens
// {your} {Your} {You} {You are}; MeshSpace::render() turns them into "your" /
// "You are" on my own page and "KD2ABC-m's" / "KD2ABC-m is" on someone else's.
struct SpaceUpdate {
    UpdateKind kind = UpdateKind::GymBeaten;
    std::string text;
    std::string time;                 // "Nov 20 11:02 AM"
};

// Somebody who BEAT the gym owner. Losses are never listed.
struct Champion {
    std::string name;
    int turns = 0;
    std::string date;                 // "Nov 14"
    std::vector<SpaceMon> party;      // up to kPartyMax
};

struct SpaceGym {
    std::string name;                 // "Summit Gym" ("" = no gym yet)
    std::string badge;                // "Summit Badge"
    std::vector<SpaceMon> party;      // leader's party, up to kPartyMax
    int defended = 0;
    int lost = 0;
    std::vector<Champion> champions;  // sorted: fewest turns first, one per name
};

// Outcome of one challenge of a player gym, as returned by record_challenge().
// The Gym tab shows my latest one after the battle.
struct GymChallenge {
    uint32_t serial = 0;              // 0 = none / not recorded; else counts my challenges
    uint32_t node = 0;                // gym owner
    bool won = false;
    int turns = 0;
    int old_rank = 0;                 // challenger's ladder rank before (0 = not on it)
    int rank = 0;                     // ... and after
    int best_turns = 0;               // turns of their ladder entry afterwards (0 = none)
    bool new_best = false;            // this run is now their ladder entry
    bool badge = false;               // first win here: the gym's badge was earned
};

class MeshSpace {
public:
    static constexpr uint32_t kMe = 0;
    static constexpr uint32_t kDemoNode = 0xa3f29c01;   // KD2ABC-m
    static constexpr uint32_t kDemoRidge = 0x7c11d402;  // Ridge_Gym
    static constexpr uint32_t kDemoTrail = 0x5be0a903;  // Trailhead-R
    static constexpr size_t kTop = 8;
    static constexpr size_t kPartyMax = 6;
    static constexpr size_t kPostMax = 200;             // bytes per wall post
    static constexpr size_t kWallMax = 50;              // posts kept per wall (oldest dropped)
    static constexpr size_t kUpdatesMax = 40;           // feed lines kept per node

    MeshSpace();
    ~MeshSpace();                                       // save() if my page changed

    // ── Persistence (space.json in the config folder, see settings.h) ───────
    // Only MY page (kMe) is kept: buddy icon, headline/about/mood, my wall, my
    // gym's record + champions ladder, my updates. load() lays the file over
    // the seeded page (called by the constructor); save() writes it when it
    // differs from what is on disk; tick() calls save() at most once a second
    // after a change. All three do nothing when store::enabled() is false.
    bool load();
    bool save();
    void tick(uint32_t now_ms);

    // ── Read ────────────────────────────────────────────────────────────────
    bool known(uint32_t node) const;                    // false: the empty page is served
    const SpaceProfile& profile(uint32_t node) const;
    const std::vector<WallPost>& wall(uint32_t node) const;   // oldest first
    const SpaceGym& gym(uint32_t node) const;
    const std::vector<SpaceUpdate>& updates(uint32_t node) const;   // oldest first
    // Feed line as shown on `node`'s page (owner tokens resolved, see SpaceUpdate).
    std::string render(uint32_t node, const SpaceUpdate& u) const;
    // "synced 2 min ago, 2 hops" ("" for my own page, "not synced yet" if unknown).
    std::string synced(uint32_t node) const;
    const std::string& my_name() const { return profile(kMe).name; }
    // Bumps on every change.
    uint32_t revision() const { return revision_; }

    // ── Actions (UI) ────────────────────────────────────────────────────────
    // Appends a post stamped with the local time. Text is trimmed and cut to
    // kPostMax bytes; false if nothing is left to post. A post by anyone but
    // the page owner also adds a WallPost update to the node's feed.
    bool post(uint32_t node, const std::string& author, const std::string& text);
    // `name` beat `node`'s gym in `turns`. Keeps one entry per name (a better
    // run replaces the old one; a worse one is ignored and returns false) and
    // the ladder sorted by fewest turns. `date` "" = today. Feeds the node's
    // updates: GymBeaten, plus a LadderMove for everyone whose rank changed;
    // when the winner is me, my own feed gets NewChampionElsewhere.
    bool record_win(uint32_t node, const std::string& name, int turns,
                    const std::vector<SpaceMon>& party, const std::string& date = "");
    // A finished fight at `node`'s gym (the owner's posted team, AI-played while
    // they are away). Win: lost+1 and record_win() (ladder + GymBeaten /
    // LadderMove / NewChampionElsewhere; BadgeEarned in my feed on my first win
    // there). Loss, draw or forfeit: defended+1 and a GymDefended update for
    // the owner. Nobody can challenge their own gym (returns serial 0, nothing
    // changes). When the challenger is me the result is kept as last_challenge().
    GymChallenge record_challenge(uint32_t node, const std::string& name, bool won, int turns,
                                  const std::vector<SpaceMon>& party);
    const GymChallenge& last_challenge() const { return last_challenge_; }
    void set_leader_party(uint32_t node, const std::vector<SpaceMon>& party);
    void set_icon(uint32_t node, uint16_t dex);          // 0 = back to the gym lead
    // Appends a feed line (`time` "" = now). The backend calls this for
    // GymDefended / BadgeEarned / NewChampionElsewhere when it learns of them.
    void add_update(uint32_t node, UpdateKind kind, const std::string& text,
                    const std::string& time = "");

    // ── Backend feed (mesh sync, later) ─────────────────────────────────────
    void set_profile(uint32_t node, const SpaceProfile& p);
    void set_wall(uint32_t node, std::vector<WallPost> posts);   // oldest first
    void add_post(uint32_t node, const WallPost& p);             // with its own time string
    void set_gym(uint32_t node, const SpaceGym& g);              // champions are re-sorted
    void set_synced(uint32_t node, const std::string& text);

private:
    struct Page {
        SpaceProfile profile;
        std::vector<WallPost> wall;
        SpaceGym gym;
        std::vector<SpaceUpdate> updates;
        std::string synced;
    };

    Page& page(uint32_t node);                // creates it
    const Page& look(uint32_t node) const;    // never creates
    static void sort_ladder(SpaceGym& g);
    void seed_demo();

    std::map<uint32_t, Page> pages_;
    mutable Page blank_;                      // served for unknown nodes
    uint32_t revision_ = 1;

    std::string my_page_json() const;         // space.json text for the kMe page
    std::string saved_json_;                  // my page as on disk (or the demo page if no file)
    uint32_t saved_rev_ = 1;                  // revision_ last looked at by tick()
    uint32_t save_since_ms_ = 0;
    bool save_armed_ = false;
    GymChallenge last_challenge_;
    uint32_t challenge_serial_ = 0;
};

MeshSpace& mesh_space();

} // namespace mm
