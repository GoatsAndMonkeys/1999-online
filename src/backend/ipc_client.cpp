// ipc_client.cpp — see ipc_client.h.
#include "ipc_client.h"

#include "IpcProtocol.h"   // monster_mesh_pi/src/shared — MMD_SOCK_PATH

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace mm {

uint64_t IpcClient::steady_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

IpcClient::IpcClient(std::string sock_path) : path_(std::move(sock_path)) {
    if (path_.empty()) path_ = MMD_SOCK_PATH;
}

IpcClient::~IpcClient() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

void IpcClient::set_enabled(bool on) {
    enabled_ = on;
    if (!on) disconnect();
    else next_attempt_ms_ = 0;
}

void IpcClient::disconnect() {
    close_fd(steady_ms(), true);
}

void IpcClient::close_fd(uint64_t now, bool schedule_retry) {
    bool was_connected = (state_ == State::Connected);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    state_ = State::Disconnected;
    rx_.clear();
    tx_.clear();
    discarding_line_ = false;
    if (schedule_retry) {
        // Dropped soon after connecting -> probably another client (mmterm)
        // is competing for mmd's single slot. Back off exponentially.
        if (was_connected && now - connected_at_ms_ < kShortSessionMs) {
            retry_ms_ = retry_ms_ * 2 > kMaxRetryMs ? kMaxRetryMs : retry_ms_ * 2;
        } else if (was_connected) {
            retry_ms_ = kBaseRetryMs;
        }
        next_attempt_ms_ = now + retry_ms_;
    }
    if (was_connected && state_cb_) state_cb_(false);
}

void IpcClient::try_connect(uint64_t now) {
    next_attempt_ms_ = now + retry_ms_;
    if (path_.size() >= sizeof(((sockaddr_un *)nullptr)->sun_path)) return;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path_.c_str(), path_.size());

    int rc = ::connect(fd, (sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        fd_ = fd;
        on_connected(now);
        return;
    }
    if (errno == EINPROGRESS) {
        fd_ = fd;
        state_ = State::Connecting;
        connect_started_ms_ = now;
        return;
    }
    // ENOENT / ECONNREFUSED (daemon not running) / EAGAIN (backlog full) etc.
    // No daemon => nobody to fight over the slot with: back to the base rate.
    if (errno == ENOENT || errno == ECONNREFUSED) {
        retry_ms_ = kBaseRetryMs;
        next_attempt_ms_ = now + retry_ms_;
    }
    ::close(fd);
}

void IpcClient::finish_connect(uint64_t now) {
    pollfd p{fd_, POLLOUT, 0};
    int r = ::poll(&p, 1, 0);
    if (r == 0) {
        if (now - connect_started_ms_ > 1000) close_fd(now, true);   // give up
        return;
    }
    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    if (r < 0 || getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr != 0) {
        close_fd(now, true);
        return;
    }
    on_connected(now);
}

void IpcClient::on_connected(uint64_t now) {
    state_ = State::Connected;
    connected_at_ms_ = now;
    ++connects_;
    rx_.clear();
    tx_.clear();
    discarding_line_ = false;
    if (state_cb_) state_cb_(true);
}

bool IpcClient::poll(uint64_t now) {
    if (now == 0) now = steady_ms();

    if (state_ == State::Disconnected) {
        if (!enabled_ || now < next_attempt_ms_) return false;
        try_connect(now);
        if (state_ != State::Connected) return false;
    }
    if (state_ == State::Connecting) {
        finish_connect(now);
        if (state_ != State::Connected) return false;
    }
    if (!flush_tx(now)) return false;
    bool got = read_rx(now);
    if (state_ == State::Connected) flush_tx(now);   // replies queued by callbacks
    return got;
}

bool IpcClient::send_raw(const std::string &json) {
    if (state_ != State::Connected) return false;
    if (tx_.size() + json.size() + 1 > kMaxTxQueue) return false;
    tx_ += json;
    if (tx_.empty() || tx_.back() != '\n') tx_ += '\n';
    return true;
}

bool IpcClient::flush_tx(uint64_t now) {
    while (!tx_.empty() && fd_ >= 0) {
#ifdef MSG_NOSIGNAL
        ssize_t n = ::send(fd_, tx_.data(), tx_.size(), MSG_NOSIGNAL);
#else
        ssize_t n = ::send(fd_, tx_.data(), tx_.size(), 0);
#endif
        if (n > 0) { tx_.erase(0, (size_t)n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        if (n < 0 && errno == EINTR) continue;
        close_fd(now, true);
        return false;
    }
    return fd_ >= 0;
}

bool IpcClient::read_rx(uint64_t now) {
    char buf[4096];
    size_t total = 0;
    bool eof = false;
    while (total < kMaxReadPerPoll) {
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0) {
            total += (size_t)n;
            rx_.append(buf, (size_t)n);
            continue;
        }
        if (n == 0) { eof = true; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        eof = true;   // ECONNRESET etc.
        break;
    }

    // Split complete lines out first so callbacks may safely send/disconnect.
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        size_t nl = rx_.find('\n', start);
        if (nl == std::string::npos) break;
        if (discarding_line_) {
            discarding_line_ = false;           // tail of an over-long line
        } else {
            size_t end = nl;
            if (end > start && rx_[end - 1] == '\r') --end;
            if (end > start) lines.emplace_back(rx_, start, end - start);
        }
        start = nl + 1;
    }
    rx_.erase(0, start);
    if (rx_.size() > kMaxLine) {                // runaway line: drop it
        rx_.clear();
        discarding_line_ = true;
        ++parse_errors_;
    }

    bool got = false;
    for (const auto &line : lines) {
        JsonValue v;
        bool ok = json_parse(line, v);
        if (!ok) ++parse_errors_;
        ++lines_rx_;
        got = true;
        if (msg_cb_) msg_cb_(v, line, ok);
        if (state_ != State::Connected) break;  // callback disconnected us
    }

    if (eof && state_ == State::Connected) close_fd(now, true);
    return got;
}

// ── Command helpers ──────────────────────────────────────────────────────────

bool IpcClient::send_challenge(uint32_t node_id) {
    char b[64];
    snprintf(b, sizeof(b), "{\"cmd\":\"SEND_CHALLENGE\",\"node_id\":%u}", (unsigned)node_id);
    return send_raw(b);
}

bool IpcClient::send_accept_challenge(uint32_t node_id) {
    char b[64];
    snprintf(b, sizeof(b), "{\"cmd\":\"ACCEPT_CHALLENGE\",\"node_id\":%u}", (unsigned)node_id);
    return send_raw(b);
}

bool IpcClient::send_decline_challenge(uint32_t node_id) {
    char b[64];
    snprintf(b, sizeof(b), "{\"cmd\":\"DECLINE_CHALLENGE\",\"node_id\":%u}", (unsigned)node_id);
    return send_raw(b);
}

bool IpcClient::send_get_neighbor_party(uint32_t node_id) {
    char b[64];
    snprintf(b, sizeof(b), "{\"cmd\":\"GET_NEIGHBOR_PARTY\",\"node_id\":%u}", (unsigned)node_id);
    return send_raw(b);
}

bool IpcClient::send_announce_result(const std::string &text) {
    // The daemon's parser stops at the first '"' (it doesn't unescape), so
    // strip quotes/backslashes rather than escaping them.
    std::string clean;
    for (char c : text)
        if (c != '"' && c != '\\' && (unsigned char)c >= 0x20) clean += c;
    if (clean.size() > 199) clean.resize(199);
    return send_raw("{\"cmd\":\"ANNOUNCE_RESULT\",\"text\":\"" + clean + "\"}");
}

bool IpcClient::send_credit_xp(int slot, uint32_t xp) {
    char b[80];
    snprintf(b, sizeof(b), "{\"cmd\":\"CREDIT_XP\",\"slot\":%d,\"xp\":%u}", slot, (unsigned)xp);
    return send_raw(b);
}

bool IpcClient::send_learn_move(int slot, int forget, int move) {
    char b[96];
    snprintf(b, sizeof(b), "{\"cmd\":\"LEARN_MOVE\",\"slot\":%d,\"forget\":%d,\"move\":%d}",
             slot, forget, move);
    return send_raw(b);
}

bool IpcClient::send_battle_action(int action, int index, int turn) {
    char b[96];
    snprintf(b, sizeof(b), "{\"cmd\":\"BATTLE_ACTION\",\"action\":%d,\"index\":%d,\"turn\":%d}",
             action, index, turn);
    return send_raw(b);
}

}  // namespace mm
