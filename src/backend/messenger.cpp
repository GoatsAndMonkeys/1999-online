#include "messenger.h"

#include "mesh_link.h"

#include "mm_json.h"
#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace mm {
namespace {

uint32_t now_s() {
    return static_cast<uint32_t>(std::time(nullptr));
}

const char* const kDemoReplies[] = {
    "lol nice",
    "omg same",
    "k, copy that",
    "brb, someone needs the phone line",
    "u coming up the ridge l8r?",
    "signal is so good 2day",
    "ttyl!!",
};
constexpr uint32_t kDemoReplyCount = sizeof(kDemoReplies) / sizeof(kDemoReplies[0]);

void set_state(Message& m, SendState st, int error = kSendErrNone) {
    m.state = st;
    m.delivered = st == SendState::Delivered;
    m.error = error;
    if (st != SendState::Sending && st != SendState::Sent) m.packet_id = 0;
}

const char* state_word(SendState st) {
    switch (st) {
        case SendState::Sending: return "sending";
        case SendState::Sent:    return "sent";
        case SendState::Failed:  return "failed";
        case SendState::Unsent:  return "unsent";
        default:                 return "";
    }
}

// "!a1b2c3d4" or "a1b2c3d4" -> the number; false if it is not one.
bool parse_node(std::string text, uint32_t& out) {
    if (!text.empty() && text[0] == '!') text.erase(0, 1);
    char* end = nullptr;
    const unsigned long id = std::strtoul(text.c_str(), &end, 16);
    if (text.empty() || !end || *end != '\0' || id > 0xfffffffful) return false;
    out = static_cast<uint32_t>(id);
    return true;
}

} // namespace

const char* send_error_text(int error) {
    switch (error) {
        case kSendErrNone:     return "";
        case kSendErrNoRadio:  return "radio not connected";
        case kSendErrNoAnswer: return "no answer";
        case kSendErrQueued:   return "waiting for them to come online";
        // Meshtastic Routing.Error
        case 1:  return "no route";
        case 2:  return "refused";
        case 3:  return "timed out";
        case 4:  return "no radio interface";
        case 5:  return "no answer after retries";
        case 6:  return "no such channel";
        case 7:  return "too long";
        case 8:  return "no response";
        case 9:  return "airtime limit reached";
        case 32: return "bad request";
        case 33: return "not authorized";
        case 34: return "encryption failed";
        case 35: return "unknown public key";
        case 38: return "sending too fast";
        default: return "mesh error";
    }
}

Messenger& messenger() {
    static Messenger m;
    return m;
}

Messenger::Messenger() {
    seed_demo();
    load();   // saved conversations replace the demo ones
}

Messenger::~Messenger() {
    if (chats_dirty_) save();
}

// ── Persistence ─────────────────────────────────────────────────────────────

bool Messenger::load() {
    JsonValue doc;
    if (store::load_json(store::path("conversations.json"), doc) != store::Load::Ok) return false;
    const JsonValue& list = doc["conversations"];
    if (!list.is_array()) return false;

    // Version 1 was written by demo-only builds: no per-chat "demo" flag, so
    // the chats with the built-in demo nodes are the demo chats.
    const bool v1 = doc.get_int("version", 1) < 2;

    chats_.clear();
    pending_.clear();
    inflight_.clear();
    for (const JsonValue& c : list.items()) {
        uint32_t node_id = 0;
        if (!parse_node(c.get_string("peer"), node_id)) continue;

        Chat chat;
        chat.demo = v1 ? is_demo_node(node_id) : c.get_bool("demo");
        for (const JsonValue& j : c["messages"].items()) {
            Message m;
            m.from_me = j.get_bool("me");
            m.text = j.get_string("text");
            m.timestamp = j.get_u32("t");
            m.hops = static_cast<int>(j.get_int("hops"));
            m.snr = static_cast<float>(j["snr"].as_double());
            m.auto_response = j.get_bool("auto");
            parse_node(j.get_string("from"), m.from_node);
            m.tries = static_cast<uint8_t>(std::clamp<int64_t>(j.get_int("tries"), 0, 255));
            m.via_outbox = j.get_bool("outbox");
            const std::string st = j.get_string("state");
            if (!m.from_me) {
                set_state(m, SendState::Delivered);
            } else if (st == "failed" || st == "unsent") {
                set_state(m, st == "failed" ? SendState::Failed : SendState::Unsent,
                          static_cast<int>(j.get_int("err")));
            } else if (st == "sending" || st == "sent") {
                set_state(m, SendState::Sent);   // the packet id did not survive the restart
            } else {
                set_state(m, j.get_bool("delivered", true) ? SendState::Delivered : SendState::Sent);
            }
            chat.messages.push_back(std::move(m));
        }
        if (chat.messages.size() > kMaxStoredMessages) {
            chat.messages.erase(chat.messages.begin(),
                                chat.messages.end() - static_cast<long>(kMaxStoredMessages));
        }
        chat.unread = std::max(0, static_cast<int>(c.get_int("unread")));
        chats_[node_id] = std::move(chat);

        // A peer we have not heard this session still needs its name.
        const std::string name = c.get_string("name");
        if (!is_channel(node_id) && !contact(node_id) && !name.empty()) {
            Contact k;
            k.node_id = node_id;
            k.name = name;
            k.presence = Presence::Offline;
            contacts_.push_back(k);
        }
    }
    chats_dirty_ = false;
    save_armed_ = false;
    bump();
    return true;
}

bool Messenger::save() {
    save_armed_ = false;
    if (!store::enabled()) {
        chats_dirty_ = false;   // nothing to retry in screenshot mode
        return false;
    }
    std::string o = "{\n  \"version\": 2,\n  \"conversations\": [";
    bool first_chat = true;
    for (const auto& kv : chats_) {
        char head[32];
        std::snprintf(head, sizeof(head), "!%08x", static_cast<unsigned>(kv.first));
        o += first_chat ? "\n" : ",\n";
        first_chat = false;
        const Contact* c = contact(kv.first);
        o += "    {\"peer\": " + store::quote(head) +
             ", \"name\": " + store::quote(c ? c->name : std::string()) +
             (kv.second.demo ? ", \"demo\": true" : "") +
             ", \"unread\": " + std::to_string(kv.second.unread) + ", \"messages\": [";
        const auto& msgs = kv.second.messages;
        const size_t from = msgs.size() > kMaxStoredMessages ? msgs.size() - kMaxStoredMessages : 0;
        for (size_t i = from; i < msgs.size(); ++i) {
            const Message& m = msgs[i];
            char num[96];
            std::snprintf(num, sizeof(num), "\"t\": %u, \"hops\": %d, \"snr\": %.2f",
                          static_cast<unsigned>(m.timestamp), m.hops, static_cast<double>(m.snr));
            o += i == from ? "\n" : ",\n";
            o += std::string("      {\"me\": ") + (m.from_me ? "true" : "false") + ", " + num +
                 ", \"delivered\": " + (m.delivered ? "true" : "false");
            if (m.from_me && m.state != SendState::Delivered) {
                o += std::string(", \"state\": \"") + state_word(m.state) + "\"";
                if (m.error != kSendErrNone) o += ", \"err\": " + std::to_string(m.error);
            }
            if (m.from_me && m.tries > 0) o += ", \"tries\": " + std::to_string(m.tries);
            if (m.from_me && m.via_outbox) o += ", \"outbox\": true";
            if (m.from_node != 0) {
                char from[32];
                std::snprintf(from, sizeof(from), ", \"from\": \"!%08x\"", static_cast<unsigned>(m.from_node));
                o += from;
            }
            o += std::string(m.auto_response ? ", \"auto\": true" : "") +
                 ", \"text\": " + store::quote(m.text) + "}";
        }
        o += msgs.empty() ? "]}" : "\n    ]}";
    }
    o += first_chat ? "]\n}\n" : "\n  ]\n}\n";
    if (!store::write_atomic(store::path("conversations.json"), o)) return false;
    chats_dirty_ = false;
    return true;
}

// ── Read side ───────────────────────────────────────────────────────────────

const Contact* Messenger::contact(uint32_t node_id) const {
    if (is_channel(node_id)) return nullptr;
    for (const Contact& c : contacts_) {
        if (c.node_id == node_id) return &c;
    }
    return nullptr;
}

std::string Messenger::channel_name() {
    const std::string& n = mesh_link().channel_name();
    return n.empty() ? std::string("LongFast") : n;
}

std::string Messenger::name_of(uint32_t node_id) const {
    if (is_channel(node_id)) return channel_name();
    if (const Contact* c = contact(node_id)) {
        if (!c->name.empty()) return c->name;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "!%08x", static_cast<unsigned>(node_id));
    return buf;
}

bool Messenger::has_chat(uint32_t node_id) const {
    return chats_.find(node_id) != chats_.end();
}

const std::vector<Message>& Messenger::messages(uint32_t node_id) const {
    static const std::vector<Message> kNone;
    const auto it = chats_.find(node_id);
    return it == chats_.end() ? kNone : it->second.messages;
}

int Messenger::unread(uint32_t node_id) const {
    const auto it = chats_.find(node_id);
    return it == chats_.end() ? 0 : it->second.unread;
}

int Messenger::total_unread() const {
    int n = 0;
    for (const auto& kv : chats_) n += kv.second.unread;
    return n;
}

// ── Actions ─────────────────────────────────────────────────────────────────

Messenger::Chat& Messenger::chat_for(uint32_t node_id) {
    const auto it = chats_.find(node_id);
    if (it != chats_.end()) return it->second;
    Chat& chat = chats_[node_id];
    chat.demo = demo_ && is_demo_node(node_id);
    return chat;
}

void Messenger::transmit(uint32_t node_id, Message& m) {
    m.packet_id = 0;
    const uint32_t id = sender_ ? sender_(node_id, m.text) : 0;
    if (id == 0) {
        set_state(m, SendState::Unsent, kSendErrNoRadio);
        return;
    }
    // Nobody acks a broadcast: it is "sent" until a relay is heard.
    set_state(m, is_channel(node_id) ? SendState::Sent : SendState::Sending);
    m.packet_id = id;
    if (!is_channel(node_id)) {
        InFlight f;
        f.packet_id = id;
        f.node_id = node_id;
        inflight_.push_back(f);
    }
}

Message* Messenger::find_packet(uint32_t packet_id, uint32_t* node) {
    if (packet_id == 0) return nullptr;
    for (auto& kv : chats_) {
        auto& msgs = kv.second.messages;
        for (auto m = msgs.rbegin(); m != msgs.rend(); ++m) {
            if (m->from_me && m->packet_id == packet_id) {
                if (node) *node = kv.first;
                return &*m;
            }
        }
    }
    return nullptr;
}

bool Messenger::send(uint32_t node_id, const std::string& text) {
    if (text.empty() || text.size() > static_cast<size_t>(kMaxMessageBytes)) return false;
    Message m;
    m.from_me = true;
    m.text = text;
    m.timestamp = now_s();
    const Contact* c = contact(node_id);
    if (demo_ && is_demo_node(node_id)) {
        // Offline nodes are queued (never delivered); everyone else acks at once.
        if (c && c->presence == Presence::Offline) {
            set_state(m, SendState::Unsent, kSendErrQueued);
        } else {
            set_state(m, SendState::Delivered);
        }
        if (c) {
            m.hops = c->hops;
            m.snr = c->snr;
        }
        if (c && c->presence != Presence::Offline) {
            PendingReply p;
            p.node_id = node_id;
            pending_.push_back(p);
        }
    } else if (c && c->presence == Presence::Offline) {
        // Not heard for a long time: hold it in the outbox until it is.
        set_state(m, SendState::Unsent, kSendErrQueued);
    } else {
        transmit(node_id, m);
    }
    chat_for(node_id).messages.push_back(std::move(m));
    touch_chats();
    bump();
    return true;
}

int Messenger::retry_index(uint32_t node_id) const {
    const auto it = chats_.find(node_id);
    if (it == chats_.end()) return -1;
    const auto& msgs = it->second.messages;
    for (int i = static_cast<int>(msgs.size()) - 1; i >= 0; --i) {
        if (msgs[i].retryable()) return i;
    }
    return -1;
}

void Messenger::resend(uint32_t node_id, Message& m) {
    if (demo_ && is_demo_node(node_id)) {
        // Simulated, like send(): nothing goes on the air for a demo node.
        const Contact* c = contact(node_id);
        if (c && c->presence == Presence::Offline) {
            set_state(m, SendState::Unsent, kSendErrQueued);
        } else {
            set_state(m, SendState::Delivered);
            if (c) {
                m.hops = c->hops;
                m.snr = c->snr;
            }
        }
        return;
    }
    transmit(node_id, m);
}

bool Messenger::retry(uint32_t node_id) {
    return retry_message(node_id, retry_index(node_id));
}

bool Messenger::retry_message(uint32_t node_id, int index) {
    const auto it = chats_.find(node_id);
    if (it == chats_.end() || index < 0 || index >= static_cast<int>(it->second.messages.size())) return false;
    Message& m = it->second.messages[index];
    if (!m.retryable()) return false;
    // By hand: the outbox gets its whole budget back for this one.
    m.tries = 0;
    m.via_outbox = false;
    m.backoff = false;
    resend(node_id, m);
    touch_chats();
    bump();
    return true;
}

bool Messenger::delete_message(uint32_t node_id, int index) {
    const auto it = chats_.find(node_id);
    if (it == chats_.end() || index < 0 || index >= static_cast<int>(it->second.messages.size())) return false;
    auto& msgs = it->second.messages;
    const Message& m = msgs[index];
    if (m.from_me && m.packet_id != 0) {
        const uint32_t id = m.packet_id;
        inflight_.erase(std::remove_if(inflight_.begin(), inflight_.end(),
                                       [id](const InFlight& f) { return f.packet_id == id; }),
                        inflight_.end());
    }
    if (!m.from_me) {
        // Was it one of the unread ones (the newest `unread` incoming)?
        int newer = 0;
        for (size_t i = static_cast<size_t>(index) + 1; i < msgs.size(); ++i) newer += msgs[i].from_me ? 0 : 1;
        if (newer < it->second.unread) --it->second.unread;
    }
    msgs.erase(msgs.begin() + index);
    touch_chats();
    bump();
    return true;
}

// ── Outbox ──────────────────────────────────────────────────────────────────

bool Messenger::auto_send_pending(uint32_t node_id, const Message& m) const {
    if (!m.retryable() || is_channel(node_id)) return false;
    if (m.tries >= kOutboxMaxTries) return false;
    const uint32_t t = now_s();
    return !(m.timestamp != 0 && t > m.timestamp && t - m.timestamp > kOutboxMaxAgeS);
}

uint32_t Messenger::auto_send_wait_ms(const Message& m) const {
    if (!m.backoff) return 0;
    const int32_t left = static_cast<int32_t>(m.retry_after_ms - now_ms_);
    return left > 0 ? static_cast<uint32_t>(left) : 0;
}

int Messenger::outbox_count() const {
    int n = 0;
    for (const auto& kv : chats_) {
        for (const Message& m : kv.second.messages) {
            if (m.retryable() || (m.from_me && m.state == SendState::Sending)) ++n;
        }
    }
    return n;
}

void Messenger::wake_outbox(uint32_t node_id) {
    const auto it = chats_.find(node_id);
    if (it == chats_.end()) return;
    for (Message& m : it->second.messages) {
        if (m.retryable()) m.backoff = false;
    }
}

void Messenger::run_outbox(uint32_t now_ms) {
    if (!sender_ || demo_) return;   // no radio (demo, --shots): nothing goes out on its own
    if (auto_sent_ && now_ms - last_auto_ms_ < kOutboxGapMs) return;

    // The oldest queued message to an online contact with nothing in flight.
    uint32_t best_node = 0;
    Message* best = nullptr;
    for (auto& kv : chats_) {
        const uint32_t node = kv.first;
        const Contact* c = contact(node);
        if (!c || !c->online() || kv.second.demo) continue;
        bool busy = false;
        for (const Message& m : kv.second.messages) {
            if (m.from_me && m.state == SendState::Sending) busy = true;
        }
        if (busy) continue;
        for (Message& m : kv.second.messages) {
            if (!auto_send_pending(node, m)) continue;
            if (m.backoff && static_cast<int32_t>(now_ms - m.retry_after_ms) < 0) continue;
            if (!best || m.timestamp < best->timestamp) {
                best = &m;
                best_node = node;
            }
            break;   // in order within one chat
        }
    }
    if (!best) return;

    transmit(best_node, *best);
    if (best->state == SendState::Unsent) {
        // The link refused it (not ready): no try spent, look again later.
        best->backoff = true;
        best->retry_after_ms = now_ms + kOutboxGapMs;
    } else {
        ++best->tries;
        best->via_outbox = true;
        best->backoff = false;
        last_auto_ms_ = now_ms;
        auto_sent_ = true;
    }
    touch_chats();
    bump();
}

void Messenger::notify(MailNotice::Kind kind, uint32_t node_id, const std::string& text) {
    MailNotice n;
    n.kind = kind;
    n.node_id = node_id;
    n.text = text;
    if (notices_.size() >= kMaxNotices) notices_.erase(notices_.begin());
    notices_.push_back(std::move(n));
}

bool Messenger::pop_mail_notice(MailNotice& out) {
    if (notices_.empty()) return false;
    out = std::move(notices_.front());
    notices_.erase(notices_.begin());
    return true;
}

void Messenger::mark_read(uint32_t node_id) {
    const auto it = chats_.find(node_id);
    if (it == chats_.end() || it->second.unread == 0) return;
    it->second.unread = 0;
    touch_chats();
    bump();
}

void Messenger::delete_chat(uint32_t node_id) {
    const auto before = pending_.size();
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [node_id](const PendingReply& p) { return p.node_id == node_id; }),
                   pending_.end());
    inflight_.erase(std::remove_if(inflight_.begin(), inflight_.end(),
                                   [node_id](const InFlight& f) { return f.node_id == node_id; }),
                    inflight_.end());
    const bool erased = chats_.erase(node_id) > 0;
    if (erased) touch_chats();   // the next save leaves it out of the file
    if (erased || pending_.size() != before) bump();
}

void Messenger::tick(uint32_t now_ms) {
    now_ms_ = now_ms;
    if (!ticked_ || now_ms - outbox_check_ms_ >= 1000) {
        ticked_ = true;
        outbox_check_ms_ = now_ms;
        run_outbox(now_ms);
    }

    // Debounced save: at most one write per kSaveMs.
    if (!chats_dirty_) {
        save_armed_ = false;
    } else if (!save_armed_) {
        save_armed_ = true;
        save_since_ms_ = now_ms;
    } else if (now_ms - save_since_ms_ >= kSaveMs) {
        save();
    }

    // Sending for too long: the report is not coming any more.
    if (!inflight_.empty()) {
        std::vector<uint32_t> late;
        for (InFlight& f : inflight_) {
            if (!f.armed) {
                f.armed = true;
                f.due_ms = now_ms + kAckTimeoutMs;
            } else if (static_cast<int32_t>(now_ms - f.due_ms) >= 0) {
                late.push_back(f.packet_id);
            }
        }
        for (uint32_t id : late) {
            if (!ack_packet(id, false, kSendErrNoAnswer)) {
                inflight_.erase(std::remove_if(inflight_.begin(), inflight_.end(),
                                               [id](const InFlight& f) { return f.packet_id == id; }),
                                inflight_.end());
            }
        }
    }

    if (!demo_ || pending_.empty()) return;
    // Collect first: receive() must not run while iterating pending_.
    std::vector<uint32_t> due;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (!it->armed) {
            it->armed = true;
            it->due_ms = now_ms + kDemoReplyMs;
            ++it;
        } else if (static_cast<int32_t>(now_ms - it->due_ms) >= 0) {
            due.push_back(it->node_id);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    for (uint32_t id : due) {
        const Contact* c = contact(id);
        if (!c) continue;
        if (c->presence == Presence::Away) {
            receive(id, c->away_msg.empty() ? "I am away from my node right now." : c->away_msg,
                    c->hops, c->snr, true);
        } else {
            receive(id, kDemoReplies[reply_cursor_++ % kDemoReplyCount], c->hops, c->snr, false);
        }
    }
}

// ── Write side ──────────────────────────────────────────────────────────────

void Messenger::set_demo(bool on) {
    if (demo_ == on) return;
    demo_ = on;
    if (!on) pending_.clear();
    bump();
}

void Messenger::go_live() {
    if (!demo_) return;
    demo_ = false;
    pending_.clear();
    bool dropped = false;
    for (auto it = chats_.begin(); it != chats_.end();) {
        if (it->second.demo) {
            const uint32_t id = it->first;
            inflight_.erase(std::remove_if(inflight_.begin(), inflight_.end(),
                                           [id](const InFlight& f) { return f.node_id == id; }),
                            inflight_.end());
            it = chats_.erase(it);
            dropped = true;
        } else {
            ++it;
        }
    }
    // A demo contact goes unless a real (saved) chat happens to use its id.
    contacts_.erase(std::remove_if(contacts_.begin(), contacts_.end(),
                                   [this](const Contact& c) {
                                       return is_demo_node(c.node_id) && !has_chat(c.node_id);
                                   }),
                    contacts_.end());
    demo_nodes_.clear();
    if (dropped) touch_chats();   // the file must forget the demo chats too
    bump();
}

void Messenger::clear() {
    contacts_.clear();
    chats_.clear();
    pending_.clear();
    inflight_.clear();
    notices_.clear();
    demo_nodes_.clear();
    touch_chats();
    bump();
}

void Messenger::set_my_name(const std::string& name) {
    if (my_name_ == name) return;
    my_name_ = name;
    bump();
}

void Messenger::upsert_contact(const Contact& c) {
    if (is_channel(c.node_id)) return;   // the channel is not a contact
    for (Contact& have : contacts_) {
        if (have.node_id == c.node_id) {
            const bool back = !have.online() && c.online();
            have = c;
            if (back) wake_outbox(c.node_id);   // heard again: queued mail may go now
            bump();
            return;
        }
    }
    contacts_.push_back(c);
    if (c.online()) wake_outbox(c.node_id);
    bump();
}

void Messenger::receive(uint32_t node_id, const std::string& text, int hops, float snr,
                        bool auto_response) {
    Message m;
    m.text = text;
    m.timestamp = now_s();
    m.hops = hops;
    m.snr = snr;
    m.auto_response = auto_response;
    set_state(m, SendState::Delivered);
    add_message(node_id, m, true);
    wake_outbox(node_id);   // it is talking to us: it can hear us too
    notify(MailNotice::Kind::NewMail, node_id, text);
}

void Messenger::receive_channel(uint32_t from_node, const std::string& text, int hops, float snr) {
    Message m;
    m.text = text;
    m.timestamp = now_s();
    m.hops = hops;
    m.snr = snr;
    m.from_node = from_node;
    set_state(m, SendState::Delivered);
    add_message(kChannelNode, m, true);
}

void Messenger::add_message(uint32_t node_id, const Message& msg, bool unread) {
    Message m = msg;
    if (!m.from_me) m.state = SendState::Delivered;
    m.delivered = m.state == SendState::Delivered;
    Chat& chat = chat_for(node_id);
    chat.messages.push_back(std::move(m));
    if (unread) ++chat.unread;
    touch_chats();
    bump();
}

bool Messenger::ack_packet(uint32_t packet_id, bool ok, int error) {
    uint32_t node = 0;
    Message* m = find_packet(packet_id, &node);
    if (!m) return false;
    inflight_.erase(std::remove_if(inflight_.begin(), inflight_.end(),
                                   [packet_id](const InFlight& f) { return f.packet_id == packet_id; }),
                    inflight_.end());
    if (ok) {
        if (const Contact* c = contact(node)) {
            m->hops = c->hops;
            m->snr = c->snr;
        }
        set_state(*m, SendState::Delivered);
        if (m->via_outbox) notify(MailNotice::Kind::Delivered, node, m->text);
    } else {
        set_state(*m, SendState::Failed, error != kSendErrNone ? error : kSendErrNoAnswer);
        if (m->via_outbox) {
            // Back off before the outbox tries again: 30 s, 1 min, 2 min, ...
            const int shift = std::min(m->tries > 0 ? m->tries - 1 : 0, 10);
            const uint32_t wait = std::min(kOutboxBackoffMs << shift, kOutboxBackoffMaxMs);
            m->backoff = true;
            m->retry_after_ms = now_ms_ + wait;
        }
    }
    touch_chats();
    bump();
    return true;
}

void Messenger::ack(uint32_t node_id, int hops, float snr) {
    const auto it = chats_.find(node_id);
    if (it == chats_.end()) return;
    auto& msgs = it->second.messages;
    for (auto m = msgs.rbegin(); m != msgs.rend(); ++m) {
        if (m->from_me && !m->delivered) {
            set_state(*m, SendState::Delivered);
            m->hops = hops;
            m->snr = snr;
            touch_chats();
            bump();
            return;
        }
    }
}

// ── Demo seed ───────────────────────────────────────────────────────────────

void Messenger::seed_demo() {
    my_name_ = "xXmeshKid03Xx";
    contacts_.clear();
    chats_.clear();
    pending_.clear();
    inflight_.clear();
    demo_nodes_.clear();

    auto node = [this](uint32_t id, const char* name, Presence p, float snr, int hops,
                       uint32_t heard_s, int battery, const char* away = "") {
        Contact c;
        c.node_id = id;
        c.name = name;
        c.presence = p;
        c.snr = snr;
        c.hops = hops;
        c.last_heard_s = heard_s;
        c.battery = battery;
        c.away_msg = away;
        contacts_.push_back(c);
        demo_nodes_.insert(id);
        return &contacts_.back();
    };
    auto trainer = [](Contact* c, int badges, int lead, int party, int wins, int losses, bool gym) {
        c->is_trainer = true;
        c->badges = badges;
        c->lead_level = lead;
        c->party_size = party;
        c->pvp_wins = wins;
        c->pvp_losses = losses;
        c->hosts_gym = gym;
    };
    contacts_.reserve(16);   // keeps the Contact* returned by node() valid below

    // Trainers (nodes running MonsterMesh).
    constexpr uint32_t kKd2abc = 0xa3f29c01, kRidge = 0x7c11d402, kAsh = 0x0a5b3c4d;
    trainer(node(kKd2abc, "KD2ABC-m", Presence::Online, 6.5f, 2, 60, 84, "at the summit brb"),
            5, 34, 6, 12, 4, true);
    trainer(node(kRidge, "Ridge_Gym", Presence::Online, 3.25f, 1, 25, 100), 8, 52, 6, 31, 9, true);
    trainer(node(kAsh, "ash_k3tchup", Presence::Offline, -7.5f, 3, 3 * 3600, 22), 2, 17, 3, 1, 6, false);

    // Plain Meshtastic nodes I chat with.
    constexpr uint32_t kTrail = 0x5be0a903, kBase = 0xba5e5701, kW2xyz = 0x3202a9f7;
    node(kTrail, "Trailhead-R", Presence::Online, 9.0f, 1, 40, 91);
    node(kBase, "base_stn", Presence::Away, 11.25f, 0, 5 * 60, 100, "out mowing the lawn, back @ 5");
    node(kW2xyz, "W2XYZ", Presence::Offline, -12.0f, 4, 26 * 3600, 8);

    // Everyone else heard on the mesh.
    node(0x41110001, "HilltopRelay", Presence::Online, 4.75f, 1, 15, 100);
    node(0x000ca117, "N0CALL-7", Presence::Online, -2.5f, 3, 4 * 60, 63);
    node(0x5011a4b2, "solar_shed", Presence::Online, 1.5f, 2, 9 * 60, 47);
    node(0xc4b10ca7, "CabinFever99", Presence::Away, -4.0f, 3, 22 * 60, 71, "gone fishin");
    node(0x9e0f00d5, "wx_balloon_2", Presence::Offline, -15.5f, 5, 2 * 86400, -1);
    node(0x77be11e5, "sk8rmesh", Presence::Offline, -9.0f, 4, 7 * 3600, 35);

    const uint32_t t = now_s();
    auto say = [this, t](uint32_t id, bool me, const char* text, uint32_t ago_s, int hops, float snr,
                         bool unread = false) {
        Message m;
        m.from_me = me;
        m.text = text;
        m.timestamp = t - ago_s;
        m.delivered = true;
        m.state = SendState::Delivered;
        m.hops = hops;
        m.snr = snr;
        Chat& chat = chats_[id];
        chat.demo = true;
        chat.messages.push_back(std::move(m));
        if (unread) ++chat.unread;
    };
    say(kKd2abc, true, "u make it to the summit??", 600, 2, 6.5f);
    say(kKd2abc, false, "yep!! 4 bars up here lol", 540, 2, 6.5f);
    say(kKd2abc, true, "nice, sending wx report", 300, 2, 6.5f);
    say(kKd2abc, false, "k. brb lunch", 240, 2, 6.5f);

    say(kTrail, true, "is the lot full?", 1800, 1, 9.0f);
    say(kTrail, false, "about half. gate closes at 8", 1700, 1, 9.0f);
    say(kTrail, false, "bring bug spray!!", 90, 1, 9.0f, true);

    say(kBase, true, "radio check", 7200, 0, 11.25f);
    say(kBase, false, "loud and clear, 5 by 9", 7100, 0, 11.25f);

    say(kW2xyz, false, "73 from the lake", 90000, 4, -12.0f);
    say(kW2xyz, true, "73! catch u next weekend", 89000, 4, -12.0f);

    bump();
}

} // namespace mm
