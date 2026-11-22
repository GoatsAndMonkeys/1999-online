// messenger.h — contacts and conversations for the 1999 Online messenger.
//
// One process-wide instance (mm::messenger()). The Buddy List and Instant
// Message screens only read this model and call send() / mark_read() /
// delete_chat(); they cache revision() and redraw when it changes.
//
// DEMO MODE (the default until a radio has answered): the model is seeded with
// a plausible set of nodes and chats, and send() to one of those demo nodes
// schedules a canned reply about 2 s later, delivered from tick().
//
// LIVE MODE: the mesh backend (mesh_backend.h) calls go_live() the first time
// the radio is ready and from then on feeds the model through set_my_name(),
// upsert_contact(), receive(), receive_channel() and ack_packet(). The model
// itself knows nothing about the radio: it hands outgoing text to the hook
// given to set_sender().
//
// SENDING (everything that is not a demo node), see SendState:
//   - send() appends my message and calls the sender hook. A packet id back
//     => Sending, until ack_packet(id, ...) makes it Delivered or Failed (with
//     the reason, see send_error_text()). No answer for kAckTimeoutMs =>
//     Failed, "no answer".
//   - No hook, or the hook returns 0 (radio not connected) => the message is
//     KEPT in the chat as Unsent ("Not sent: radio not connected"). Nothing is
//     queued and nothing is sent later on its own: the user retries.
//   - retry(node) sends the newest Failed / Unsent message of that chat again
//     (the IM screen: Enter on an empty input line). The message stays where
//     it is in the transcript.
//   - Text longer than kMaxMessageBytes is refused: send() returns false and
//     adds nothing (the IM screen shows a note and keeps the draft).
//   - A channel message (kChannelNode) is a broadcast. Nobody acks those, so
//     it starts as Sent and becomes Delivered if the radio hears another node
//     relay it; it never times out.
//
// THE CHANNEL: public channel texts live in one conversation under the
// reserved pseudo node id kChannelNode (0xffffffff, Meshtastic's broadcast
// address). It is not a contact: contact(kChannelNode) is nullptr and it is
// never in contacts(); name_of(kChannelNode) is channel_name(). Each incoming
// channel message remembers its sender in Message::from_node.
//
// PERSISTENCE: conversations (not contacts) are kept in conversations.json in
// the config folder (see settings.h, mm::store). When that file exists it
// replaces the demo chats at start; peers that are not in the contact list are
// added as offline contacts under their saved name. Changes are written from
// tick() at most once a second and on destruction; at most kMaxStoredMessages
// per peer are stored. Nothing is read or written when store::enabled() is
// false (--shots).
//   File format, version 2 (version 1 files still load): a conversation with
//   a demo node carries "demo": true; a message of mine that is not Delivered
//   carries "state": "sending" | "sent" | "failed" | "unsent" (and "err": the
//   reason code when failed / unsent); a channel message carries "from":
//   "!hexid". "delivered" is still written for older builds. Packet ids are
//   not stored: a message that was still Sending when the app closed loads as
//   Sent (handed to the radio, delivery not confirmed). In a version 1 file a
//   mine-but-not-delivered message loads as Sent too, and the chats with the
//   built-in demo node ids count as demo chats (version 1 was demo-only).
//
// LVGL-free. Not thread-safe: call everything from the UI thread.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mm {

enum class Presence : uint8_t { Online, Away, Offline };

struct Contact {
    uint32_t node_id = 0;
    std::string name;             // screen name
    Presence presence = Presence::Offline;
    std::string away_msg;         // "" if none
    float snr = 0.0f;             // dB, last packet
    int hops = 0;
    static constexpr uint32_t kNeverHeard = 0xffffffffu;
    uint32_t last_heard_s = 0;    // seconds since the node was last heard (kNeverHeard: unknown)
    int battery = -1;             // percent, -1 unknown

    // Trainer info (nodes running MonsterMesh).
    bool is_trainer = false;
    int badges = 0;               // 0..8
    int lead_level = 0;
    int party_size = 0;
    int pvp_wins = 0;
    int pvp_losses = 0;
    bool hosts_gym = false;

    bool online() const { return presence == Presence::Online; }
};

// Where one of MY messages is on its way out. Incoming messages are Delivered.
enum class SendState : uint8_t {
    Sending,      // handed to the radio, waiting for the delivery report
    Sent,         // handed to the radio, no report (channel message, or unknown after a restart)
    Delivered,    // acked by the destination (channel: relayed by another node)
    Failed,       // the mesh reported an error, or nothing came back: Message::error
    Unsent,       // never left: the radio was not connected. Kept; retry() sends it
};

// Reasons in Message::error. > 0: Meshtastic Routing.Error as reported by the
// radio (1 no route, 3 timeout, 5 max retransmit, ...). < 0: decided here.
constexpr int kSendErrNone = 0;
constexpr int kSendErrNoRadio = -1;   // link not ready when sending
constexpr int kSendErrNoAnswer = -2;  // no delivery report within kAckTimeoutMs
// Short lower-case reason: "no route", "timed out", "radio not connected", ...
const char* send_error_text(int error);

struct Message {
    bool from_me = false;
    std::string text;
    uint32_t timestamp = 0;       // unix seconds
    bool delivered = false;       // == (state == SendState::Delivered)
    int hops = 0;
    float snr = 0.0f;
    bool auto_response = false;   // their away message, sent automatically
    SendState state = SendState::Sending;   // mine only
    int error = kSendErrNone;     // why it is Failed / Unsent
    uint32_t packet_id = 0;       // mine: the radio's packet id while Sending (not saved)
    uint32_t from_node = 0;       // channel messages: who wrote it (0: n/a)

    bool retryable() const {
        return from_me && (state == SendState::Failed || state == SendState::Unsent);
    }
};

class Messenger {
public:
    static constexpr int kMaxMessageBytes = 200;
    static constexpr uint32_t kDemoReplyMs = 2000;
    static constexpr size_t kMaxStoredMessages = 200;   // per peer, newest kept
    static constexpr uint32_t kSaveMs = 1000;           // at most one write per second
    static constexpr uint32_t kAckTimeoutMs = 120000;   // Sending -> Failed "no answer"
    // The public channel's conversation (see the header comment).
    static constexpr uint32_t kChannelNode = 0xffffffffu;
    // Hands a text to the radio: the packet id, or 0 if it could not be sent.
    // `node` is a node number, or kChannelNode for a broadcast.
    using Sender = std::function<uint32_t(uint32_t node, const std::string& text)>;

    Messenger();
    ~Messenger();                                     // save() if there are unsaved changes

    // ── Persistence (conversations.json) ───────────────────────────────────
    // Replaces all chats with the file's. false (and nothing changes) when
    // the file is missing, corrupt or files are disabled.
    bool load();
    // Writes every chat now (atomic). false when files are disabled or on error.
    bool save();
    bool unsaved() const { return chats_dirty_; }

    // ── Read side (screens) ────────────────────────────────────────────────
    const std::string& my_name() const { return my_name_; }
    // Stable order: the order contacts were first seen.
    const std::vector<Contact>& contacts() const { return contacts_; }
    // nullptr for an unknown node and for kChannelNode.
    const Contact* contact(uint32_t node_id) const;
    // Screen name, or "!0a1b2c3d" for a node we have never heard;
    // channel_name() for kChannelNode.
    std::string name_of(uint32_t node_id) const;
    static bool is_channel(uint32_t node_id) { return node_id == kChannelNode; }
    // The radio's primary channel name ("LongFast" on the default channel).
    static std::string channel_name();

    bool has_chat(uint32_t node_id) const;
    // Empty vector if there is no conversation.
    const std::vector<Message>& messages(uint32_t node_id) const;
    int unread(uint32_t node_id) const;
    int total_unread() const;
    // Bumps on any change to contacts, messages or unread counts.
    uint32_t revision() const { return revision_; }

    // ── Actions (screens) ──────────────────────────────────────────────────
    // Appends my message (creating the conversation) and tries to send it.
    // false, and nothing is added, for an empty text or one longer than
    // kMaxMessageBytes. To a demo node (demo mode only): marked delivered
    // unless the contact is offline, and a reply is scheduled. To anyone
    // else: through the sender hook, see SENDING above.
    bool send(uint32_t node_id, const std::string& text);
    // Index of the newest Failed / Unsent message of mine in that chat, or -1.
    int retry_index(uint32_t node_id) const;
    // Sends that message again. false if there is none; true otherwise (look
    // at the message's state for how it went: Sending / Sent, or Unsent again).
    bool retry(uint32_t node_id);
    void mark_read(uint32_t node_id);
    void delete_chat(uint32_t node_id);
    // Call ~30 Hz from the screen on top; delivers due demo replies.
    void tick(uint32_t now_ms);

    // ── Write side (a live backend) ────────────────────────────────────────
    bool demo() const { return demo_; }
    // false: drops pending demo replies; seeded data stays until clear().
    void set_demo(bool on);
    // Leave demo mode for good: drops the demo contacts, the demo chats (the
    // seeded ones, and chats with demo nodes that came back from the file) and
    // pending demo replies. Conversations with real nodes that were loaded
    // from conversations.json stay, with their (offline) contacts. The saved
    // file is only rewritten if a demo chat had to go. No-op when already live.
    void go_live();
    bool is_demo_node(uint32_t node_id) const { return demo_nodes_.count(node_id) != 0; }
    // Forget all contacts and chats. NOTE: this also empties the saved file
    // at the next save; a backend wants go_live().
    void clear();
    void set_my_name(const std::string& name);
    void upsert_contact(const Contact& c);            // add, or replace by node_id
    void set_sender(Sender s) { sender_ = std::move(s); }
    // An incoming direct message; counts as unread until mark_read().
    void receive(uint32_t node_id, const std::string& text, int hops, float snr,
                 bool auto_response = false);
    // An incoming channel message written by `from_node`.
    void receive_channel(uint32_t from_node, const std::string& text, int hops, float snr);
    // Appends a ready-made message as it is (shots, tests, imports).
    void add_message(uint32_t node_id, const Message& m, bool unread = false);
    // The delivery report for the message sent as `packet_id`: Delivered
    // (hops / snr are filled in from the contact), or Failed with `error`.
    // false if no message is waiting under that id.
    bool ack_packet(uint32_t packet_id, bool ok, int error = kSendErrNone);
    // Marks my most recent undelivered message to node_id as delivered.
    void ack(uint32_t node_id, int hops, float snr);

private:
    struct Chat {
        std::vector<Message> messages;
        int unread = 0;
        bool demo = false;        // with a demo node: dropped by go_live()
    };
    struct InFlight {             // a Sending message waiting for ack_packet()
        uint32_t packet_id = 0;
        uint32_t node_id = 0;
        uint32_t due_ms = 0;
        bool armed = false;       // due_ms is set (done on the first tick after send)
    };
    struct PendingReply {
        uint32_t node_id = 0;
        uint32_t due_ms = 0;
        bool armed = false;       // due_ms is set (done on the first tick after send)
    };

    void seed_demo();
    Chat& chat_for(uint32_t node_id);                 // creates it (demo flag set here)
    void transmit(uint32_t node_id, Message& m);      // through sender_: sets state
    Message* find_packet(uint32_t packet_id, uint32_t* node = nullptr);
    void bump() { ++revision_; }
    void touch_chats() { chats_dirty_ = true; }   // a chat changed: save soon

    std::string my_name_;
    std::vector<Contact> contacts_;
    std::map<uint32_t, Chat> chats_;
    std::vector<PendingReply> pending_;
    std::vector<InFlight> inflight_;
    std::set<uint32_t> demo_nodes_;   // the seeded contacts, until go_live() / clear()
    Sender sender_;
    uint32_t revision_ = 1;
    uint32_t reply_cursor_ = 0;
    bool demo_ = true;
    bool chats_dirty_ = false;
    bool save_armed_ = false;       // save_since_ms_ is valid
    uint32_t save_since_ms_ = 0;
};

Messenger& messenger();

} // namespace mm
