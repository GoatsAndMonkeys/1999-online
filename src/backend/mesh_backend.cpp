#include "mesh_backend.h"

#include "mesh_link.h"
#include "messenger.h"
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace mm {
namespace {

static_assert(static_cast<size_t>(Messenger::kMaxMessageBytes) <= MeshLink::kMaxTextBytes,
              "a message the messenger accepts must fit one packet");
static_assert(Messenger::kChannelNode == MeshLink::kBroadcast,
              "the channel conversation uses the broadcast address");

bool is_off(const std::string& spec) {
    return spec.empty() || spec == "off" || spec == "none" || spec == "0" || spec == "no";
}

std::string node_name(const MeshNode& n) {
    if (!n.long_name.empty()) return n.long_name;
    if (!n.short_name.empty()) return n.short_name;
    return MeshLink::node_id_text(n.num);
}

bool same(const Contact& a, const Contact& b) {
    return a.name == b.name && a.presence == b.presence && a.snr == b.snr && a.hops == b.hops &&
           a.last_heard_s == b.last_heard_s && a.battery == b.battery;
}

// The messenger fields the radio knows about; everything else in `c` stays.
void fill_from_node(Contact& c, const MeshNode& n, uint32_t now_s) {
    c.node_id = n.num;
    c.name = node_name(n);
    c.snr = n.snr;
    c.hops = n.hops_away < 0 ? 0 : n.hops_away;
    c.battery = n.battery < 0 ? -1 : (n.battery > 100 ? 100 : n.battery);   // 101 = powered
    if (n.last_heard == 0) {
        c.last_heard_s = Contact::kNeverHeard;
        c.presence = Presence::Offline;
    } else {
        // A radio clock ahead of ours reads as "just now".
        c.last_heard_s = now_s > n.last_heard ? now_s - n.last_heard : 0;
        c.presence = c.last_heard_s <= MeshBackend::kOnlineS ? Presence::Online
                   : c.last_heard_s <= MeshBackend::kAwayS   ? Presence::Away
                                                             : Presence::Offline;
    }
}

void ensure_contact(uint32_t num) {
    Messenger& m = messenger();
    if (num == 0 || Messenger::is_channel(num) || m.contact(num)) return;
    Contact c;
    c.node_id = num;
    c.name = MeshLink::node_id_text(num);
    c.presence = Presence::Online;   // we are hearing it right now
    if (const MeshNode* n = mesh_link().node(num)) {
        fill_from_node(c, *n, static_cast<uint32_t>(std::time(nullptr)));
        c.presence = Presence::Online;
        c.last_heard_s = 0;
    }
    m.upsert_contact(c);
}

} // namespace

MeshBackend& mesh_backend() {
    static MeshBackend b;
    return b;
}

std::string MeshBackend::resolve_target() {
    std::string spec;
    if (const char* env = std::getenv("PC1999_MESH")) {
        spec = env;
    } else if (settings().has("mesh.target")) {
        spec = settings().get_string("mesh.target");
    } else {
#if defined(__linux__)
        spec = "tcp:localhost:4403";
#endif
    }
    return is_off(spec) ? std::string() : spec;
}

bool MeshBackend::start(const Options& opt) {
    if (on_) return true;
    if (opt.demo || !store::enabled()) return false;
    const std::string spec = resolve_target();
    if (spec.empty()) return false;

    MeshLink& link = mesh_link();
    link.set_text_callback([this](const MeshText& t) {
        if (!live_) return;                          // nothing is shown before the first Ready
        MeshLink& l = mesh_link();
        if (t.text.empty() || t.from == 0 || t.from == l.my_node()) return;
        const int hops = t.hops < 0 ? 0 : t.hops;
        if (t.to == MeshLink::kBroadcast) {
            ensure_contact(t.from);
            messenger().receive_channel(
                t.from, t.channel == 0 ? t.text : "[ch " + std::to_string(t.channel) + "] " + t.text,
                hops, t.snr);
        } else if (t.direct()) {
            ensure_contact(t.from);
            messenger().receive(t.from, t.text, hops, t.snr);
        }
    });
    link.set_ack_callback([](uint32_t packet_id, bool ok, int error) {
        messenger().ack_packet(packet_id, ok, error);
    });
    messenger().set_sender([](uint32_t node, const std::string& text) -> uint32_t {
        MeshLink& l = mesh_link();
        if (!l.ready()) return 0;
        return l.send_text(Messenger::is_channel(node) ? MeshLink::kBroadcast : node, text, 0);
    });
    if (!link.open(spec)) {
        std::fprintf(stderr, "mesh: cannot use \"%s\" (PC1999_MESH / mesh.target)\n", spec.c_str());
        messenger().set_sender(nullptr);
        return false;
    }
    on_ = true;
    return true;
}

void MeshBackend::stop() {
    if (!on_) return;
    mesh_link().close();
    messenger().set_sender(nullptr);
    on_ = false;
}

void MeshBackend::tick(uint32_t now_ms) {
    if (!on_) return;
    clock_ms_ += static_cast<uint32_t>(now_ms - last_ms_);   // unsigned: survives the 49-day wrap
    last_ms_ = now_ms;

    MeshLink& link = mesh_link();
    link.poll(clock_ms_);
    if (!link.ready()) return;

    if (!live_) on_first_ready();
    if (link.nodes_revision() != seen_nodes_rev_ || clock_ms_ - last_refresh_ms_ >= kRefreshMs) {
        mirror_nodes();
    }
}

void MeshBackend::on_first_ready() {
    live_ = true;
    seen_nodes_rev_ = 0;
    messenger().go_live();
}

void MeshBackend::mirror_nodes() {
    MeshLink& link = mesh_link();
    Messenger& m = messenger();
    seen_nodes_rev_ = link.nodes_revision();
    last_refresh_ms_ = clock_ms_;
    const uint32_t now_s = static_cast<uint32_t>(std::time(nullptr));

    for (const MeshNode& n : link.nodes()) {
        if (n.num == 0 || Messenger::is_channel(n.num)) continue;
        if (n.is_me || n.num == link.my_node()) {
            m.set_my_name(node_name(n));
            continue;
        }
        const Contact* have = m.contact(n.num);
        Contact c = have ? *have : Contact{};        // keeps trainer fields and the away message
        fill_from_node(c, n, now_s);
        if (!have || !same(*have, c)) m.upsert_contact(c);
    }
    if (link.my_node() != 0 && !link.node(link.my_node())) {
        m.set_my_name(MeshLink::node_id_text(link.my_node()));
    }
}

MeshBackend::Status MeshBackend::status() const {
    if (!on_) return Status::Off;
    switch (mesh_link().state()) {
        case MeshLink::State::Ready:   return Status::Ready;
        case MeshLink::State::Syncing: return Status::Sync;
        default:                       return Status::NoRadio;
    }
}

int MeshBackend::node_count() const {
    const MeshLink& link = mesh_link();
    int n = 0;
    for (const MeshNode& node : link.nodes()) {
        if (!node.is_me && node.num != link.my_node()) ++n;
    }
    return n;
}

std::string MeshBackend::status_text() const {
    switch (status()) {
        case Status::Ready:   return "MESH  " + std::to_string(node_count());
        case Status::Sync:    return "SYNC";
        case Status::NoRadio: return "NO RADIO";
        default:              return std::string();
    }
}

} // namespace mm
