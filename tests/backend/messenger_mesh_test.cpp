// messenger_mesh_test.cpp — the messenger's live (radio) side with a fake
// sender: no LVGL, no SDL, no MeshLink.
//
// Build (from the project root):
//   clang++ -std=c++17 -Wall -Wextra -I src/backend \
//     tests/backend/messenger_mesh_test.cpp src/backend/messenger.cpp \
//     src/backend/settings.cpp src/backend/mm_json.cpp -o <out>/messenger_mesh_test
//
// Usage: messenger_mesh_test <scratch_dir>
// The scratch dir is used as PC1999_CONFIG_DIR; the real ~/.config is never touched.
#include "messenger.h"
#include "mm_json.h"
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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
static void wipe(const std::string& dir) {
    if (DIR* d = ::opendir(dir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") ::unlink((dir + "/" + n).c_str());
        }
        ::closedir(d);
    }
}

// A radio that records what it was asked to send.
struct FakeRadio {
    bool ready = true;
    uint32_t next_id = 100;
    struct Sent {
        uint32_t id, node;
        std::string text;
    };
    std::vector<Sent> sent;
    Messenger::Sender hook() {
        return [this](uint32_t node, const std::string& text) -> uint32_t {
            if (!ready) return 0;
            sent.push_back({next_id, node, text});
            return next_id++;
        };
    }
};

constexpr uint32_t kTrail = 0x5be0a903, kKd = 0xa3f29c01;   // demo nodes
constexpr uint32_t kAnna = 0x11aa22bb, kBob = 0x33cc44dd;    // "real" nodes
constexpr uint32_t kChan = Messenger::kChannelNode;

static Contact real(uint32_t id, const char* name, Presence p = Presence::Online) {
    Contact c;
    c.node_id = id;
    c.name = name;
    c.presence = p;
    c.hops = 2;
    c.snr = 5.5f;
    return c;
}

static void test_send_states(const std::string& dir) {
    wipe(dir);
    FakeRadio radio;
    Messenger m;
    m.set_sender(radio.hook());
    m.go_live();
    m.upsert_contact(real(kAnna, "anna"));

    // send -> sending -> ack -> delivered
    CHECK(m.send(kAnna, "hi anna"));
    CHECK(radio.sent.size() == 1 && radio.sent[0].node == kAnna && radio.sent[0].text == "hi anna");
    CHECK(m.messages(kAnna).size() == 1);
    {
        const Message& a = m.messages(kAnna)[0];
        CHECK(a.from_me && a.state == SendState::Sending && !a.delivered);
        CHECK(a.packet_id == radio.sent[0].id);
    }
    const uint32_t rev = m.revision();
    CHECK(m.ack_packet(radio.sent[0].id, true));
    CHECK(m.revision() != rev);
    {
        const Message& a = m.messages(kAnna)[0];
        CHECK(a.state == SendState::Delivered && a.delivered && a.error == kSendErrNone);
        CHECK(a.hops == 2 && a.snr == 5.5f);              // from the contact
        CHECK(a.packet_id == 0);
    }
    CHECK(!m.ack_packet(radio.sent[0].id, true));         // a second report: nobody waiting
    CHECK(!m.ack_packet(424242, true));
    CHECK(m.retry_index(kAnna) == -1 && !m.retry(kAnna));

    // NAK -> failed with the reason; retry -> sending again -> delivered
    CHECK(m.send(kAnna, "second"));
    CHECK(m.ack_packet(radio.sent[1].id, false, 1));
    {
        const Message& b = m.messages(kAnna)[1];
        CHECK(b.state == SendState::Failed && b.error == 1 && !b.delivered && b.retryable());
        CHECK(std::string(send_error_text(b.error)) == "no route");
    }
    CHECK(std::string(send_error_text(3)) == "timed out");
    CHECK(std::string(send_error_text(5)) == "no answer after retries");
    CHECK(std::string(send_error_text(kSendErrNoRadio)) == "radio not connected");
    CHECK(std::string(send_error_text(999)) == "mesh error");
    CHECK(m.retry_index(kAnna) == 1);
    CHECK(m.retry(kAnna));
    CHECK(radio.sent.size() == 3 && radio.sent[2].text == "second" && radio.sent[2].node == kAnna);
    CHECK(m.messages(kAnna).size() == 2);                 // same message, not a copy
    CHECK(m.messages(kAnna)[1].state == SendState::Sending && m.messages(kAnna)[1].error == kSendErrNone);
    CHECK(!m.ack_packet(radio.sent[1].id, true));         // the old packet id is dead
    CHECK(m.ack_packet(radio.sent[2].id, true));
    CHECK(m.messages(kAnna)[1].state == SendState::Delivered);

    // a failed report with no code still fails, as "no answer"
    CHECK(m.send(kAnna, "third"));
    CHECK(m.ack_packet(radio.sent[3].id, false));
    CHECK(m.messages(kAnna)[2].state == SendState::Failed && m.messages(kAnna)[2].error == kSendErrNoAnswer);

    // not ready -> kept as not sent -> retry (still down) -> retry (up)
    radio.ready = false;
    CHECK(m.send(kAnna, "from the valley"));
    CHECK(radio.sent.size() == 4);
    CHECK(m.messages(kAnna).size() == 4);
    CHECK(m.messages(kAnna)[3].state == SendState::Unsent && m.messages(kAnna)[3].error == kSendErrNoRadio);
    CHECK(m.retry_index(kAnna) == 3);                     // the newest one first
    CHECK(m.retry(kAnna));
    CHECK(m.messages(kAnna)[3].state == SendState::Unsent && radio.sent.size() == 4);
    radio.ready = true;
    CHECK(m.retry(kAnna));
    CHECK(radio.sent.size() == 5 && radio.sent[4].text == "from the valley");
    CHECK(m.messages(kAnna)[3].state == SendState::Sending);
    CHECK(m.retry_index(kAnna) == 2);                     // now the older failed one
    CHECK(m.ack_packet(radio.sent[4].id, true));

    // no report at all: failed "no answer" after kAckTimeoutMs of ticks
    CHECK(m.send(kAnna, "into the void"));
    const size_t last = m.messages(kAnna).size() - 1;
    m.tick(5000);
    m.tick(5000 + Messenger::kAckTimeoutMs - 1);
    CHECK(m.messages(kAnna)[last].state == SendState::Sending);
    m.tick(5000 + Messenger::kAckTimeoutMs);
    CHECK(m.messages(kAnna)[last].state == SendState::Failed);
    CHECK(m.messages(kAnna)[last].error == kSendErrNoAnswer);

    // no sender at all behaves like "not connected"
    m.set_sender(nullptr);
    CHECK(m.send(kBob, "anyone?"));
    CHECK(m.messages(kBob)[0].state == SendState::Unsent);

    // too long / empty: refused, nothing added
    const size_t before = m.messages(kAnna).size();
    CHECK(!m.send(kAnna, std::string(Messenger::kMaxMessageBytes + 1, 'x')));
    CHECK(!m.send(kAnna, ""));
    CHECK(m.messages(kAnna).size() == before);
    m.set_sender(radio.hook());
    CHECK(m.send(kAnna, std::string(Messenger::kMaxMessageBytes, 'x')));
    CHECK(radio.sent.back().text.size() == static_cast<size_t>(Messenger::kMaxMessageBytes));

    // deleting a chat forgets its packets
    const uint32_t pending = radio.sent.back().id;
    m.delete_chat(kAnna);
    CHECK(!m.ack_packet(pending, true));
    CHECK(!m.has_chat(kAnna));
}

static void test_demo_unchanged(const std::string& dir) {
    wipe(dir);
    FakeRadio radio;
    Messenger m;
    m.set_sender(radio.hook());          // the backend is on but the radio never answered
    CHECK(m.demo() && m.is_demo_node(kTrail));
    CHECK(m.send(kTrail, "hello"));
    CHECK(radio.sent.empty());           // demo nodes never reach the radio
    CHECK(m.messages(kTrail).back().state == SendState::Delivered && m.messages(kTrail).back().delivered);
    for (uint32_t t = 0; t < 3000; t += 33) m.tick(t);
    CHECK(!m.messages(kTrail).back().from_me);            // the canned reply
    // an offline demo node: the message waits in the outbox
    CHECK(m.send(0x3202a9f7, "u there?"));
    CHECK(!m.messages(0x3202a9f7).back().delivered);
    CHECK(m.messages(0x3202a9f7).back().state == SendState::Unsent);
    CHECK(m.messages(0x3202a9f7).back().error == kSendErrQueued);
    m.tick(3000 + 2 * Messenger::kAckTimeoutMs);
    CHECK(m.messages(0x3202a9f7).back().state == SendState::Unsent);    // demo: the outbox never sends
}

static void test_go_live(const std::string& dir) {
    wipe(dir);
    const std::string file = dir + "/conversations.json";
    // 1. a demo-seeded session: go_live() leaves nothing behind
    {
        Messenger m;
        const uint32_t rev = m.revision();
        CHECK(m.demo() && !m.contacts().empty() && m.has_chat(kTrail));
        m.go_live();
        CHECK(!m.demo() && m.revision() != rev);
        CHECK(m.contacts().empty());
        CHECK(!m.has_chat(kTrail) && !m.has_chat(kKd) && m.total_unread() == 0);
        CHECK(!m.is_demo_node(kTrail));
        m.go_live();                                      // again: no-op
        m.upsert_contact(real(kAnna, "anna"));
        m.receive(kAnna, "first contact", 1, 2.0f);
        CHECK(m.unread(kAnna) == 1);
        m.receive_channel(kBob, "hello mesh", 3, -1.0f);
    }
    CHECK(exists(file));
    {
        JsonValue doc;
        CHECK(json_parse(slurp(file), doc));
        CHECK(doc.get_int("version") == 2 && doc["conversations"].size() == 2);
    }
    // 2. next start: demo mode again (demo contacts + the saved chats), then
    //    live: the saved chats stay, the demo contacts go, the file survives
    {
        Messenger m;
        CHECK(m.demo());
        CHECK(m.has_chat(kAnna) && m.has_chat(kChan) && !m.has_chat(kTrail));
        CHECK(m.contact(kAnna) && m.contact(kAnna)->name == "anna");
        CHECK(m.contact(kTrail) != nullptr);              // demo contacts until the radio answers
        // before the radio answers: a message to a real node is kept as not sent
        CHECK(m.send(kAnna, "u there?"));
        CHECK(m.messages(kAnna).back().state == SendState::Unsent);
        // ... and a chat started with a demo node meanwhile is a demo chat
        CHECK(m.send(kTrail, "demo talk"));
        CHECK(m.save());
        m.go_live();
        CHECK(m.has_chat(kAnna) && m.messages(kAnna).size() == 2);
        CHECK(m.messages(kAnna)[0].text == "first contact" && m.unread(kAnna) == 1);
        CHECK(m.has_chat(kChan) && m.messages(kChan).size() == 1);
        CHECK(!m.has_chat(kTrail));
        CHECK(m.contact(kAnna) != nullptr && m.contact(kTrail) == nullptr);
        CHECK(m.contacts().size() == 1);
        CHECK(m.unsaved());                               // a demo chat was dropped
    }
    {
        JsonValue doc;
        CHECK(json_parse(slurp(file), doc));
        CHECK(doc["conversations"].size() == 2);          // anna + the channel, not Trailhead
    }
    // 3. the demo chat marker survives a restart (saved in demo, dropped when live)
    {
        Messenger m;
        CHECK(m.send(kKd, "still demo"));
        CHECK(m.save());
    }
    {
        Messenger m;
        CHECK(m.has_chat(kKd) && m.has_chat(kAnna));
        CHECK(!m.unsaved());
        m.go_live();
        CHECK(!m.has_chat(kKd) && m.has_chat(kAnna));
    }
    // 4. going live with only real chats does not touch the file
    {
        Messenger m;
        CHECK(!m.has_chat(kKd));
        m.go_live();
        CHECK(!m.unsaved());
        CHECK(m.has_chat(kAnna) && m.has_chat(kChan));
    }
}

static void test_channel(const std::string& dir) {
    wipe(dir);
    FakeRadio radio;
    {
        Messenger m;
        m.set_sender(radio.hook());
        m.go_live();
        m.upsert_contact(real(kAnna, "anna"));

        CHECK(Messenger::is_channel(kChan) && !Messenger::is_channel(kAnna));
        CHECK(m.name_of(kChan) == Messenger::channel_name());
        CHECK(m.contact(kChan) == nullptr);
        Contact bogus = real(kChan, "nope");
        m.upsert_contact(bogus);                          // the channel never becomes a contact
        CHECK(m.contacts().size() == 1 && m.contact(kChan) == nullptr);
        CHECK(!m.has_chat(kChan));

        m.receive_channel(kAnna, "morning all", 2, 5.5f);
        m.receive_channel(kBob, "gm", 4, -9.0f);          // not a contact (yet)
        CHECK(m.has_chat(kChan) && m.unread(kChan) == 2 && m.total_unread() == 2);
        CHECK(!m.has_chat(kAnna));                        // not in her private chat
        const auto& msgs = m.messages(kChan);
        CHECK(msgs.size() == 2);
        CHECK(msgs[0].from_node == kAnna && m.name_of(msgs[0].from_node) == "anna");
        CHECK(msgs[1].from_node == kBob && m.name_of(msgs[1].from_node) == "!33cc44dd");
        m.upsert_contact(real(kBob, "bob"));              // his name is heard later
        CHECK(m.name_of(m.messages(kChan)[1].from_node) == "bob");

        // sending there is a broadcast: "sent" at once, "delivered" on a relay
        CHECK(m.send(kChan, "hi everyone"));
        CHECK(radio.sent.size() == 1 && radio.sent[0].node == kChan);
        CHECK(m.messages(kChan)[2].state == SendState::Sent && m.messages(kChan)[2].from_node == 0);
        m.tick(0);
        m.tick(10 * Messenger::kAckTimeoutMs);
        CHECK(m.messages(kChan)[2].state == SendState::Sent);        // never times out
        CHECK(m.ack_packet(radio.sent[0].id, true));
        CHECK(m.messages(kChan)[2].state == SendState::Delivered);
        radio.ready = false;
        CHECK(m.send(kChan, "second"));
        CHECK(m.messages(kChan)[3].state == SendState::Unsent && m.retry_index(kChan) == 3);
        m.mark_read(kChan);
        CHECK(m.total_unread() == 0);
    }
    {
        Messenger m;                                      // from the file
        CHECK(m.has_chat(kChan));
        const auto& msgs = m.messages(kChan);
        CHECK(msgs.size() == 4);
        if (msgs.size() == 4) {
            CHECK(msgs[0].from_node == kAnna && msgs[1].from_node == kBob && !msgs[0].from_me);
            CHECK(msgs[2].from_me && msgs[2].state == SendState::Delivered);
            CHECK(msgs[3].state == SendState::Unsent && msgs[3].error == kSendErrNoRadio);
        }
        CHECK(m.contact(kChan) == nullptr);               // no contact made up for it
        m.go_live();
        CHECK(m.has_chat(kChan));
        m.delete_chat(kChan);
        CHECK(!m.has_chat(kChan));
    }
}

static void test_persist_states(const std::string& dir) {
    wipe(dir);
    const std::string file = dir + "/conversations.json";
    FakeRadio radio;
    {
        Messenger m;
        m.set_sender(radio.hook());
        m.go_live();
        m.upsert_contact(real(kAnna, "anna"));
        m.send(kAnna, "delivered one");
        m.ack_packet(radio.sent[0].id, true);
        m.send(kAnna, "failed one");
        m.ack_packet(radio.sent[1].id, false, 5);
        radio.ready = false;
        m.send(kAnna, "unsent one");
        radio.ready = true;
        m.send(kAnna, "still sending");
        m.receive(kAnna, "reply", 2, 5.5f);
    }
    const std::string text = slurp(file);
    CHECK(text.find("\"state\": \"failed\", \"err\": 5") != std::string::npos);
    CHECK(text.find("\"state\": \"unsent\"") != std::string::npos);
    CHECK(text.find("\"state\": \"sending\"") != std::string::npos);
    CHECK(text.find("\"delivered\": true") != std::string::npos);      // still there for old builds
    {
        Messenger m;
        const auto& msgs = m.messages(kAnna);
        CHECK(msgs.size() == 5);
        if (msgs.size() == 5) {
            CHECK(msgs[0].state == SendState::Delivered && msgs[0].delivered && msgs[0].hops == 2);
            CHECK(msgs[1].state == SendState::Failed && msgs[1].error == 5 && !msgs[1].delivered);
            CHECK(msgs[2].state == SendState::Unsent && msgs[2].error == kSendErrNoRadio);
            CHECK(msgs[3].state == SendState::Sent && msgs[3].packet_id == 0);   // unknown after a restart
            CHECK(!msgs[4].from_me && msgs[4].state == SendState::Delivered && msgs[4].delivered);
        }
        CHECK(m.retry_index(kAnna) == 2);
        // nothing waits for an ack after a restart: no timeout fires
        m.tick(0);
        m.tick(3 * Messenger::kAckTimeoutMs);
        CHECK(m.messages(kAnna)[3].state == SendState::Sent);
        CHECK(!m.unsaved());
    }
}

// A file written before any of this existed (version 1, no state, no demo flag).
static void test_old_file(const std::string& dir) {
    wipe(dir);
    const std::string file = dir + "/conversations.json";
    spit(file,
         "{\n  \"version\": 1,\n  \"conversations\": [\n"
         "    {\"peer\": \"!5be0a903\", \"name\": \"Trailhead-R\", \"unread\": 1, \"messages\": [\n"
         "      {\"me\": true, \"t\": 1700000000, \"hops\": 1, \"snr\": 9.00, \"delivered\": true, \"text\": \"is the lot full?\"},\n"
         "      {\"me\": false, \"t\": 1700000100, \"hops\": 1, \"snr\": 9.00, \"delivered\": true, \"text\": \"about half\"}\n"
         "    ]},\n"
         "    {\"peer\": \"!3202a9f7\", \"name\": \"W2XYZ\", \"unread\": 0, \"messages\": [\n"
         "      {\"me\": true, \"t\": 1700000200, \"hops\": 0, \"snr\": 0.00, \"delivered\": false, \"text\": \"u there?\"}\n"
         "    ]},\n"
         "    {\"peer\": \"!00c0ffee\", \"name\": \"stranger_42\", \"unread\": 2, \"messages\": [\n"
         "      {\"me\": false, \"t\": 1700000300, \"hops\": 3, \"snr\": -4.25, \"delivered\": true, \"text\": \"who dis\"},\n"
         "      {\"me\": false, \"t\": 1700000301, \"hops\": 3, \"snr\": -4.25, \"delivered\": true, \"auto\": true, \"text\": \"auto\"}\n"
         "    ]}\n  ]\n}\n");
    {
        Messenger m;
        CHECK(m.has_chat(kTrail) && m.has_chat(0x3202a9f7) && m.has_chat(0x00c0ffee));
        const auto& tr = m.messages(kTrail);
        CHECK(tr.size() == 2 && m.unread(kTrail) == 1);
        if (tr.size() == 2) {
            CHECK(tr[0].from_me && tr[0].delivered && tr[0].state == SendState::Delivered);
            CHECK(tr[0].text == "is the lot full?" && tr[0].timestamp == 1700000000 && tr[0].snr == 9.0f);
            CHECK(!tr[1].from_me && tr[1].state == SendState::Delivered && tr[1].from_node == 0);
        }
        const auto& w = m.messages(0x3202a9f7);
        CHECK(w.size() == 1 && !w[0].delivered && w[0].state == SendState::Sent && !w[0].retryable());
        const auto& st = m.messages(0x00c0ffee);
        CHECK(st.size() == 2 && st[1].auto_response && m.unread(0x00c0ffee) == 2);
        CHECK(m.name_of(0x00c0ffee) == "stranger_42");
        CHECK(!m.unsaved());                              // loading rewrites nothing
        CHECK(slurp(file).find("\"version\": 1") != std::string::npos);

        // version 1 was demo-only: its chats with the built-in demo nodes go
        // when the radio answers, any other peer stays
        m.go_live();
        CHECK(!m.has_chat(kTrail) && !m.has_chat(0x3202a9f7));
        CHECK(m.has_chat(0x00c0ffee) && m.contact(0x00c0ffee) != nullptr);
        CHECK(m.contacts().size() == 1);
    }
    {
        JsonValue doc;
        CHECK(json_parse(slurp(file), doc));
        CHECK(doc.get_int("version") == 2 && doc["conversations"].size() == 1);
    }
}

static void test_files_disabled(const std::string& dir) {
    wipe(dir);
    store::set_enabled(false);
    {
        Messenger m;
        Message a;
        a.from_me = true;
        a.text = "x";
        a.state = SendState::Failed;
        a.error = 1;
        m.add_message(0x5011a4b2, a);                     // what the --shots states do
        CHECK(m.messages(0x5011a4b2).size() == 1 && m.retry_index(0x5011a4b2) == 0);
        CHECK(m.total_unread() == 1);                     // only the seeded demo one
        m.receive_channel(0x41110001, "y", 1, 0.0f);
        m.delete_chat(Messenger::kChannelNode);
        m.delete_chat(0x5011a4b2);
        CHECK(m.total_unread() == 1);
    }
    CHECK(!exists(dir + "/conversations.json"));
    store::set_enabled(true);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <scratch_dir>\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    ::mkdir(dir.c_str(), 0700);
    ::setenv("PC1999_CONFIG_DIR", dir.c_str(), 1);
    if (store::dir() != dir) {
        std::printf("config dir is not the scratch dir, stopping\n");
        return 2;
    }
    test_send_states(dir);
    test_demo_unchanged(dir);
    test_go_live(dir);
    test_channel(dir);
    test_persist_states(dir);
    test_old_file(dir);
    test_files_disabled(dir);
    wipe(dir);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
