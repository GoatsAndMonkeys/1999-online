// mesh_link.h — a Meshtastic client: the radio side of 1999 Online.
//
// Speaks Meshtastic's client API (the "stream" protocol: 0x94 0xC3, a 16-bit
// big-endian length, then a ToRadio / FromRadio protobuf) over either
//   - TCP, port 4403: meshtasticd on the Cardputer itself (LoRa add-on), or
//     any node with Wi-Fi/Ethernet. Several clients may connect at once, so
//     the MonsterMesh game daemon can keep its own connection; or
//   - a serial device: a Meshtastic node on USB (one client only).
//
// What it does: the want_config handshake (my node number, the node list),
// keeps the node list fresh from NodeInfo / telemetry / positions / every
// heard packet, sends and receives text messages (TEXT_MESSAGE_APP, portnum 1)
// and reports delivery (ROUTING_APP acks and errors). Probes for Mesh
// Neighborhood: traceroute (TRACEROUTE_APP, portnum 70) and ping (an empty
// want_ack packet, see send_ping()). Admin messages for Radio Setup will go
// through here later.
//
// Non-blocking and single-threaded: call poll() from the UI tick. No call
// blocks for longer than a non-blocking connect/read/write takes. Standard
// library + POSIX only; LVGL-free.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mm {

struct MeshNode {
    uint32_t num = 0;             // node number (the "!a1b2c3d4" id as an integer)
    std::string long_name;        // "" until its NodeInfo has been heard
    std::string short_name;
    float snr = 0.0f;             // dB, last packet heard from it
    int hops_away = -1;           // -1 unknown, 0 direct
    uint32_t last_heard = 0;      // unix seconds, 0 unknown
    int battery = -1;             // percent (101 = powered), -1 unknown
    bool is_me = false;
    // Last known position (POSITION_APP packets and the radio's node DB).
    // Meshtastic sends latitude_i / longitude_i in 1e-7 degrees; (0, 0) means
    // "no fix" and is never stored.
    bool has_position = false;
    double latitude = 0.0;        // degrees, + north
    double longitude = 0.0;       // degrees, + east
    int altitude = 0;             // metres above sea level (0 also when unknown)
    uint32_t position_time = 0;   // unix seconds of the fix (else when heard), 0 unknown
};

// A ping or traceroute sent with MeshLink::send_ping() / send_traceroute().
struct MeshProbe {
    enum class Kind : uint8_t { Ping, Traceroute };
    enum class Status : uint8_t {
        Waiting,      // sent, no answer yet
        Done,         // answered: rtt_ms() is valid
        Failed,       // routing error, timeout or link lost: `error`
    };
    static constexpr float kNoSnr = -1000.0f;   // an SNR the mesh did not record

    uint32_t id = 0;              // packet id of the request
    uint32_t to = 0;
    Kind kind = Kind::Ping;
    Status status = Status::Waiting;
    int error = 0;                // Routing.Error (1 no route, 3 timeout, 5 max retransmit, 8 no response, ...)
    uint64_t sent_ms = 0;         // poll() clock
    uint64_t done_ms = 0;
    // Traceroute: the nodes in between, me -> `to` (`route`) and `to` -> me
    // (`route_back`), ends not included. 0xffffffff is a hop that did not
    // add itself (an old firmware or an encrypted relay). snr_* has one entry
    // per hop (route.size() + 1), dB, kNoSnr where unknown; snr_back may be
    // empty when the replying firmware is too old to record the way back.
    std::vector<uint32_t> route, route_back;
    std::vector<float> snr_towards, snr_back;
    // Ping: the hops and SNR of the ack as it reached my radio (-1 / kNoSnr unknown).
    int hops = -1;
    float snr = kNoSnr;

    uint32_t rtt_ms() const { return done_ms > sent_ms ? (uint32_t)(done_ms - sent_ms) : 0; }
};

struct MeshText {
    uint32_t from = 0;
    uint32_t to = 0;              // kBroadcast for a channel message
    uint32_t id = 0;              // packet id
    int channel = 0;              // channel index
    std::string text;             // UTF-8
    float snr = 0.0f;
    int hops = -1;                // hop_start - hop_limit, -1 unknown
    uint32_t rx_time = 0;         // unix seconds, 0 unknown
    bool direct() const;          // addressed to me, not to a channel
};

class MeshLink {
public:
    static constexpr uint32_t kBroadcast = 0xffffffffu;
    static constexpr int kDefaultPort = 4403;
    static constexpr size_t kMaxTextBytes = 200;      // a text payload that fits one packet

    enum class State : uint8_t {
        Off,          // not configured, or close()d
        Connecting,   // opening the socket / device (retries with backoff)
        Syncing,      // connected, reading the node list
        Ready,        // handshake complete: can send
    };

    using TextCallback = std::function<void(const MeshText&)>;
    // Delivery report for a packet from send_text(): ok, or the routing error
    // code (Routing.Error: 0 none, 1 no route, 3 timeout, 5 max retransmit, ...).
    // A broadcast is reported ok when the radio hears a rebroadcast (implicit
    // ack); a direct message when the destination acks it.
    using AckCallback = std::function<void(uint32_t packet_id, bool ok, int error)>;
    using StateCallback = std::function<void(State)>;

    MeshLink();
    ~MeshLink();
    MeshLink(const MeshLink&) = delete;
    MeshLink& operator=(const MeshLink&) = delete;

    // Choose where the radio is. Either returns at once; poll() does the work
    // and reconnects (1 s, then doubling to 30 s) for as long as it is open.
    void open_tcp(const std::string& host, int port = kDefaultPort);
    void open_serial(const std::string& device, int baud = 115200);
    // "tcp:host[:port]", "serial:/dev/ttyACM0", a bare "/dev/..." path or a
    // bare host name. false (and stays Off) if the spec makes no sense.
    bool open(const std::string& spec);
    void close();

    void set_text_callback(TextCallback cb) { on_text_ = std::move(cb); }
    void set_ack_callback(AckCallback cb) { on_ack_ = std::move(cb); }
    void set_state_callback(StateCallback cb) { on_state_ = std::move(cb); }

    // Pump the connection: connect, read, decode, write, heartbeat. Callbacks
    // run from inside poll(). `now_ms` is any monotonic millisecond clock.
    void poll(uint64_t now_ms);

    State state() const { return state_; }
    bool ready() const { return state_ == State::Ready; }
    const std::string& target() const { return target_; }      // "tcp:localhost:4403"
    const std::string& last_error() const { return last_error_; }

    uint32_t my_node() const { return my_node_; }              // 0 until known
    // Every node the radio knows, me included, in first-heard order.
    const std::vector<MeshNode>& nodes() const { return nodes_; }
    const MeshNode* node(uint32_t num) const;
    // Bumps whenever nodes() changes in any way.
    uint32_t nodes_revision() const { return nodes_rev_; }
    // The primary channel as Meshtastic apps name it ("LongFast" on the
    // default channel); "" until the radio has sent its config.
    const std::string& channel_name() const { return channel_name_; }

    // Queue a text message. `to` = a node number, or kBroadcast for the
    // channel. Returns the packet id (never 0), or 0 if not ready() or the
    // text is empty / longer than kMaxTextBytes.
    uint32_t send_text(uint32_t to, const std::string& text, int channel = 0);

    // ── Probes (Mesh Neighborhood) ─────────────────────────────────────────
    // Both return the request's packet id (never 0), or 0 if not ready(), `to`
    // is me / 0 / broadcast, or a probe is already Waiting (one at a time: the
    // firmware rate-limits traceroutes anyway). Results: probe(id).
    //   send_traceroute: a RouteDiscovery request (TRACEROUTE_APP, portnum 70,
    //     want_response); Done when `to` sends the route back. Fails after
    //     kProbeTimeoutMs or on a routing error.
    //   send_ping: an EMPTY packet on PRIVATE_APP (256) with want_ack to `to`.
    //     The destination's router acks it; Done on that ack (not on an
    //     implicit relay ack). The empty payload is ignored by MonsterMesh and
    //     other PRIVATE_APP listeners (they need a payload); no text is sent.
    static constexpr uint64_t kProbeTimeoutMs = 60000;
    uint32_t send_traceroute(uint32_t to);
    uint32_t send_ping(uint32_t to);
    // The most recent probes (newest last, at most 8 kept); nullptr if gone.
    const MeshProbe* probe(uint32_t id) const;
    const std::vector<MeshProbe>& probes() const { return probes_; }
    // Bumps whenever a probe is added or changes.
    uint32_t probes_revision() const { return probes_rev_; }

    // "!a1b2c3d4"
    static std::string node_id_text(uint32_t num);

private:
    struct Impl;
    Impl* impl_ = nullptr;        // transport, buffers, codec state

    State state_ = State::Off;
    std::string target_, last_error_;
    uint32_t my_node_ = 0;
    std::vector<MeshNode> nodes_;
    uint32_t nodes_rev_ = 1;
    std::string channel_name_;
    std::vector<MeshProbe> probes_;
    uint32_t probes_rev_ = 1;
    TextCallback on_text_;
    AckCallback on_ack_;
    StateCallback on_state_;
};

MeshLink& mesh_link();

// ── Canned data for demo mode and --shots (never touches the radio) ────────
// A small neighbourhood around a made-up town: my node (first, is_me) and
// nine others, most with positions; last_heard relative to `now_unix`.
std::vector<MeshNode> mesh_demo_nodes(uint32_t now_unix);
// A finished traceroute / ping from my demo node to `to` (a demo node), as
// probe() would report it. `kind` Ping gives a one-hop ack.
MeshProbe mesh_demo_probe(uint32_t to, MeshProbe::Kind kind);

} // namespace mm
