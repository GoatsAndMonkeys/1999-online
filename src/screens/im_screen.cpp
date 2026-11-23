// im_screen.cpp — Instant Message window with one contact (Ctx::peer).
//
// Transcript on top (newest at the bottom, word-wrapped here so a coloured
// bold "<name>: " can lead each message), then a byte counter and the input
// line. Every key goes to the input first, so ; . , / type as characters;
// only the real arrow keys scroll the transcript.
//
// Under my newest message, and under every message of mine that is not
// delivered, a status line says where it is: sending, delivered (hops, SNR),
// not delivered (why), not sent. Enter on an EMPTY input line sends the newest
// failed / not-sent message again (Messenger::retry). Ctx::peer may be
// Messenger::kChannelNode: the public channel, where each line starts with its
// sender's name.
#include "screens.h"

#include "messenger.h"
#include "router.h"
#include "widgets.h"
#include "win.h"

#include <algorithm>
#include <string>
#include <vector>

namespace mm::ui {
namespace {

constexpr int kFieldH = 84;
constexpr int kTextX = 3;                       // inside the transcript box
constexpr int kTextW = kClientW - 2 - 3 - 6;    // room for the scroll thumb on the right
constexpr int kInfoY = kFieldH + 1;
constexpr int kInputH = 16;
constexpr int kInputY = kClientH - kInputH - 2;
constexpr int kSendW = 56;
constexpr int kInputW = kClientW - kSendW - 6;  // 250

int text_width(const std::string& s, const lv_font_t* font) {
    int w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const uint32_t next = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
        w += lv_font_get_glyph_width(font, static_cast<unsigned char>(s[i]), next);
    }
    return w;
}

// Greedy word wrap. The first line may be narrower (it follows the name).
std::vector<std::string> wrap(const std::string& s, int first_w, int rest_w, const lv_font_t* font) {
    std::vector<std::string> out;
    std::string cur;
    int limit = first_w;
    auto flush = [&] {
        out.push_back(cur);
        cur.clear();
        limit = rest_w;
    };
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ') ++j;
        if (j == i) break;
        std::string word = s.substr(i, j - i);
        i = j;
        const std::string cand = cur.empty() ? word : cur + " " + word;
        if (text_width(cand, font) <= limit) {
            cur = cand;
            continue;
        }
        // Does not fit: start a new line (also when only the name is on this one).
        if (!cur.empty() || (out.empty() && text_width(word, font) <= rest_w)) flush();
        // A single word wider than a whole line is cut by characters.
        while (text_width(word, font) > limit) {
            size_t n = 1;
            while (n < word.size() && text_width(word.substr(0, n + 1), font) <= limit) ++n;
            cur = word.substr(0, n);
            word.erase(0, n);
            flush();
        }
        cur = word;
    }
    if (!cur.empty() || out.empty()) out.push_back(cur);
    return out;
}

class ImScreen final : public Screen {
public:
    explicit ImScreen(Ctx& ctx) : Screen(ctx) {
        uint32_t peer = ctx.peer;
        if (peer == 0) peer = find_by_name("KD2ABC-m");   // --shots / opened without a contact
        if (peer == 0 && !messenger().contacts().empty()) peer = messenger().contacts().front().node_id;
        set_peer(peer);
    }

    ~ImScreen() override {
        if (seeded_peer_) messenger().delete_chat(seeded_peer_);   // --shots only
    }

    const char* title() const override { return title_.c_str(); }
    bool windowed() const override { return true; }
    std::string hints() const override {
        return messenger().retry_index(peer_) >= 0 ? "Enter:send, or retry if empty  Esc:buddy list"
                                                   : "Enter:send  Esc:buddy list";
    }

    void build(lv_obj_t* body) override {
        const Skin& s = skin();
        lv_obj_t* f = win::field(body, 0, 0, kClientW, kFieldH);
        lines_box_ = box(f, 1, 1, kClientW - 2, kFieldH - 2);
        row_h_ = std::max(11, static_cast<int>(lv_font_get_line_height(font_ui())));
        visible_ = std::max(1, (kFieldH - 4) / row_h_);

        info_ = win::text(body, "", 2, kInfoY, s.dim, 200, false);
        counter_ = win::text(body, "", 0, kInfoY, s.dim, 0, false);
        lv_obj_set_align(counter_, LV_ALIGN_TOP_RIGHT);
        lv_obj_set_pos(counter_, -2, kInfoY);

        input_ = std::make_unique<win::TextInput>(body, 0, kInputY, kInputW, kInputH,
                                                  Messenger::kMaxMessageBytes);
        input_->set_placeholder("Type a message");
        input_->set_value(draft_);
        win::button(body, "Send", kClientW - kSendW, kInputY, kSendW, true, kInputH);

        sync();
        paint_counter();
    }

    void on_show() override {
        if (lines_box_) sync();
    }

    void tick(uint32_t now_ms) override {
        messenger().tick(now_ms);
        if (lines_box_ && messenger().revision() != seen_rev_) sync();
    }

    bool typing() const override { return input_ != nullptr; }

    bool on_key(const KeyEvent& e) override {
        if (!input_) return false;
        if (input_->on_key(e)) {
            draft_ = input_->value();
            paint_counter();
            return true;
        }
        if (e.raw == LV_KEY_ENTER || e.raw == '\r') {
            Messenger& m = messenger();
            if (draft_.empty()) {
                // Nothing typed: send the newest failed / not-sent message again.
                if (m.retry(peer_)) {
                    const int i = m.retry_index(peer_);
                    if (i >= 0 && m.messages(peer_)[i].state == SendState::Unsent) {
                        toast("Not sent: radio not connected", 2000, color::kWarn);
                    }
                    scroll_ = 0;
                    sync();
                }
                return true;
            }
            if (!m.send(peer_, draft_)) {
                // The draft stays so it can be shortened.
                toast(fmt("Too long: %d of %d bytes", static_cast<int>(draft_.size()),
                          Messenger::kMaxMessageBytes), 2500, color::kWarn);
                return true;
            }
            draft_.clear();
            input_->set_value(draft_);
            paint_counter();
            scroll_ = 0;
            sync();
            return true;
        }
        if (e.raw == LV_KEY_UP || e.raw == LV_KEY_DOWN) {
            const int max_scroll = std::max(0, static_cast<int>(lines_.size()) - visible_);
            const int next = std::clamp(scroll_ + (e.raw == LV_KEY_UP ? 1 : -1), 0, max_scroll);
            if (next != scroll_) {
                scroll_ = next;
                paint_lines();
            }
            return true;
        }
        return false;   // Esc, or Backspace on an empty line: back to the buddy list
    }

    int demo_state_count() const override { return 3; }
    const char* demo_state_name(int i) const override {
        return i == 1 ? "empty" : i == 2 ? "delivery" : "main";
    }
    void set_demo_state(int i) override {
        scroll_ = 0;
        if (i == 2) {
            // One failed and one sending message, as with a real radio.
            Messenger& m = messenger();
            const uint32_t peer = find_by_name("solar_shed");
            if (peer && m.demo() && !m.has_chat(peer)) {
                seeded_peer_ = peer;
                Message a;
                a.text = "anyone up at the shed?";
                a.timestamp = 1;
                a.hops = 2;
                a.snr = 1.5f;
                m.add_message(peer, a);
                Message b;
                b.from_me = true;
                b.text = "me! is the panel still on?";
                b.state = SendState::Failed;
                b.error = 1;   // no route
                m.add_message(peer, b);
                Message d;
                d.from_me = true;
                d.text = "trying again from the ridge";
                d.state = SendState::Sending;
                m.add_message(peer, d);
            }
            if (peer) set_peer(peer);
            draft_.clear();
        } else if (i == 1) {
            uint32_t peer = 0;
            for (const Contact& c : messenger().contacts()) {
                if (!c.is_trainer && !messenger().has_chat(c.node_id)) {
                    peer = c.node_id;
                    break;
                }
            }
            if (peer) set_peer(peer);
            draft_.clear();
        } else {
            if (const uint32_t peer = find_by_name("KD2ABC-m")) set_peer(peer);
            draft_ = "save me a sandwich, omw";
        }
    }

private:
    struct Line {
        std::string prefix;      // bold, coloured; "" on continuation lines
        uint32_t prefix_color = 0;
        std::string text;
        uint32_t color = 0;
        int indent = 0;
    };

    static uint32_t find_by_name(const char* name) {
        for (const Contact& c : messenger().contacts()) {
            if (c.name == name) return c.node_id;
        }
        return 0;
    }

    void set_peer(uint32_t peer) {
        peer_ = peer;
        title_ = messenger().name_of(peer_) +
                 (Messenger::is_channel(peer_) ? " - Channel" : " - Instant Message");
    }

    // Model changed (or we just became visible): everything here is read now.
    void sync() {
        Messenger& m = messenger();
        m.mark_read(peer_);
        const bool grew = m.messages(peer_).size() != seen_count_;
        seen_count_ = m.messages(peer_).size();
        seen_rev_ = m.revision();
        if (grew) scroll_ = 0;   // a new message: jump back to the newest
        layout_lines();
        paint_lines();
        paint_info();
    }

    void layout_lines() {
        const Skin& s = skin();
        Messenger& m = messenger();
        const std::string them = m.name_of(peer_);
        const std::vector<Message>& msgs = m.messages(peer_);
        lines_.clear();

        const bool channel = Messenger::is_channel(peer_);
        int last_mine = -1;
        for (int i = static_cast<int>(msgs.size()) - 1; i >= 0 && last_mine < 0; --i) {
            if (msgs[i].from_me) last_mine = i;
        }
        const int retry = m.retry_index(peer_);

        for (int i = 0; i < static_cast<int>(msgs.size()); ++i) {
            const Message& msg = msgs[i];
            Line first;
            if (msg.from_me) {
                first.prefix = m.my_name() + ": ";
                first.prefix_color = s.me;
            } else {
                const std::string who = channel && msg.from_node ? m.name_of(msg.from_node) : them;
                first.prefix = (msg.auto_response ? "Auto response from " : "") + who + ": ";
                first.prefix_color = s.them;
            }
            const int pw = text_width(first.prefix, font_ui_bold()) + 1;
            const std::vector<std::string> parts = wrap(msg.text, kTextW - pw, kTextW, font_ui());
            for (size_t p = 0; p < parts.size(); ++p) {
                Line l;
                if (p == 0) l = first;
                l.text = parts[p];
                l.color = s.field_text;
                lines_.push_back(std::move(l));
            }
            // Where my message is: always under the newest, and under any
            // older one that did not make it.
            if (msg.from_me && (i == last_mine || msg.state != SendState::Delivered)) {
                Line l;
                l.indent = 10;
                l.color = s.dim;
                switch (msg.state) {
                    case SendState::Sending:
                        l.text = "Sending...";
                        break;
                    case SendState::Sent:
                        l.text = channel ? "Sent to the channel" : "Sent, delivery not confirmed";
                        break;
                    case SendState::Failed:
                        l.text = std::string("Not delivered: ") + send_error_text(msg.error);
                        break;
                    case SendState::Unsent:
                        l.text = msg.error == kSendErrQueued ? std::string("Queued: sends when they come online") : std::string("Not sent: ") + send_error_text(msg.error);
                        break;
                    case SendState::Delivered:
                        if (channel) {
                            l.text = "Sent, relayed by the mesh";
                        } else if (msg.hops <= 0) {
                            l.text = fmt("Delivered, direct, SNR %.1f", static_cast<double>(msg.snr));
                        } else {
                            l.text = fmt("Delivered, %d hop%s, SNR %.1f", msg.hops, msg.hops == 1 ? "" : "s",
                                         static_cast<double>(msg.snr));
                        }
                        break;
                }
                if (msg.retryable()) {
                    l.color = s.me;   // stands out from the dim "delivered" lines
                    if (i == retry) l.text += ".  Enter: retry";
                }
                lines_.push_back(std::move(l));
            }
        }
        if (msgs.empty()) {
            Line l;
            l.color = s.dim;
            l.text = (channel ? "No messages in " : "No messages with ") + them + " yet.";
            lines_.push_back(std::move(l));
        }
    }

    void paint_lines() {
        const Skin& s = skin();
        lv_obj_clean(lines_box_);
        const int n = static_cast<int>(lines_.size());
        const int max_scroll = std::max(0, n - visible_);
        scroll_ = std::clamp(scroll_, 0, max_scroll);
        const int top = max_scroll - scroll_;
        for (int v = 0; v < visible_ && top + v < n; ++v) {
            const Line& l = lines_[top + v];
            const int y = 1 + v * row_h_;
            int x = kTextX + l.indent;
            if (!l.prefix.empty()) {
                win::text(lines_box_, l.prefix, x, y, l.prefix_color, 0, true);
                x += text_width(l.prefix, font_ui_bold()) + 1;
            }
            if (!l.text.empty()) win::text(lines_box_, l.text, x, y, l.color, 0, false);
        }
        if (n > visible_) {
            const int track = kFieldH - 2;
            const int th = std::max(6, track * visible_ / n);
            const int ty = (track - th) * top / max_scroll;
            lv_obj_t* thumb = box(lines_box_, kClientW - 2 - 2, ty, 2, th);
            lv_obj_set_style_bg_color(thumb, c(s.dim), 0);
            lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
        }
    }

    void paint_info() {
        const Contact* c = messenger().contact(peer_);
        std::string t;
        if (Messenger::is_channel(peer_)) {
            t = "Public channel: everyone can read";
        } else if (c) {
            switch (c->presence) {
                case Presence::Online:  t = c->name + " is online"; break;
                case Presence::Away:    t = c->name + " is away"; break;
                case Presence::Offline: t = c->name + " is offline"; break;
            }
        }
        set_text(info_, t);
    }

    void paint_counter() {
        set_text(counter_, fmt("%d/%d bytes", static_cast<int>(draft_.size()), Messenger::kMaxMessageBytes));
    }

    uint32_t peer_ = 0;
    uint32_t seeded_peer_ = 0;   // chat made up for a --shots state: removed again
    std::string title_;
    std::string draft_;          // survives rebuilds (skin change)
    std::unique_ptr<win::TextInput> input_;
    lv_obj_t* lines_box_ = nullptr;
    lv_obj_t* info_ = nullptr;
    lv_obj_t* counter_ = nullptr;
    std::vector<Line> lines_;
    int row_h_ = 13, visible_ = 6;
    int scroll_ = 0;             // lines scrolled up from the newest
    uint32_t seen_rev_ = 0;
    size_t seen_count_ = 0;
};

} // namespace

std::unique_ptr<Screen> make_im_screen(Ctx& ctx) {
    return std::make_unique<ImScreen>(ctx);
}

} // namespace mm::ui
