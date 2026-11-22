// mesh_link_test.cpp — the Meshtastic client against a simulated node.
//
// The node is tools/fake_meshtasticd.py, which uses the official `meshtastic`
// Python package's generated protobuf classes: an independent codec, so a
// wrong field number or wire type in mesh_link.cpp shows up here. Nothing
// leaves 127.0.0.1 and no radio is involved.
//
// Build (from the project root):
//   clang++ -std=c++17 -Wall -Wextra -g -I src/backend \
//     tests/backend/mesh_link_test.cpp src/backend/mesh_link.cpp src/backend/mm_json.cpp \
//     -o <out>/mesh_link_test            (add -fsanitize=address,undefined for the sanitizer run)
//
// Usage (from the project root): mesh_link_test <scratch_dir> [first_port]
//   MESH_PY    python with `meshtastic` installed (default ~/meshtastic-venv/bin/python3)
//   MESH_FAKE  path to fake_meshtasticd.py        (default tools/fake_meshtasticd.py)
// Ports first_port .. first_port+9 are used (default 44100).
#include "mesh_link.h"
#include "mm_json.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

using namespace mm;
using State = MeshLink::State;

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

static const uint32_t kMe = 0x0a0b0c0d, kAlice = 0x11111111, kBob = 0x22222222, kZoe = 0x33333333,
                      kNoName = 0x44444444, kEvil = 0x55555555;
static const uint32_t kT0 = 1795000000;

// ── clock: real monotonic time plus a skew the tests can jump forward ────────
static uint64_t g_skew = 0;
static uint64_t real_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
static uint64_t now_ms() { return real_ms() + g_skew; }

static std::function<void()> g_idle;     // extra work per pump round (the pty bridge)

static bool pump(MeshLink& link, const std::function<bool()>& done, int timeout_ms = 5000) {
    uint64_t end = real_ms() + (uint64_t)timeout_ms;
    for (;;) {
        if (g_idle) g_idle();
        link.poll(now_ms());
        if (done()) return true;
        if (real_ms() >= end) return false;
        usleep(2000);
    }
}
static void pump_for(MeshLink& link, int ms) {
    pump(link, [] { return false; }, ms);
}

static std::string slurp(const std::string& p) {
    std::string out;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}
static std::string hex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string o;
    for (unsigned char c : s) {
        o.push_back(d[c >> 4]);
        o.push_back(d[c & 15]);
    }
    return o;
}

// ── the fake node process ────────────────────────────────────────────────────
static std::vector<pid_t> g_children;
static void kill_children() {
    for (pid_t p : g_children) kill(p, SIGKILL);
}

struct Fake {
    pid_t pid = -1;
    int in_fd = -1;
    int port = 0;
    std::string log_path;

    bool start(int port_, const std::string& log, const std::vector<std::string>& extra = {}) {
        port = port_;
        log_path = log;
        ::unlink(log.c_str());
        std::string py = std::getenv("MESH_PY") ? std::getenv("MESH_PY")
                                                 : std::string(std::getenv("HOME") ? std::getenv("HOME") : "") +
                                                       "/meshtastic-venv/bin/python3";
        std::string script = std::getenv("MESH_FAKE") ? std::getenv("MESH_FAKE") : "tools/fake_meshtasticd.py";
        int to_child[2], from_child[2];
        if (pipe(to_child) != 0 || pipe(from_child) != 0) return false;
        std::string port_s = std::to_string(port);
        pid = fork();
        if (pid == 0) {
            dup2(to_child[0], 0);
            dup2(from_child[1], 1);
            ::close(to_child[0]); ::close(to_child[1]);
            ::close(from_child[0]); ::close(from_child[1]);
            std::vector<const char*> argv = {py.c_str(), script.c_str(), "--port", port_s.c_str(),
                                             "--log", log.c_str()};
            for (const std::string& e : extra) argv.push_back(e.c_str());
            argv.push_back(nullptr);
            execv(py.c_str(), (char* const*)argv.data());
            _exit(127);
        }
        ::close(to_child[0]);
        ::close(from_child[1]);
        in_fd = to_child[1];
        fcntl(in_fd, F_SETFD, FD_CLOEXEC);
        g_children.push_back(pid);
        // Wait for "LISTENING".
        std::string out;
        uint64_t end = real_ms() + 15000;
        bool ok = false;
        while (real_ms() < end) {
            struct pollfd pfd = {from_child[0], POLLIN, 0};
            if (::poll(&pfd, 1, 200) <= 0) continue;
            char buf[256];
            ssize_t n = ::read(from_child[0], buf, sizeof buf);
            if (n <= 0) break;
            out.append(buf, (size_t)n);
            if (out.find("LISTENING") != std::string::npos && out.find('\n') != std::string::npos) {
                ok = true;
                break;
            }
            if (out.find("ERROR") != std::string::npos) break;
        }
        ::close(from_child[0]);
        if (!ok) {
            std::printf("fake node on port %d did not start: %s\n", port, out.c_str());
            stop();
        }
        return ok;
    }
    void cmd(const std::string& json) {
        std::string line = json + "\n";
        ssize_t n = ::write(in_fd, line.data(), line.size());
        (void)n;
    }
    void stop() {
        if (pid <= 0) return;
        if (in_fd >= 0) {
            cmd("{\"cmd\":\"quit\"}");
            ::close(in_fd);
            in_fd = -1;
        }
        for (int i = 0; i < 100; ++i) {
            int st;
            if (waitpid(pid, &st, WNOHANG) == pid) { pid = -1; return; }
            usleep(20000);
        }
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        pid = -1;
    }
    std::vector<JsonValue> log() const {
        std::vector<JsonValue> out;
        std::string all = slurp(log_path);
        size_t pos = 0;
        while (pos < all.size()) {
            size_t nl = all.find('\n', pos);
            if (nl == std::string::npos) break;          // a line still being written
            JsonValue v;
            if (json_parse(all.data() + pos, nl - pos, v)) out.push_back(v);
            pos = nl + 1;
        }
        return out;
    }
    int count(const std::string& variant) const {
        int n = 0;
        for (const JsonValue& v : log())
            if (v.get_string("ev") == "toradio" && v.get_string("variant") == variant) ++n;
        return n;
    }
    // The ToRadio packet with this id, as the real protobuf classes decoded it.
    bool packet(uint32_t id, JsonValue& out) const {
        for (const JsonValue& v : log())
            if (v.get_string("ev") == "toradio" && v.get_string("variant") == "packet" && v.get_u32("id") == id) {
                out = v;
                return true;
            }
        return false;
    }
    void text(uint32_t from, uint32_t to, const std::string& t, int channel = 0, uint32_t id = 0,
              const char* verb = "text") {
        char buf[160];
        std::snprintf(buf, sizeof buf, "{\"cmd\":\"%s\",\"from\":%u,\"to\":%u,\"channel\":%d,", verb, from, to, channel);
        std::string c = buf;
        if (id) c += "\"id\":" + std::to_string(id) + ",";
        c += "\"text\":\"" + json_escape(t) + "\"}";
        cmd(c);
    }
};

struct Recorder {
    std::vector<State> states;
    std::vector<MeshText> texts;
    struct Ack { uint32_t id; bool ok; int error; };
    std::vector<Ack> acks;

    void attach(MeshLink& link) {
        link.set_state_callback([this](State s) { states.push_back(s); });
        link.set_text_callback([this](const MeshText& t) { texts.push_back(t); });
        link.set_ack_callback([this](uint32_t id, bool ok, int err) { acks.push_back({id, ok, err}); });
    }
    const Ack* ack(uint32_t id) const {
        for (const Ack& a : acks)
            if (a.id == id) return &a;
        return nullptr;
    }
    int ack_count(uint32_t id) const {
        int n = 0;
        for (const Ack& a : acks) n += a.id == id;
        return n;
    }
    const MeshText* text(const std::string& t) const {
        for (const MeshText& m : texts)
            if (m.text == t) return &m;
        return nullptr;
    }
};

// test-side protobuf bits for hand-made (mostly broken) frames
static void pb_varint(std::string& o, uint64_t v) {
    while (v >= 0x80) { o.push_back((char)(v | 0x80)); v >>= 7; }
    o.push_back((char)v);
}
static void pb_len(std::string& o, uint32_t field, const std::string& b) {
    pb_varint(o, (field << 3) | 2);
    pb_varint(o, b.size());
    o += b;
}
static void pb_uint(std::string& o, uint32_t field, uint64_t v) {
    pb_varint(o, (field << 3) | 0);
    pb_varint(o, v);
}

static const char* kFffd = "\xEF\xBF\xBD";

// ─────────────────────────────────────────────────────────────────────────────
static void test_statics() {
    CHECK(MeshLink::node_id_text(0xa1b2c3d4) == "!a1b2c3d4");
    CHECK(MeshLink::node_id_text(0x1f) == "!0000001f");
    CHECK(MeshLink::kBroadcast == 0xffffffffu);

    MeshLink link;
    CHECK(link.state() == State::Off);
    CHECK(!link.ready());
    CHECK(link.my_node() == 0);
    CHECK(link.nodes().empty());
    CHECK(link.send_text(kAlice, "hi") == 0);            // not ready
    link.poll(now_ms());                                 // Off: nothing happens
    CHECK(link.state() == State::Off);

    const char* bad[] = {"", "   ", "tcp:", "serial:", "tcp:host:", "tcp:host:0", "tcp:host:70000",
                         "tcp:host:12x", "host name", "tcp:[::1", "tcp:[::1]4403", "ho$t", "tcp:[::1]:"};
    for (const char* s : bad) {
        CHECK(!link.open(s));
        CHECK(link.state() == State::Off);
    }

    struct { const char* spec; const char* target; } good[] = {
        {"tcp:localhost", "tcp:localhost:4403"},
        {"tcp:localhost:44199", "tcp:localhost:44199"},
        {"localhost", "tcp:localhost:4403"},
        {"127.0.0.1:44199", "tcp:127.0.0.1:44199"},
        {"tcp:[::1]:44199", "tcp:[::1]:44199"},
        {"::1", "tcp:[::1]:4403"},
        {"serial:/dev/ttyACM0", "serial:/dev/ttyACM0"},
        {"/dev/ttyUSB0", "serial:/dev/ttyUSB0"},
        {"  tcp:localhost:44199\n", "tcp:localhost:44199"},
    };
    for (auto& g : good) {
        MeshLink l;
        CHECK(l.open(g.spec));
        CHECK(l.target() == g.target);
        CHECK(l.state() == State::Connecting);           // no poll(): nothing was opened
        l.close();
        CHECK(l.state() == State::Off);
    }
    MeshText t;
    t.to = MeshLink::kBroadcast;
    CHECK(!t.direct());
    t.to = kMe;
    CHECK(t.direct());
    CHECK(&mesh_link() == &mesh_link());
}

static void test_main(const std::string& scratch, int port) {
    Fake fake;
    if (!fake.start(port, scratch + "/fake_main.jsonl", {"--lora-hop-limit", "5"})) {
        CHECK(!"fake node started");
        return;
    }
    MeshLink link;
    Recorder rec;
    rec.attach(link);

    // ── handshake ────────────────────────────────────────────────────────────
    CHECK(link.open("tcp:127.0.0.1:" + std::to_string(port)));
    CHECK(link.state() == State::Connecting);
    CHECK(rec.states.empty());                           // callbacks only from poll()
    CHECK(pump(link, [&] { return link.ready(); }));
    CHECK(rec.states.size() == 3);
    if (rec.states.size() == 3) {
        CHECK(rec.states[0] == State::Connecting);
        CHECK(rec.states[1] == State::Syncing);
        CHECK(rec.states[2] == State::Ready);
    }
    CHECK(link.target() == "tcp:127.0.0.1:" + std::to_string(port));
    CHECK(link.last_error().empty());
    CHECK(link.my_node() == kMe);

    {
        bool got = false;
        for (const JsonValue& v : fake.log())
            if (v.get_string("variant") == "want_config_id") { got = true; CHECK(v.get_u32("id") != 0); }
        CHECK(got);
    }

    // ── node list ────────────────────────────────────────────────────────────
    CHECK(link.nodes().size() == 6);
    if (link.nodes().size() == 6) {
        const auto& n = link.nodes();
        CHECK(n[0].num == kMe && n[0].is_me && n[0].long_name == "Pocket 1999" && n[0].short_name == "PK99");
        CHECK(n[0].battery == 101 && n[0].hops_away == -1 && n[0].last_heard == kT0);
        CHECK(n[1].num == kAlice && !n[1].is_me && n[1].long_name == "Alice Base" && n[1].short_name == "ALC");
        CHECK(n[1].snr == 7.25f && n[1].hops_away == 0 && n[1].battery == 88 && n[1].last_heard == kT0 - 60);
        CHECK(n[2].num == kBob && n[2].snr == -3.5f && n[2].hops_away == 2 && n[2].battery == 15);
        CHECK(n[2].last_heard == kT0 - 3600);
        CHECK(n[3].num == kZoe && n[3].long_name == "Zo\xC3\xAB \xF0\x9F\xA6\x8A Fuchs");
        CHECK(n[3].short_name == "\xF0\x9F\xA6\x8A" && n[3].hops_away == 1 && n[3].battery == 42);
        CHECK(n[4].num == kNoName && n[4].long_name.empty() && n[4].short_name.empty());
        CHECK(n[4].snr == -12.75f && n[4].hops_away == -1 && n[4].battery == -1 && n[4].last_heard == 0);
        // control characters removed, cut to 64 bytes
        CHECK(n[5].num == kEvil && n[5].long_name == "EvilName " + std::string(55, 'x'));
        CHECK(n[5].battery == 0 && n[5].hops_away == 3 && n[5].snr == 0.5f);
        int me = 0;
        for (const MeshNode& x : n) me += x.is_me;
        CHECK(me == 1);
    }
    CHECK(link.node(kAlice) && link.node(kAlice)->long_name == "Alice Base");
    CHECK(link.node(0x99999999) == nullptr);

    // heartbeat right after the handshake, like the Python client
    CHECK(pump(link, [&] { return fake.count("heartbeat") == 1; }));

    // ── direct text: exact fields, ack, reply ────────────────────────────────
    const std::string dm = "Gr\xC3\xBC\xC3\x9F" "e \xF0\x9F\x91\x8B \"quoted\" \\ end";
    uint32_t id1 = link.send_text(kAlice, dm);
    CHECK(id1 != 0);
    CHECK(pump(link, [&] { return rec.ack(id1) && rec.text("re: " + dm); }));
    {
        JsonValue p;
        CHECK(fake.packet(id1, p));
        CHECK(p.get_u32("to") == kAlice);
        CHECK(p.get_u32("from") == 0);
        CHECK(p.get_int("channel") == 0);
        CHECK(p.get_bool("want_ack"));
        CHECK(p.get_int("hop_limit") == 5);              // the radio's LoRa config value
        CHECK(p.get_int("hop_start") == 0);
        CHECK(p.get_int("priority") == 70);              // RELIABLE
        CHECK(p.get_int("portnum") == 1);
        CHECK(p.get_string("payload_variant") == "decoded");
        CHECK(p.get_string("payload_hex") == hex(dm));
        CHECK(p.get_string("text") == dm);
        CHECK(!p.get_bool("want_response"));
    }
    if (const Recorder::Ack* a = rec.ack(id1)) CHECK(a->ok && a->error == 0);
    CHECK(rec.ack_count(id1) == 1);
    if (const MeshText* t = rec.text("re: " + dm)) {
        CHECK(t->from == kAlice && t->to == kMe && t->direct() && t->channel == 0);
        CHECK(t->hops == 1 && t->snr == 6.5f && t->rx_time == kT0 + 20 && t->id != 0);
    }
    // hearing Alice refreshed her entry
    CHECK(link.node(kAlice)->last_heard == kT0 + 20 && link.node(kAlice)->snr == 6.5f);
    CHECK(link.node(kAlice)->hops_away == 1);

    // ── broadcast on a channel: implicit ack ─────────────────────────────────
    uint32_t id2 = link.send_text(MeshLink::kBroadcast, "hello channel", 2);
    CHECK(id2 != 0 && id2 != id1);
    CHECK(pump(link, [&] { return rec.ack(id2) != nullptr; }));
    {
        JsonValue p;
        CHECK(fake.packet(id2, p));
        CHECK(p.get_u32("to") == 0xffffffffu && p.get_int("channel") == 2 && p.get_bool("want_ack"));
        CHECK(p.get_string("text") == "hello channel");
    }
    if (const Recorder::Ack* a = rec.ack(id2)) CHECK(a->ok && a->error == 0);

    // ── refused texts ────────────────────────────────────────────────────────
    CHECK(link.send_text(kAlice, "") == 0);
    CHECK(link.send_text(kAlice, std::string(MeshLink::kMaxTextBytes + 1, 'a')) == 0);
    CHECK(link.send_text(kAlice, "x", -1) == 0);
    CHECK(link.send_text(kAlice, "x", 8) == 0);
    CHECK(link.send_text(0, "x") == 0);
    {
        int before = fake.count("packet");
        const std::string big(MeshLink::kMaxTextBytes, 'z');
        uint32_t idb = link.send_text(kAlice, big, 7);
        CHECK(idb != 0);
        CHECK(pump(link, [&] { return rec.ack(idb) && rec.text("re: " + big); }));
        JsonValue p;
        CHECK(fake.packet(idb, p) && p.get_string("text") == big && p.get_int("channel") == 7);
        CHECK(fake.count("packet") == before + 1);       // the refused ones never left
    }

    // ── received: broadcast, unknown node, encrypted, live nodeinfo/telemetry ─
    size_t texts_before = rec.texts.size();
    fake.text(kBob, MeshLink::kBroadcast, "to everyone \xF0\x9F\x93\xA1", 1);
    CHECK(pump(link, [&] { return rec.text("to everyone \xF0\x9F\x93\xA1") != nullptr; }));
    if (const MeshText* t = rec.text("to everyone \xF0\x9F\x93\xA1")) {
        CHECK(t->from == kBob && t->to == MeshLink::kBroadcast && !t->direct() && t->channel == 1);
        CHECK(t->hops == 0 && t->snr == 5.5f && t->rx_time == kT0 + 10);
    }
    CHECK(rec.texts.size() == texts_before + 1);

    uint32_t rev = link.nodes_revision();
    fake.text(0x77777777, kMe, "who am i");
    CHECK(pump(link, [&] { return rec.text("who am i") != nullptr; }));
    CHECK(link.nodes().size() == 7 && link.node(0x77777777) != nullptr);
    if (const MeshNode* n = link.node(0x77777777)) CHECK(n->long_name.empty() && n->last_heard == kT0 + 10 && !n->is_me);
    CHECK(link.nodes_revision() != rev);

    rev = link.nodes_revision();
    texts_before = rec.texts.size();
    fake.cmd("{\"cmd\":\"encrypted\",\"from\":" + std::to_string(kBob) + "}");
    CHECK(pump(link, [&] { return link.node(kBob)->last_heard == kT0 + 30; }));
    CHECK(link.node(kBob)->snr == -7.75f && link.node(kBob)->hops_away == 0);
    CHECK(link.nodes_revision() != rev);
    fake.cmd("{\"cmd\":\"encrypted\",\"from\":2290649224}");             // 0x88888888, never seen
    CHECK(pump(link, [&] { return link.node(0x88888888) != nullptr; }));
    CHECK(rec.texts.size() == texts_before);             // nothing readable arrived

    fake.cmd("{\"cmd\":\"nodeinfo\",\"from\":2290649224,\"long\":\"Late Namer\",\"short\":\"LN\"}");
    CHECK(pump(link, [&] { return link.node(0x88888888)->long_name == "Late Namer"; }));
    CHECK(link.node(0x88888888)->short_name == "LN");
    fake.cmd("{\"cmd\":\"telemetry\",\"from\":2290649224,\"battery\":63}");
    CHECK(pump(link, [&] { return link.node(0x88888888)->battery == 63; }));
    fake.cmd("{\"cmd\":\"telemetry\",\"from\":" + std::to_string(kMe) + ",\"battery\":77}");
    CHECK(pump(link, [&] { return link.node(kMe)->battery == 77; }));

    // a text for somebody else and a repeated packet are not delivered
    texts_before = rec.texts.size();
    fake.text(kBob, kZoe, "not for me");
    fake.text(kBob, kMe, "once", 0, 0x5000);
    fake.text(kBob, kMe, "once", 0, 0x5000);
    fake.text(kMe, MeshLink::kBroadcast, "my own words");
    fake.text(kBob, kMe, "marker-1");
    CHECK(pump(link, [&] { return rec.text("marker-1") != nullptr; }));
    CHECK(rec.texts.size() == texts_before + 2);
    CHECK(!rec.text("not for me") && !rec.text("my own words"));

    // ── NAK ──────────────────────────────────────────────────────────────────
    fake.cmd("{\"cmd\":\"mode\",\"ack\":\"nak\"}");
    uint32_t id3 = link.send_text(kBob, "will be refused");
    CHECK(id3 != 0);
    CHECK(pump(link, [&] { return rec.ack(id3) != nullptr; }));
    if (const Recorder::Ack* a = rec.ack(id3)) CHECK(!a->ok && a->error == 8);       // NO_RESPONSE

    // the radio refusing to queue the packet at all (QueueStatus.res)
    fake.cmd("{\"cmd\":\"mode\",\"ack\":\"queue_fail\"}");
    uint32_t id3b = link.send_text(kBob, "no channel");
    CHECK(pump(link, [&] { return rec.ack(id3b) != nullptr; }));
    if (const Recorder::Ack* a = rec.ack(id3b)) CHECK(!a->ok && a->error == 6);      // NO_CHANNEL

    // ── implicit ack for a direct message is not delivery; then timeout ──────
    fake.cmd("{\"cmd\":\"mode\",\"ack\":\"implicit\"}");
    uint32_t id4 = link.send_text(kBob, "anyone there?");
    CHECK(id4 != 0);
    { JsonValue p; CHECK(pump(link, [&] { return fake.packet(id4, p); })); }
    pump_for(link, 300);
    CHECK(rec.ack(id4) == nullptr);
    // an ack for an id we never sent is ignored
    fake.cmd("{\"cmd\":\"ack\",\"request_id\":123456,\"from\":" + std::to_string(kBob) + "}");
    g_skew += 60000;
    pump_for(link, 100);
    CHECK(rec.ack(id4) == nullptr);
    g_skew += 31000;                                     // 91 s after sending
    CHECK(pump(link, [&] { return rec.ack(id4) != nullptr; }));
    if (const Recorder::Ack* a = rec.ack(id4)) CHECK(!a->ok && a->error == 3);       // TIMEOUT
    // the real ack turning up late changes nothing
    fake.cmd("{\"cmd\":\"ack\",\"request_id\":" + std::to_string(id4) + ",\"from\":" + std::to_string(kBob) + "}");
    pump_for(link, 150);
    CHECK(rec.ack_count(id4) == 1);

    // implicit first, then the destination's ack: reported once, ok
    uint32_t id5 = link.send_text(kBob, "second try");
    { JsonValue p; CHECK(pump(link, [&] { return fake.packet(id5, p); })); }
    pump_for(link, 150);
    CHECK(rec.ack(id5) == nullptr);
    fake.cmd("{\"cmd\":\"ack\",\"request_id\":" + std::to_string(id5) + ",\"from\":" + std::to_string(kBob) + "}");
    CHECK(pump(link, [&] { return rec.ack(id5) != nullptr; }));
    if (const Recorder::Ack* a = rec.ack(id5)) CHECK(a->ok && a->error == 0);
    CHECK(rec.ack_count(id5) == 1);

    // ── our own packet echoed by the radio is not an incoming text ───────────
    fake.cmd("{\"cmd\":\"mode\",\"ack\":\"normal\",\"reply\":false,\"echo\":true}");
    texts_before = rec.texts.size();
    uint32_t id6 = link.send_text(MeshLink::kBroadcast, "echo echo");
    CHECK(pump(link, [&] { return rec.ack(id6) != nullptr; }));
    pump_for(link, 100);
    CHECK(rec.texts.size() == texts_before && !rec.text("echo echo"));
    fake.cmd("{\"cmd\":\"mode\",\"echo\":false,\"reply\":true}");

    // ── garbage, debug text, false headers, split frames ─────────────────────
    fake.cmd("{\"cmd\":\"garbage\"}");
    fake.text(kAlice, kMe, "after garbage");
    CHECK(pump(link, [&] { return rec.text("after garbage") != nullptr; }));
    fake.text(kAlice, kMe, "in three pieces", 0, 0, "split");
    CHECK(pump(link, [&] { return rec.text("in three pieces") != nullptr; }));
    CHECK(link.ready());

    // hand-made broken frames
    {
        size_t nodes_before = link.nodes().size();
        // top level: a length that runs off the end
        fake.cmd("{\"cmd\":\"frame\",\"hex\":\"12200d01\"}");
        // packet whose `decoded` claims 127 bytes
        fake.cmd("{\"cmd\":\"frame\",\"hex\":\"1204227f0801\"}");
        // an endless varint, a group wire type, field number 0, an empty frame
        fake.cmd("{\"cmd\":\"frame\",\"hex\":\"08ffffffffffffffffffffff01\"}");
        fake.cmd("{\"cmd\":\"frame\",\"hex\":\"130814\"}");
        fake.cmd("{\"cmd\":\"frame\",\"hex\":\"0001\"}");
        fake.cmd("{\"cmd\":\"frame\",\"hex\":\"\"}");
        // node_info with a truncated user, and a my_info with node number 0
        fake.cmd("{\"cmd\":\"frame\",\"hex\":\"220908e707120412ff4142\"}");
        fake.cmd("{\"cmd\":\"frame\",\"hex\":\"1a020800\"}");
        // a packet with fixed32 fields cut short
        fake.cmd("{\"cmd\":\"frame\",\"hex\":\"12030d0102\"}");
        // routing ack with a broken Routing payload, text with the wrong wire type
        fake.cmd("{\"cmd\":\"frame\",\"hex\":\"120c0d111111112205080512011a\"}");
        fake.cmd("{\"cmd\":\"frame\",\"hex\":\"120b0d11111111220408011005\"}");

        // names that are not UTF-8
        std::string user, ni, fr;
        pb_len(user, 2, std::string("\xff\xfe" "A" "\xc3(" "B" "\xe2\x82", 8));
        pb_len(user, 3, "\xf0\x9f");
        pb_uint(ni, 1, 0x66666666);
        pb_len(ni, 2, user);
        pb_len(fr, 4, ni);
        fake.cmd("{\"cmd\":\"frame\",\"hex\":\"" + hex(fr) + "\"}");
        // an overlong "/" and a UTF-16 surrogate, inside a text
        std::string data, pkt, fr2;
        pb_uint(data, 1, 1);
        pb_len(data, 2, std::string("a\xc0\xaf" "b\xed\xa0\x80" "c\x01\x7f" "d\n\te", 14));
        pkt += "\x0d";
        pkt += std::string("\x22\x22\x22\x22", 4);                        // from = Bob
        pkt += "\x15";
        pkt += std::string("\x0d\x0c\x0b\x0a", 4);                        // to = me
        pb_len(pkt, 4, data);
        pb_len(fr2, 2, pkt);
        fake.cmd("{\"cmd\":\"frame\",\"hex\":\"" + hex(fr2) + "\"}");

        fake.text(kAlice, kMe, "still alive");
        CHECK(pump(link, [&] { return rec.text("still alive") != nullptr; }));
        CHECK(link.ready() && link.my_node() == kMe);
        CHECK(link.nodes().size() == nodes_before + 1);
        const MeshNode* odd = link.node(0x66666666);
        CHECK(odd != nullptr);
        if (odd) {
            std::string f = kFffd;
            CHECK(odd->long_name == f + f + "A" + f + "(B" + f + f);
            CHECK(odd->short_name == f + f);
        }
        std::string f = kFffd;
        const MeshText* t = rec.text("a" + f + f + "b" + f + f + f + "cd\n\te");
        CHECK(t != nullptr && t->from == kBob && t->hops == -1 && t->rx_time == 0);
    }

    // ── a callback may use the link ──────────────────────────────────────────
    {
        uint32_t pong_id = 0;
        link.set_text_callback([&](const MeshText& t) {
            rec.texts.push_back(t);
            if (t.text == "ping") pong_id = link.send_text(t.from, "pong");
        });
        fake.text(kZoe, kMe, "ping");
        CHECK(pump(link, [&] { return pong_id != 0 && rec.ack(pong_id) && rec.text("re: pong"); }));
        JsonValue p;
        CHECK(fake.packet(pong_id, p) && p.get_u32("to") == kZoe && p.get_string("text") == "pong");
        rec.attach(link);
    }

    // ── heartbeat after 300 s of sending nothing ─────────────────────────────
    pump_for(link, 50);
    CHECK(fake.count("heartbeat") == 1);
    g_skew += 299000;
    pump_for(link, 100);
    CHECK(fake.count("heartbeat") == 1);
    g_skew += 2000;
    CHECK(pump(link, [&] { return fake.count("heartbeat") == 2; }));
    pump_for(link, 150);
    CHECK(fake.count("heartbeat") == 2);
    {
        bool empty_hb = false;
        for (const JsonValue& v : fake.log())
            if (v.get_string("variant") == "heartbeat") empty_hb = v.get_string("raw_hex") == "3a00";
        CHECK(empty_hb);
    }

    // ── the radio reboots: sync again ────────────────────────────────────────
    rec.states.clear();
    fake.cmd("{\"cmd\":\"rebooted\"}");
    CHECK(pump(link, [&] { return rec.states.size() == 2; }));
    if (rec.states.size() == 2) CHECK(rec.states[0] == State::Syncing && rec.states[1] == State::Ready);
    CHECK(fake.count("want_config_id") == 2);

    // ── the connection drops: reconnect after the 1 s backoff ────────────────
    rec.states.clear();
    size_t nodes_kept = link.nodes().size();
    uint64_t t_drop = real_ms();
    fake.cmd("{\"cmd\":\"drop\"}");
    CHECK(pump(link, [&] { return link.state() == State::Connecting; }));
    CHECK(!link.last_error().empty());
    CHECK(link.send_text(kAlice, "into the void") == 0);
    CHECK(link.nodes().size() == nodes_kept && link.my_node() == kMe);   // the list survives
    CHECK(pump(link, [&] { return link.ready(); }, 8000));
    uint64_t took = real_ms() - t_drop;
    CHECK(took >= 900 && took < 3000);
    CHECK(rec.states.size() == 3);
    if (rec.states.size() == 3)
        CHECK(rec.states[0] == State::Connecting && rec.states[1] == State::Syncing && rec.states[2] == State::Ready);
    CHECK(link.nodes().size() == nodes_kept);
    CHECK(link.node(kAlice)->last_heard == kT0 - 60);    // refreshed from the radio's database
    uint32_t id7 = link.send_text(kAlice, "back again");
    CHECK(id7 != 0);
    CHECK(pump(link, [&] { return rec.ack(id7) && rec.text("re: back again"); }));
    if (const Recorder::Ack* a = rec.ack(id7)) CHECK(a->ok);

    // ── bounded queue ────────────────────────────────────────────────────────
    {
        fake.cmd("{\"cmd\":\"mode\",\"reply\":false}");
        pump_for(link, 50);
        std::vector<uint32_t> ids;
        int refused = 0;
        for (int i = 0; i < 40; ++i) {
            uint32_t id = link.send_text(MeshLink::kBroadcast, "burst " + std::to_string(i));
            if (id) ids.push_back(id);
            else ++refused;
        }
        CHECK(ids.size() == 32 && refused == 8);
        CHECK(pump(link, [&] {
            for (uint32_t id : ids)
                if (!rec.ack(id)) return false;
            return true;
        }, 8000));
        for (uint32_t id : ids) {
            const Recorder::Ack* a = rec.ack(id);
            CHECK(a && a->ok);
        }
        fake.cmd("{\"cmd\":\"mode\",\"reply\":true}");
    }

    // ── close(): pending texts fail, the radio is told, state goes Off ───────
    fake.cmd("{\"cmd\":\"mode\",\"ack\":\"silent\"}");
    uint32_t id8 = link.send_text(kBob, "last words");
    { JsonValue p; CHECK(pump(link, [&] { return fake.packet(id8, p); })); }
    rec.states.clear();
    link.close();
    CHECK(link.state() == State::Off && !link.ready());
    CHECK(rec.states.empty() && rec.ack(id8) == nullptr);                // not before poll()
    link.poll(now_ms());
    CHECK(rec.states.size() == 1 && rec.states[0] == State::Off);
    if (const Recorder::Ack* a = rec.ack(id8)) CHECK(!a->ok && a->error == 4);
    else CHECK(!"pending text reported at close");
    CHECK(link.send_text(kAlice, "closed") == 0);
    {
        uint64_t end = real_ms() + 3000;
        while (fake.count("disconnect") == 0 && real_ms() < end) usleep(10000);
        CHECK(fake.count("disconnect") == 1);
    }
    // nothing the link sent was undecodable for the real protobuf classes
    for (const JsonValue& v : fake.log()) CHECK(v.get_string("ev") != "bad_toradio" && v.get_string("ev") != "junk");

    fake.stop();
}

// Nobody is listening at first; the node appears later. Uses the name
// "localhost" (::1 refuses, 127.0.0.1 answers) and a node that sends no LoRa
// config, so hop_limit must be left out.
static void test_late_server(const std::string& scratch, int port) {
    MeshLink link;
    Recorder rec;
    rec.attach(link);
    CHECK(link.open("tcp:localhost:" + std::to_string(port)));
    pump_for(link, 400);
    CHECK(link.state() == State::Connecting);
    CHECK(!link.last_error().empty());
    CHECK(rec.states.size() == 1 && rec.states[0] == State::Connecting);

    // keeps retrying with backoff and stays Connecting
    pump_for(link, 1500);
    CHECK(link.state() == State::Connecting);

    Fake fake;
    if (!fake.start(port, scratch + "/fake_late.jsonl")) {
        CHECK(!"late fake node started");
        return;
    }
    g_skew += 31000;                                     // past any backoff
    CHECK(pump(link, [&] { return link.ready(); }));
    CHECK(rec.states.size() == 3 && rec.states.back() == State::Ready);
    CHECK(link.my_node() == kMe && link.nodes().size() == 6);
    CHECK(link.last_error().empty());

    uint32_t id = link.send_text(kAlice, "no config");
    CHECK(pump(link, [&] { return rec.ack(id) != nullptr; }));
    JsonValue p;
    CHECK(fake.packet(id, p) && p.get_int("hop_limit") == 0 && p.get_bool("want_ack"));

    // the node vanishes for good: backoff grows, the link keeps trying quietly
    fake.stop();
    CHECK(pump(link, [&] { return link.state() == State::Connecting; }));
    pump_for(link, 1300);
    CHECK(link.state() == State::Connecting && !link.last_error().empty());

    // switching to another target forgets the old node list
    CHECK(link.open("tcp:127.0.0.1:" + std::to_string(port + 1)));
    CHECK(link.nodes().empty() && link.my_node() == 0);
    link.close();
}

static void test_ipv6(const std::string& scratch, int port) {
    Fake fake;
    if (!fake.start(port, scratch + "/fake_v6.jsonl", {"--v6"})) {
        CHECK(!"v6 fake node started");
        return;
    }
    MeshLink link;
    link.open_tcp("::1", port);
    CHECK(link.target() == "tcp:[::1]:" + std::to_string(port));
    CHECK(pump(link, [&] { return link.ready(); }));
    CHECK(link.my_node() == kMe);
    fake.stop();
    CHECK(pump(link, [&] { return link.state() == State::Connecting; }));
}

// Serial: the link opens the slave side of a pty; the test shovels bytes
// between the pty master and a TCP connection to the fake node.
static void test_serial(const std::string& scratch, int port) {
    Fake fake;
    if (!fake.start(port, scratch + "/fake_serial.jsonl")) {
        CHECK(!"serial fake node started");
        return;
    }
    int master = -1, slave = -1;
    char name[256] = {0};
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0) {
        std::printf("openpty failed: serial not exercised\n");
        CHECK(!"openpty");
        fake.stop();
        return;
    }
    fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    CHECK(connect(sock, (struct sockaddr*)&sa, sizeof sa) == 0);
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);

    std::string from_link;                               // everything the link wrote
    bool bridge = true;
    g_idle = [&] {
        if (!bridge) return;
        char buf[2048];
        ssize_t n = ::read(master, buf, sizeof buf);
        if (n > 0) {
            from_link.append(buf, (size_t)n);
            ssize_t w = ::write(sock, buf, (size_t)n);
            (void)w;
        }
        n = ::read(sock, buf, sizeof buf);
        if (n > 0) {
            ssize_t w = ::write(master, buf, (size_t)n);
            (void)w;
        }
    };

    MeshLink link;
    Recorder rec;
    rec.attach(link);
    CHECK(link.open(std::string("serial:") + name));
    CHECK(link.target() == std::string("serial:") + name);
    CHECK(pump(link, [&] { return link.ready(); }));
    CHECK(rec.states.size() == 3 && rec.states[1] == State::Syncing && rec.states[2] == State::Ready);
    CHECK(link.my_node() == kMe && link.nodes().size() == 6);
    // the wake-up preamble came first: 32 x 0xC3, then a frame
    CHECK(from_link.size() > 36 && from_link.compare(0, 32, std::string(32, '\xC3')) == 0);
    CHECK(from_link.size() > 33 && (unsigned char)from_link[32] == 0x94 && (unsigned char)from_link[33] == 0xC3);
    {
        bool junk32 = false;
        for (const JsonValue& v : fake.log())
            if (v.get_string("ev") == "junk") junk32 = v.get_int("n") == 32;
        CHECK(junk32);
    }
    uint32_t id = link.send_text(kAlice, "over the wire \xE2\x9C\x93");
    CHECK(id != 0);
    CHECK(pump(link, [&] { return rec.ack(id) && rec.text("re: over the wire \xE2\x9C\x93"); }));
    JsonValue p;
    CHECK(fake.packet(id, p) && p.get_string("text") == "over the wire \xE2\x9C\x93" && p.get_u32("to") == kAlice);

    // unplug: both ends of the pty go away
    bridge = false;
    ::close(master);
    ::close(slave);
    CHECK(pump(link, [&] { return link.state() == State::Connecting; }));
    CHECK(!link.last_error().empty());
    pump_for(link, 1300);                                // a retry on the dead device
    CHECK(link.state() == State::Connecting);
    link.close();
    link.poll(now_ms());
    CHECK(rec.states.back() == State::Off);

    g_idle = nullptr;
    ::close(sock);
    fake.stop();

    // not a tty / not there
    MeshLink l2;
    CHECK(l2.open("serial:/dev/null"));
    pump_for(l2, 50);
    CHECK(l2.state() == State::Connecting && !l2.last_error().empty());
    MeshLink l3;
    CHECK(l3.open("/dev/does-not-exist-1999"));
    pump_for(l3, 50);
    CHECK(l3.state() == State::Connecting && !l3.last_error().empty());
}

// Mutated frames by the thousand (run this under ASan/UBSan), then the node
// list cap. The link must come out of it able to talk.
static void test_fuzz(const std::string& scratch, int port) {
    Fake fake;
    if (!fake.start(port, scratch + "/fake_fuzz.jsonl")) {
        CHECK(!"fuzz fake node started");
        return;
    }
    MeshLink link;
    Recorder rec;
    rec.attach(link);
    CHECK(link.open("tcp:127.0.0.1:" + std::to_string(port)));
    CHECK(pump(link, [&] { return link.ready(); }));
    for (int seed = 1; seed <= 4; ++seed) {
        fake.cmd("{\"cmd\":\"fuzz\",\"seed\":" + std::to_string(seed) + ",\"count\":3000}");
        std::string mark = "fuzz-marker-" + std::to_string(seed);
        // the mutations may have changed "my node": resync, then talk
        fake.cmd("{\"cmd\":\"rebooted\"}");
        CHECK(pump(link, [&] {
            if (link.ready() && link.my_node() == kMe && !rec.text(mark)) {
                static uint64_t last = 0;
                if (real_ms() - last > 300) { last = real_ms(); fake.text(kAlice, kMe, mark); }
            }
            return rec.text(mark) != nullptr;
        }, 20000));
        CHECK(link.nodes().size() <= 512);
        int me = 0;
        for (const MeshNode& n : link.nodes()) {
            me += n.is_me;
            CHECK(n.long_name.size() <= 64 && n.short_name.size() <= 64);
        }
        CHECK(me == 1);
    }
    fake.cmd("{\"cmd\":\"many_nodes\",\"count\":700}");
    fake.text(kAlice, kMe, "after the crowd");
    CHECK(pump(link, [&] { return rec.text("after the crowd") != nullptr; }, 10000));
    CHECK(link.nodes().size() == 512);
    CHECK(link.node(kMe) && link.node(kAlice));
    uint32_t id = link.send_text(kAlice, "survived");
    CHECK(id != 0);
    CHECK(pump(link, [&] { return rec.ack(id) != nullptr; }));
    fake.stop();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <scratch_dir> [first_port]\n", argv[0]);
        return 2;
    }
    std::string scratch = argv[1];
    int port = argc > 2 ? std::atoi(argv[2]) : 44100;
    if (port < 44100 || port > 44190) {
        std::printf("first_port must be 44100..44190\n");
        return 2;
    }
    atexit(kill_children);

    test_statics();
    test_main(scratch, port);
    test_late_server(scratch, port + 1);
    test_ipv6(scratch, port + 3);
    test_serial(scratch, port + 4);
    test_fuzz(scratch, port + 5);

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
