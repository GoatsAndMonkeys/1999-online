// ipc_client.h — non-blocking client for the mmd daemon's Unix socket.
//
// Wire format: newline-delimited JSON (see monster_mesh_pi/src/shared/IpcProtocol.h
// and src/daemon/MonsterMeshDaemon.cpp::onIpcMessage for what is really parsed).
//
// Design:
//  * Never blocks: the socket is O_NONBLOCK *before* connect(); an in-progress
//    connect is finished on a later poll(). Each poll() reads at most
//    kMaxReadPerPoll bytes, so a flood can't stall the UI thread.
//  * Reconnect with backoff: retries every ~2 s. mmd serves ONE client and a
//    new connection kicks the old one, so if we keep getting dropped right
//    after connecting (e.g. mmterm is running too) the backoff doubles up to
//    30 s instead of ping-ponging the other client every 2 s.
//  * Outgoing commands are queued and flushed as the socket allows.
//
// Plain C++17 + POSIX. No LVGL, no SDL.
#pragma once

#include "mm_json.h"

#include <cstdint>
#include <functional>
#include <string>

namespace mm {

class IpcClient {
public:
    // Called once per complete line. `parsed` is false if the line wasn't valid
    // JSON (then `msg` is Null); `raw` is the line without the trailing '\n'.
    using MessageCallback =
        std::function<void(const JsonValue &msg, const std::string &raw, bool parsed)>;
    using StateCallback = std::function<void(bool connected)>;

    explicit IpcClient(std::string sock_path = std::string());
    ~IpcClient();
    IpcClient(const IpcClient &) = delete;
    IpcClient &operator=(const IpcClient &) = delete;

    void set_socket_path(const std::string &path) { path_ = path; }
    const std::string &socket_path() const { return path_; }
    void set_message_callback(MessageCallback cb) { msg_cb_ = std::move(cb); }
    void set_state_callback(StateCallback cb) { state_cb_ = std::move(cb); }

    // Pump: (re)connect when due, finish pending connects, flush tx, read rx and
    // dispatch complete lines. `now_ms` is a monotonic millisecond clock
    // (0 = use the internal steady clock). Returns true if any line arrived.
    bool poll(uint64_t now_ms = 0);

    bool connected() const { return state_ == State::Connected; }
    bool connecting() const { return state_ == State::Connecting; }
    void disconnect();              // drop the link; poll() will retry after backoff
    void set_enabled(bool on);      // false = stay disconnected (no retries)
    void retry_now() { next_attempt_ms_ = 0; }

    // Queue a raw JSON command (a '\n' is appended). Returns false if not
    // connected (commands are NOT buffered across disconnects).
    bool send_raw(const std::string &json);

    // ── Commands, in the exact shapes mmd's onIpcMessage() parses ───────────
    bool send_get_party()                  { return send_raw("{\"cmd\":\"GET_PARTY\"}"); }
    bool send_get_status()                 { return send_raw("{\"cmd\":\"GET_STATUS\"}"); }
    // NB: IpcProtocol.h documents "CHALLENGE", but the daemon only handles
    // "SEND_CHALLENGE" (with numeric node_id) — that's what mmterm sends too.
    bool send_challenge(uint32_t node_id);
    bool send_accept_challenge(uint32_t node_id);    // daemon uses its queued challenge
    bool send_decline_challenge(uint32_t node_id);
    bool send_force_beacon()               { return send_raw("{\"cmd\":\"FORCE_BEACON\"}"); }
    bool send_force_event()                { return send_raw("{\"cmd\":\"FORCE_EVENT\"}"); }
    bool send_ping()                       { return send_raw("{\"cmd\":\"PING\"}"); }
    // Extra commands the daemon supports (used by mmterm):
    bool send_hollaback()                  { return send_raw("{\"cmd\":\"HOLLABACK\"}"); }
    bool send_get_neighbor_party(uint32_t node_id);
    bool send_announce_result(const std::string &text);
    bool send_credit_xp(int slot, uint32_t xp);
    bool send_writeback_sav()              { return send_raw("{\"cmd\":\"WRITEBACK_SAV\"}"); }
    bool send_learn_move(int slot, int forget, int move);
    bool send_battle_action(int action, int index, int turn);

    // Stats (for a debug overlay).
    uint32_t lines_received() const { return lines_rx_; }
    uint32_t parse_errors() const { return parse_errors_; }
    uint32_t connect_count() const { return connects_; }

    static constexpr uint32_t kBaseRetryMs = 2000;
    static constexpr uint32_t kMaxRetryMs = 30000;
    static constexpr uint32_t kShortSessionMs = 5000;   // dropped sooner => back off
    static constexpr size_t kMaxReadPerPoll = 64 * 1024;
    static constexpr size_t kMaxLine = 64 * 1024;
    static constexpr size_t kMaxTxQueue = 64 * 1024;

private:
    enum class State : uint8_t { Disconnected, Connecting, Connected };

    std::string path_;
    int fd_ = -1;
    State state_ = State::Disconnected;
    bool enabled_ = true;
    uint64_t next_attempt_ms_ = 0;
    uint64_t connect_started_ms_ = 0;
    uint64_t connected_at_ms_ = 0;
    uint32_t retry_ms_ = kBaseRetryMs;
    std::string rx_;
    std::string tx_;
    bool discarding_line_ = false;
    MessageCallback msg_cb_;
    StateCallback state_cb_;
    uint32_t lines_rx_ = 0, parse_errors_ = 0, connects_ = 0;

    void try_connect(uint64_t now);
    void finish_connect(uint64_t now);
    void on_connected(uint64_t now);
    void close_fd(uint64_t now, bool schedule_retry);
    bool flush_tx(uint64_t now);
    bool read_rx(uint64_t now);
    static uint64_t steady_ms();
};

}  // namespace mm
