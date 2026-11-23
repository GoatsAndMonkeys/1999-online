// mailbox_screen.cpp — Mailbox: the messenger's mail view, 1999 e-mail style.
//
// Folders on the left (Inbox, Outbox, Sent), the selected folder's messages on
// the right under a column header (From / To, Message, Status) and a preview
// pane under the list. Enter opens a message in the right-hand pane.
//
//   Inbox   direct messages to me (not the public channel), newest first; the
//           newest Messenger::unread() of a chat are New (bold).
//   Outbox  my messages that have not arrived: Queued (never left: offline
//           contact or no radio), Sending, Failed. The messenger's outbox
//           sends them on its own when the contact is heard again (see
//           messenger.h, THE OUTBOX); R sends one now, D deletes it.
//   Sent    my Delivered / Sent messages, channel ones too.
//
// Keys: Left/Right or Tab change folder, Up/Down pick, Enter opens; in an
// open message Enter replies (Instant Message window), Esc goes back to the
// list. A "You've got mail" notice that arrives while the Mailbox is on top
// is shown here (toast + chime); elsewhere that is the Router's job.
//
// All data comes from mm::messenger(). The --shots states add made-up
// messages (demo nodes only, never transmitted) and remove them again.
#include "screens.h"

#include "mail_chime.h"
#include "messenger.h"
#include "router.h"
#include "widgets.h"
#include "win.h"

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

namespace mm::ui {
namespace {

constexpr int kFolderW = 66;
constexpr int kPaneX = kFolderW + 3;            // 69
constexpr int kPaneW = kClientW - kPaneX;       // 243
constexpr int kHeadH = 13;                      // column header
constexpr int kListH = 54;                      // 4 rows
constexpr int kPrevY = kHeadH + kListH + 2;     // 69
constexpr int kPrevH = kClientH - kPrevY;       // 52
constexpr int kWhoW = 64;                       // From / To column
constexpr int kStatW = 58;                      // Status column ("Delivered")
constexpr int kTextColW = kPaneW - kWhoW - kStatW;

enum Folder { kInbox = 0, kOutbox, kSent, kFolderCount };
const char* const kFolderName[kFolderCount] = {"Inbox", "Outbox", "Sent"};

struct Item {
    uint32_t node = 0;
    int index = 0;           // into messenger().messages(node)
    uint32_t ts = 0;
    bool unread = false;
};

uint32_t now_s() { return static_cast<uint32_t>(std::time(nullptr)); }

std::string ago_text(uint32_t ts) {
    const uint32_t t = now_s();
    const uint32_t s = t > ts ? t - ts : 0;
    if (s < 60) return "now";
    if (s < 3600) return fmt("%um", s / 60);
    if (s < 86400) return fmt("%uh", s / 3600);
    return fmt("%ud", s / 86400);
}

std::string date_text(uint32_t ts) {
    if (ts < 86400) return "unknown";
    const std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%a %b %d, %H:%M", &tm);
    return buf;
}

std::string wait_text(uint32_t ms) {
    const uint32_t s = (ms + 999) / 1000;
    if (s < 60) return fmt("%us", s);
    return fmt("%u min", (s + 59) / 60);
}

bool queued(const Message& m) { return m.from_me && m.state == SendState::Unsent; }

// Status column word.
const char* status_word(const Message& m) {
    if (!m.from_me) return "";
    switch (m.state) {
        case SendState::Sending:   return "Sending";
        case SendState::Sent:      return "Sent";
        case SendState::Delivered: return "Delivered";
        case SendState::Failed:    return "Failed";
        case SendState::Unsent:    return "Queued";
    }
    return "";
}

class MailboxScreen final : public Screen {
public:
    explicit MailboxScreen(Ctx& ctx) : Screen(ctx) {}

    ~MailboxScreen() override { unseed(); }

    const char* title() const override { return "Mailbox"; }
    bool windowed() const override { return true; }

    std::string hints() const override {
        if (open_) {
            const Item* it = current();
            const Message* m = it ? msg(*it) : nullptr;
            if (m && m->retryable()) return "Enter:reply  R:retry now  D:delete  Esc:list";
            return "Enter:reply  Esc:back to list";
        }
        if (folder_ == kOutbox && !items_.empty()) return "Enter:open  R:retry  D:delete  Tab:folder  Esc:back";
        return items_.empty() ? "Tab:folder  Esc:back" : "Enter:open  Tab:folder  Esc:back";
    }

    void build(lv_obj_t* body) override {
        body_ = body;
        folders_ = std::make_unique<win::ListBox>(body, 0, 0, kFolderW, kClientH);
        pane_ = box(body, kPaneX, 0, kPaneW, kClientH);
        refresh();
    }

    void on_show() override {
        // Notices from before the Mailbox was on top are old news here.
        MailNotice n;
        while (messenger().pop_mail_notice(n)) {}
        if (pane_) refresh();
    }

    void tick(uint32_t now_ms) override {
        Messenger& m = messenger();
        m.tick(now_ms);
        MailNotice n;
        while (m.pop_mail_notice(n)) announce(n);
        if (!pane_) return;
        // Repaint on changes, and every second while a backoff counts down.
        if (m.revision() != seen_rev_) {
            refresh();
        } else if (folder_ == kOutbox && now_ms - last_paint_ms_ >= 1000) {
            last_paint_ms_ = now_ms;
            paint_pane();
        }
    }

    bool on_key(const KeyEvent& e) override {
        if (!pane_) return false;
        if (open_) return on_key_open(e);
        if (e.key == Key::Up || e.key == Key::Down) {
            if (items_.empty()) return true;
            const int n = static_cast<int>(items_.size());
            sel_ = std::clamp(sel_ + (e.key == Key::Up ? -1 : 1), 0, n - 1);
            paint_pane();
            return true;
        }
        if (e.key == Key::Left || e.key == Key::Right || e.key == Key::Tab) {
            const int step = e.key == Key::Left ? kFolderCount - 1 : 1;
            set_folder((folder_ + step) % kFolderCount);
            return true;
        }
        if (e.key == Key::A) {
            if (current()) open_message();
            return true;
        }
        if (e.key == Key::Char && (e.ch == 'r' || e.ch == 'd')) {
            if (folder_ != kOutbox) {
                toast(e.ch == 'r' ? "Only queued mail can be retried" : "Only queued mail can be deleted", 1500);
                return true;
            }
            if (e.ch == 'r') retry_now();
            else ask_delete();
            return true;
        }
        if (e.key == Key::Char && e.ch >= '1' && e.ch <= '3') {
            set_folder(e.ch - '1');
            return true;
        }
        return false;
    }

    int demo_state_count() const override { return 6; }
    const char* demo_state_name(int i) const override {
        static const char* const kNames[] = {"inbox", "outbox", "sent", "open_queued", "open_failed", "mail_notice"};
        return i >= 0 && i < 6 ? kNames[i] : "main";
    }
    void set_demo_state(int i) override {
        seed();
        open_ = false;
        sel_ = 0;
        switch (i) {
            case 1: folder_ = kOutbox; break;
            case 2: folder_ = kSent; break;
            case 3:
            case 4: {
                folder_ = kOutbox;
                collect();
                const Message* want = nullptr;
                for (int k = 0; k < static_cast<int>(items_.size()); ++k) {
                    const Message* m = msg(items_[k]);
                    if (m && m->state == (i == 3 ? SendState::Unsent : SendState::Failed)) {
                        sel_ = k;
                        want = m;
                        break;
                    }
                }
                open_ = want != nullptr;
                break;
            }
            case 5: {
                folder_ = kInbox;
                MailNotice n;
                n.kind = MailNotice::Kind::NewMail;
                n.node_id = find_by_name("CabinFever99");
                n.text = "caught a 5lb bass!! come see";
                announce(n);   // the toast only: no chime in --shots (files are off)
                break;
            }
            default: folder_ = kInbox; break;
        }
    }

private:
    // ── Model ───────────────────────────────────────────────────────────────

    static uint32_t find_by_name(const char* name) {
        for (const Contact& c : messenger().contacts()) {
            if (c.name == name) return c.node_id;
        }
        return 0;
    }

    static const Message* msg(const Item& it) {
        const std::vector<Message>& v = messenger().messages(it.node);
        return it.index >= 0 && it.index < static_cast<int>(v.size()) ? &v[it.index] : nullptr;
    }

    const Item* current() const {
        return sel_ >= 0 && sel_ < static_cast<int>(items_.size()) ? &items_[sel_] : nullptr;
    }

    static bool in_folder(int folder, uint32_t node, const Message& m) {
        switch (folder) {
            case kInbox:  return !m.from_me && !Messenger::is_channel(node);
            case kOutbox: return m.retryable() || (m.from_me && m.state == SendState::Sending);
            default:      return m.from_me && (m.state == SendState::Delivered || m.state == SendState::Sent);
        }
    }

    static int count_folder(int folder, int* unread_out = nullptr) {
        Messenger& mm = messenger();
        int n = 0, unread = 0;
        for (uint32_t node : chat_nodes()) {
            const std::vector<Message>& v = mm.messages(node);
            int left = folder == kInbox ? mm.unread(node) : 0;
            for (int i = static_cast<int>(v.size()) - 1; i >= 0; --i) {
                if (!in_folder(folder, node, v[i])) continue;
                ++n;
                if (left > 0) {
                    ++unread;
                    --left;
                }
            }
        }
        if (unread_out) *unread_out = unread;
        return n;
    }

    // Nodes with a conversation (contacts in order, then the channel).
    static std::vector<uint32_t> chat_nodes() {
        Messenger& m = messenger();
        std::vector<uint32_t> out;
        for (const Contact& c : m.contacts()) {
            if (m.has_chat(c.node_id)) out.push_back(c.node_id);
        }
        if (m.has_chat(Messenger::kChannelNode)) out.push_back(Messenger::kChannelNode);
        return out;
    }

    void collect() {
        Messenger& m = messenger();
        // Keep the selection on the same message when the list changes.
        const Item* was = current();
        const uint32_t keep_node = was ? was->node : 0;
        const uint32_t keep_ts = was ? was->ts : 0;
        const std::string keep_text = was && msg(*was) ? msg(*was)->text : std::string();

        items_.clear();
        for (uint32_t node : chat_nodes()) {
            const std::vector<Message>& v = m.messages(node);
            int left = folder_ == kInbox ? m.unread(node) : 0;
            for (int i = static_cast<int>(v.size()) - 1; i >= 0; --i) {
                if (!in_folder(folder_, node, v[i])) continue;
                Item it;
                it.node = node;
                it.index = i;
                it.ts = v[i].timestamp;
                if (left > 0) {
                    it.unread = true;
                    --left;
                }
                items_.push_back(it);
            }
        }
        std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.ts > b.ts; });

        if (keep_node) {
            for (int k = 0; k < static_cast<int>(items_.size()); ++k) {
                const Message* mm = msg(items_[k]);
                if (items_[k].node == keep_node && items_[k].ts == keep_ts && mm && mm->text == keep_text) {
                    sel_ = k;
                    break;
                }
            }
        }
        sel_ = items_.empty() ? 0 : std::clamp(sel_, 0, static_cast<int>(items_.size()) - 1);
        if (items_.empty()) open_ = false;
    }

    // ── Actions ─────────────────────────────────────────────────────────────

    void set_folder(int f) {
        if (f == folder_) return;
        folder_ = f;
        sel_ = 0;
        top_ = 0;
        open_ = false;
        items_.clear();
        refresh();
    }

    void open_message() {
        const Item* it = current();
        if (!it) return;
        open_ = true;
        if (it->unread) messenger().mark_read(it->node);   // tick() sees the revision
        paint_pane();
        ctx_.router.refresh_chrome();
    }

    bool on_key_open(const KeyEvent& e) {
        const Item* it = current();
        const Message* m = it ? msg(*it) : nullptr;
        if (e.key == Key::B) {
            open_ = false;
            refresh();
            ctx_.router.refresh_chrome();
            return true;
        }
        if (e.key == Key::A || (e.key == Key::Char && e.ch == 'i')) {
            if (it) {
                ctx_.peer = it->node;
                open_ = false;
                ctx_.router.push(ScreenId::Im);
            }
            return true;
        }
        if (e.key == Key::Char && m && m->retryable()) {
            if (e.ch == 'r') {
                retry_now();
                return true;
            }
            if (e.ch == 'd') {
                ask_delete();
                return true;
            }
        }
        return true;   // the open message keeps every other key
    }

    void retry_now() {
        const Item* it = current();
        const Message* m = it ? msg(*it) : nullptr;
        if (!m || !m->retryable()) {
            toast(m && m->state == SendState::Sending ? "Still sending..." : "Nothing to retry", 1500);
            return;
        }
        Messenger& mm = messenger();
        const uint32_t node = it->node;
        const int index = it->index;
        mm.retry_message(node, index);
        const Message* after = index < static_cast<int>(mm.messages(node).size()) ? &mm.messages(node)[index] : nullptr;
        if (after && after->state == SendState::Unsent) {
            toast(after->error == kSendErrQueued ? mm.name_of(node) + " is still offline: kept in the Outbox"
                                                 : std::string("Not sent: radio not connected"),
                  2000, color::kWarn);
        } else if (after && after->state == SendState::Delivered) {
            toast("Delivered to " + mm.name_of(node), 1500, color::kGood);
        } else {
            toast("Sending to " + mm.name_of(node) + "...", 1500);
        }
        refresh();
    }

    void ask_delete() {
        const Item* it = current();
        const Message* m = it ? msg(*it) : nullptr;
        if (!m || !m->retryable()) {
            toast(m && m->state == SendState::Sending ? "On its way: cannot delete now" : "Only queued mail can be deleted",
                  1500);
            return;
        }
        const uint32_t node = it->node;
        const uint32_t ts = m->timestamp;
        const std::string text = m->text;
        const std::string who = messenger().name_of(node);
        ctx_.router.show_modal(std::make_unique<Modal>(
            "Delete message", "Delete this message to " + who + "? It will not be sent.",
            std::vector<std::string>{"Delete", "Cancel"},
            [this, node, ts, text](int choice) {
                if (choice != 0) return;
                // Find it again: the outbox may have moved things meanwhile.
                const std::vector<Message>& v = messenger().messages(node);
                for (int i = static_cast<int>(v.size()) - 1; i >= 0; --i) {
                    if (v[i].from_me && v[i].timestamp == ts && v[i].text == text && v[i].retryable()) {
                        messenger().delete_message(node, i);
                        toast("Message deleted", 1200);
                        break;
                    }
                }
                open_ = false;
                if (pane_) refresh();
                ctx_.router.refresh_chrome();
            }));
    }

    void announce(const MailNotice& n) {
        const std::string who = messenger().name_of(n.node_id);
        std::string preview = n.text;
        if (preview.size() > 60) preview = preview.substr(0, 57) + "...";
        if (n.kind == MailNotice::Kind::NewMail) {
            toast(std::string(LV_SYMBOL_ENVELOPE "  You've got mail!\n") + who + ": " + preview, 3500,
                  color::kAccent);
        } else {
            toast(std::string(LV_SYMBOL_OK "  Delivered to ") + who + "\n" + preview, 3000, color::kGood);
        }
        play_mail_chime();
    }

    // ── Painting ────────────────────────────────────────────────────────────

    void refresh() {
        seen_rev_ = messenger().revision();
        collect();
        paint_folders();
        paint_pane();
        const int unread = messenger().total_unread();
        ctx_.router.set_status_note(unread == 0 ? std::string()
                                    : fmt("%d new message%s", unread, unread == 1 ? "" : "s"));
    }

    void paint_folders() {
        std::vector<win::Row> rows;
        for (int f = 0; f < kFolderCount; ++f) {
            int unread = 0;
            const int n = count_folder(f, &unread);
            win::Row r;
            r.text = kFolderName[f];
            if (f == kInbox && unread > 0) {
                r.text += fmt(" (%d)", unread);
                r.bold = true;
            } else if (f == kOutbox && n > 0) {
                r.text += fmt(" (%d)", n);
                r.bold = true;
            }
            rows.push_back(r);
        }
        folders_->set_rows(rows);
        folders_->select(folder_);
        folders_->set_active(false);   // the message list has the keys
        folders_->repaint();
    }

    void paint_pane() {
        lv_obj_clean(pane_);
        if (open_ && current()) {
            paint_open();
        } else {
            open_ = false;
            paint_list();
            paint_preview();
        }
    }

    void column_header(int x, int w, const char* name) {
        const Skin& s = skin();
        lv_obj_t* h = win::frame(pane_, x, 0, w, kHeadH, win::Edge::Raised, s.face);
        win::text(h, name, 3, 0, s.text, w - 5, false);
    }

    void paint_list() {
        const Skin& s = skin();
        column_header(0, kWhoW, folder_ == kInbox ? "From" : "To");
        column_header(kWhoW, kTextColW, "Message");
        column_header(kWhoW + kTextColW, kStatW, folder_ == kInbox ? "Recv'd" : "Status");

        lv_obj_t* f = win::field(pane_, 0, kHeadH, kPaneW, kListH);
        const int row_h = std::max(12, static_cast<int>(lv_font_get_line_height(font_ui())) + 1);
        const int visible = std::max(1, (kListH - 2) / row_h);
        const int n = static_cast<int>(items_.size());
        if (n == 0) {
            static const char* const kEmpty[kFolderCount] = {
                "No mail yet.", "Outbox is empty: everything went out.", "Nothing sent yet."};
            win::text(f, kEmpty[folder_], 4, 2, s.dim, kPaneW - 8, false);
            return;
        }
        if (sel_ < top_) top_ = sel_;
        if (sel_ >= top_ + visible) top_ = sel_ - visible + 1;
        top_ = std::clamp(top_, 0, std::max(0, n - visible));

        Messenger& mm = messenger();
        for (int v = 0; v < visible && top_ + v < n; ++v) {
            const Item& it = items_[top_ + v];
            const Message* m = msg(it);
            if (!m) continue;
            const bool sel = top_ + v == sel_;
            const int y = 1 + v * row_h;
            uint32_t fg = s.field_text, dimc = s.dim;
            if (sel) {
                lv_obj_t* hl = box(f, 1, y, kPaneW - 2, row_h);
                lv_obj_set_style_bg_color(hl, c(s.sel), 0);
                lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, 0);
                fg = dimc = s.sel_text;
            }
            const bool bold = it.unread;
            win::text(f, mm.name_of(it.node), 3, y, fg, kWhoW - 5, bold);
            win::text(f, m->text, kWhoW + 2, y, fg, kTextColW - 5, bold);
            std::string st;
            uint32_t stc = dimc;
            bool stbold = false;
            if (folder_ == kInbox) {
                st = it.unread ? "New" : ago_text(m->timestamp);
                stbold = it.unread;
                if (it.unread && !sel) stc = s.me;
            } else {
                st = status_word(*m);
                if (m->state == SendState::Failed) {
                    stbold = true;
                    if (!sel) stc = s.me;
                } else if (queued(*m) && !sel) {
                    stc = s.them;
                }
            }
            win::text(f, st, kWhoW + kTextColW + 2, y, stc, kStatW - 6, stbold);
        }
        if (n > visible) {
            const int track = kListH - 2;
            const int th = std::max(6, track * visible / n);
            const int ty = 1 + (track - th) * top_ / std::max(1, n - visible);
            lv_obj_t* thumb = box(f, kPaneW - 3, ty, 2, th);
            lv_obj_set_style_bg_color(thumb, c(s.dim), 0);
            lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
        }
    }

    // One line saying where a message of mine is and what happens next.
    std::string detail(const Item& it, const Message& m) const {
        Messenger& mm = messenger();
        const std::string who = mm.name_of(it.node);
        const Contact* c = mm.contact(it.node);
        const bool online = c && c->online();
        switch (m.state) {
            case SendState::Sending:
                return "Sending to " + who + "...";
            case SendState::Sent:
                return Messenger::is_channel(it.node) ? "Sent to the channel" : "Sent, delivery not confirmed";
            case SendState::Delivered:
                if (Messenger::is_channel(it.node)) return "Sent, relayed by the mesh";
                if (m.hops <= 0) return fmt("Delivered direct, SNR %.1f", static_cast<double>(m.snr));
                return fmt("Delivered, %d hop%s, SNR %.1f", m.hops, m.hops == 1 ? "" : "s",
                           static_cast<double>(m.snr));
            case SendState::Unsent:
            case SendState::Failed: {
                const bool failed = m.state == SendState::Failed;
                const std::string why = failed ? std::string("Failed: ") + send_error_text(m.error)
                                        : m.error == kSendErrQueued ? std::string("Queued")
                                                                    : std::string("Queued: ") + send_error_text(m.error);
                if (!mm.auto_send_pending(it.node, m)) {
                    if (m.tries >= Messenger::kOutboxMaxTries) return why + fmt(", gave up after %d tries", m.tries);
                    return why + ", R to send";
                }
                if (!online) return failed ? why + ", retries when online" : "Queued till " + who + " is online";
                const uint32_t wait = mm.auto_send_wait_ms(m);
                if (wait > 0) return why + ", retry in " + wait_text(wait);
                return why + ", sending soon";
            }
        }
        return "";
    }

    void paint_preview() {
        const Skin& s = skin();
        lv_obj_t* f = win::card(pane_, 0, kPrevY, kPaneW, kPrevH);
        const Item* it = current();
        const Message* m = it ? msg(*it) : nullptr;
        const int tx = 4, tw = kPaneW - 8;
        if (!m) {
            const int out = messenger().outbox_count();
            win::text(f, folder_ == kOutbox ? "Mail to an offline buddy waits here" : "Select a message to read it.",
                      tx, 2, s.card_text, tw, false);
            if (folder_ == kOutbox) {
                win::text(f, "and goes out when they are heard again.", tx, 14, s.card_text, tw, false);
            } else if (out > 0) {
                win::text(f, fmt("%d message%s waiting in the Outbox.", out, out == 1 ? "" : "s"), tx, 14,
                          s.card_text, tw, false);
            }
            return;
        }
        Messenger& mm = messenger();
        const std::string who = mm.name_of(it->node);
        const std::string head = (m->from_me ? "To: " : "From: ") + who;
        lv_obj_t* when = win::text(f, ago_text(m->timestamp) == "now" ? "just now" : ago_text(m->timestamp) + " ago",
                                   0, 2, s.card_text, 0, false);
        lv_obj_update_layout(when);
        const int ww = lv_obj_get_width(when);
        lv_obj_set_x(when, kPaneW - 6 - ww);
        win::text(f, head, tx, 2, s.card_text, tw - ww - 6, true);
        int y = 14;
        if (m->from_me) {
            win::text(f, detail(*it, *m), tx, y, m->retryable() ? s.me : s.card_text, tw, false);
            y += 12;
        }
        lv_obj_t* body = win::text_wrap(f, m->text, tx, y, tw, s.card_text);
        // Clip to the card: a long message continues when opened.
        lv_obj_set_height(body, kPrevH - y - 3);
        lv_obj_set_style_clip_corner(body, false, 0);
        lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_DOTS);
    }

    void paint_open() {
        const Skin& s = skin();
        const Item& it = *current();
        const Message* m = msg(it);
        if (!m) return;
        Messenger& mm = messenger();
        const std::string me = mm.my_name();
        const std::string them = mm.name_of(it.node);
        auto field_line = [&](int y, const char* label, const std::string& value, uint32_t color) {
            win::text(pane_, label, 2, y, s.dim, 40, false);
            win::text(pane_, value, 40, y, color, kPaneW - 42, false);
        };
        field_line(0, "From:", m->from_me ? me : them, s.text);
        field_line(11, "To:", m->from_me ? them : me, s.text);
        field_line(22, "Date:", date_text(m->timestamp), s.text);
        int y = 33;
        if (m->from_me) {
            field_line(y, "Status:", detail(it, *m), m->retryable() ? s.me : s.text);
            y += 11;
        } else if (m->auto_response) {
            field_line(y, "Note:", "Auto response (away message)", s.text);
            y += 11;
        }
        const int by = kClientH - 16;
        lv_obj_t* f = win::field(pane_, 0, y + 1, kPaneW, by - y - 3);
        win::text_wrap(f, m->text, 4, 2, kPaneW - 10, s.field_text);

        int bx = 0;
        win::button(pane_, "Reply", bx, by, 54, true);
        bx += 57;
        if (m->retryable()) {
            win::button(pane_, "Retry now", bx, by, 70);
            bx += 73;
            win::button(pane_, "Delete", bx, by, 54);
        }
    }

    // ── --shots data ────────────────────────────────────────────────────────

    struct Seeded {
        uint32_t node;
        uint32_t ts;
        std::string text;
    };

    void seed() {
        Messenger& m = messenger();
        if (seeded_ || !m.demo()) return;
        seeded_ = true;
        const uint32_t t = now_s();
        const uint32_t tick = lv_tick_get();
        auto add = [&](const char* name, bool mine, const char* text, uint32_t ago, SendState st,
                       int err = kSendErrNone, int tries = 0, bool unread = false) {
            const uint32_t node = find_by_name(name);
            if (!node) return;
            if (!m.has_chat(node) && std::find(created_.begin(), created_.end(), node) == created_.end()) {
                created_.push_back(node);
            }
            Message x;
            x.from_me = mine;
            x.text = text;
            x.timestamp = t - ago;
            x.state = st;
            x.error = err;
            x.tries = static_cast<uint8_t>(tries);
            x.via_outbox = tries > 0;
            if (const Contact* c = m.contact(node)) {
                x.hops = c->hops;
                x.snr = c->snr;
            }
            if (st == SendState::Failed && tries > 0 && tries < Messenger::kOutboxMaxTries) {
                x.backoff = true;
                x.retry_after_ms = tick + 70000;
            }
            m.add_message(node, x, unread);
            seeded_list_.push_back(Seeded{node, x.timestamp, text});
        };
        // Inbox: two new letters.
        add("CabinFever99", false, "caught a 5lb bass!! come see", 4 * 60, SendState::Delivered, 0, 0, true);
        add("HilltopRelay", false, "relay maintenance sat 9am, expect a short outage", 52 * 60,
            SendState::Delivered);
        // Outbox: queued for offline buddies, one failing and retrying, one on its way.
        add("ash_k3tchup", true, "rematch tmrw? my charmander leveled up", 25 * 60, SendState::Unsent,
            kSendErrQueued);
        add("W2XYZ", true, "73! u still on the lake this wknd?", 3 * 3600, SendState::Unsent, kSendErrQueued);
        add("solar_shed", true, "is the panel still on?", 40, SendState::Sending);
        add("N0CALL-7", true, "can u relay to the ridge?", 18 * 60, SendState::Failed, 1, 2);
        add("sk8rmesh", true, "skate park sat?", 2 * 86400, SendState::Failed, 3, Messenger::kOutboxMaxTries);
    }

    void unseed() {
        if (!seeded_) return;
        Messenger& m = messenger();
        for (uint32_t node : created_) m.delete_chat(node);
        for (const Seeded& sd : seeded_list_) {
            if (std::find(created_.begin(), created_.end(), sd.node) != created_.end()) continue;
            const std::vector<Message>& v = m.messages(sd.node);
            for (int i = static_cast<int>(v.size()) - 1; i >= 0; --i) {
                if (v[i].timestamp == sd.ts && v[i].text == sd.text) {
                    m.delete_message(sd.node, i);
                    break;
                }
            }
        }
        MailNotice n;
        while (m.pop_mail_notice(n)) {}
    }

    lv_obj_t* body_ = nullptr;
    lv_obj_t* pane_ = nullptr;
    std::unique_ptr<win::ListBox> folders_;
    std::vector<Item> items_;
    int folder_ = kInbox;
    int sel_ = 0;
    int top_ = 0;
    bool open_ = false;
    uint32_t seen_rev_ = 0;
    uint32_t last_paint_ms_ = 0;
    bool seeded_ = false;
    std::vector<uint32_t> created_;      // chats the --shots data made: deleted again
    std::vector<Seeded> seeded_list_;    // messages it added to existing chats
};

} // namespace

std::unique_ptr<Screen> make_mailbox_screen(Ctx& ctx) {
    return std::make_unique<MailboxScreen>(ctx);
}

} // namespace mm::ui
