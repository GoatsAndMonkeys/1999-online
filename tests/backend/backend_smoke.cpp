// backend_smoke.cpp — smoke test for cardputer/src/backend (no LVGL, no SDL).
//
// Build (from monster_mesh_pi/):
//   clang++ -std=c++17 -Wall -Wextra \
//     -I cardputer/src/backend -I src/shared -I src/battle \
//     cardputer/tests/backend/backend_smoke.cpp cardputer/src/backend/*.cpp \
//     src/shared/BreedingApp.cpp -o <out>/backend_smoke
//
// Usage:
//   backend_smoke <scratch_dir>            offline tests (JSON, demo, box, badges)
//   backend_smoke <scratch_dir> --live S   also connect to a running mmd for S seconds
#include "game_state.h"
#include "ipc_client.h"
#include "mm_json.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace mm;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) ++g_pass;                                                    \
        else { ++g_fail; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

// Lines in the exact shapes MonsterMeshDaemon.cpp emits.
static const char *kPartyUpdate =
    "{\"type\":\"PARTY_UPDATE\",\"active\":1,\"count\":2,\"party\":["
    "{\"dex\":25,\"name\":\"PIKACHU\",\"nick\":\"SPARKY\",\"level\":32,\"sav_level\":30,"
    "\"total_xp_gained\":1840,\"total_hours\":51,\"mood\":2,\"moves\":[84,98,86,104],"
    "\"dvs\":[234,188],\"stat_exp\":[5000,4000,3000,2000,1000]},"
    "{\"dex\":143,\"name\":\"SNORLAX\",\"nick\":\"Z\\\"ZZ\",\"level\":30,\"sav_level\":30,"
    "\"total_xp_gained\":0,\"total_hours\":0,\"mood\":0,\"moves\":[34,156,0,0],"
    "\"dvs\":[0,0],\"stat_exp\":[0,0,0,0,0]}]}";
static const char *kStatus =
    "{\"type\":\"STATUS\",\"active\":1,\"serial_connected\":0,\"serial_port\":\"/dev/ttyACM0\","
    "\"neighbors\":2,\"total_events\":17,\"last_event\":\"PIKACHU found a berry!\","
    "\"last_event_xp\":40,\"sav_path\":\"/tmp/mm-test-saves/red.sav\"}";
static const char *kNeighbors =
    "{\"type\":\"NEIGHBORS\",\"count\":2,\"list\":["
    "{\"node_id\":439041101,\"short_name\":\"MIST\",\"game_name\":\"MISTY\",\"party_count\":6,"
    "\"lead_nick\":\"STARMIE\",\"lead_level\":38,\"lead_dex\":121,\"last_seen_ms\":123456},"
    "{\"node_id\":4294967295,\"short_name\":\"MAX\",\"game_name\":\"\",\"party_count\":0,"
    "\"lead_nick\":\"\",\"lead_level\":0,\"lead_dex\":0,\"last_seen_ms\":0}]}";
static const char *kDaycareEvt =
    "{\"type\":\"DAYCARE_EVENT\",\"text\":\"SPARKY said \\\"pika!\\\" \\u00e9\\ud83d\\ude00\",\"xp\":25,\"slot\":0}";
static const char *kMeshEvt =
    "{\"type\":\"DAYCARE_EVENT\",\"text\":\"MIST's STARMIE waved\",\"xp\":0,\"slot\":-1,\"from\":439041101}";
static const char *kChallenge =
    "{\"type\":\"CHALLENGE_RECEIVED\",\"node_id\":439041101,\"trainer\":\"MIST\"}";
static const char *kNeighborParty =
    "{\"type\":\"NEIGHBOR_PARTY\",\"ok\":1,\"node_id\":439041101,\"short_name\":\"MIST\","
    "\"game_name\":\"MISTY\",\"count\":1,\"party\":[{\"dex\":121,\"level\":38,\"nick\":\"STARMIE\","
    "\"moves\":[57,94,105,61]}]}";
static const char *kAchievement =
    "{\"type\":\"ACHIEVEMENT\",\"name\":\"First Steps\",\"description\":\"Connected to the mesh\"}";

static void test_json() {
    JsonValue v;
    CHECK(json_parse(kPartyUpdate, v));
    CHECK(v.get_string("type") == "PARTY_UPDATE");
    CHECK(v["party"].size() == 2);
    CHECK(v["party"][1].get_string("nick") == "Z\"ZZ");
    CHECK(v["party"][0]["moves"][3].as_int() == 104);
    CHECK(v.get_bool("active"));

    CHECK(json_parse(kNeighbors, v));
    CHECK(v["list"][1].get_u32("node_id") == 0xFFFFFFFFu);

    CHECK(json_parse(kDaycareEvt, v));
    CHECK(v.get_string("text") == "SPARKY said \"pika!\" \xC3\xA9\xF0\x9F\x98\x80");
    CHECK(v.get_int("slot") == 0);
    CHECK(json_parse(kMeshEvt, v) && v.get_int("slot") == -1);

    // Malformed / hostile lines.
    const char *bad[] = {"", "{", "{\"type\":}", "{\"a\":1,}", "[1,2", "{\"a\":\"unterminated}",
                         "nope", "{\"a\":1} trailing", "{\"a\":\"\\u12\"}"};
    for (const char *b : bad) CHECK(!json_parse(b, v));
    std::string deep(100, '[');
    CHECK(!json_parse(deep, v));
    // Missing keys degrade to defaults.
    CHECK(json_parse("{\"type\":\"STATUS\"}", v));
    CHECK(v.get_int("neighbors", -7) == -7 && v["x"]["y"][3].is_null());
    CHECK(json_escape("a\"b\\c\n") == "a\\\"b\\\\c\\n");

    PartyMon m = GameState::party_mon_from_json(JsonValue());
    CHECK(m.dex == 0 && m.max_hp == 0);
    JsonValue p;
    json_parse(kPartyUpdate, p);
    m = GameState::party_mon_from_json(p["party"][0]);
    CHECK(m.nickname == "SPARKY" && m.species == "PIKACHU" && m.level == 32);
    CHECK(m.max_hp > 60 && m.max_hp < 100 && m.hp == m.max_hp && m.exp > 0);
    printf("  json: SPARKY L%d HP %u/%u EXP %u\n", m.level, m.hp, m.max_hp, m.exp);
}

static void test_demo(const std::string &dir) {
    GameState::Options o;
    o.enable_ipc = false;
    o.box_path = dir + "/no_such_box.dat";
    o.lord_path = dir + "/no_such_lord.dat";
    GameState gs(o);
    CHECK(gs.demo_mode() && !gs.daemon_connected());
    CHECK(gs.party().size() == 6);
    CHECK(gs.daycare().neighbor_list.size() == 3 && gs.daycare().neighbors == 3);
    CHECK(gs.daycare().active);
    CHECK(gs.events().size() == 5);
    CHECK(gs.box_is_test_roster() && gs.breeding().size() > 0);
    CHECK(gs.badges() == 0);
    for (const auto &pm : gs.party()) {
        CHECK(pm.dex >= 1 && pm.dex <= 151 && pm.max_hp > 0 && pm.moves[0] != 0);
        printf("  demo: #%03u %-10s %-9s L%u HP %3u/%3u moves %u,%u,%u,%u\n", pm.dex,
               pm.species.c_str(), pm.nickname.c_str(), pm.level, pm.hp, pm.max_hp,
               pm.moves[0], pm.moves[1], pm.moves[2], pm.moves[3]);
    }
    uint32_t r = gs.revision();
    gs.force_event();
    CHECK(gs.revision() != r && gs.events().size() == 6);
    gs.force_event();
    CHECK(!gs.pop_achievement());
    gs.force_event();
    auto a = gs.pop_achievement();
    CHECK(a && !a->name.empty());
    for (int i = 0; i < 60; ++i) gs.force_event();
    CHECK(gs.events().size() == GameState::kMaxEvents);

    CHECK(!gs.pending_challenge());
    gs.simulate_incoming_challenge();
    auto c = gs.pending_challenge();
    CHECK(c && c->trainer == "MIST" && !c->party.empty());
    gs.accept_challenge();
    CHECK(!gs.pending_challenge() && !gs.last_pvp_status().empty());
    gs.challenge(gs.daycare().neighbor_list[2].node_id);
    CHECK(gs.last_pvp_status().find("GARY") != std::string::npos);
    gs.force_beacon();
    CHECK(gs.daycare().last_beacon_result == "ok");
    gs.tick();   // IPC disabled: must be a no-op-ish
    CHECK(gs.demo_mode());
}

static void test_box_and_badges(const std::string &dir) {
    std::string box = dir + "/box_rt.dat", lord = dir + "/lord_rt.dat";
    unlink(box.c_str());
    unlink(lord.c_str());
    size_t n = 0;
    {
        GameState::Options o; o.enable_ipc = false; o.box_path = box; o.lord_path = lord;
        GameState gs(o);
        CHECK(gs.box_is_test_roster());
        n = gs.breeding().size();
        CHECK(gs.save_box());
        CHECK(!gs.box_is_test_roster());
        gs.set_badge(0);
        gs.set_badge(3);
        gs.set_badge(9);   // ignored
        CHECK(gs.badges() == 0x09 && gs.badge_count() == 2 && gs.has_badge(3) && !gs.has_badge(1));
    }
    FILE *f = fopen(box.c_str(), "rb");
    CHECK(f != nullptr);
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char hdr[5] = {0};
        CHECK(fread(hdr, 1, 5, f) == 5);
        fclose(f);
        CHECK(hdr[0] == 0xFF && hdr[1] == 0x58 && hdr[2] == 0x42 && hdr[3] == 0x4D && hdr[4] == 3);
        CHECK((size_t)len == 5 + n * 34);
    }
    {
        GameState::Options o; o.enable_ipc = false; o.box_path = box; o.lord_path = lord;
        GameState gs(o);
        CHECK(!gs.box_is_test_roster());
        CHECK(gs.breeding().size() == n);
        CHECK(gs.badges() == 0x09);
        printf("  box: %zu mons round-tripped (%s), badges=0x%02x\n", n, box.c_str(), gs.badges());
    }
    // Env overrides (stash any caller-provided values so they don't interfere).
    std::string old_box = getenv("MM_BOX_PATH") ? getenv("MM_BOX_PATH") : "";
    std::string old_lord = getenv("MM_LORD_PATH") ? getenv("MM_LORD_PATH") : "";
    unsetenv("MM_BOX_PATH");
    unsetenv("MM_LORD_PATH");
    setenv("MM_STATE_DIR", dir.c_str(), 1);
    {
        GameState::Options o; o.enable_ipc = false;
        GameState gs(o);
        CHECK(gs.box_path() == dir + "/breeding_box.dat");
        CHECK(gs.lord_path() == dir + "/lord.dat");
    }
    setenv("MM_BOX_PATH", box.c_str(), 1);
    {
        GameState::Options o; o.enable_ipc = false;
        GameState gs(o);
        CHECK(gs.box_path() == box && gs.breeding().size() == n);
    }
    unsetenv("MM_BOX_PATH");
    unsetenv("MM_STATE_DIR");
    if (!old_box.empty()) setenv("MM_BOX_PATH", old_box.c_str(), 1);
    if (!old_lord.empty()) setenv("MM_LORD_PATH", old_lord.c_str(), 1);
}

// Timed breeder rooms: place -> egg -> hatch, mmterm file header, reload
// re-matching of parent ids + persisted cooldowns, cancel, load_files=false.
static std::string slurp(const std::string &path) {
    std::string out;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

static long find_nick(const GameState &gs, const char *nick) {
    const auto &r = gs.breeding().roster();
    for (size_t i = 0; i < r.size(); ++i)
        if (strcmp(r[i].nick, nick) == 0) return (long)i;
    return -1;
}

static void test_rooms(const std::string &dir) {
    using namespace breeding;
    std::string box = dir + "/rooms_box.dat", lord = dir + "/rooms_lord.dat";
    std::string rooms = dir + "/breeder_rooms.dat", cds = dir + "/breeder_cooldowns.dat";
    for (const auto &p : {box, lord, rooms, cds}) unlink(p.c_str());
    GameState::Options o; o.enable_ipc = false; o.box_path = box; o.lord_path = lord;
    const long T = (long)time(nullptr);
    size_t n = 0;
    {
        GameState gs(o);
        CHECK(gs.rooms_path() == rooms);
        CHECK(gs.save_box());
        n = gs.breeding().size();
        for (int i = 0; i < gs.rooms().roomCount(); ++i) CHECK(gs.rooms().room(i).state == ROOM_EMPTY);
        long a = find_nick(gs, "Rosa"), b = find_nick(gs, "Prism");
        CHECK(a >= 0 && b >= 0);
        std::string err;
        CHECK(gs.place_pair((size_t)a, (size_t)b, T, err) == 0 && err.empty());
        CHECK(gs.place_pair((size_t)a, (size_t)b, T, err) == -1 && !err.empty());
        CHECK(gs.rooms().room(0).state == ROOM_INCUBATING);
        CHECK(gs.rooms().inAnyRoom(gs.breeding().roster()[a].id));
        CHECK(BreederManager::cooldownRemaining(gs.breeding().roster()[a], T) == BREED_COOLDOWN_SEC);
        CHECK(gs.tick_rooms(T + EGG_APPEAR_DELAY_SEC - 1) == 0 && gs.rooms().room(0).state == ROOM_INCUBATING);
        CHECK(gs.tick_rooms(T + EGG_APPEAR_DELAY_SEC) == 0 && gs.rooms().room(0).state == ROOM_EGG);
    }
    std::string blob = slurp(rooms);
    CHECK(blob.size() == 7 + 3 * sizeof(BreederRoom));
    if (blob.size() >= 7) {
        const unsigned char *h = (const unsigned char *)blob.data();
        uint16_t rsz = (uint16_t)(h[5] | (h[6] << 8));
        CHECK(h[0] == 0x52 && h[1] == 0x52 && h[2] == 0x4D && h[3] == 0x42 && h[4] == 1);
        CHECK(rsz == sizeof(BreederRoom));
    }
    {
        GameState gs(o);   // fresh ids; rooms + cooldowns come back from disk
        CHECK(gs.rooms().room(0).state == ROOM_EGG);
        long a = find_nick(gs, "Rosa"), b = find_nick(gs, "Prism");
        CHECK(a >= 0 && b >= 0);
        CHECK(gs.rooms().room(0).idA == gs.breeding().roster()[a].id);
        CHECK(gs.rooms().room(0).idB == gs.breeding().roster()[b].id);
        CHECK(gs.breeding().roster()[a].lastBredAt == T);
        std::string err;
        CHECK(gs.place_pair((size_t)a, (size_t)find_nick(gs, "Rock"), T + 60, err) == -1);   // in a room
        CHECK(!gs.pop_hatch());
        CHECK(gs.tick_rooms(T + EGG_APPEAR_DELAY_SEC + EGG_HATCH_DELAY_SEC) == 1);
        CHECK(gs.rooms().room(0).state == ROOM_EMPTY);
        auto h = gs.pop_hatch();
        CHECK(h.has_value() && !gs.pop_hatch());
        // ...and a one-line notice for the global toast, queued separately.
        auto note = gs.pop_hatch_notice();
        CHECK(note.has_value() && note->rfind("Room 1:", 0) == 0 && !gs.pop_hatch_notice());
        if (h) {
            CHECK(h->room == 0 && strcmp(h->parent_a.nick, "Rosa") == 0);
            CHECK(h->result.status == BREED_OK || h->result.status == BREED_NO_HATCH);
            if (h->result.status == BREED_OK) {
                CHECK(gs.breeding().size() == n + 1 && h->result.child.id != 0);
                CHECK(gs.breeding().roster().back().id == h->result.child.id);
            }
            printf("  rooms: hatch status %d, \"%s\"\n", (int)h->result.status, h->result.message.c_str());
            if (h->result.status == BREED_OK) ++n;
        }
    }
    {
        GameState gs(o);
        CHECK(gs.breeding().size() == n);   // hatch was saved to the box
        CHECK(gs.rooms().room(0).state == ROOM_EMPTY);
        long a = find_nick(gs, "Rosa");
        CHECK(a >= 0 && BreederManager::cooldownRemaining(gs.breeding().roster()[a], T + 60) > 0);
        long c = find_nick(gs, "Sable"), d = find_nick(gs, "Cinder");
        std::string err;
        CHECK(gs.place_pair((size_t)c, (size_t)d, T, err) == 0);
        CHECK(gs.cancel_room(0) && !gs.cancel_room(0) && gs.rooms().room(0).state == ROOM_EMPTY);
    }
    {
        // Another process (mmterm) atomically replaces the rooms file: tick() reloads it.
        unlink(cds.c_str());   // clear the earlier blocks' cooldowns
        GameState gs(o);
        long c = find_nick(gs, "Sable"), d = find_nick(gs, "Cinder");
        std::string err;
        CHECK(gs.place_pair((size_t)c, (size_t)d, T, err) == 0);
        std::string blob(7 + 3 * sizeof(BreederRoom), '\0');
        const uint32_t magic = 0x424D5252;
        const uint16_t rsz = (uint16_t)sizeof(BreederRoom);
        memcpy(&blob[0], &magic, 4);
        blob[4] = 1;
        memcpy(&blob[5], &rsz, 2);
        const std::string tmp = rooms + ".ext";
        FILE *f = fopen(tmp.c_str(), "wb");
        CHECK(f && fwrite(blob.data(), 1, blob.size(), f) == blob.size());
        if (f) fclose(f);
        CHECK(rename(tmp.c_str(), rooms.c_str()) == 0);
        const uint32_t rev = gs.revision();
        gs.tick();
        CHECK(gs.rooms().room(0).state == ROOM_EMPTY && gs.revision() != rev);
    }
    {
        std::string before = slurp(rooms), cd_before = slurp(cds);
        GameState::Options q = o; q.load_files = false;
        GameState gs(q);   // test roster, empty rooms, never writes
        for (int i = 0; i < gs.rooms().roomCount(); ++i) CHECK(gs.rooms().room(i).state == ROOM_EMPTY);
        std::string err;
        CHECK(gs.place_pair((size_t)find_nick(gs, "Shade"), (size_t)find_nick(gs, "Umbra"), T, err) == 0);
        CHECK(gs.tick_rooms(T + 4 * 3600) == 1);
        CHECK(!gs.save_rooms());
        CHECK(slurp(rooms) == before && slurp(cds) == cd_before);
        CHECK(slurp(box).size() == 5 + n * 34);
    }
}

// Fake daemon on a scratch socket: verifies the client's wire shapes + live switch.
static void test_fake_daemon(const std::string &dir) {
    std::string sock = dir + "/fake_mmd.sock";
    unlink(sock.c_str());
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (sock.size() >= sizeof(a.sun_path)) {   // macOS: 104-byte limit
        sock = "/tmp/mm_backend_smoke.sock";
        unlink(sock.c_str());
    }
    strncpy(a.sun_path, sock.c_str(), sizeof(a.sun_path) - 1);
    CHECK(bind(lfd, (sockaddr *)&a, sizeof(a)) == 0);
    CHECK(listen(lfd, 1) == 0);

    GameState::Options o;
    o.sock_path = sock; o.box_path = dir + "/no_box.dat"; o.lord_path = dir + "/no_lord.dat";
    GameState gs(o);
    CHECK(gs.demo_mode());
    gs.tick();   // non-blocking connect
    int cfd = accept(lfd, nullptr, nullptr);
    CHECK(cfd >= 0);
    std::string all = std::string(kPartyUpdate) + "\n" + kStatus + "\n" + kNeighbors + "\n" +
                      kDaycareEvt + "\n" + kMeshEvt + "\n" + "garbage line\n" + kChallenge +
                      "\n" + kAchievement + "\n";
    // Send in two fragments to exercise line reassembly.
    size_t half = all.size() / 2 + 3;
    write(cfd, all.data(), half);
    for (int i = 0; i < 5; ++i) { gs.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    write(cfd, all.data() + half, all.size() - half);
    for (int i = 0; i < 10; ++i) { gs.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }

    CHECK(gs.daemon_connected() && !gs.demo_mode());
    CHECK(gs.party().size() == 2 && gs.party()[1].nickname == "Z\"ZZ");
    CHECK(gs.daycare().sav_path == "/tmp/mm-test-saves/red.sav");
    CHECK(gs.daycare().neighbor_list.size() == 2);
    CHECK(gs.events().size() == 2 && gs.events().front().from_node == 439041101u);
    CHECK(gs.ipc().parse_errors() == 1);
    auto c = gs.pending_challenge();
    CHECK(c && c->node_id == 439041101u && c->party.empty());
    auto ach = gs.pop_achievement();
    CHECK(ach && ach->name == "First Steps");

    std::string np = std::string(kNeighborParty) + "\n";
    write(cfd, np.data(), np.size());
    for (int i = 0; i < 5; ++i) { gs.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    c = gs.pending_challenge();
    CHECK(c && c->party.size() == 1 && c->party[0].dex == 121);

    gs.challenge(439041101u);
    gs.accept_challenge();
    gs.force_event();
    gs.force_beacon();
    gs.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    char buf[4096];
    ssize_t n = read(cfd, buf, sizeof(buf) - 1);
    std::string got = n > 0 ? std::string(buf, (size_t)n) : std::string();
    CHECK(got.find("{\"cmd\":\"GET_STATUS\"}\n") != std::string::npos);
    CHECK(got.find("{\"cmd\":\"GET_PARTY\"}\n") != std::string::npos);
    CHECK(got.find("{\"cmd\":\"GET_NEIGHBOR_PARTY\",\"node_id\":439041101}\n") != std::string::npos);
    CHECK(got.find("{\"cmd\":\"SEND_CHALLENGE\",\"node_id\":439041101}\n") != std::string::npos);
    CHECK(got.find("{\"cmd\":\"ACCEPT_CHALLENGE\",\"node_id\":439041101}\n") != std::string::npos);
    CHECK(got.find("{\"cmd\":\"FORCE_EVENT\"}\n") != std::string::npos);
    CHECK(got.find("{\"cmd\":\"FORCE_BEACON\"}\n") != std::string::npos);

    close(cfd);   // daemon "dies": keep last live data
    for (int i = 0; i < 5; ++i) { gs.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(!gs.daemon_connected() && !gs.demo_mode() && gs.party().size() == 2);
    close(lfd);
    unlink(sock.c_str());
    printf("  fake daemon: lines=%u parse_errors=%u\n", gs.ipc().lines_received(), gs.ipc().parse_errors());
}

// Talk to a real mmd on the default (or MM_SOCK_PATH) socket.
static void test_live(int seconds) {
    GameState gs;
    std::string types;
    gs.set_message_hook([&](const std::string &t, const JsonValue &) {
        if (types.find(t + " ") == std::string::npos) types += t + " ";
    });
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    bool pinged = false;
    while (std::chrono::steady_clock::now() < end) {
        gs.tick();
        if (gs.daemon_connected() && !pinged) { gs.ipc().send_ping(); pinged = true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    printf("  live: connected=%d demo=%d connects=%u lines=%u parse_errors=%u\n",
           gs.daemon_connected(), gs.demo_mode(), gs.ipc().connect_count(),
           gs.ipc().lines_received(), gs.ipc().parse_errors());
    printf("  live: types seen: %s\n", types.c_str());
    printf("  live: node=0x%08x short=%s game=%s party=%zu active=%d sav=%s neighbors=%d\n",
           gs.daycare().node_id, gs.daycare().short_name.c_str(), gs.daycare().game_name.c_str(),
           gs.party().size(), gs.daycare().active, gs.daycare().sav_path.c_str(), gs.daycare().neighbors);
    for (const auto &pm : gs.party())
        printf("  live: #%03u %s \"%s\" L%u HP %u\n", pm.dex, pm.species.c_str(), pm.nickname.c_str(),
               pm.level, pm.max_hp);
    CHECK(gs.daemon_connected() && !gs.demo_mode());
    CHECK(types.find("HELLO ") != std::string::npos);
    CHECK(types.find("PARTY_UPDATE ") != std::string::npos);
    CHECK(types.find("STATUS ") != std::string::npos);
    CHECK(types.find("PONG ") != std::string::npos);
    CHECK(gs.ipc().parse_errors() == 0);
}

int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : "/tmp";
    printf("[json]\n");          test_json();
    printf("[demo]\n");          test_demo(dir);
    printf("[box/badges]\n");    test_box_and_badges(dir);
    printf("[rooms]\n");        test_rooms(dir);
    printf("[fake daemon]\n");   test_fake_daemon(dir);
    if (argc > 3 && strcmp(argv[2], "--live") == 0) {
        printf("[live mmd]\n");
        test_live(atoi(argv[3]));
    }
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
