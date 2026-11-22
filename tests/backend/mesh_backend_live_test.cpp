// mesh_backend_live_test.cpp — mesh_backend + mesh_link + messenger against
// tools/fake_meshtasticd.py on 127.0.0.1 (never a real radio).
//
// Build (from the project root):
//   clang++ -std=c++17 -Wall -Wextra -I src/backend \
//     tests/backend/mesh_backend_live_test.cpp src/backend/mesh_backend.cpp \
//     src/backend/mesh_link.cpp src/backend/messenger.cpp src/backend/settings.cpp \
//     src/backend/mm_json.cpp -o <out>/mesh_backend_live_test
//
// Run:
//   mkfifo <dir>/cmd
//   ~/meshtastic-venv/bin/python3 tools/fake_meshtasticd.py --port 44178 < <dir>/cmd &
//   mesh_backend_live_test <scratch_cfg_dir> 44178 <dir>/cmd
// The third argument is where the fake reads its commands (its stdin).
#include "mesh_backend.h"
#include "mesh_link.h"
#include "messenger.h"
#include "settings.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
#include <thread>

#include <sys/stat.h>

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

static uint32_t g_ms = 1000;
static bool pump_until(const std::function<bool()>& done, int max_ms = 5000) {
    for (int waited = 0; waited < max_ms; waited += 10) {
        mesh_backend().tick(g_ms);
        messenger().tick(g_ms);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        g_ms += 10;
    }
    return done();
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: %s <scratch_cfg_dir> <port> <fake_stdin_fifo>\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    ::mkdir(dir.c_str(), 0700);
    ::setenv("PC1999_CONFIG_DIR", dir.c_str(), 1);
    ::setenv("PC1999_MESH", (std::string("tcp:127.0.0.1:") + argv[2]).c_str(), 1);
    FILE* cmd = std::fopen(argv[3], "w");
    if (!cmd || store::dir() != dir) return 2;
    auto tell = [cmd](const char* json) {
        std::fprintf(cmd, "%s\n", json);
        std::fflush(cmd);
    };

    constexpr uint32_t kAlice = 0x11111111, kBob = 0x22222222, kNameless = 0x44444444;
    constexpr uint32_t kChan = Messenger::kChannelNode;
    Messenger& m = messenger();

    // --demo and "off" never start
    MeshBackend::Options demo;
    demo.demo = true;
    CHECK(!mesh_backend().start(demo) && !mesh_backend().on());
    CHECK(mesh_backend().status_text().empty());
    CHECK(MeshBackend::resolve_target() == std::string("tcp:127.0.0.1:") + argv[2]);

    CHECK(m.demo() && m.contact(0x5be0a903) != nullptr);
    CHECK(mesh_backend().start());
    CHECK(mesh_backend().on() && !mesh_backend().live());
    CHECK(mesh_backend().status() != MeshBackend::Status::Ready);
    CHECK(m.demo());                                      // still demo until the radio answers

    CHECK(pump_until([] { return mesh_backend().live(); }));
    CHECK(!m.demo());
    CHECK(mesh_backend().status() == MeshBackend::Status::Ready);
    std::printf("status: \"%s\", me: \"%s\"\n", mesh_backend().status_text().c_str(), m.my_name().c_str());
    CHECK(mesh_backend().status_text() == "MESH  5");
    CHECK(m.my_name() == "Pocket 1999");
    CHECK(m.contact(0x5be0a903) == nullptr && !m.has_chat(0x5be0a903));   // demo data is gone
    CHECK(m.contacts().size() == 5);                      // everyone but me
    CHECK(m.contact(mesh_link().my_node()) == nullptr);
    if (const Contact* a = m.contact(kAlice)) {
        const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
        CHECK(a->name == "Alice Base" && a->snr == 7.25f && a->hops == 0 && a->battery == 88);
        // the fake's clock is fixed at 1795000000 - 60 for her
        const uint32_t want = now - (1795000000u - 60u);
        CHECK(a->last_heard_s + 5 >= want && a->last_heard_s <= want + 5);
        CHECK(a->presence == (want <= MeshBackend::kOnlineS ? Presence::Online
                              : want <= MeshBackend::kAwayS ? Presence::Away : Presence::Offline));
    } else {
        CHECK(false);
    }
    if (const Contact* n = m.contact(kNameless)) {
        CHECK(n->name == "!44444444" && n->last_heard_s == Contact::kNeverHeard);
        CHECK(n->presence == Presence::Offline && n->battery == -1);
    } else {
        CHECK(false);
    }

    // trainer fields survive the mirror
    {
        Contact b = *m.contact(kBob);
        b.is_trainer = true;
        b.badges = 3;
        b.away_msg = "afk";
        m.upsert_contact(b);
        tell("{\"cmd\":\"telemetry\",\"from\":572662306,\"battery\":55}");
        CHECK(pump_until([&] { return m.contact(kBob)->battery == 55; }));
        CHECK(m.contact(kBob)->is_trainer && m.contact(kBob)->badges == 3 && m.contact(kBob)->away_msg == "afk");
    }

    // direct message out: sending -> delivered, and the fake's reply comes in
    CHECK(m.send(kAlice, "ping"));
    CHECK(m.messages(kAlice).back().state == SendState::Sending);
    CHECK(pump_until([&] { return m.messages(kAlice).size() >= 2 &&
                                  m.messages(kAlice)[0].state == SendState::Delivered; }));
    CHECK(m.messages(kAlice).size() == 2 && m.messages(kAlice)[1].text == "re: ping");
    CHECK(!m.messages(kAlice)[1].from_me && m.unread(kAlice) == 1);

    // NAK: failed with the radio's reason, then a retry that works
    tell("{\"cmd\":\"mode\",\"ack\":\"nak\",\"reply\":false}");
    pump_until([] { return false; }, 300);
    CHECK(m.send(kAlice, "lost"));
    CHECK(pump_until([&] { return m.messages(kAlice).back().state == SendState::Failed; }));
    std::printf("nak reason: %d \"%s\"\n", m.messages(kAlice).back().error,
                send_error_text(m.messages(kAlice).back().error));
    CHECK(m.messages(kAlice).back().error > 0);
    tell("{\"cmd\":\"mode\",\"ack\":\"normal\",\"reply\":false}");
    pump_until([] { return false; }, 300);
    CHECK(m.retry(kAlice));
    CHECK(pump_until([&] { return m.messages(kAlice).back().state == SendState::Delivered; }));

    // incoming: direct, channel, a stranger, and things that are not for us
    tell("{\"cmd\":\"text\",\"from\":572662306,\"to\":4294967295,\"text\":\"hi all\",\"channel\":0}");
    tell("{\"cmd\":\"text\",\"from\":572662306,\"to\":4294967295,\"text\":\"side talk\",\"channel\":1}");
    tell("{\"cmd\":\"text\",\"from\":2578103244,\"to\":168496141,\"text\":\"who am i\"}");
    tell("{\"cmd\":\"text\",\"from\":572662306,\"to\":286331153,\"text\":\"not for pocket\"}");
    CHECK(pump_until([&] { return m.has_chat(0x99aabbcc) && m.messages(kChan).size() >= 2; }));
    pump_until([] { return false; }, 300);
    CHECK(m.messages(kChan).size() == 2);
    if (m.messages(kChan).size() == 2) {
        CHECK(m.messages(kChan)[0].text == "hi all" && m.messages(kChan)[0].from_node == kBob);
        CHECK(m.messages(kChan)[1].text == "[ch 1] side talk");
        CHECK(m.name_of(m.messages(kChan)[0].from_node) == "Bob Mobile");
    }
    CHECK(m.contact(0x99aabbcc) && m.name_of(0x99aabbcc) == "!99aabbcc");
    CHECK(m.messages(0x99aabbcc).size() == 1 && m.messages(0x99aabbcc)[0].text == "who am i");
    CHECK(!m.has_chat(kBob));                             // the overheard one went nowhere
    tell("{\"cmd\":\"nodeinfo\",\"from\":2578103244,\"long\":\"Stranger Things\",\"short\":\"ST\"}");
    CHECK(pump_until([&] { return m.name_of(0x99aabbcc) == "Stranger Things"; }));

    // channel out: a broadcast
    CHECK(m.send(kChan, "hello mesh"));
    CHECK(m.messages(kChan).back().state == SendState::Sent ||
          m.messages(kChan).back().state == SendState::Delivered);
    pump_until([&] { return m.messages(kChan).back().state == SendState::Delivered; }, 1500);
    std::printf("channel message state after 1.5 s: %d\n", static_cast<int>(m.messages(kChan).back().state));

    // the radio goes away: status says so, sends are kept as not sent; it comes
    // back: still live, the chats are still there, retry works
    tell("{\"cmd\":\"drop\"}");
    CHECK(pump_until([] { return mesh_backend().status() != MeshBackend::Status::Ready; }));
    std::printf("status after drop: \"%s\"\n", mesh_backend().status_text().c_str());
    if (mesh_backend().status() != MeshBackend::Status::Ready) {
        CHECK(m.send(kAlice, "while away"));
        CHECK(m.messages(kAlice).back().state == SendState::Unsent);
        CHECK(!m.demo() && m.has_chat(kAlice) && m.contact(kAlice) != nullptr);
        CHECK(pump_until([] { return mesh_backend().status() == MeshBackend::Status::Ready; }, 8000));
        CHECK(m.retry(kAlice));
        CHECK(pump_until([&] { return m.messages(kAlice).back().state == SendState::Delivered; }));
    }

    mesh_backend().stop();
    CHECK(!mesh_backend().on() && mesh_backend().status() == MeshBackend::Status::Off);
    CHECK(m.send(kAlice, "after stop"));
    CHECK(m.messages(kAlice).back().state == SendState::Unsent);
    tell("{\"cmd\":\"quit\"}");
    std::fclose(cmd);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
