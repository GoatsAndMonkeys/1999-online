// mesh_link.cpp — the Meshtastic client behind mesh_link.h.
//
// Three layers, all in this file:
//   1. a transport (non-blocking TCP socket or serial tty) with reconnect,
//   2. the stream framing (0x94 0xC3 len_hi len_lo + protobuf), and
//   3. a hand-written, bounds-checked protobuf codec for the handful of
//      messages a messenger needs.
//
// Callbacks are never run from the middle of the state machine: everything
// that happens is queued as an event and handed out at the end of poll(), so
// a callback may call send_text(), close() or open() on the link freely.
//
// Conventions copied from the official Python client (meshtastic 2.7.8,
// mesh_interface.py / stream_interface.py):
//   - serial wake-up preamble: 32 x 0xC3, then 100 ms before want_config;
//   - heartbeat: an empty Heartbeat as soon as the handshake completes, then
//     every 300 s (here: after 300 s without writing anything to the radio);
//   - texts go out with priority RELIABLE and want_ack;
//   - hop_limit: the Python client copies Config.lora.hop_limit from the
//     handshake into every packet. We do the same when the radio sent its
//     LoRa config; otherwise the field is left out (0), which the firmware
//     replaces with its configured default for want_ack packets.
//   - ToRadio.disconnect = true on a clean close.
#include "mesh_link.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <random>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

namespace mm {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Protobuf field numbers. Source: meshtastic/protobufs (master), files
// mesh.proto, portnums.proto, telemetry.proto, config.proto as fetched on
// 2026-11-20. Wire types: V = varint, F = fixed32 (also float), L = length-
// delimited. Nothing here uses sint (zigzag) or 64-bit fields.
//
//   ToRadio        packet=1 L(MeshPacket)  want_config_id=3 V  disconnect=4 V
//                  heartbeat=7 L(Heartbeat, sent empty)
//   FromRadio      id=1 V  packet=2 L  my_info=3 L  node_info=4 L  config=5 L
//                  config_complete_id=7 V  rebooted=8 V  queueStatus=11 L
//   MyNodeInfo     my_node_num=1 V
//   NodeInfo       num=1 V  user=2 L(User)  snr=4 F(float)  last_heard=5 F
//                  device_metrics=6 L(DeviceMetrics)  hops_away=9 V (optional)
//   User           id=1 L  long_name=2 L  short_name=3 L
//   DeviceMetrics  battery_level=1 V (optional)
//   Telemetry      device_metrics=2 L(DeviceMetrics)
//   MeshPacket     from=1 F  to=2 F  channel=3 V  decoded=4 L(Data)
//                  encrypted=5 L  id=6 F  rx_time=7 F  rx_snr=8 F(float)
//                  hop_limit=9 V  want_ack=10 V  priority=11 V  via_mqtt=14 V
//                  hop_start=15 V
//   Data           portnum=1 V  payload=2 L  want_response=3 V  dest=4 F
//                  source=5 F  request_id=6 F  reply_id=7 F
//   Routing        error_reason=3 V
//   QueueStatus    res=1 V(int32)  mesh_packet_id=4 V
//   Config         lora=6 L(LoRaConfig)
//   LoRaConfig     use_preset=1 V  modem_preset=2 V  hop_limit=8 V
//   Channel        index=1 V  settings=2 L(ChannelSettings)  role=3 V (FromRadio.channel=10)
//   ChannelSettings name=3 L
//   PortNum        TEXT_MESSAGE_APP=1  NODEINFO_APP=4  ROUTING_APP=5
//                  TELEMETRY_APP=67
//   Priority       RELIABLE=70
//   Routing.Error  NONE=0  TIMEOUT=3  NO_INTERFACE=4
// ─────────────────────────────────────────────────────────────────────────────
enum : uint32_t {
    kToRadioPacket = 1, kToRadioWantConfigId = 3, kToRadioDisconnect = 4, kToRadioHeartbeat = 7,

    kFromRadioPacket = 2, kFromRadioMyInfo = 3, kFromRadioNodeInfo = 4, kFromRadioConfig = 5,
    kFromRadioConfigCompleteId = 7, kFromRadioRebooted = 8, kFromRadioChannel = 10, kFromRadioQueueStatus = 11,

    kMyNodeInfoMyNodeNum = 1,

    kNodeInfoNum = 1, kNodeInfoUser = 2, kNodeInfoSnr = 4, kNodeInfoLastHeard = 5,
    kNodeInfoDeviceMetrics = 6, kNodeInfoHopsAway = 9,

    kUserLongName = 2, kUserShortName = 3,

    kDeviceMetricsBatteryLevel = 1,
    kTelemetryDeviceMetrics = 2,

    kPacketFrom = 1, kPacketTo = 2, kPacketChannel = 3, kPacketDecoded = 4, kPacketEncrypted = 5,
    kPacketId = 6, kPacketRxTime = 7, kPacketRxSnr = 8, kPacketHopLimit = 9, kPacketWantAck = 10,
    kPacketPriority = 11, kPacketViaMqtt = 14, kPacketHopStart = 15,

    kDataPortnum = 1, kDataPayload = 2, kDataWantResponse = 3, kDataDest = 4, kDataSource = 5,
    kDataRequestId = 6, kDataReplyId = 7,

    kRoutingErrorReason = 3,

    kQueueStatusRes = 1, kQueueStatusMeshPacketId = 4,

    kConfigLora = 6, kLoraUsePreset = 1, kLoraModemPreset = 2, kLoraHopLimit = 8,

    kChannelIndex = 1, kChannelSettings = 2, kChannelSettingsName = 3,

    kPortText = 1, kPortNodeInfo = 4, kPortRouting = 5, kPortTelemetry = 67,

    kPriorityReliable = 70,
};
enum : int { kErrNone = 0, kErrTimeout = 3, kErrNoInterface = 4 };

enum : uint32_t { kWireVarint = 0, kWireFixed64 = 1, kWireLen = 2, kWireFixed32 = 5 };

// Limits.
constexpr uint8_t kStart1 = 0x94, kStart2 = 0xC3;
constexpr size_t kMaxFrame = 512;            // MAX_TO_FROM_RADIO_SIZE
constexpr size_t kMaxNodes = 512;
constexpr size_t kMaxNameBytes = 64;
constexpr size_t kMaxTxFrames = 32;          // frames waiting for the socket
constexpr size_t kMaxPending = 64;           // texts waiting for an ack
constexpr size_t kMaxEvents = 256;
constexpr size_t kMaxReadPerPoll = 16384;
constexpr size_t kSeenTexts = 32;
constexpr int kMaxChannel = 7;

constexpr uint64_t kBackoffMinMs = 1000, kBackoffMaxMs = 30000;
constexpr uint64_t kConnectTimeoutMs = 10000;
constexpr uint64_t kSyncSilenceMs = 20000;   // no bytes at all while Syncing
constexpr uint64_t kHeartbeatMs = 300000;
constexpr uint64_t kAckTimeoutMs = 90000;
constexpr uint64_t kSerialWakeMs = 100;

// ── protobuf reader ─────────────────────────────────────────────────────────
struct Reader {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    bool ok = true;

    Reader() = default;
    Reader(const uint8_t* data, size_t len) : p(data), end(data + len) {}

    size_t left() const { return (size_t)(end - p); }

    uint64_t varint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p >= end) { ok = false; return 0; }
            uint8_t b = *p++;
            v |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;                      // more than 10 bytes
        return 0;
    }
    uint32_t fixed32() {
        if (left() < 4) { ok = false; p = end; return 0; }
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                     ((uint32_t)p[3] << 24);
        p += 4;
        return v;
    }
    float f32() {
        uint32_t u = fixed32();
        float f;
        std::memcpy(&f, &u, sizeof f);
        return std::isfinite(f) ? f : 0.0f;
    }
    // A length-delimited field as a sub-reader over its bytes.
    Reader bytes() {
        uint64_t n = varint();
        if (!ok || n > left()) { ok = false; p = end; return Reader(); }
        Reader r(p, (size_t)n);
        p += n;
        return r;
    }
    // Next tag. false at the end of the message or on an error (check ok).
    bool next(uint32_t& field, uint32_t& wire) {
        if (!ok || p >= end) return false;
        uint64_t t = varint();
        if (!ok || t > 0xffffffffull || (t >> 3) == 0) { ok = false; return false; }
        field = (uint32_t)(t >> 3);
        wire = (uint32_t)(t & 7);
        return true;
    }
    void skip(uint32_t wire) {
        switch (wire) {
        case kWireVarint: varint(); break;
        case kWireFixed64:
            if (left() < 8) { ok = false; p = end; } else p += 8;
            break;
        case kWireLen: bytes(); break;
        case kWireFixed32: fixed32(); break;
        default: ok = false; break;      // groups: not used by Meshtastic
        }
    }
};

// ── protobuf writer ─────────────────────────────────────────────────────────
void put_varint(std::string& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back((char)(uint8_t)(v | 0x80));
        v >>= 7;
    }
    out.push_back((char)(uint8_t)v);
}
void put_tag(std::string& out, uint32_t field, uint32_t wire) { put_varint(out, ((uint64_t)field << 3) | wire); }
void put_uint(std::string& out, uint32_t field, uint64_t v) {
    put_tag(out, field, kWireVarint);
    put_varint(out, v);
}
void put_fixed32(std::string& out, uint32_t field, uint32_t v) {
    put_tag(out, field, kWireFixed32);
    for (int i = 0; i < 4; ++i) out.push_back((char)(uint8_t)(v >> (8 * i)));
}
void put_bytes(std::string& out, uint32_t field, const std::string& b) {
    put_tag(out, field, kWireLen);
    put_varint(out, b.size());
    out.append(b);
}

// A complete stream frame around a ToRadio message.
std::string frame(const std::string& msg) {
    std::string f;
    f.reserve(msg.size() + 4);
    f.push_back((char)kStart1);
    f.push_back((char)kStart2);
    f.push_back((char)(uint8_t)(msg.size() >> 8));
    f.push_back((char)(uint8_t)(msg.size() & 0xff));
    f.append(msg);
    return f;
}

// ── text hygiene ────────────────────────────────────────────────────────────
// Returns valid UTF-8 of at most max_bytes: bad sequences become U+FFFD,
// control characters are dropped (newline and tab survive if keep_newlines).
std::string clean_utf8(const uint8_t* s, size_t n, size_t max_bytes, bool keep_newlines) {
    std::string out;
    out.reserve(std::min(n, max_bytes));
    size_t i = 0;
    while (i < n) {
        uint32_t cp = 0xFFFD;
        size_t len = 1;
        uint8_t c = s[i];
        if (c < 0x80) {
            cp = c;
        } else {
            size_t need = 0;
            uint32_t min = 0;
            if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; min = 0x80; }
            else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; min = 0x800; }
            else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; min = 0x10000; }
            bool good = need != 0 && need < n - i;   // all continuation bytes present
            if (good) {
                for (size_t k = 1; k <= need; ++k) {
                    uint8_t cc = s[i + k];
                    if ((cc & 0xC0) != 0x80) { good = false; break; }
                    cp = (cp << 6) | (cc & 0x3F);
                }
            }
            if (good && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) good = false;
            if (good) len = need + 1;
            else cp = 0xFFFD;
        }
        i += len;

        if (cp < 0x20) {
            if (!(keep_newlines && (cp == '\n' || cp == '\t'))) continue;
        } else if (cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) {
            continue;
        }

        char buf[4];
        size_t m;
        if (cp < 0x80) { buf[0] = (char)cp; m = 1; }
        else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); m = 2; }
        else if (cp < 0x10000) {
            buf[0] = (char)(0xE0 | (cp >> 12));
            buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (cp & 0x3F));
            m = 3;
        } else {
            buf[0] = (char)(0xF0 | (cp >> 18));
            buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[3] = (char)(0x80 | (cp & 0x3F));
            m = 4;
        }
        if (out.size() + m > max_bytes) break;
        out.append(buf, m);
    }
    return out;
}

std::string clean_name(Reader r) {
    std::string s = clean_utf8(r.p, r.left(), kMaxNameBytes, false);
    size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}

// ── decoded messages ────────────────────────────────────────────────────────
struct PbData {
    uint32_t portnum = 0;
    Reader payload;                  // points into the frame buffer
    bool want_response = false;
    uint32_t dest = 0, source = 0, request_id = 0, reply_id = 0;
};

struct PbPacket {
    uint32_t from = 0, to = 0, channel = 0, id = 0, rx_time = 0;
    uint32_t hop_limit = 0, hop_start = 0, priority = 0;
    float rx_snr = 0.0f;
    bool has_snr = false, has_decoded = false, encrypted = false;
    bool want_ack = false, via_mqtt = false;
    PbData data;
};

bool decode_data(Reader r, PbData& d) {
    uint32_t f, w;
    while (r.next(f, w)) {
        if (f == kDataPortnum && w == kWireVarint) d.portnum = (uint32_t)r.varint();
        else if (f == kDataPayload && w == kWireLen) d.payload = r.bytes();
        else if (f == kDataWantResponse && w == kWireVarint) d.want_response = r.varint() != 0;
        else if (f == kDataDest && w == kWireFixed32) d.dest = r.fixed32();
        else if (f == kDataSource && w == kWireFixed32) d.source = r.fixed32();
        else if (f == kDataRequestId && w == kWireFixed32) d.request_id = r.fixed32();
        else if (f == kDataReplyId && w == kWireFixed32) d.reply_id = r.fixed32();
        else r.skip(w);
    }
    return r.ok;
}

bool decode_packet(Reader r, PbPacket& p) {
    uint32_t f, w;
    while (r.next(f, w)) {
        if (f == kPacketFrom && w == kWireFixed32) p.from = r.fixed32();
        else if (f == kPacketTo && w == kWireFixed32) p.to = r.fixed32();
        else if (f == kPacketChannel && w == kWireVarint) p.channel = (uint32_t)r.varint();
        else if (f == kPacketDecoded && w == kWireLen) {
            Reader sub = r.bytes();
            if (!r.ok) break;
            p.data = PbData();
            if (!decode_data(sub, p.data)) return false;
            p.has_decoded = true;
            p.encrypted = false;
        } else if (f == kPacketEncrypted && w == kWireLen) {
            r.bytes();
            p.encrypted = true;
            p.has_decoded = false;
        } else if (f == kPacketId && w == kWireFixed32) p.id = r.fixed32();
        else if (f == kPacketRxTime && w == kWireFixed32) p.rx_time = r.fixed32();
        else if (f == kPacketRxSnr && w == kWireFixed32) { p.rx_snr = r.f32(); p.has_snr = true; }
        else if (f == kPacketHopLimit && w == kWireVarint) p.hop_limit = (uint32_t)r.varint();
        else if (f == kPacketWantAck && w == kWireVarint) p.want_ack = r.varint() != 0;
        else if (f == kPacketPriority && w == kWireVarint) p.priority = (uint32_t)r.varint();
        else if (f == kPacketViaMqtt && w == kWireVarint) p.via_mqtt = r.varint() != 0;
        else if (f == kPacketHopStart && w == kWireVarint) p.hop_start = (uint32_t)r.varint();
        else r.skip(w);
    }
    return r.ok;
}

struct PbUser {
    bool has_long = false, has_short = false;
    std::string long_name, short_name;
};

bool decode_user(Reader r, PbUser& u) {
    uint32_t f, w;
    while (r.next(f, w)) {
        if (f == kUserLongName && w == kWireLen) {
            Reader s = r.bytes();
            if (r.ok) { u.long_name = clean_name(s); u.has_long = true; }
        } else if (f == kUserShortName && w == kWireLen) {
            Reader s = r.bytes();
            if (r.ok) { u.short_name = clean_name(s); u.has_short = true; }
        } else r.skip(w);
    }
    return r.ok;
}

// battery: -1 if the field is absent.
bool decode_device_metrics(Reader r, int& battery) {
    uint32_t f, w;
    while (r.next(f, w)) {
        if (f == kDeviceMetricsBatteryLevel && w == kWireVarint) {
            uint64_t v = r.varint();
            battery = v > 101 ? 101 : (int)v;
        } else r.skip(w);
    }
    return r.ok;
}

// True if `r` parses as a sequence of well-formed top-level fields.
bool well_formed(Reader r) {
    uint32_t f, w;
    while (r.next(f, w)) r.skip(w);
    return r.ok;
}

bool same_node(const MeshNode& a, const MeshNode& b) {
    return a.num == b.num && a.long_name == b.long_name && a.short_name == b.short_name &&
           a.snr == b.snr && a.hops_away == b.hops_away && a.last_heard == b.last_heard &&
           a.battery == b.battery && a.is_me == b.is_me;
}

bool set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

void set_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

bool baud_constant(int baud, speed_t& out) {
    switch (baud) {
    case 9600: out = B9600; return true;
    case 19200: out = B19200; return true;
    case 38400: out = B38400; return true;
    case 57600: out = B57600; return true;
    case 115200: out = B115200; return true;
    case 230400: out = B230400; return true;
#ifdef B460800
    case 460800: out = B460800; return true;
#endif
#ifdef B921600
    case 921600: out = B921600; return true;
#endif
    default: return false;
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
struct MeshLink::Impl {
    enum class Kind { None, Tcp, Serial };

    struct OutFrame {
        std::string bytes;
        uint32_t packet_id = 0;      // != 0: a text waiting in `pending`
    };
    struct Pending {
        uint32_t id = 0;
        uint32_t to = 0;
        uint64_t deadline = 0;
    };
    struct Event {
        enum Type { StateEv, TextEv, AckEv } type = StateEv;
        State state = State::Off;
        MeshText text;
        uint32_t id = 0;
        bool ok = false;
        int error = 0;
    };

    MeshLink& L;
    Kind kind = Kind::None;
    std::string host, device;
    int port = kDefaultPort;
    int baud = 115200;

    int fd = -1;
    bool connecting = false;         // TCP connect() in progress
    struct addrinfo* ai = nullptr;
    struct addrinfo* ai_next = nullptr;
    std::string connect_error;

    uint64_t now = 0;
    uint64_t next_attempt = 0;
    uint64_t backoff = kBackoffMinMs;
    uint64_t connect_started = 0;
    uint64_t last_rx = 0;
    uint64_t last_tx = 0;
    uint64_t want_config_at = 0;
    bool want_config_due = false;

    std::string rx;
    std::deque<OutFrame> txq;
    size_t tx_off = 0;

    uint32_t config_id = 0;
    uint32_t lora_hop_limit = 0;     // 0 = the radio did not tell us
    int lora_preset = -1;            // ModemPreset; -1 unknown, -2 custom (use_preset off)
    std::string ch0_name;            // primary channel's own name ("" = the preset's)
    bool ch0_known = false;

    std::vector<Pending> pending;
    std::vector<Event> events;
    bool in_poll = false;

    uint32_t seen_from[kSeenTexts] = {};
    uint32_t seen_id[kSeenTexts] = {};
    size_t seen_pos = 0;

    std::mt19937 rng;

    explicit Impl(MeshLink& link) : L(link) {
        std::random_device rd;
        std::seed_seq seed{rd(), rd(), rd(), (unsigned)::getpid(), (unsigned)std::time(nullptr)};
        rng.seed(seed);
    }
    ~Impl() {
        close_fd();
        free_addrs();
    }

    uint64_t since(uint64_t t) const { return now > t ? now - t : 0; }

    uint32_t random_id() {
        for (;;) {
            uint32_t v = (uint32_t)rng();
            if (v != 0 && v != kBroadcast) return v;
        }
    }

    // ── events ──────────────────────────────────────────────────────────────
    void push(Event&& e) {
        if (events.size() >= kMaxEvents) {
            // Never lose a state change or a delivery report: drop the oldest text.
            auto it = std::find_if(events.begin(), events.end(),
                                   [](const Event& x) { return x.type == Event::TextEv; });
            if (it == events.end()) return;
            events.erase(it);
        }
        events.push_back(std::move(e));
    }
    void set_state(State s) {
        if (L.state_ == s) return;
        L.state_ = s;
        Event e;
        e.type = Event::StateEv;
        e.state = s;
        push(std::move(e));
    }
    void report_ack(uint32_t id, bool ok, int error) {
        Event e;
        e.type = Event::AckEv;
        e.id = id;
        e.ok = ok;
        e.error = error;
        push(std::move(e));
    }
    void dispatch() {
        while (!events.empty()) {
            std::vector<Event> batch;
            batch.swap(events);
            for (Event& e : batch) {
                switch (e.type) {
                case Event::StateEv: if (L.on_state_) L.on_state_(e.state); break;
                case Event::TextEv: if (L.on_text_) L.on_text_(e.text); break;
                case Event::AckEv: if (L.on_ack_) L.on_ack_(e.id, e.ok, e.error); break;
                }
            }
        }
    }

    // ── nodes ───────────────────────────────────────────────────────────────
    MeshNode* find_node(uint32_t num) {
        for (MeshNode& n : L.nodes_)
            if (n.num == num) return &n;
        return nullptr;
    }
    // Apply `fn` to the node (created if there is room); bump the revision if
    // anything changed.
    template <typename Fn> void edit_node(uint32_t num, Fn fn) {
        if (num == 0 || num == kBroadcast) return;
        MeshNode* n = find_node(num);
        if (!n) {
            if (L.nodes_.size() >= kMaxNodes) return;
            MeshNode fresh;
            fresh.num = num;
            fresh.is_me = (num == L.my_node_);
            fn(fresh);
            L.nodes_.push_back(std::move(fresh));
            ++L.nodes_rev_;
            return;
        }
        MeshNode before = *n;
        fn(*n);
        n->num = num;
        if (!same_node(before, *n)) ++L.nodes_rev_;
    }
    void forget_nodes() {
        if (!L.nodes_.empty() || L.my_node_ != 0) ++L.nodes_rev_;
        L.nodes_.clear();
        L.my_node_ = 0;
    }

    // ── transport ───────────────────────────────────────────────────────────
    void close_fd() {
        if (fd >= 0) ::close(fd);
        fd = -1;
        connecting = false;
    }
    void free_addrs() {
        if (ai) freeaddrinfo(ai);
        ai = ai_next = nullptr;
    }
    bool connected() const { return fd >= 0 && !connecting; }

    // Texts that never reached the radio are reported as failed.
    void fail_unsent() {
        for (const OutFrame& f : txq) {
            if (f.packet_id == 0) continue;
            auto it = std::find_if(pending.begin(), pending.end(),
                                   [&](const Pending& p) { return p.id == f.packet_id; });
            if (it == pending.end()) continue;
            pending.erase(it);
            report_ack(f.packet_id, false, kErrNoInterface);
        }
        txq.clear();
        tx_off = 0;
    }

    void schedule_retry(const std::string& why) {
        L.last_error_ = why;
        next_attempt = now + backoff;
        backoff = std::min<uint64_t>(backoff * 2, kBackoffMaxMs);
    }

    // The link was up (or half up) and is gone.
    void drop(const std::string& why) {
        close_fd();
        free_addrs();
        fail_unsent();
        rx.clear();
        want_config_due = false;
        schedule_retry(why);
        set_state(State::Connecting);
    }

    void reset(bool to_off) {
        // Best effort: tell the radio we are leaving (as the Python client does).
        if (connected() && tx_off == 0) {
            std::string m;
            put_uint(m, kToRadioDisconnect, 1);
            std::string f = frame(m);
            write_some(f.data(), f.size());
        }
        close_fd();
        free_addrs();
        fail_unsent();
        // Texts already handed to the radio can no longer be confirmed.
        for (const Pending& p : pending) report_ack(p.id, false, kErrNoInterface);
        pending.clear();
        rx.clear();
        want_config_due = false;
        backoff = kBackoffMinMs;
        next_attempt = 0;
        lora_hop_limit = 0;
        if (to_off) {
            kind = Kind::None;
            set_state(State::Off);
        }
    }

    ssize_t write_some(const char* data, size_t len) {
        if (kind == Kind::Tcp) {
#ifdef MSG_NOSIGNAL
            return ::send(fd, data, len, MSG_NOSIGNAL);
#else
            return ::send(fd, data, len, 0);     // SO_NOSIGPIPE is set on the socket
#endif
        }
        return ::write(fd, data, len);
    }

    void start_handshake() {
        config_id = random_id();
        std::string m;
        put_uint(m, kToRadioWantConfigId, config_id);
        enqueue(frame(m), 0, true);
    }

    void on_connected() {
        connecting = false;
        rx.clear();
        txq.clear();
        tx_off = 0;
        last_rx = last_tx = now;
        L.last_error_.clear();
        set_state(State::Syncing);
        if (kind == Kind::Serial) {
            // Wake a sleeping node and force its parser to resync, then give it
            // a moment before asking for the config.
            OutFrame f;
            f.bytes.assign(32, (char)kStart2);
            txq.push_back(std::move(f));
            want_config_due = true;
            want_config_at = now + kSerialWakeMs;
        } else {
            start_handshake();
        }
    }

    void open_serial_now() {
        int f = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_NOCTTY);
        if (f < 0) {
            schedule_retry(device + ": " + std::strerror(errno));
            return;
        }
        set_cloexec(f);
        struct termios tio;
        if (tcgetattr(f, &tio) != 0) {
            std::string why = device + ": not a serial device";
            ::close(f);
            schedule_retry(why);
            return;
        }
        cfmakeraw(&tio);
        tio.c_cflag &= ~(tcflag_t)(CSIZE | PARENB | CSTOPB | HUPCL);
#ifdef CRTSCTS
        tio.c_cflag &= ~(tcflag_t)CRTSCTS;
#endif
        tio.c_cflag |= CS8 | CLOCAL | CREAD;
        // VMIN 1: with O_NONBLOCK an idle line reads as EAGAIN, so a read of 0
        // bytes can only mean the device is gone (VMIN 0 would make "no data"
        // look like EOF).
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;
        speed_t sp = B115200;
        baud_constant(baud, sp);
        cfsetispeed(&tio, sp);
        cfsetospeed(&tio, sp);
        if (tcsetattr(f, TCSANOW, &tio) != 0) {
            std::string why = device + ": " + std::strerror(errno);
            ::close(f);
            schedule_retry(why);
            return;
        }
#ifdef TIOCEXCL
        ioctl(f, TIOCEXCL);              // a serial node serves one client
#endif
        tcflush(f, TCIOFLUSH);
        fd = f;
        on_connected();
    }

    // Try the next resolved address. Leaves fd < 0 and a retry scheduled when
    // every address failed.
    void connect_next() {
        while (ai_next) {
            struct addrinfo* a = ai_next;
            ai_next = a->ai_next;
            int s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s < 0) {
                connect_error = std::strerror(errno);
                continue;
            }
            set_cloexec(s);
            if (!set_nonblock(s)) {
                connect_error = std::strerror(errno);
                ::close(s);
                continue;
            }
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
#ifdef SO_NOSIGPIPE
            setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            int rc;
            do {
                rc = ::connect(s, a->ai_addr, a->ai_addrlen);
            } while (rc != 0 && errno == EINTR);
            if (rc == 0) {
                fd = s;
                on_connected();
                return;
            }
            if (errno == EINPROGRESS) {
                fd = s;
                connecting = true;
                connect_started = now;
                return;
            }
            connect_error = std::strerror(errno);
            ::close(s);
        }
        free_addrs();
        schedule_retry(L.target_ + ": " + (connect_error.empty() ? "cannot connect" : connect_error));
    }

    void start_connect() {
        if (kind == Kind::Serial) {
            open_serial_now();
            return;
        }
        free_addrs();
        connect_error.clear();
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICSERV | AI_NUMERICHOST;
        char service[16];
        std::snprintf(service, sizeof service, "%d", port);
        // A numeric address never touches the resolver. Names do: that lookup
        // is the one call here that can take a while (see the header caveat in
        // the report); "localhost" only reads /etc/hosts.
        int rc = getaddrinfo(host.c_str(), service, &hints, &ai);
        if (rc != 0) {
            ai = nullptr;
            hints.ai_flags = AI_NUMERICSERV;
            rc = getaddrinfo(host.c_str(), service, &hints, &ai);
        }
        if (rc != 0 || !ai) {
            ai = nullptr;
            schedule_retry(host + ": " + (rc != 0 ? gai_strerror(rc) : "no address"));
            return;
        }
        ai_next = ai;
        connect_next();
    }

    void check_connect() {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int rc = ::poll(&pfd, 1, 0);
        if (rc == 0 || (rc < 0 && errno == EINTR)) {
            if (since(connect_started) < kConnectTimeoutMs) return;
            connect_error = "connection timed out";
            close_fd();
            connect_next();
            return;
        }
        int err = 0;
        socklen_t len = sizeof err;
        if (rc < 0) err = errno;
        else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) err = errno;
        if (err == 0) {
            free_addrs();
            on_connected();
            return;
        }
        connect_error = std::strerror(err);
        close_fd();
        connect_next();
    }

    // ── writing ─────────────────────────────────────────────────────────────
    bool enqueue(std::string bytes, uint32_t packet_id, bool force = false) {
        if (!force && txq.size() >= kMaxTxFrames) return false;
        OutFrame f;
        f.bytes = std::move(bytes);
        f.packet_id = packet_id;
        txq.push_back(std::move(f));
        return true;
    }

    void flush_tx() {
        while (connected() && !txq.empty()) {
            const std::string& b = txq.front().bytes;
            ssize_t n = write_some(b.data() + tx_off, b.size() - tx_off);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                drop(std::string("write: ") + std::strerror(errno));
                return;
            }
            last_tx = now;
            tx_off += (size_t)n;
            if (tx_off >= b.size()) {
                txq.pop_front();
                tx_off = 0;
            } else if (n == 0) {
                return;
            }
        }
    }

    // ── reading ─────────────────────────────────────────────────────────────
    void read_rx() {
        size_t total = 0;
        while (connected() && total < kMaxReadPerPoll) {
            char buf[4096];
            ssize_t n = ::read(fd, buf, sizeof buf);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                drop(std::string("read: ") + std::strerror(errno));
                return;
            }
            if (n == 0) {
                drop("the radio closed the connection");
                return;
            }
            total += (size_t)n;
            last_rx = now;
            rx.append(buf, (size_t)n);
            parse_rx();
        }
    }

    // Pull every complete frame out of `rx`. Anything that is not a frame
    // (the node's debug log text, line noise) is skipped a byte at a time.
    void parse_rx() {
        size_t pos = 0;
        const size_t n = rx.size();
        const uint8_t* d = (const uint8_t*)rx.data();
        while (pos < n) {
            const void* hit = std::memchr(d + pos, kStart1, n - pos);
            if (!hit) { pos = n; break; }
            pos = (size_t)((const uint8_t*)hit - d);
            if (n - pos < 2) break;
            if (d[pos + 1] != kStart2) { ++pos; continue; }
            if (n - pos < 4) break;
            size_t len = ((size_t)d[pos + 2] << 8) | d[pos + 3];
            if (len > kMaxFrame) { ++pos; continue; }
            if (n - pos < 4 + len) break;
            if (!well_formed(Reader(d + pos + 4, len))) {
                // Not a FromRadio: a false header inside garbage. Rescan right
                // behind it instead of throwing `len` bytes away, so a real
                // frame that follows the garbage is not swallowed.
                pos += 2;
                continue;
            }
            // Copy out: handling a frame must not depend on `rx` staying put.
            std::string msg((const char*)d + pos + 4, len);
            pos += 4 + len;
            handle_from_radio((const uint8_t*)msg.data(), msg.size());
        }
        rx.erase(0, pos);
    }

    void handle_from_radio(const uint8_t* data, size_t len) {
        Reader r(data, len);
        uint32_t f, w;
        while (r.next(f, w)) {
            if (w == kWireLen) {
                Reader sub = r.bytes();
                if (!r.ok) return;
                switch (f) {
                case kFromRadioPacket: handle_packet(sub); break;
                case kFromRadioMyInfo: handle_my_info(sub); break;
                case kFromRadioNodeInfo: handle_node_info(sub); break;
                case kFromRadioConfig: handle_config(sub); break;
                case kFromRadioChannel: handle_channel(sub); break;
                case kFromRadioQueueStatus: handle_queue_status(sub); break;
                default: break;
                }
            } else if (w == kWireVarint) {
                uint64_t v = r.varint();
                if (!r.ok) return;
                if (f == kFromRadioConfigCompleteId) {
                    if (L.state_ == State::Syncing && config_id != 0 && v == config_id) {
                        backoff = kBackoffMinMs;
                        set_state(State::Ready);
                        send_heartbeat();
                    }
                } else if (f == kFromRadioRebooted) {
                    // The node restarted under us: its state is gone, sync again.
                    if (v != 0 && L.state_ != State::Off) {
                        set_state(State::Syncing);
                        last_rx = now;
                        start_handshake();
                    }
                }
            } else {
                r.skip(w);
            }
        }
    }

    void send_heartbeat() {
        std::string m;
        put_bytes(m, kToRadioHeartbeat, std::string());
        enqueue(frame(m), 0, true);
    }

    void handle_my_info(Reader r) {
        uint32_t f, w, num = 0;
        while (r.next(f, w)) {
            if (f == kMyNodeInfoMyNodeNum && w == kWireVarint) num = (uint32_t)r.varint();
            else r.skip(w);
        }
        if (!r.ok || num == 0 || num == kBroadcast) return;
        if (L.my_node_ != num) {
            L.my_node_ = num;
            ++L.nodes_rev_;
            for (MeshNode& n : L.nodes_) n.is_me = (n.num == num);
        }
        edit_node(num, [](MeshNode& n) { n.is_me = true; });
    }

    void handle_node_info(Reader r) {
        uint32_t f, w, num = 0, last_heard = 0;
        bool has_user = false, has_hops = false, has_metrics = false;
        PbUser user;
        float snr = 0.0f;
        int hops = -1, battery = -1;
        while (r.next(f, w)) {
            if (f == kNodeInfoNum && w == kWireVarint) num = (uint32_t)r.varint();
            else if (f == kNodeInfoUser && w == kWireLen) {
                Reader sub = r.bytes();
                if (!r.ok) break;
                user = PbUser();
                if (!decode_user(sub, user)) return;
                has_user = true;
            } else if (f == kNodeInfoSnr && w == kWireFixed32) snr = r.f32();
            else if (f == kNodeInfoLastHeard && w == kWireFixed32) last_heard = r.fixed32();
            else if (f == kNodeInfoDeviceMetrics && w == kWireLen) {
                Reader sub = r.bytes();
                if (!r.ok) break;
                if (!decode_device_metrics(sub, battery)) return;
                has_metrics = true;
            } else if (f == kNodeInfoHopsAway && w == kWireVarint) {
                uint64_t v = r.varint();
                hops = v > 64 ? 64 : (int)v;
                has_hops = true;
            } else r.skip(w);
        }
        if (!r.ok || num == 0) return;
        // A NodeInfo is the radio's whole record of the node: take all of it.
        edit_node(num, [&](MeshNode& n) {
            if (has_user) {
                n.long_name = user.long_name;
                n.short_name = user.short_name;
            }
            n.snr = snr;
            n.last_heard = last_heard;
            n.hops_away = has_hops ? hops : -1;
            if (has_metrics && battery >= 0) n.battery = battery;
            n.is_me = (num == L.my_node_);
        });
    }

    void handle_config(Reader r) {
        uint32_t f, w;
        while (r.next(f, w)) {
            if (f == kConfigLora && w == kWireLen) {
                Reader lora = r.bytes();
                if (!r.ok) return;
                uint32_t lf, lw, hop = 0, use_preset = 0, preset = 0;
                while (lora.next(lf, lw)) {
                    if (lf == kLoraHopLimit && lw == kWireVarint) hop = (uint32_t)lora.varint();
                    else if (lf == kLoraUsePreset && lw == kWireVarint) use_preset = (uint32_t)lora.varint();
                    else if (lf == kLoraModemPreset && lw == kWireVarint) preset = (uint32_t)lora.varint();
                    else lora.skip(lw);
                }
                if (lora.ok) {
                    lora_hop_limit = hop <= 7 ? hop : 0;
                    lora_preset = use_preset ? (int)preset : -2;
                    update_channel_name();
                }
            } else r.skip(w);
        }
    }

    // The primary channel (index 0). Its name is what sets the channel hash
    // the radio decodes with; an empty name means "the preset's name".
    void handle_channel(Reader r) {
        uint32_t f, w, index = 0;
        std::string name;
        while (r.next(f, w)) {
            if (f == kChannelIndex && w == kWireVarint) index = (uint32_t)r.varint();
            else if (f == kChannelSettings && w == kWireLen) {
                Reader st = r.bytes();
                if (!r.ok) return;
                uint32_t sf, sw;
                while (st.next(sf, sw)) {
                    if (sf == kChannelSettingsName && sw == kWireLen) {
                        Reader nm = st.bytes();
                        if (st.ok) name = clean_name(nm);
                    }
                    else st.skip(sw);
                }
            } else r.skip(w);
        }
        if (!r.ok || index != 0) return;
        ch0_name = name;
        ch0_known = true;
        update_channel_name();
    }

    // As Meshtastic apps show it: the channel's own name, else the preset's.
    void update_channel_name() {
        static const char* const kPresets[] = {"LongFast", "LongSlow", "VLongSlow", "MediumSlow", "MediumFast",
                                               "ShortSlow", "ShortFast", "LongMod", "ShortTurbo", "LongTurbo"};
        std::string n;
        if (!ch0_name.empty()) n = ch0_name;
        else if (lora_preset == -2) n = "Custom";
        else if (lora_preset >= 0 && lora_preset < (int)(sizeof kPresets / sizeof *kPresets)) n = kPresets[lora_preset];
        else if (ch0_known) n = "LongFast";
        if (n != L.channel_name_) {
            L.channel_name_ = n;
            ++L.nodes_rev_;
        }
    }

    void finish_pending(uint32_t id, bool ok, int error) {
        auto it = std::find_if(pending.begin(), pending.end(),
                               [&](const Pending& p) { return p.id == id; });
        if (it == pending.end()) return;
        pending.erase(it);
        report_ack(id, ok, error);
    }

    // The radio's verdict on queueing a packet we handed it. `res` is a
    // Routing.Error in the firmware (0 = queued); a failure here means the
    // packet never went on air.
    void handle_queue_status(Reader r) {
        uint32_t f, w, packet_id = 0;
        int64_t res = 0;
        while (r.next(f, w)) {
            if (f == kQueueStatusRes && w == kWireVarint) res = (int64_t)r.varint();
            else if (f == kQueueStatusMeshPacketId && w == kWireVarint) packet_id = (uint32_t)r.varint();
            else r.skip(w);
        }
        if (!r.ok || packet_id == 0 || res == 0) return;
        int err = (res > 0 && res < 1000) ? (int)res : kErrNoInterface;
        finish_pending(packet_id, false, err);
    }

    bool seen_text(uint32_t from, uint32_t id) {
        if (id == 0) return false;
        for (size_t i = 0; i < kSeenTexts; ++i)
            if (seen_id[i] == id && seen_from[i] == from) return true;
        seen_from[seen_pos] = from;
        seen_id[seen_pos] = id;
        seen_pos = (seen_pos + 1) % kSeenTexts;
        return false;
    }

    void handle_packet(Reader r) {
        PbPacket pk;
        if (!decode_packet(r, pk)) return;

        const uint32_t me = L.my_node_;
        // from == 0 means "this node" in packets the radio made itself.
        const uint32_t from = pk.from != 0 ? pk.from : me;
        const bool from_me = (from == me);

        // Anything heard on air, readable or not, shows the sender is alive.
        if (pk.from != 0 && pk.from != kBroadcast && !from_me) {
            uint32_t heard = pk.rx_time;
            if (heard == 0) {
                std::time_t t = std::time(nullptr);
                if (t > 1600000000) heard = (uint32_t)t;     // only a set clock
            }
            edit_node(pk.from, [&](MeshNode& n) {
                if (heard > n.last_heard) n.last_heard = heard;
                if (!pk.via_mqtt) {
                    if (pk.has_snr) n.snr = pk.rx_snr;
                    if (pk.hop_start != 0 && pk.hop_start >= pk.hop_limit)
                        n.hops_away = (int)std::min<uint32_t>(pk.hop_start - pk.hop_limit, 64);
                }
            });
        }
        if (!pk.has_decoded) return;

        const PbData& d = pk.data;
        switch (d.portnum) {
        case kPortText: {
            if (from == 0 || from_me) return;                // our own text echoed back
            if (pk.to != kBroadcast && me != 0 && pk.to != me) return;   // not for us
            if (d.payload.left() == 0) return;
            if (seen_text(from, pk.id)) return;
            Event e;
            e.type = Event::TextEv;
            MeshText& t = e.text;
            t.from = from;
            t.to = pk.to;
            t.id = pk.id;
            t.channel = pk.channel <= 255 ? (int)pk.channel : 0;
            t.text = clean_utf8(d.payload.p, d.payload.left(), kMaxFrame, true);
            t.snr = pk.has_snr ? pk.rx_snr : 0.0f;
            t.hops = (pk.hop_start != 0 && pk.hop_start >= pk.hop_limit)
                         ? (int)std::min<uint32_t>(pk.hop_start - pk.hop_limit, 64) : -1;
            t.rx_time = pk.rx_time;
            if (t.text.empty()) return;
            push(std::move(e));
            break;
        }
        case kPortNodeInfo: {
            PbUser u;
            if (from == 0 || !decode_user(d.payload, u)) return;
            edit_node(from, [&](MeshNode& n) {
                if (u.has_long) n.long_name = u.long_name;
                if (u.has_short) n.short_name = u.short_name;
            });
            break;
        }
        case kPortTelemetry: {
            if (from == 0) return;
            Reader t = d.payload;
            uint32_t f, w;
            int battery = -1;
            while (t.next(f, w)) {
                if (f == kTelemetryDeviceMetrics && w == kWireLen) {
                    Reader sub = t.bytes();
                    if (!t.ok) break;
                    if (!decode_device_metrics(sub, battery)) return;
                } else t.skip(w);
            }
            if (!t.ok || battery < 0) return;
            edit_node(from, [&](MeshNode& n) { n.battery = battery; });
            break;
        }
        case kPortRouting: {
            if (d.request_id == 0) return;
            auto it = std::find_if(pending.begin(), pending.end(),
                                   [&](const Pending& p) { return p.id == d.request_id; });
            if (it == pending.end()) return;
            Reader rt = d.payload;
            uint32_t f, w;
            uint64_t err = 0;
            while (rt.next(f, w)) {
                if (f == kRoutingErrorReason && w == kWireVarint) err = rt.varint();
                else rt.skip(w);
            }
            if (!rt.ok) return;
            if (err != 0) {
                finish_pending(d.request_id, false, err < 1000 ? (int)err : kErrNoInterface);
            } else if (it->to == kBroadcast || from == it->to) {
                finish_pending(d.request_id, true, kErrNone);
            }
            // else: an implicit ack for a direct message (our own radio heard
            // a relay). The packet is on its way but the destination has not
            // confirmed it; keep waiting for the real ack.
            break;
        }
        default:
            break;
        }
    }

    // ── timers ──────────────────────────────────────────────────────────────
    void expire_pending() {
        for (size_t i = 0; i < pending.size();) {
            if (now >= pending[i].deadline) {
                uint32_t id = pending[i].id;
                pending.erase(pending.begin() + (long)i);
                report_ack(id, false, kErrTimeout);
            } else {
                ++i;
            }
        }
    }

    void run(uint64_t now_ms) {
        now = now_ms;
        if (kind != Kind::None) {
            if (fd < 0 && now >= next_attempt) start_connect();
            if (fd >= 0 && connecting) check_connect();
            if (connected()) read_rx();
            if (connected()) {
                if (want_config_due && now >= want_config_at) {
                    want_config_due = false;
                    start_handshake();
                }
                if (L.state_ == State::Syncing && since(last_rx) >= kSyncSilenceMs) {
                    drop("no answer from the radio");
                } else if (L.state_ == State::Ready && since(last_tx) >= kHeartbeatMs && txq.empty()) {
                    send_heartbeat();
                }
            }
            if (connected()) flush_tx();
        }
        expire_pending();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
bool MeshText::direct() const { return to != MeshLink::kBroadcast; }

MeshLink::MeshLink() : impl_(new Impl(*this)) {}

MeshLink::~MeshLink() {
    delete impl_;
    impl_ = nullptr;
}

void MeshLink::open_tcp(const std::string& host, int port) {
    Impl& im = *impl_;
    if (port <= 0 || port > 65535) port = kDefaultPort;
    std::string target;
    if (host.find(':') != std::string::npos) target = "tcp:[" + host + "]:" + std::to_string(port);
    else target = "tcp:" + host + ":" + std::to_string(port);

    im.reset(host.empty());
    if (host.empty()) {
        last_error_ = "no host";
        return;
    }
    if (target != target_) im.forget_nodes();
    target_ = target;
    last_error_.clear();
    im.kind = Impl::Kind::Tcp;
    im.host = host;
    im.port = port;
    im.set_state(State::Connecting);
}

void MeshLink::open_serial(const std::string& device, int baud) {
    Impl& im = *impl_;
    im.reset(device.empty());
    if (device.empty()) {
        last_error_ = "no device";
        return;
    }
    std::string target = "serial:" + device;
    if (target != target_) im.forget_nodes();
    target_ = target;
    last_error_.clear();
    im.kind = Impl::Kind::Serial;
    im.device = device;
    speed_t unused;
    im.baud = baud_constant(baud, unused) ? baud : 115200;
    im.set_state(State::Connecting);
}

bool MeshLink::open(const std::string& spec) {
    size_t a = spec.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    size_t b = spec.find_last_not_of(" \t\r\n");
    std::string s = spec.substr(a, b - a + 1);
    for (unsigned char c : s)
        if (c <= 0x20 || c == 0x7f) return false;

    if (s.compare(0, 7, "serial:") == 0) {
        std::string dev = s.substr(7);
        if (dev.empty()) return false;
        open_serial(dev);
        return true;
    }
    if (s[0] == '/') {
        open_serial(s);
        return true;
    }
    if (s.compare(0, 4, "tcp:") == 0) {
        s = s.substr(4);
        if (s.compare(0, 2, "//") == 0) s = s.substr(2);
    }
    if (s.empty()) return false;

    std::string host, port_text;
    if (s[0] == '[') {
        size_t close_br = s.find(']');
        if (close_br == std::string::npos) return false;
        host = s.substr(1, close_br - 1);
        std::string rest = s.substr(close_br + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') return false;
            port_text = rest.substr(1);
            if (port_text.empty()) return false;
        }
    } else {
        size_t colons = (size_t)std::count(s.begin(), s.end(), ':');
        if (colons == 1) {
            size_t c = s.find(':');
            host = s.substr(0, c);
            port_text = s.substr(c + 1);
            if (port_text.empty()) return false;
        } else {
            host = s;                    // a name, an IPv4 or a bare IPv6 address
        }
    }
    if (host.empty() || host.size() > 253) return false;
    for (unsigned char c : host) {
        bool fine = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    c == '.' || c == '-' || c == '_' || c == ':' || c == '%';
        if (!fine) return false;
    }
    int port = kDefaultPort;
    if (!port_text.empty()) {
        if (port_text.size() > 5) return false;
        port = 0;
        for (char c : port_text) {
            if (c < '0' || c > '9') return false;
            port = port * 10 + (c - '0');
        }
        if (port < 1 || port > 65535) return false;
    }
    open_tcp(host, port);
    return true;
}

void MeshLink::close() { impl_->reset(true); }

void MeshLink::poll(uint64_t now_ms) {
    Impl& im = *impl_;
    if (im.in_poll) return;              // a callback called poll(): ignore
    im.in_poll = true;
    im.run(now_ms);
    im.dispatch();
    im.in_poll = false;
}

const MeshNode* MeshLink::node(uint32_t num) const {
    for (const MeshNode& n : nodes_)
        if (n.num == num) return &n;
    return nullptr;
}

uint32_t MeshLink::send_text(uint32_t to, const std::string& text, int channel) {
    Impl& im = *impl_;
    if (state_ != State::Ready || !im.connected()) return 0;
    if (text.empty() || text.size() > kMaxTextBytes) return 0;
    if (to == 0 || channel < 0 || channel > kMaxChannel) return 0;
    if (im.txq.size() >= kMaxTxFrames || im.pending.size() >= kMaxPending) return 0;

    uint32_t id;
    for (;;) {
        id = im.random_id();
        bool clash = false;
        for (const Impl::Pending& p : im.pending) clash = clash || p.id == id;
        if (!clash) break;
    }

    std::string data;
    put_uint(data, kDataPortnum, kPortText);
    put_bytes(data, kDataPayload, text);

    // `from` is left out: the radio fills in its own node number.
    std::string pkt;
    put_fixed32(pkt, kPacketTo, to);
    if (channel != 0) put_uint(pkt, kPacketChannel, (uint32_t)channel);
    put_bytes(pkt, kPacketDecoded, data);
    put_fixed32(pkt, kPacketId, id);
    if (im.lora_hop_limit != 0) put_uint(pkt, kPacketHopLimit, im.lora_hop_limit);
    put_uint(pkt, kPacketWantAck, 1);
    put_uint(pkt, kPacketPriority, kPriorityReliable);

    std::string msg;
    put_bytes(msg, kToRadioPacket, pkt);
    if (msg.size() > kMaxFrame) return 0;
    if (!im.enqueue(frame(msg), id)) return 0;

    Impl::Pending p;
    p.id = id;
    p.to = to;
    p.deadline = im.now + kAckTimeoutMs;
    im.pending.push_back(p);
    return id;
}

std::string MeshLink::node_id_text(uint32_t num) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "!%08x", (unsigned)num);
    return buf;
}

MeshLink& mesh_link() {
    static MeshLink link;
    return link;
}

} // namespace mm
