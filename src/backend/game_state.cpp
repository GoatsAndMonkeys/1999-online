// game_state.cpp — see game_state.h.
#include "game_state.h"

// Read-only reuse of monster_mesh_pi/src (include dirs: src/shared, src/battle).
#include "IpcProtocol.h"              // MMD_SOCK_PATH, MMD_STATE_DIR
#include "LordSave.h"                 // LordSave struct, LORD_SAVE_PATH, LORD_MAGIC/VERSION
#include "DaycareSavPatcher.h"        // speciesGrowthRate[] / expForLevel() (header-only)
#include "showdown_gen1_basestats.h"  // GEN1_BASE_STATS[152]
#include "showdown_gen3_basestats.h"  // GEN3_BASE_STATS[387] (Gen 2/3 saves)

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <type_traits>

#include <sys/stat.h>
#include <unistd.h>

namespace mm {

namespace {

const char *env_or_null(const char *name) {
    const char *v = getenv(name);
    return (v && *v) ? v : nullptr;
}

bool env_flag(const char *name) {
    const char *v = env_or_null(name);
    return v && (strcmp(v, "0") != 0) && (strcmp(v, "false") != 0);
}

std::string dir_of(const std::string &path) {
    size_t s = path.find_last_of('/');
    if (s == std::string::npos || s == 0) return std::string();
    return path.substr(0, s);
}

// mkdir -p for the parent directory of `path` (best effort).
void ensure_parent_dir(const std::string &path) {
    std::string d = dir_of(path);
    if (d.empty()) return;
    std::string cur;
    size_t i = 0;
    while (i <= d.size()) {
        size_t j = d.find('/', i);
        if (j == std::string::npos) j = d.size();
        cur = d.substr(0, j);
        if (!cur.empty()) mkdir(cur.c_str(), 0755);
        i = j + 1;
    }
}

bool read_file(const std::string &path, std::vector<uint8_t> &out, size_t max_len) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = false;
    if (len >= 0 && (size_t)len <= max_len) {
        out.resize((size_t)len);
        ok = (len == 0) || fread(out.data(), 1, (size_t)len, f) == (size_t)len;
    }
    fclose(f);
    return ok;
}

// Write via temp file + rename so a crash never leaves a half-written save.
bool write_file_atomic(const std::string &path, const void *data, size_t len) {
    ensure_parent_dir(path);
    std::string tmp = path + ".mmtmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = (len == 0) || fwrite(data, 1, len, f) == len;
    ok = (fflush(f) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { unlink(tmp.c_str()); return false; }
    if (rename(tmp.c_str(), path.c_str()) != 0) { unlink(tmp.c_str()); return false; }
    return true;
}

// Byte-identical to TerminalUI.cpp's wifiEncSerializeMon (box format v2/v3,
// 34-byte record): 22-byte CaughtMon prefix + 11-byte trip tail + tritan byte.
void serialize_box_mon(const breeding::BreedMon &m, uint8_t rec[breeding::BREEDBOX_REC_SIZE]) {
    memset(rec, 0, breeding::BREEDBOX_REC_SIZE);
    rec[0] = m.dex;
    rec[1] = m.level;
    rec[2] = m.geno.rainbow;   rec[3] = m.geno.shiny;     rec[4] = m.geno.dark;
    rec[5] = m.geno.sterile;   rec[6] = m.geno.cantFight; rec[7] = m.geno.noHatch;
    rec[8] = m.geno.female;
    rec[9]  = (uint8_t)(m.caughtSec & 0xFF);
    rec[10] = (uint8_t)((m.caughtSec >> 8) & 0xFF);
    rec[11] = (uint8_t)((m.caughtSec >> 16) & 0xFF);
    rec[12] = (uint8_t)((m.caughtSec >> 24) & 0xFF);
    rec[13] = (uint8_t)(((m.prov == breeding::PROV_WILD) ? 0 : (m.provGen ? m.provGen : 1)) |
                        (m.favorite ? 0x80 : 0));
    memcpy(rec + 14, m.nick, 8);
    uint8_t *t = rec + 22;
    t[0]  = m.tripLevel;
    t[1]  = (uint8_t)(m.tripXp & 0xFF);
    t[2]  = (uint8_t)((m.tripXp >> 8) & 0xFF);
    t[3]  = (uint8_t)((m.tripXp >> 16) & 0xFF);
    t[4]  = (uint8_t)((m.tripXp >> 24) & 0xFF);
    t[5]  = (uint8_t)(m.tripGymBeaten & 0xFF);
    t[6]  = (uint8_t)((m.tripGymBeaten >> 8) & 0xFF);
    t[7]  = (uint8_t)(m.tripWins & 0xFF);
    t[8]  = (uint8_t)((m.tripWins >> 8) & 0xFF);
    t[9]  = (uint8_t)(m.tripLosses & 0xFF);
    t[10] = (uint8_t)((m.tripLosses >> 8) & 0xFF);
    rec[22 + breeding::BREEDBOX_TRIP_SIZE] = m.geno.tritan;
}

// ── Breeder rooms file (byte-identical to TerminalUI::wifiEncSaveRooms) ──────
// magic u32 0x424D5252 ("RRMB" on disk), ver u8 1, u16 sizeof(BreederRoom),
// then NUM_BREEDER_ROOMS raw BreederRoom structs. Same-device / same-ABI blob:
// the size field makes a layout change (or a foreign ABI) discard cleanly.
constexpr uint32_t kRoomsMagic = 0x424D5252u;
constexpr uint8_t kRoomsVersion = 1;
constexpr size_t kRoomsHdr = 4 + 1 + 2;
static_assert(std::is_trivially_copyable<breeding::BreederRoom>::value,
              "BreederRoom is fwrite()n raw by mmterm; it must stay trivially copyable");
static_assert(std::is_trivially_copyable<breeding::BreedMon>::value, "BreedMon must be POD-like");

// Cooldown sidecar (cardputer only): magic "MMCD", ver 1, u32 count, then
// count x { u64 fingerprint, i64 lastBredAt }, little-endian host order.
constexpr uint32_t kCooldownMagic = 0x44434D4Du;
constexpr uint8_t kCooldownVersion = 1;

// Identity of a box mon that survives a save/reload (ids are reassigned on
// every import). Only fields the 34-byte box record round-trips: dex, the 8
// genes, the first 8 nick bytes, and caughtSec. Level is excluded (mmterm's
// nursery levels babies up). Identical twins share a fingerprint (harmless).
uint64_t mon_fingerprint(const breeding::BreedMon &m) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint8_t b) { h = (h ^ b) * 1099511628211ull; };
    mix(m.dex);
    const breeding::Genotype &g = m.geno;
    for (uint8_t b : {g.rainbow, g.shiny, g.dark, g.sterile, g.cantFight, g.noHatch, g.female, g.tritan})
        mix(b);
    const size_t nl = strnlen(m.nick, 8);
    for (size_t i = 0; i < 8; ++i) mix(i < nl ? (uint8_t)m.nick[i] : 0);
    for (int i = 0; i < 4; ++i) mix((uint8_t)(m.caughtSec >> (8 * i)));
    return h;
}

// Same signature mmterm uses to decide when the rooms changed.
uint64_t rooms_sig(const breeding::BreederManager &m) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < m.roomCount(); ++i) {
        const auto &r = m.room(i);
        uint64_t v = (uint64_t)r.state ^ ((uint64_t)r.idA << 8) ^ ((uint64_t)r.idB << 20) ^
                     ((uint64_t)r.eggHatchAt << 3) ^ ((uint64_t)r.rolled << 40) ^
                     ((uint64_t)r.pendingChild.dex << 44);
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

bool read_lord(const std::string &path, LordSave &s) {
    memset(&s, 0, sizeof(s));
    s.magic = LORD_MAGIC;
    s.version = LORD_VERSION;
    std::vector<uint8_t> buf;
    if (!read_file(path, buf, 4096) || buf.size() < sizeof(LordSave)) return false;
    LordSave tmp;
    memcpy(&tmp, buf.data(), sizeof(tmp));
    if (tmp.magic != LORD_MAGIC || tmp.version != LORD_VERSION) return false;
    s = tmp;
    return true;
}

// ── Demo data ────────────────────────────────────────────────────────────────

struct DemoMon {
    uint16_t dex; uint8_t level; const char *nick;
    uint16_t moves[4]; uint8_t dv0, dv1; uint32_t xp_gained, hours; int mood;
};

// Real Gen 1 dex numbers and Gen 1 move ids (verified against
// src/battle/showdown_gen1_moves.h).
const DemoMon kDemoParty[6] = {
    { 25, 32, "SPARKY",   { 84, 98, 86, 104}, 0xEA, 0xBC, 1840, 51, 2},  // Pikachu: ThunderShock, Quick Attack, Thunder Wave, Double Team
    {  6, 38, "CHARIZARD",{ 53, 163, 19, 52}, 0xDB, 0xCA, 2210, 63, 1},  // Charizard: Flamethrower, Slash, Fly, Ember
    {  9, 36, "BLASTOISE",{ 57, 44, 55, 110}, 0xAC, 0x9D, 1655, 48, 0},  // Blastoise: Surf, Bite, Water Gun, Withdraw
    {  3, 34, "VENUSAUR", { 75, 79, 73, 22},  0xBB, 0xA9, 1320, 40, 1},  // Venusaur: Razor Leaf, Sleep Powder, Leech Seed, Vine Whip
    {143, 30, "SNORLAX",  { 34, 156, 29, 133},0xCE, 0x8F,  980, 72, 3},  // Snorlax: Body Slam, Rest, Headbutt, Amnesia
    { 94, 35, "GENGAR",   {101, 95, 138, 122},0x9F, 0xED, 1475, 37, -1}, // Gengar: Night Shade, Hypnosis, Dream Eater, Lick
};

struct DemoEvt { const char *text; int xp; int slot; };
const DemoEvt kDemoEvents[] = {
    {"SPARKY and SNORLAX napped under the big shade tree.", 120, 0},
    {"CHARIZARD practiced FLAMETHROWER on a boulder. It's toasty now!", 210, 1},
    {"GENGAR snuck up behind BLASTOISE. BLASTOISE withdrew into its shell!", 95, 5},
    {"VENUSAUR soaked up the afternoon sun. Its bulb looks healthy.", 140, 3},
    {"MIST's STARMIE wandered over to say hello.", 60, 2},
    {"BLASTOISE splashed around in the pond with a wild PSYDUCK.", 110, 2},
    {"SNORLAX ate the daycare's entire lunch. The old man sighed.", 50, 4},
    {"SPARKY zapped a passing MAGNEMITE. They're friends now.", 130, 0},
    {"BRCK's ONIX rumbled past the fence. GENGAR hid in its shadow.", 80, 5},
    {"CHARIZARD flew a lap over Route 5 and came back proud.", 175, 1},
};

const Neighbor kDemoNeighbors[3] = {
    {0x1A2B3C4Du, "MIST", "MISTY", 6, "STARMIE", 38, 121, 0},
    {0x5E6F7081u, "BRCK", "BROCK", 5, "ONIX",    28,  95, 0},
    {0x92A3B4C5u, "GARY", "BLUE",  6, "PIDGEOT", 44,  18, 0},
};

std::string fmt_room_notice(int room, bool hatched) {
    char b[64];
    std::snprintf(b, sizeof(b), hatched ? "Room %d: an egg hatched! See Breeding."
                                        : "Room %d: the egg didn't hatch.", room + 1);
    return b;
}

}  // namespace

// ── Construction / paths ─────────────────────────────────────────────────────

uint64_t GameState::now_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

GameState::GameState() : GameState(Options()) {}

GameState::GameState(const Options &opt) : opt_(opt) {
    if (env_flag("MM_FORCE_DEMO")) opt_.force_demo = true;
    resolve_paths();

    ipc_.set_message_callback([this](const JsonValue &m, const std::string &raw, bool ok) {
        on_message(m, raw, ok);
    });
    ipc_.set_state_callback([this](bool up) { on_connection(up); });
    ipc_.set_enabled(opt_.enable_ipc && !opt_.force_demo);

    load_demo();

    if (opt_.load_files) {
        reload_box();
        reload_rooms();
        reload_badges();
    } else {
        breed_.seedTestRoster();
        box_test_ = true;
    }

    if (const char *s = env_or_null("MM_DEMO_CHALLENGE_SEC")) {
        long sec = strtol(s, nullptr, 10);
        if (sec >= 0) demo_challenge_at_ms_ = now_ms() + (uint64_t)sec * 1000 + 1;
    }
}

GameState::~GameState() = default;

void GameState::resolve_paths() {
    std::string state_dir = env_or_null("MM_STATE_DIR") ? env_or_null("MM_STATE_DIR")
                                                         : std::string();
    std::string sock = opt_.sock_path;
    if (sock.empty()) sock = env_or_null("MM_SOCK_PATH") ? env_or_null("MM_SOCK_PATH") : MMD_SOCK_PATH;
    ipc_.set_socket_path(sock);

    box_path_ = opt_.box_path;
    if (box_path_.empty() && env_or_null("MM_BOX_PATH")) box_path_ = env_or_null("MM_BOX_PATH");
    if (box_path_.empty())
        box_path_ = (state_dir.empty() ? std::string(MMD_STATE_DIR) : state_dir) + "/breeding_box.dat";

    lord_path_ = opt_.lord_path;
    if (lord_path_.empty() && env_or_null("MM_LORD_PATH")) lord_path_ = env_or_null("MM_LORD_PATH");
    if (lord_path_.empty())
        lord_path_ = state_dir.empty() ? std::string(LORD_SAVE_PATH) : state_dir + "/lord.dat";

    // Rooms live next to the box they belong to. With no overrides that is
    // MMD_STATE_DIR/breeder_rooms.dat == mmterm's wifiEncRoomsPath().
    rooms_path_ = opt_.rooms_path;
    if (rooms_path_.empty() && env_or_null("MM_ROOMS_PATH")) rooms_path_ = env_or_null("MM_ROOMS_PATH");
    if (rooms_path_.empty()) {
        std::string d = dir_of(box_path_);
        rooms_path_ = d.empty() ? std::string("breeder_rooms.dat") : d + "/breeder_rooms.dat";
    }
    std::string rd = dir_of(rooms_path_);
    cooldown_path_ = rd.empty() ? std::string("breeder_cooldowns.dat") : rd + "/breeder_cooldowns.dat";
}

bool GameState::daemon_connected() const { return ipc_.connected(); }

// ── Tick ─────────────────────────────────────────────────────────────────────

void GameState::tick() {
    uint64_t now = now_ms();
    ipc_.poll(now);   // dispatches into on_message / on_connection

    if (ipc_.connected()) {
        if (now - last_status_poll_ms_ >= kStatusPollMs) {
            last_status_poll_ms_ = now;
            ipc_.send_get_status();   // STATUS + NEIGHBORS; doubles as keepalive
            ipc_.poll(now);           // flush right away
        }
    }
    if (demo_) demo_tick(now);
    if (opt_.load_files && now - last_file_watch_ms_ >= kFileWatchMs) {
        last_file_watch_ms_ = now;
        watch_files();
    }
    if (now - last_room_tick_ms_ >= kRoomTickMs) {
        last_room_tick_ms_ = now;
        tick_rooms((long)time(nullptr));
    }
}

GameState::FileStamp GameState::stamp_of(const std::string &path) {
    FileStamp f;
    struct stat st {};
    if (path.empty() || stat(path.c_str(), &st) != 0) return f;
    f.exists = true;
    f.ino = (uint64_t)st.st_ino;   // atomic temp+rename writes always change it
    f.size = (uint64_t)st.st_size;
    f.mtime = (int64_t)st.st_mtime;
    return f;
}

void GameState::watch_files() {
    // Box first: a rooms reload re-matches parents against the current box.
    if (stamp_of(box_path_) != box_stamp_) reload_box();
    if (stamp_of(rooms_path_) != rooms_stamp_) reload_rooms();
}

void GameState::on_connection(bool up) {
    if (up) {
        if (!opt_.force_demo) go_live();
        status_.connected_to_daemon = true;
        // The daemon pushes HELLO + NODE_INFO + PARTY_UPDATE + MY_WIRE_PARTY on
        // accept; ask for STATUS/NEIGHBORS too (mmterm does the same).
        ipc_.send_get_status();
        ipc_.send_get_party();
        last_status_poll_ms_ = now_ms();
    } else {
        status_.connected_to_daemon = false;
        // Keep last live data; the daemon will push fresh state on reconnect.
    }
    bump();
}

void GameState::go_live() {
    if (!demo_) return;
    demo_ = false;
    ever_live_ = true;
    party_.clear();
    events_.clear();
    challenge_.reset();
    achievements_.clear();
    neighbor_parties_.clear();
    pvp_status_.clear();
    status_ = DaycareStatus();
    status_.connected_to_daemon = true;
    bump();
}

// ── Daemon messages ──────────────────────────────────────────────────────────

void GameState::push_event(DaycareEvent e) {
    if (e.timestamp == 0) e.timestamp = time(nullptr);
    events_.push_front(std::move(e));
    while (events_.size() > kMaxEvents) events_.pop_back();
}

void GameState::on_message(const JsonValue &msg, const std::string &raw, bool parsed) {
    (void)raw;
    if (!parsed || !msg.is_object()) return;
    const std::string type = msg.get_string("type");
    if (hook_) hook_(type, msg);
    if (demo_) return;   // forced demo: ignore live data

    bool changed = true;
    if (type == "PARTY_UPDATE") {
        party_.clear();
        const JsonValue &arr = msg["party"];
        for (size_t i = 0; i < arr.size() && i < 6; ++i)
            party_.push_back(party_mon_from_json(arr[i]));
        if (msg.has("active")) status_.active = msg.get_bool("active");
    } else if (type == "STATUS") {
        status_.active           = msg.get_bool("active", status_.active);
        status_.serial_connected = msg.get_bool("serial_connected");
        status_.serial_port      = msg.get_string("serial_port");
        status_.neighbors        = (int)msg.get_int("neighbors");
        status_.total_events     = msg.get_u32("total_events");
        status_.last_event       = msg.get_string("last_event");
        status_.last_event_xp    = (int)msg.get_int("last_event_xp");
        status_.sav_path         = msg.get_string("sav_path");
    } else if (type == "NEIGHBORS") {
        status_.neighbor_list.clear();
        const JsonValue &arr = msg["list"];
        for (size_t i = 0; i < arr.size(); ++i) {
            const JsonValue &n = arr[i];
            Neighbor nb;
            nb.node_id      = n.get_u32("node_id");
            nb.short_name   = n.get_string("short_name");
            nb.game_name    = n.get_string("game_name");
            nb.party_count  = (int)n.get_int("party_count");
            nb.lead_nick    = n.get_string("lead_nick");
            nb.lead_level   = (int)n.get_int("lead_level");
            nb.lead_dex     = (int)n.get_int("lead_dex");
            nb.last_seen_ms = n.get_u32("last_seen_ms");
            status_.neighbor_list.push_back(std::move(nb));
        }
        status_.neighbors = (int)msg.get_int("count", (int64_t)status_.neighbor_list.size());
    } else if (type == "DAYCARE_EVENT") {
        DaycareEvent e;
        e.text = msg.get_string("text");
        e.xp = (int)msg.get_int("xp");
        e.slot = (int)msg.get_int("slot");
        e.from_node = msg.get_u32("from");
        if (e.from_node == 0) {
            status_.last_event = e.text;
            status_.last_event_xp = e.xp;
            status_.total_events++;
        }
        push_event(std::move(e));
    } else if (type == "CHALLENGE_RECEIVED") {
        IncomingChallenge c;
        c.node_id = msg.get_u32("node_id");
        c.trainer = msg.get_string("trainer");
        const JsonValue &arr = msg["party"];   // not sent by today's daemon; future-proof
        for (size_t i = 0; i < arr.size() && i < 6; ++i)
            c.party.push_back(party_mon_from_json(arr[i]));
        if (c.party.empty()) {
            auto it = neighbor_parties_.find(c.node_id);
            if (it != neighbor_parties_.end()) c.party = it->second;
            else ipc_.send_get_neighbor_party(c.node_id);
        }
        challenge_ = std::move(c);
    } else if (type == "NEIGHBOR_PARTY") {
        if (msg.get_bool("ok")) {
            uint32_t node = msg.get_u32("node_id");
            std::vector<PartyMon> p;
            const JsonValue &arr = msg["party"];
            for (size_t i = 0; i < arr.size() && i < 6; ++i)
                p.push_back(party_mon_from_json(arr[i]));
            if (challenge_ && challenge_->node_id == node && challenge_->party.empty())
                challenge_->party = p;
            neighbor_parties_[node] = std::move(p);
        } else {
            changed = false;
        }
    } else if (type == "ACHIEVEMENT") {
        achievements_.push_back({msg.get_string("name"), msg.get_string("description")});
    } else if (type == "NODE_INFO") {
        status_.node_id    = msg.get_u32("node_id");
        status_.short_name = msg.get_string("short_name");
        status_.game_name  = msg.get_string("game_name");
    } else if (type == "MY_WIRE_PARTY") {
        status_.sav_gen = (int)msg.get_int("sav_gen");
    } else if (type == "BEACON_RESULT") {
        status_.last_beacon_result = msg.get_bool("ok") ? "ok" : msg.get_string("reason", "failed");
    } else if (type == "PVP_CHALLENGE_SENT") {
        pvp_status_ = msg.get_bool("ok") ? "Challenge sent"
                                         : "Challenge failed: " + msg.get_string("reason", "?");
    } else if (type == "PVP_ACCEPT_RECEIVED") {
        pvp_status_ = msg.get_bool("accepted")
                          ? msg.get_string("trainer", "Opponent") + " accepted your challenge!"
                          : "Challenge declined.";
    } else if (type == "ERROR") {
        last_error_ = msg.get_string("message");
    } else {
        // HELLO, ACK, PONG, MESH_SEND_OK/FAIL, BROADCAST, BATTLE_UPDATE,
        // PVP_ACTION_RECEIVED, SAV_WRITEBACK, LEARN_MOVE_RESULT ... -> hook only.
        changed = false;
    }
    if (changed) bump();
}

PartyMon GameState::party_mon_from_json(const JsonValue &s) {
    PartyMon m;
    m.dex = (uint16_t)s.get_int("dex");
    m.level = (uint8_t)s.get_int("level");
    m.nickname = s.has("nick") ? s.get_string("nick") : s.get_string("nickname");
    m.species = s.get_string("name");
    if (m.species.empty() && m.dex >= 1 && m.dex <= 151)
        m.species = breeding::BreedingApp::dexName((uint8_t)m.dex);
    if (m.nickname.empty()) m.nickname = m.species;
    const JsonValue &mv = s["moves"];
    for (size_t i = 0; i < 4; ++i) m.moves[i] = (uint16_t)mv[i].as_int(0);
    m.total_xp_gained = s.get_u32("total_xp_gained");
    m.sav_level = (uint8_t)s.get_int("sav_level", m.level);
    m.total_hours = s.get_u32("total_hours");
    m.mood = (int)s.get_int("mood");
    const JsonValue &dv = s["dvs"];
    if (dv.size() >= 2) { m.dvs[0] = (uint8_t)dv[0].as_int(); m.dvs[1] = (uint8_t)dv[1].as_int(); }
    const JsonValue &se = s["stat_exp"];
    for (size_t i = 0; i < 5 && i < se.size(); ++i) m.stat_exp[i] = (uint16_t)se[i].as_int();

    derive_stats(m);
    // Prefer real values if a future daemon sends them.
    if (s.has("max_hp")) { m.max_hp = (uint16_t)s.get_int("max_hp"); m.derived_stats = false; }
    if (s.has("hp"))     m.hp = (uint16_t)s.get_int("hp");
    else                 m.hp = m.max_hp;
    if (s.has("exp"))    m.exp = s.get_u32("exp");
    return m;
}

void GameState::derive_stats(PartyMon &m) {
    m.derived_stats = true;
    uint8_t L = m.level ? m.level : 1;
    uint8_t base_hp = 0;
    if (m.dex >= 1 && m.dex <= 151) base_hp = GEN1_BASE_STATS[m.dex].hp;
    else if (m.dex >= 152 && m.dex <= 386) base_hp = GEN3_BASE_STATS[m.dex].hp;
    // Gen 1 HP DV = low bit of each of atk/def/spd/spc DVs.
    uint8_t atk = m.dvs[0] >> 4, def = m.dvs[0] & 0xF, spd = m.dvs[1] >> 4, spc = m.dvs[1] & 0xF;
    uint8_t hp_dv = (uint8_t)(((atk & 1) << 3) | ((def & 1) << 2) | ((spd & 1) << 1) | (spc & 1));
    uint32_t se = (uint32_t)std::ceil(std::sqrt((double)m.stat_exp[0]));
    if (se > 255) se = 255;
    uint32_t hp = (((uint32_t)(base_hp + hp_dv) * 2 + se / 4) * L) / 100 + L + 10;
    if (base_hp == 0) hp = 0;
    m.max_hp = (uint16_t)hp;
    m.hp = m.max_hp;
    if (m.dex >= 1 && m.dex <= 151) m.exp = expForLevel((uint8_t)m.dex, L);
    else m.exp = (L > 1) ? (uint32_t)L * L * L : 0;   // Medium Fast approximation
}

// ── Accessors ────────────────────────────────────────────────────────────────

std::optional<IncomingChallenge> GameState::pending_challenge() { return challenge_; }

std::optional<Achievement> GameState::pop_achievement() {
    if (achievements_.empty()) return std::nullopt;
    Achievement a = std::move(achievements_.front());
    achievements_.pop_front();
    bump();
    return a;
}

const std::vector<PartyMon> *GameState::neighbor_party(uint32_t node_id) const {
    auto it = neighbor_parties_.find(node_id);
    return it == neighbor_parties_.end() ? nullptr : &it->second;
}

// ── Box ──────────────────────────────────────────────────────────────────────

bool GameState::reload_box() {
    box_stamp_ = stamp_of(box_path_);
    bool ok = load_box_file();
    sync_rooms_with_box();
    return ok;
}

bool GameState::load_box_file() {
    breed_.clear();
    box_test_ = false;
    std::vector<uint8_t> buf;
    int n = -1;
    bool needs_upgrade = false;
    if (read_file(box_path_, buf, 1u << 20) && !buf.empty()) {
        uint32_t magic = 0;
        if (buf.size() >= breeding::BREEDBOX_HDR_SIZE) memcpy(&magic, buf.data(), 4);
        needs_upgrade = !(magic == breeding::BREEDBOX_MAGIC &&
                          buf[4] == breeding::BREEDBOX_VERSION);
        n = breed_.importBox(buf.data(), buf.size());
    }
    if (n <= 0 || breed_.size() == 0) {
        breed_.clear();
        breed_.seedTestRoster();
        box_test_ = true;
        bump();
        return false;
    }
    // Same ordering mmterm applies on load: dex, then skin.
    auto &ros = breed_.mutableRoster();
    std::sort(ros.begin(), ros.end(), [](const breeding::BreedMon &a, const breeding::BreedMon &b) {
        if (a.dex != b.dex) return a.dex < b.dex;
        return (int)breeding::skinOf(a.geno) < (int)breeding::skinOf(b.geno);
    });
    // importBox retro-seeds the tritan gene for pre-v3 files; persist it once
    // (as mmterm does) so it never re-rolls.
    if (needs_upgrade) save_box();
    bump();
    return true;
}

bool GameState::save_box() {
    std::vector<uint8_t> out;
    out.reserve(breeding::BREEDBOX_HDR_SIZE + breed_.size() * breeding::BREEDBOX_REC_SIZE);
    uint32_t magic = breeding::BREEDBOX_MAGIC;   // little-endian on disk (as mmterm fwrite()s it)
    uint8_t hdr[breeding::BREEDBOX_HDR_SIZE];
    memcpy(hdr, &magic, 4);
    hdr[4] = breeding::BREEDBOX_VERSION;
    out.insert(out.end(), hdr, hdr + sizeof(hdr));
    uint8_t rec[breeding::BREEDBOX_REC_SIZE];
    for (const auto &m : breed_.roster()) {
        serialize_box_mon(m, rec);
        out.insert(out.end(), rec, rec + sizeof(rec));
    }
    bool ok = write_file_atomic(box_path_, out.data(), out.size());
    if (ok) box_test_ = false;
    box_stamp_ = stamp_of(box_path_);
    return ok;
}

// ── Breeder rooms ───────────────────────────────────────────────────────────

int GameState::place_pair(size_t a, size_t b, long now, std::string &err) {
    int room = rooms_.placePair(breed_, a, b, now, err);
    if (room < 0) return -1;
    save_rooms();   // also records both parents' new cooldown
    bump();
    return room;
}

bool GameState::cancel_room(int room) {
    if (!rooms_.cancel(room)) return false;
    save_rooms();
    bump();
    return true;
}

int GameState::tick_rooms(long now) {
    bool busy = false;
    for (int i = 0; i < rooms_.roomCount(); ++i)
        busy = busy || rooms_.room(i).state != breeding::ROOM_EMPTY;
    if (!busy) return 0;

    breeding::BreederRoom before[breeding::NUM_BREEDER_ROOMS];
    for (int i = 0; i < breeding::NUM_BREEDER_ROOMS; ++i) before[i] = rooms_.room(i);
    const uint64_t sig = rooms_sig(rooms_);
    const size_t n_before = breed_.size();
    breeding::Rng rng((uint64_t)now ^ ((uint64_t)n_before << 8) ^
                      (++room_rng_ctr_ * 0x9E3779B97F4A7C15ull) ^ now_ms());
    std::vector<breeding::HatchEvent> evs = rooms_.tick(breed_, now, rng);

    // tick() appends each BREED_OK child to the roster in event order.
    size_t next = n_before;
    for (auto &ev : evs) {
        Hatch h;
        h.room = ev.room;
        h.result = std::move(ev.result);
        if (ev.room >= 0 && ev.room < breeding::NUM_BREEDER_ROOMS) {
            h.parent_a = before[ev.room].parentA;
            h.parent_b = before[ev.room].parentB;
        }
        if (h.result.status == breeding::BREED_OK && next < breed_.size())
            h.result.child.id = breed_.roster()[next++].id;
        hatch_notices_.push_back(h.result.status == breeding::BREED_OK
                                     ? fmt_room_notice(h.room, true)
                                     : fmt_room_notice(h.room, false));
        while (hatch_notices_.size() > kMaxHatches) hatch_notices_.pop_front();
        hatches_.push_back(std::move(h));
        while (hatches_.size() > kMaxHatches) hatches_.pop_front();
    }
    if (next > n_before && opt_.load_files) save_box();
    if (rooms_sig(rooms_) != sig) {   // egg appeared, hatched or failed
        save_rooms();
        bump();
    }
    return (int)evs.size();
}

std::optional<std::string> GameState::pop_hatch_notice() {
    if (hatch_notices_.empty()) return std::nullopt;
    std::string n = std::move(hatch_notices_.front());
    hatch_notices_.pop_front();
    return n;
}

std::optional<GameState::Hatch> GameState::pop_hatch() {
    if (hatches_.empty()) return std::nullopt;
    Hatch h = std::move(hatches_.front());
    hatches_.pop_front();
    return h;
}

void GameState::refresh_cooldowns(long now) {
    for (const auto &m : breed_.roster()) {
        if (m.lastBredAt <= 0 || breeding::BreederManager::cooldownRemaining(m, now) == 0) continue;
        long &v = cooldowns_[mon_fingerprint(m)];
        if (m.lastBredAt > v) v = m.lastBredAt;
    }
    for (auto it = cooldowns_.begin(); it != cooldowns_.end();) {
        if (it->second + breeding::BREED_COOLDOWN_SEC <= now) it = cooldowns_.erase(it);
        else ++it;
    }
}

bool GameState::save_rooms() {
    refresh_cooldowns((long)time(nullptr));
    if (!opt_.load_files) return false;

    // Rooms: exactly mmterm's layout.
    std::vector<uint8_t> out(kRoomsHdr + sizeof(breeding::BreederRoom) * breeding::NUM_BREEDER_ROOMS, 0);
    const uint32_t magic = kRoomsMagic;
    const uint16_t rsz = (uint16_t)sizeof(breeding::BreederRoom);
    memcpy(out.data(), &magic, 4);
    out[4] = kRoomsVersion;
    memcpy(out.data() + 5, &rsz, 2);
    memcpy(out.data() + kRoomsHdr, rooms_.rawRooms(),
           sizeof(breeding::BreederRoom) * breeding::NUM_BREEDER_ROOMS);
    bool ok = write_file_atomic(rooms_path_, out.data(), out.size());
    rooms_stamp_ = stamp_of(rooms_path_);

    // Cooldown sidecar: the box record has no lastBredAt, so without this a
    // relaunch would forget every finished parent's 7-day cooldown.
    std::vector<uint8_t> cd(4 + 1 + 4);
    const uint32_t cm = kCooldownMagic, cnt = (uint32_t)cooldowns_.size();
    memcpy(cd.data(), &cm, 4);
    cd[4] = kCooldownVersion;
    memcpy(cd.data() + 5, &cnt, 4);
    for (const auto &[fp, t] : cooldowns_) {
        const int64_t t64 = t;
        uint8_t rec[16];
        memcpy(rec, &fp, 8);
        memcpy(rec + 8, &t64, 8);
        cd.insert(cd.end(), rec, rec + 16);
    }
    ok = write_file_atomic(cooldown_path_, cd.data(), cd.size()) && ok;
    return ok;
}

bool GameState::reload_rooms() {
    rooms_stamp_ = stamp_of(rooms_path_);
    breeding::BreederRoom tmp[breeding::NUM_BREEDER_ROOMS] = {};
    bool ok = false;
    std::vector<uint8_t> buf;
    const size_t want = kRoomsHdr + sizeof(breeding::BreederRoom) * breeding::NUM_BREEDER_ROOMS;
    if (read_file(rooms_path_, buf, 1u << 16) && buf.size() >= want) {
        uint32_t magic = 0;
        uint16_t rsz = 0;
        memcpy(&magic, buf.data(), 4);
        memcpy(&rsz, buf.data() + 5, 2);
        if (magic == kRoomsMagic && buf[4] == kRoomsVersion && rsz == sizeof(breeding::BreederRoom)) {
            memcpy(tmp, buf.data() + kRoomsHdr, sizeof(tmp));
            ok = true;
            for (auto &r : tmp) {
                // Don't trust another process's bytes blindly.
                if (r.state > breeding::ROOM_EGG) { r = breeding::BreederRoom{}; continue; }
                r.parentA.nick[sizeof(r.parentA.nick) - 1] = 0;
                r.parentB.nick[sizeof(r.parentB.nick) - 1] = 0;
                r.pendingChild.nick[sizeof(r.pendingChild.nick) - 1] = 0;
            }
        }
    }
    if (!ok)
        for (auto &r : tmp) r = breeding::BreederRoom{};
    rooms_.restoreRooms(tmp);

    cooldowns_.clear();
    buf.clear();
    if (read_file(cooldown_path_, buf, 1u << 20) && buf.size() >= 9) {
        uint32_t magic = 0, cnt = 0;
        memcpy(&magic, buf.data(), 4);
        memcpy(&cnt, buf.data() + 5, 4);
        if (magic == kCooldownMagic && buf[4] == kCooldownVersion && buf.size() >= 9 + (size_t)cnt * 16) {
            for (uint32_t i = 0; i < cnt; ++i) {
                uint64_t fp = 0;
                int64_t t = 0;
                memcpy(&fp, buf.data() + 9 + i * 16, 8);
                memcpy(&t, buf.data() + 9 + i * 16 + 8, 8);
                cooldowns_[fp] = (long)t;
            }
        }
    }
    sync_rooms_with_box();
    bump();
    return ok;
}

void GameState::sync_rooms_with_box() {
    auto &ros = breed_.mutableRoster();
    // Persisted cooldowns first.
    if (!cooldowns_.empty())
        for (auto &m : ros) {
            auto it = cooldowns_.find(mon_fingerprint(m));
            if (it != cooldowns_.end() && it->second > m.lastBredAt) m.lastBredAt = it->second;
        }
    // Re-point each occupied room at the current roster ids of its parents
    // (0 = parent no longer in the box; the room still runs on its copies).
    breeding::BreederRoom tmp[breeding::NUM_BREEDER_ROOMS];
    std::vector<bool> claimed(ros.size(), false);
    bool any = false;
    for (int i = 0; i < breeding::NUM_BREEDER_ROOMS; ++i) {
        tmp[i] = rooms_.room(i);
        if (tmp[i].state == breeding::ROOM_EMPTY) continue;
        any = true;
        auto match = [&](const breeding::BreedMon &p) -> uint32_t {
            const uint64_t fp = mon_fingerprint(p);
            for (size_t k = 0; k < ros.size(); ++k) {
                if (claimed[k] || mon_fingerprint(ros[k]) != fp) continue;
                claimed[k] = true;
                if (ros[k].lastBredAt < tmp[i].placedAt) ros[k].lastBredAt = tmp[i].placedAt;
                return ros[k].id;
            }
            return 0;
        };
        tmp[i].idA = match(tmp[i].parentA);
        tmp[i].idB = match(tmp[i].parentB);
    }
    if (any) rooms_.restoreRooms(tmp);
}

// ── Badges ───────────────────────────────────────────────────────────────────

bool GameState::reload_badges() {
    LordSave s;
    bool ok = read_lord(lord_path_, s);
    uint8_t b = ok ? s.badges : 0;
    if (b != badges_) { badges_ = b; bump(); }
    return ok;
}

int GameState::badge_count() const {
    int c = 0;
    for (int i = 0; i < 8; ++i) c += (badges_ >> i) & 1;
    return c;
}

void GameState::set_badge(int gym) {
    if (gym < 0 || gym >= 8) return;
    LordSave s;
    read_lord(lord_path_, s);           // re-read: keep mmterm's other progress intact
    s.badges = (uint8_t)(s.badges | badges_ | (1u << gym));
    badges_ = s.badges;
    write_file_atomic(lord_path_, &s, sizeof(s));
    bump();
}

// ── Actions ──────────────────────────────────────────────────────────────────

void GameState::request_party() {
    if (!demo_) ipc_.send_get_party();
}

void GameState::request_status() {
    if (!demo_) ipc_.send_get_status();
}

void GameState::force_event() {
    if (!demo_) { ipc_.send_force_event(); return; }
    const DemoEvt &d = kDemoEvents[demo_event_idx_++ % (sizeof(kDemoEvents) / sizeof(kDemoEvents[0]))];
    DaycareEvent e;
    e.text = d.text; e.xp = d.xp; e.slot = d.slot;
    status_.last_event = e.text;
    status_.last_event_xp = e.xp;
    status_.total_events++;
    if (d.slot >= 0 && (size_t)d.slot < party_.size()) party_[(size_t)d.slot].total_xp_gained += (uint32_t)d.xp;
    push_event(std::move(e));
    if (++demo_forced_events_ == 3)
        achievements_.push_back({"Daycare Regular", "Triggered 3 daycare events in one sitting."});
    bump();
}

void GameState::force_beacon() {
    if (!demo_) { ipc_.send_force_beacon(); return; }
    status_.last_beacon_result = "ok";
    uint32_t t = (uint32_t)(now_ms() & 0xFFFFFFFFu);
    for (auto &n : status_.neighbor_list) n.last_seen_ms = t;
    bump();
}

void GameState::hollaback() {
    if (!demo_) { ipc_.send_hollaback(); return; }
    force_beacon();
}

void GameState::challenge(uint32_t node_id) {
    if (!demo_) { ipc_.send_challenge(node_id); return; }
    std::string who = "node";
    for (const auto &n : status_.neighbor_list) if (n.node_id == node_id) who = n.short_name;
    pvp_status_ = "Challenge sent to " + who + " (demo)";
    bump();
}

void GameState::accept_challenge() {
    if (!challenge_) return;
    if (!demo_) ipc_.send_accept_challenge(challenge_->node_id);
    else pvp_status_ = "Accepted " + challenge_->trainer + "'s challenge (demo)";
    challenge_.reset();
    bump();
}

void GameState::decline_challenge() {
    if (!challenge_) return;
    if (!demo_) ipc_.send_decline_challenge(challenge_->node_id);
    else pvp_status_ = "Declined " + challenge_->trainer + "'s challenge (demo)";
    challenge_.reset();
    bump();
}

void GameState::request_neighbor_party(uint32_t node_id) {
    if (!demo_) ipc_.send_get_neighbor_party(node_id);
}

// ── Demo ─────────────────────────────────────────────────────────────────────

void GameState::load_demo() {
    demo_ = true;
    party_.clear();
    for (const auto &d : kDemoParty) {
        PartyMon m;
        m.dex = d.dex;
        m.level = d.level;
        m.sav_level = (uint8_t)(d.level - 2);
        m.nickname = d.nick;
        m.species = breeding::BreedingApp::dexName((uint8_t)d.dex);
        for (int i = 0; i < 4; ++i) m.moves[i] = d.moves[i];
        m.dvs[0] = d.dv0; m.dvs[1] = d.dv1;
        for (int i = 0; i < 5; ++i) m.stat_exp[i] = (uint16_t)(d.level * 180);
        m.total_xp_gained = d.xp_gained;
        m.total_hours = d.hours;
        m.mood = d.mood;
        derive_stats(m);
        party_.push_back(m);
    }
    // A little damage so HP bars aren't all identical.
    party_[1].hp = (uint16_t)(party_[1].max_hp * 3 / 4);
    party_[4].hp = (uint16_t)(party_[4].max_hp / 2);

    status_ = DaycareStatus();
    status_.connected_to_daemon = false;
    status_.active = true;
    status_.serial_connected = true;
    status_.serial_port = "demo";
    status_.node_id = 0xC0FFEE01u;
    status_.short_name = "CPZ0";
    status_.game_name = "RED";
    status_.sav_gen = 1;
    status_.sav_path = "(demo)";
    uint32_t t = (uint32_t)(now_ms() & 0xFFFFFFFFu);
    for (size_t i = 0; i < 3; ++i) {
        Neighbor n = kDemoNeighbors[i];
        n.last_seen_ms = t - (uint32_t)(i * 95000);
        status_.neighbor_list.push_back(n);
    }
    status_.neighbors = (int)status_.neighbor_list.size();

    // A Gym-leader-flavoured demo party for each neighbour (for challenge screens).
    neighbor_parties_.clear();
    auto mk = [](uint16_t dex, uint8_t lvl, const char *nick, std::initializer_list<uint16_t> mv) {
        PartyMon m; m.dex = dex; m.level = lvl; m.sav_level = lvl; m.nickname = nick;
        m.species = breeding::BreedingApp::dexName((uint8_t)dex);
        size_t i = 0; for (uint16_t x : mv) if (i < 4) m.moves[i++] = x;
        derive_stats(m); return m;
    };
    neighbor_parties_[kDemoNeighbors[0].node_id] = {
        mk(121, 38, "STARMIE", {57, 94, 105, 61}),   // Surf, Psychic, Recover, BubbleBeam
        mk(120, 34, "STARYU",  {55, 105, 61, 33})};  // Water Gun, Recover, BubbleBeam, Tackle
    neighbor_parties_[kDemoNeighbors[1].node_id] = {
        mk(95, 28, "ONIX",     {88, 20, 106, 33}),   // Rock Throw, Bind, Harden, Tackle
        mk(74, 24, "GEODUDE",  {88, 111, 33, 0})};   // Rock Throw, Defense Curl, Tackle
    neighbor_parties_[kDemoNeighbors[2].node_id] = {
        mk(18, 44, "PIDGEOT",  {17, 98, 16, 28}),    // Wing Attack, Quick Attack, Gust, Sand-Attack
        mk(65, 42, "ALAKAZAM", {94, 105, 60, 50})};  // Psychic, Recover, Psybeam, Disable

    events_.clear();
    time_t now = time(nullptr);
    for (int i = 4; i >= 0; --i) {   // oldest first so newest ends up at front
        DaycareEvent e;
        e.text = kDemoEvents[i].text;
        e.xp = kDemoEvents[i].xp;
        e.slot = kDemoEvents[i].slot;
        e.timestamp = now - (time_t)(i * 17 * 60);
        push_event(e);
    }
    demo_event_idx_ = 5;
    status_.last_event = events_.front().text;
    status_.last_event_xp = events_.front().xp;
    status_.total_events = 128;
    demo_next_event_ms_ = now_ms() + kDemoEventMs;
    bump();
}

void GameState::demo_tick(uint64_t now) {
    if (now >= demo_next_event_ms_) {
        demo_next_event_ms_ = now + kDemoEventMs;
        int saved = demo_forced_events_;
        force_event();
        demo_forced_events_ = saved;   // ambient events don't count toward the toast
    }
    if (demo_challenge_at_ms_ && now >= demo_challenge_at_ms_) {
        demo_challenge_at_ms_ = 0;
        simulate_incoming_challenge();
    }
}

void GameState::simulate_incoming_challenge() {
    if (!demo_) return;
    IncomingChallenge c;
    c.node_id = kDemoNeighbors[0].node_id;
    c.trainer = kDemoNeighbors[0].short_name;
    auto it = neighbor_parties_.find(c.node_id);
    if (it != neighbor_parties_.end()) c.party = it->second;
    challenge_ = std::move(c);
    bump();
}

}  // namespace mm
