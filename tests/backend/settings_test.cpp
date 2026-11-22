// settings_test.cpp — persistence tests (no LVGL, no SDL, no game logic).
//
// Build (from the project root):
//   clang++ -std=c++17 -Wall -Wextra -I src/backend \
//     tests/backend/settings_test.cpp src/backend/settings.cpp \
//     src/backend/mm_json.cpp src/backend/messenger.cpp src/backend/mesh_space.cpp \
//     -o <out>/settings_test
//
// Usage: settings_test <scratch_dir>
// The scratch dir is used as PC1999_CONFIG_DIR; the real ~/.config is never touched.
#include "mesh_space.h"
#include "messenger.h"
#include "mm_json.h"
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace mm;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++g_pass;                                                            \
        } else {                                                                 \
            ++g_fail;                                                            \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

static bool exists(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0;
}
static uint64_t inode(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_ino) : 0;
}
static std::string slurp(const std::string& p) {
    std::string out;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}
static void spit(const std::string& p, const std::string& text) {
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}
static int count_files(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return -1;
    int n = 0;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") ++n;
    }
    ::closedir(d);
    return n;
}
static void wipe(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") ::unlink((dir + "/" + name).c_str());
    }
    ::closedir(d);
}

static void test_store_paths(const std::string& dir) {
    CHECK(store::dir() == dir);
    CHECK(store::path("settings.json") == dir + "/settings.json");
    ::unsetenv("PC1999_CONFIG_DIR");
    ::setenv("XDG_CONFIG_HOME", "/xdg/", 1);
    CHECK(store::dir() == "/xdg//1999online" || store::dir() == "/xdg/1999online");
    ::unsetenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    if (home) CHECK(store::dir() == std::string(home) + "/.config/1999online");
    ::setenv("PC1999_CONFIG_DIR", dir.c_str(), 1);
    CHECK(store::dir() == dir);
}

static void test_round_trip(const std::string& dir) {
    wipe(dir);
    const std::string file = dir + "/settings.json";
    {
        Settings s;
        CHECK(s.file() == file);
        CHECK(!s.has("ui.skin"));
        CHECK(s.get_string("ui.skin", "w95") == "w95");
        CHECK(s.get_int("amp.volume", 80) == 80);
        CHECK(s.get_bool("amp.shuffle", true));
        s.set_string("ui.skin", "winamp");
        s.set_int("amp.volume", 37);
        s.set_string("amp.kind", "radio");
        s.set_int("amp.index", 4);
        s.set_int("amp.viz_preset", -2);
        s.set_bool("amp.shuffle", false);
        s.set_bool("amp.repeat", true);
        s.set_string("odd", "quote \" slash \\ newline \n tab \t caf\xc3\xa9");
        s.set_string("gone", "x");
        s.remove("gone");
        CHECK(s.dirty());
        CHECK(!exists(file));          // nothing written until tick/flush
        CHECK(s.flush());
        CHECK(!s.dirty());
        CHECK(exists(file));
        CHECK(s.writes() == 1);
    }
    {
        Settings s;                    // a second "run"
        CHECK(s.get_string("ui.skin") == "winamp");
        CHECK(s.get_int("amp.volume") == 37);
        CHECK(s.get_string("amp.kind") == "radio");
        CHECK(s.get_int("amp.index") == 4);
        CHECK(s.get_int("amp.viz_preset") == -2);
        CHECK(s.get_bool("amp.shuffle", true) == false);
        CHECK(s.get_bool("amp.repeat") == true);
        CHECK(s.get_string("odd") == "quote \" slash \\ newline \n tab \t caf\xc3\xa9");
        CHECK(!s.has("gone"));
        // wrong type => default
        CHECK(s.get_int("ui.skin", 9) == 9);
        CHECK(s.get_string("amp.volume", "d") == "d");
        // same value again: not dirty, no write on destruction
        s.set_int("amp.volume", 37);
        s.set_string("ui.skin", "winamp");
        CHECK(!s.dirty());
    }
    // The file is a JSON object the project parser reads.
    JsonValue doc;
    CHECK(json_parse(slurp(file), doc) && doc.is_object());
    CHECK(doc.get_string("ui.skin") == "winamp");
    CHECK(doc.get_int("amp.volume") == 37);
    // The destructor flushes.
    {
        Settings s;
        s.set_int("amp.volume", 55);
    }
    {
        Settings s;
        CHECK(s.get_int("amp.volume") == 55);
    }
    // No temp files left behind.
    CHECK(count_files(dir) == 1);
}

static void test_corrupt(const std::string& dir) {
    wipe(dir);
    const std::string file = dir + "/settings.json";
    const char* bad[] = {"{\"ui.skin\": \"xp\", ", "", "not json at all", "[1,2,3]", "\"str\"",
                         "{\"a\": }"};
    for (const char* text : bad) {
        wipe(dir);
        spit(file, text);
        Settings s;
        CHECK(!s.has("ui.skin"));
        CHECK(s.get_string("ui.skin", "w95") == "w95");
        CHECK(!s.dirty());
        // the bad file was kept aside, not destroyed
        CHECK(!exists(file));
        CHECK(slurp(file + ".corrupt") == text);
        s.set_string("ui.skin", "beos");
        CHECK(s.flush());
        Settings again;
        CHECK(again.get_string("ui.skin") == "beos");
    }
    // Values of other types in a valid file are skipped, the rest is kept.
    wipe(dir);
    spit(file, "{\"a\": null, \"b\": [1], \"c\": {\"x\": 1}, \"d\": 2.5, \"e\": \"ok\", \"f\": true}");
    Settings s;
    CHECK(!s.has("a") && !s.has("b") && !s.has("c"));
    CHECK(s.get_int("d") == 3 || s.get_int("d") == 2);
    CHECK(s.get_string("e") == "ok");
    CHECK(s.get_bool("f"));
    CHECK(!exists(file + ".corrupt"));
}

static void test_debounce(const std::string& dir) {
    wipe(dir);
    const std::string file = dir + "/settings.json";
    Settings s;
    uint32_t t = 0xfffffe00u;          // also crosses the uint32 wrap
    s.tick(t);
    CHECK(s.writes() == 0);
    // 5 simulated seconds of a volume knob being turned, ticked every 33 ms.
    int v = 0;
    for (int i = 0; i < 150; ++i) {
        s.set_int("amp.volume", ++v);
        s.tick(t);
        t += 33;
    }
    CHECK(s.writes() >= 3 && s.writes() <= 5);     // ~1 per second, never 150
    const uint32_t during = s.writes();
    // Let it settle: one final write carries the last value.
    for (int i = 0; i < 40; ++i) {
        s.tick(t);
        t += 33;
    }
    CHECK(s.writes() == during + 1 || s.writes() == during);
    CHECK(!s.dirty());
    const uint32_t settled = s.writes();
    for (int i = 0; i < 100; ++i) {
        s.tick(t);
        t += 33;
    }
    CHECK(s.writes() == settled);                  // idle: no writes
    Settings back;
    CHECK(back.get_int("amp.volume") == v);
    // A single change is written no sooner than kFlushMs after tick() saw it.
    s.set_string("ui.skin", "xp");
    s.tick(t);
    s.tick(t + Settings::kFlushMs - 1);
    CHECK(s.writes() == settled);
    s.tick(t + Settings::kFlushMs);
    CHECK(s.writes() == settled + 1);
    CHECK(exists(file));
}

static void test_atomic(const std::string& dir) {
    wipe(dir);
    const std::string file = dir + "/settings.json";
    Settings s;
    s.set_string("ui.skin", "os2");
    CHECK(s.flush());
    const uint64_t ino1 = inode(file);
    // A reader that opened the old file keeps a complete old document.
    FILE* old = std::fopen(file.c_str(), "rb");
    CHECK(old != nullptr);
    s.set_string("ui.skin", "x11");
    CHECK(s.flush());
    const uint64_t ino2 = inode(file);
    CHECK(ino1 != 0 && ino2 != 0 && ino1 != ino2);     // replaced by rename, not rewritten
    if (old) {
        std::string was;
        char buf[512];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), old)) > 0) was.append(buf, n);
        std::fclose(old);
        JsonValue doc;
        CHECK(json_parse(was, doc) && doc.get_string("ui.skin") == "os2");
    }
    JsonValue now;
    CHECK(json_parse(slurp(file), now) && now.get_string("ui.skin") == "x11");
    CHECK(count_files(dir) == 1);                      // no .tmp left
    struct stat st {};
    CHECK(::stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);

    // A failed write leaves the old file intact and no temp file.
    const std::string sub = dir + "/ro";
    ::mkdir(sub.c_str(), 0700);
    spit(sub + "/settings.json", "{\"ui.skin\": \"me\"}");
    ::chmod(sub.c_str(), 0500);
    {
        Settings ro(sub + "/settings.json");
        CHECK(ro.get_string("ui.skin") == "me");
        ro.set_string("ui.skin", "vista");
        if (::geteuid() != 0) {
            CHECK(!ro.flush());
            CHECK(ro.dirty());                         // will be retried
            CHECK(slurp(sub + "/settings.json") == "{\"ui.skin\": \"me\"}");
            CHECK(count_files(sub) == 1);
        }
        ::chmod(sub.c_str(), 0700);
        CHECK(ro.flush());                             // retry works once writable
    }
    ::unlink((sub + "/settings.json").c_str());
    ::rmdir(sub.c_str());

    // The folder is created on demand (nested).
    const std::string deep = dir + "/a/b";
    CHECK(store::write_atomic(deep + "/x.json", "{}"));
    CHECK(slurp(deep + "/x.json") == "{}");
    ::unlink((deep + "/x.json").c_str());
    ::rmdir(deep.c_str());
    ::rmdir((dir + "/a").c_str());
}

static void test_disabled(const std::string& dir) {
    wipe(dir);
    const std::string file = dir + "/settings.json";
    spit(file, "{\"ui.skin\": \"mac9\"}");
    store::set_enabled(false);
    {
        Settings s;
        CHECK(!s.has("ui.skin"));                      // not even read
        s.set_string("ui.skin", "sys7");
        CHECK(s.get_string("ui.skin") == "sys7");      // still works in memory
        CHECK(!s.flush());
        for (uint32_t t = 0; t < 5000; t += 33) s.tick(t);
        CHECK(s.writes() == 0);
        std::string text;
        CHECK(!store::read_text(file, text));
        CHECK(!store::write_atomic(dir + "/other.json", "{}"));
        CHECK(!store::remove_file(file));

        Messenger m;                                   // demo data, no file
        m.send(0x5be0a903, "hello");
        for (uint32_t t = 0; t < 5000; t += 33) m.tick(t);
        CHECK(!m.save());

        MeshSpace sp;
        sp.set_icon(MeshSpace::kMe, 25);
        sp.post(MeshSpace::kMe, "me", "hi");
    }
    CHECK(count_files(dir) == 1);
    CHECK(slurp(file) == "{\"ui.skin\": \"mac9\"}");   // untouched
    store::set_enabled(true);
}

static void test_messenger(const std::string& dir) {
    wipe(dir);
    const std::string file = dir + "/conversations.json";
    constexpr uint32_t kTrail = 0x5be0a903, kKd = 0xa3f29c01, kBase = 0xba5e5701;
    constexpr uint32_t kStranger = 0x00c0ffee;
    size_t demo_contacts = 0;
    {
        Messenger m;                                   // no file: demo chats
        demo_contacts = m.contacts().size();
        CHECK(m.has_chat(kTrail) && m.has_chat(kKd) && m.has_chat(kBase));
        CHECK(m.unread(kTrail) == 1);
        CHECK(!m.unsaved());
        CHECK(!exists(file));                          // just looking writes nothing
        m.send(kTrail, "on my way \"soon\"\n:)");
        Contact c;
        c.node_id = kStranger;
        c.name = "stranger_42";
        m.upsert_contact(c);
        m.receive(kStranger, "who dis", 3, -4.25f);
        m.receive(kStranger, "auto", 3, -4.25f, true);
        m.delete_chat(kKd);
        m.mark_read(kTrail);
        CHECK(m.unsaved());
        // debounce: 3 s of ticks => a handful of writes at most, then clean
        uint32_t t = 1000;
        m.tick(t);
        CHECK(!exists(file));
        for (int i = 0; i < 90; ++i) m.tick(t += 33);
        CHECK(exists(file));
        // the demo reply to my send() arrived (~2 s) and is saved by the destructor
    }
    {
        Messenger m;                                   // second run: from the file
        CHECK(!m.has_chat(kKd));                       // deleted stays deleted
        CHECK(m.has_chat(kBase));
        CHECK(m.unread(kTrail) == 1);                  // the demo reply, unread
        const auto& tr = m.messages(kTrail);
        CHECK(tr.size() == 5);                         // 3 demo + mine + reply
        if (tr.size() == 5) {
            CHECK(tr[3].from_me && tr[3].text == "on my way \"soon\"\n:)" && tr[3].delivered);
            CHECK(!tr[4].from_me);
            CHECK(tr[0].text == "is the lot full?" && tr[0].from_me);
            CHECK(tr[0].timestamp != 0 && tr[0].timestamp <= tr[3].timestamp);
            CHECK(tr[0].hops == 1 && tr[0].snr == 9.0f);
        }
        const auto& st = m.messages(kStranger);
        CHECK(st.size() == 2 && m.unread(kStranger) == 2);
        if (st.size() == 2) {
            CHECK(st[0].text == "who dis" && !st[0].from_me && st[0].hops == 3);
            CHECK(st[0].snr == -4.25f && !st[0].auto_response && st[1].auto_response);
        }
        // a peer that is not in the (demo) contact list comes back by name
        CHECK(m.name_of(kStranger) == "stranger_42");
        CHECK(m.contacts().size() == demo_contacts + 1);
        CHECK(!m.unsaved());

        // history cap
        for (int i = 0; i < 260; ++i) m.receive(kStranger, "m" + std::to_string(i), 1, 0.0f);
        CHECK(m.save());
    }
    {
        Messenger m;
        const auto& st = m.messages(kStranger);
        CHECK(st.size() == Messenger::kMaxStoredMessages);
        CHECK(!st.empty() && st.back().text == "m259");
        CHECK(!st.empty() && st.front().text == "m60");
        // deleting everything leaves an empty list, not the demo data
        m.delete_chat(kStranger);
        m.delete_chat(kTrail);
        m.delete_chat(kBase);
        m.delete_chat(0x3202a9f7);
        CHECK(m.total_unread() == 0);
    }
    {
        Messenger m;
        CHECK(!m.has_chat(kTrail) && !m.has_chat(kStranger) && !m.has_chat(kBase));
        CHECK(m.contacts().size() >= demo_contacts);   // contacts are not ours to forget
    }
    JsonValue doc;
    CHECK(json_parse(slurp(file), doc) && doc["conversations"].is_array());
    CHECK(doc["conversations"].size() == 0);
    // corrupt file: demo data again, bad file kept aside
    spit(file, "{\"conversations\": [ {\"peer\": ");
    {
        Messenger m;
        CHECK(m.has_chat(kTrail) && m.has_chat(kKd));
        CHECK(exists(file + ".corrupt") && !exists(file));
    }
    // odd entries are skipped, not fatal
    spit(file, "{\"version\":1,\"conversations\":[{\"peer\":\"zz\"},{\"name\":\"x\"},7,"
               "{\"peer\":\"!0000beef\",\"name\":\"beef\",\"unread\":-3,"
               "\"messages\":[{\"me\":false,\"t\":5,\"text\":\"moo\"},8]}]}");
    {
        Messenger m;
        CHECK(m.has_chat(0xbeef) && m.messages(0xbeef).size() == 2);
        CHECK(m.messages(0xbeef)[0].text == "moo" && m.unread(0xbeef) == 0);
        CHECK(m.name_of(0xbeef) == "beef");
        CHECK(!m.has_chat(kTrail));
    }
}

static void test_space(const std::string& dir) {
    wipe(dir);
    const std::string file = dir + "/space.json";
    constexpr uint32_t kMe = MeshSpace::kMe, kOther = MeshSpace::kDemoNode;
    size_t wall0 = 0, upd0 = 0, champs0 = 0, other_wall0 = 0;
    int lost0 = 0;
    {
        MeshSpace sp;                                  // no file: demo page
        wall0 = sp.wall(kMe).size();
        upd0 = sp.updates(kMe).size();
        champs0 = sp.gym(kMe).champions.size();
        other_wall0 = sp.wall(kOther).size();
        lost0 = sp.gym(kMe).lost;
        CHECK(sp.profile(kMe).icon_dex == 0);
        // changes to OTHER pages, and my leader party, never create the file
        sp.post(kOther, sp.my_name(), "hi from the test");
        sp.set_leader_party(kMe, {{150, 70}});
        for (uint32_t t = 0; t < 4000; t += 33) sp.tick(t);
        CHECK(sp.save());
        CHECK(!exists(file));

        sp.set_icon(kMe, 25);
        CHECK(sp.post(kMe, "Tom", "saved \"post\" \xe2\x99\xa5"));
        CHECK(sp.record_win(kMe, "Trailhead-R", 20, {{9, 36}, {65, 34}}, "Nov 20"));
        sp.add_update(kMe, UpdateKind::BadgeEarned, "{You} earned the Test Badge", "Nov 20 01:00 PM");
        SpaceProfile pr = sp.profile(kMe);
        pr.about = "new about";
        pr.mood = "testing";
        sp.set_profile(kMe, pr);
        // debounced: nothing on the first tick, written within ~1 s
        uint32_t t = 10000;
        sp.tick(t);
        CHECK(!exists(file));
        sp.tick(t + 999);
        CHECK(!exists(file));
        sp.tick(t + 1000);
        CHECK(exists(file));
        const uint64_t ino = inode(file);
        for (uint32_t k = 0; k < 4000; k += 33) sp.tick(t + 1000 + k);
        CHECK(inode(file) == ino);                     // idle: not rewritten
        sp.set_icon(kMe, 133);                         // saved by the destructor
    }
    JsonValue doc;
    CHECK(json_parse(slurp(file), doc) && doc.is_object());
    CHECK(doc["profile"].get_int("icon_dex") == 133);
    {
        MeshSpace sp;                                  // second run
        CHECK(sp.profile(kMe).icon_dex == 133);
        CHECK(sp.profile(kMe).about == "new about" && sp.profile(kMe).mood == "testing");
        CHECK(sp.profile(kMe).name == "xXmeshKid03Xx");
        CHECK(sp.wall(kMe).size() == wall0 + 1);
        CHECK(sp.wall(kMe).back().author == "Tom");
        CHECK(sp.wall(kMe).back().text == "saved \"post\" \xe2\x99\xa5");
        CHECK(!sp.wall(kMe).back().time.empty());
        const SpaceGym& g = sp.gym(kMe);
        CHECK(g.champions.size() == champs0 + 1 && g.lost == lost0);
        CHECK(g.name == "Basement Gym" && g.badge == "Solder Badge");
        if (!g.champions.empty()) {
            CHECK(g.champions[0].name == "Trailhead-R" && g.champions[0].turns == 20);
            CHECK(g.champions[0].date == "Nov 20" && g.champions[0].party.size() == 2);
            CHECK(g.champions[0].party.size() == 2 && g.champions[0].party[1].dex == 65 &&
                  g.champions[0].party[1].level == 34);
        }
        // wall post by Tom + win + 2 ladder drops + badge
        CHECK(sp.updates(kMe).size() == upd0 + 5);
        CHECK(sp.updates(kMe).back().kind == UpdateKind::BadgeEarned);
        CHECK(sp.updates(kMe).back().text == "{You} earned the Test Badge");
        CHECK(sp.updates(kMe).back().time == "Nov 20 01:00 PM");
        CHECK(sp.updates(kMe)[0].kind == UpdateKind::GymBeaten);
        // other people's pages are not persisted
        CHECK(sp.wall(kOther).size() == other_wall0);
        // an unchanged page is not rewritten
        const uint64_t ino = inode(file);
        CHECK(sp.save());
        CHECK(inode(file) == ino);
        sp.set_icon(kMe, 0);                           // back to the gym lead
    }
    {
        MeshSpace sp;
        CHECK(sp.profile(kMe).icon_dex == 0);
        CHECK(sp.wall(kMe).size() == wall0 + 1);
    }
    CHECK(count_files(dir) == 1);
    // corrupt: demo page, file kept aside
    spit(file, "{\"profile\": {\"icon_dex\": 7");
    {
        MeshSpace sp;
        CHECK(sp.profile(kMe).icon_dex == 0 && sp.wall(kMe).size() == wall0);
        CHECK(exists(file + ".corrupt") && !exists(file));
    }
    // partial / odd file: what is there is used, the rest stays
    spit(file, "{\"profile\": {\"icon_dex\": 7}, \"wall\": [3, {\"author\": \"a\", \"text\": \"t\"}],"
               " \"updates\": [{\"kind\": \"nope\", \"text\": \"x\"}]}");
    {
        MeshSpace sp;
        CHECK(sp.profile(kMe).icon_dex == 7);
        CHECK(sp.profile(kMe).about != "" && sp.wall(kMe).size() == 1 && sp.updates(kMe).empty());
        CHECK(sp.gym(kMe).champions.size() == champs0);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: settings_test <scratch_dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    ::mkdir(dir.c_str(), 0700);
    ::setenv("PC1999_CONFIG_DIR", dir.c_str(), 1);

    test_store_paths(dir);
    test_round_trip(dir);
    test_corrupt(dir);
    test_debounce(dir);
    test_atomic(dir);
    test_disabled(dir);
    test_messenger(dir);
    test_space(dir);
    wipe(dir);

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
