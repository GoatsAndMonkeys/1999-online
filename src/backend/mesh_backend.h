// mesh_backend.h — the glue between the messenger model and the radio.
//
// mm::mesh_backend() owns no data of its own: it opens mm::mesh_link() (the
// Meshtastic client, mesh_link.h), pumps it from the UI tick and copies what
// the radio knows into mm::messenger() (messenger.h), which stays
// radio-agnostic.
//
// WHICH RADIO (start()), first match wins:
//   1. env PC1999_MESH
//   2. settings key "mesh.target" (settings.json)
//   3. the default: "tcp:localhost:4403" on Linux (meshtasticd on the device),
//      off on macOS
// Values: "tcp:host[:port]", "serial:/dev/ttyACM0" (anything MeshLink::open()
// takes), or "off" / "none" / "0" / "" to stay in demo mode.
// start() does nothing when Options::demo is set (--demo) or when files are
// disabled (store::enabled() == false: --shots, --icon).
//
// DEMO -> LIVE: until the link has been Ready once the messenger stays in demo
// mode exactly as without a radio. On the first Ready: Messenger::go_live()
// (demo contacts and demo chats go, saved conversations stay), my name becomes
// my node's long name, and from then on:
//   - MeshLink::nodes() is mirrored into the contacts whenever it changes and
//     every kRefreshMs (presence ages). Name = long name, else short name, else
//     "!hexid". Presence: heard in the last 15 min = Online, up to 2 h = Away,
//     else Offline. Trainer fields and the away message a contact already has
//     are kept. My own node is not a contact. Nodes the radio forgets stay in
//     the list (offline).
//   - a direct text goes to that node's chat; a channel text (any channel
//     index; "[ch N] " is put in front for N != 0) goes to the one channel
//     conversation, Messenger::kChannelNode; my own echoes and texts between
//     other nodes are ignored. An unknown sender becomes a contact at once.
//   - Messenger::send() reaches the radio through the sender hook installed
//     here (kChannelNode = broadcast on channel 0) and delivery reports go to
//     Messenger::ack_packet().
// Once live it stays live: when the radio goes away the contacts and chats
// stay, sends are kept as "not sent" (see messenger.h) and status() says so.
//
// LVGL-free. Call everything from the UI thread.
#pragma once

#include <cstdint>
#include <string>

namespace mm {

class MeshBackend {
public:
    static constexpr uint32_t kRefreshMs = 30000;     // presence / "heard" refresh
    static constexpr uint32_t kOnlineS = 15 * 60;
    static constexpr uint32_t kAwayS = 2 * 3600;

    struct Options {
        bool demo = false;        // --demo: never touch a radio
    };
    enum class Status : uint8_t {
        Off,          // not started: the status strip keeps its old text ("DEMO")
        NoRadio,      // connecting / reconnecting
        Sync,         // connected, reading the node list
        Ready,
    };

    // Picks the radio (see above) and opens the link. false if it stays off.
    bool start(const Options& opt);
    bool start() { return start(Options{}); }
    void stop();
    bool on() const { return on_; }
    // Pumps the link and mirrors it into the messenger. `now_ms`: the UI's
    // 32-bit millisecond clock (wrap-around is handled).
    void tick(uint32_t now_ms);

    Status status() const;
    bool live() const { return live_; }               // has been Ready at least once
    int node_count() const;                           // nodes the radio knows, without me
    // For the status strip: "MESH  12", "SYNC", "NO RADIO"; "" when off.
    std::string status_text() const;
    // The spec start() would use: "" when it resolves to off.
    static std::string resolve_target();

private:
    void on_first_ready();
    void mirror_nodes();

    bool on_ = false;
    bool live_ = false;
    uint32_t seen_nodes_rev_ = 0;
    uint32_t last_ms_ = 0;
    uint64_t clock_ms_ = 0;       // now_ms widened
    uint64_t last_refresh_ms_ = 0;
};

MeshBackend& mesh_backend();

} // namespace mm
