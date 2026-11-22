// buddy_screen.cpp — the Buddy List: grouped contacts on the left, an info
// card and actions for the selected contact on the right.
//
// Groups, in this order: MonsterMesh Trainers (every node running
// MonsterMesh), Channels (the public mesh channel: one row; only with a radio,
// or once a channel conversation exists), Buddies (other nodes we have a
// conversation with, until the chat is deleted), Everyone else (nodes heard on
// the mesh we never chatted with; collapsed at first). Data comes from
// mm::messenger().
#include "screens.h"

#include "messenger.h"
#include "router.h"
#include "widgets.h"
#include "win.h"

#include <string>
#include <vector>

namespace mm::ui {
namespace {

constexpr int kListW = 152;
constexpr int kPaneX = 156;
constexpr int kPaneW = kClientW - kPaneX;   // 156
constexpr int kCardH = 79;
constexpr int kLineH = 12;

enum Group { kTrainers = 0, kChannels, kBuddies, kOthers, kGroupCount };

constexpr uint32_t kChannel = Messenger::kChannelNode;
const char* const kGroupLong[kGroupCount] = {"MonsterMesh Trainers", "Channels", "Buddies", "Everyone else"};
const char* const kGroupShort[kGroupCount] = {"Trainers", "Channels", "Buddies", "Others"};

// The Channels group exists once there is a radio (live) or a channel chat.
bool channels_shown() {
    return !messenger().demo() || messenger().has_chat(kChannel);
}

int group_of(const Contact& c) {
    if (c.is_trainer) return kTrainers;
    return messenger().has_chat(c.node_id) ? kBuddies : kOthers;
}

std::string heard_text(uint32_t s) {
    if (s == Contact::kNeverHeard) return "a while ago";
    if (s < 45) return "just now";
    if (s < 3600) return fmt("%u min ago", (s + 30) / 60);
    if (s < 86400) return fmt("%u hr ago", s / 3600);
    const unsigned d = s / 86400;
    return fmt("%u day%s ago", d, d == 1 ? "" : "s");
}

const char* presence_text(Presence p) {
    switch (p) {
        case Presence::Online: return "online";
        case Presence::Away:   return "away";
        default:               return "offline";
    }
}

class BuddyScreen final : public Screen {
public:
    explicit BuddyScreen(Ctx& ctx) : Screen(ctx) {
        title_ = messenger().my_name() + "'s Buddy List";
        seen_name_ = messenger().my_name();
        collapsed_[kOthers] = true;
        // Start on the first trainer / buddy rather than on a header.
        for (const Contact& c : messenger().contacts()) {
            if (group_of(c) != kOthers) {
                sel_node_ = c.node_id;
                break;
            }
        }
    }

    ~BuddyScreen() override {
        if (seeded_channel_) messenger().delete_chat(kChannel);   // --shots only
    }

    const char* title() const override { return title_.c_str(); }
    bool windowed() const override { return true; }
    std::string hints() const override {
        if (channel_selected()) return "Enter:open channel  D:del chat  Esc:back";
        const Contact* c = selected_contact();
        if (!c) return "Enter:open/close group  Esc:back";
        return std::string("Enter:IM  B:") + (c->is_trainer ? "battle" : "ping") + "  S:space  D:del chat  Esc:back";
    }

    void build(lv_obj_t* body) override {
        list_ = std::make_unique<win::ListBox>(body, 0, 0, kListW, kClientH);
        pane_ = box(body, kPaneX, 0, kPaneW, kClientH);
        refresh();
    }

    void on_show() override {
        if (list_) refresh();
    }

    void tick(uint32_t now_ms) override {
        messenger().tick(now_ms);
        if (list_ && messenger().revision() != seen_rev_) refresh();
    }

    bool on_key(const KeyEvent& e) override {
        if (!list_) return false;
        if (list_->on_key(e)) {
            remember_selection();
            paint_pane();
            return true;
        }
        if (e.key == Key::Up || e.key == Key::Down) return true;   // at either end
        const Contact* c = selected_contact();
        if (channel_selected()) {
            if (e.key == Key::A || (e.key == Key::Char && e.ch == 'i')) {
                ctx_.peer = kChannel;
                ctx_.router.push(ScreenId::Im);
                return true;
            }
            if (e.key == Key::Char && e.ch == 'd') {
                ask_delete(kChannel, Messenger::channel_name());
                return true;
            }
            return false;
        }
        if (e.key == Key::A) {
            if (c) {
                open_im(*c);
            } else if (sel_group_ >= 0) {
                collapsed_[sel_group_] = !collapsed_[sel_group_];
                refresh();
            }
            return true;
        }
        if (e.key != Key::Char || !c) return false;
        switch (e.ch) {
            case 'i':
                open_im(*c);
                return true;
            case 'b':
                if (c->is_trainer) {
                    ctx_.battle = BattleRequest{BattleRequest::Kind::Mesh, 0, c->node_id, 0, 0};
                    ctx_.router.push(ScreenId::Battle);
                } else {
                    toast("Ping sent", 1500);
                }
                return true;
            case 's':
            case 'w':
                ctx_.peer = c->node_id;
                ctx_.router.push(ScreenId::Space);
                return true;
            case 'd':
                ask_delete(c->node_id, c->name);
                return true;
            default:
                return false;
        }
    }

    int demo_state_count() const override { return 4; }
    const char* demo_state_name(int i) const override {
        switch (i) {
            case 1:  return "others";
            case 2:  return "buddy";
            case 3:  return "channels";
            default: return "main";
        }
    }
    void set_demo_state(int i) override {
        collapsed_[kTrainers] = collapsed_[kChannels] = collapsed_[kBuddies] = false;
        collapsed_[kOthers] = i != 1;
        if (i == 3) {
            // The channel group as it looks with a radio: one read, one new line.
            Messenger& m = messenger();
            if (m.demo() && !m.has_chat(kChannel)) {
                seeded_channel_ = true;
                m.receive_channel(0x41110001, "net check-in at 8, all stations", 1, 4.75f);
                m.mark_read(kChannel);
                m.receive_channel(0x5011a4b2, "solar_shed checking in", 2, 1.5f);
            }
            sel_node_ = kChannel;
            sel_group_ = -1;
            return;
        }
        const char* want = i == 1 ? "N0CALL-7" : i == 2 ? "Trailhead-R" : "KD2ABC-m";
        for (const Contact& c : messenger().contacts()) {
            if (c.name == want) {
                sel_node_ = c.node_id;
                sel_group_ = -1;
            }
        }
    }

private:
    struct RowRef {
        int group = -1;          // header row of this group, or -1
        uint32_t node = 0;       // contact row
    };

    // nullptr on a header and on the channel row.
    const Contact* selected_contact() const {
        return sel_group_ < 0 && sel_node_ ? messenger().contact(sel_node_) : nullptr;
    }
    bool channel_selected() const { return sel_group_ < 0 && sel_node_ == kChannel; }

    void remember_selection() {
        const int i = list_->selected();
        if (i < 0 || i >= static_cast<int>(refs_.size())) return;
        sel_group_ = refs_[i].group;
        sel_node_ = refs_[i].node;
    }

    // Rebuild the rows from the model, keeping the selection on the same
    // contact (or header), then repaint the right-hand pane.
    void refresh() {
        Messenger& m = messenger();
        seen_rev_ = m.revision();
        if (m.my_name() != seen_name_) {   // the radio told us who we are
            seen_name_ = m.my_name();
            title_ = seen_name_ + "'s Buddy List";
            ctx_.router.rebuild();         // repaints the title bar; build() calls refresh()
            return;
        }
        const bool channels = channels_shown();

        int total[kGroupCount] = {}, on[kGroupCount] = {};
        for (const Contact& c : m.contacts()) {
            const int g = group_of(c);
            ++total[g];
            if (c.online()) ++on[g];
        }
        std::vector<win::Row> rows;
        refs_.clear();
        for (int g = 0; g < kGroupCount; ++g) {
            if (g == kChannels && !channels) continue;
            const std::string mark = collapsed_[g] ? "> " : "v ";
            const std::string count = g == kChannels ? std::string(" (1)")
                                    : g == kOthers   ? fmt(" (%d)", total[g])
                                                     : fmt(" (%d/%d)", on[g], total[g]);
            win::Row h;
            h.header = true;
            h.text = mark + kGroupLong[g] + count;
            h.short_text = mark + kGroupShort[g] + count;
            rows.push_back(h);
            refs_.push_back(RowRef{g, 0});
            if (collapsed_[g]) continue;
            if (g == kChannels) {
                win::Row r;
                r.indent = 10;
                r.text = Messenger::channel_name();
                const int unread = m.unread(kChannel);
                if (unread > 0) {
                    r.text += fmt(" (%d)", unread);
                    r.bold = true;
                }
                rows.push_back(r);
                refs_.push_back(RowRef{-1, kChannel});
                continue;
            }
            for (const Contact& c : m.contacts()) {
                if (group_of(c) != g) continue;
                win::Row r;
                r.indent = 10;
                r.text = c.name;
                r.dim = !c.online();
                const int unread = m.unread(c.node_id);
                if (unread > 0) {
                    r.text += fmt(" (%d)", unread);
                    r.bold = true;
                }
                rows.push_back(r);
                refs_.push_back(RowRef{-1, c.node_id});
            }
        }
        list_->set_rows(rows);

        // Find the remembered selection again; a contact whose group is now
        // collapsed (or that vanished) falls back to its group header.
        int want = -1;
        if (sel_group_ < 0 && sel_node_) {
            for (int i = 0; i < static_cast<int>(refs_.size()) && want < 0; ++i) {
                if (refs_[i].group < 0 && refs_[i].node == sel_node_) want = i;
            }
            if (want < 0) {
                const Contact* c = m.contact(sel_node_);
                sel_group_ = c ? group_of(*c) : (sel_node_ == kChannel && channels) ? kChannels : kTrainers;
            }
        }
        if (want < 0) {
            for (int i = 0; i < static_cast<int>(refs_.size()) && want < 0; ++i) {
                if (refs_[i].group == sel_group_) want = i;
            }
        }
        if (want >= 0) list_->select(want);
        remember_selection();

        const int unread = m.total_unread();
        ctx_.router.set_status_note(unread == 0 ? std::string()
                                    : fmt("%d new message%s", unread, unread == 1 ? "" : "s"));
        paint_pane();
    }

    void paint_pane() {
        const Skin& s = skin();
        lv_obj_clean(pane_);
        win::card(pane_, 0, 0, kPaneW, kCardH);
        const int tx = 5, tw = kPaneW - 10;
        int y = 3;
        auto line = [&](const std::string& t) {
            win::text(pane_, t, tx, y, s.card_text, tw);
            y += kLineH;
        };

        if (channel_selected()) {
            const std::vector<Message>& msgs = messenger().messages(kChannel);
            win::text(pane_, Messenger::channel_name(), tx, y, s.card_text, tw, true);
            y += kLineH + 1;
            line("Public channel 0.");
            line("Everyone in range can read it.");
            if (msgs.empty()) {
                line("No messages yet.");
            } else {
                const int unread = messenger().unread(kChannel);
                line(fmt("%d message%s, %d new", static_cast<int>(msgs.size()), msgs.size() == 1 ? "" : "s", unread));
                const Message& last = msgs.back();
                line("Last: " + (last.from_me ? messenger().my_name() : messenger().name_of(last.from_node)));
            }
            const int by = kCardH + 4;
            win::button(pane_, "Open", 0, by, 50, true);
            if (messenger().has_chat(kChannel)) win::button(pane_, "Delete chat", 0, by + 20, kPaneW);
            return;
        }

        const Contact* c = selected_contact();
        if (!c) {
            static const char* const kWhat[kGroupCount] = {
                "Nodes running MonsterMesh.",
                "Public mesh channels.",
                "Nodes you have a chat with.",
                "Other nodes heard on the mesh.",
            };
            const int g = sel_group_ < 0 ? 0 : sel_group_;
            win::text(pane_, kGroupLong[g], tx, y, s.card_text, tw, true);
            y += kLineH + 1;
            line(kWhat[g]);
            line(collapsed_[g] ? "Enter: open the group" : "Enter: close the group");
            return;
        }

        // Name (bold) with the presence right-aligned on the same line.
        lv_obj_t* pres = win::text(pane_, presence_text(c->presence), 0, y, s.card_text, 0, false);
        lv_obj_update_layout(pres);
        const int pw = lv_obj_get_width(pres);
        lv_obj_set_x(pres, kPaneW - 5 - pw);
        win::text(pane_, c->name, tx, y, s.card_text, tw - pw - 4, true);
        y += kLineH + 1;

        if (c->is_trainer) {
            line(fmt("Badges: %d of 8, %dW %dL", c->badges, c->pvp_wins, c->pvp_losses));
            line(fmt("Lead: Lv %d, party of %d", c->lead_level, c->party_size));
        }
        line(fmt("SNR %.1f dB, %d hop%s", static_cast<double>(c->snr), c->hops, c->hops == 1 ? "" : "s"));
        const std::string heard = "Heard " + heard_text(c->last_heard_s);
        const std::string batt = c->battery >= 0 ? fmt("Battery %d%%", c->battery) : std::string("Battery unknown");
        if (c->is_trainer) {
            // Two more lines of trainer info: keep heard and battery on one.
            line(c->battery >= 0 ? heard + fmt(", batt %d%%", c->battery) : heard);
        } else {
            line(heard);
            line(batt);
        }
        if (!c->away_msg.empty()) {
            line("Away: " + c->away_msg);
        } else if (c->is_trainer && c->hosts_gym) {
            line("Hosts a gym");
        }

        const int by = kCardH + 4;
        win::button(pane_, "IM", 0, by, 50, true);
        win::button(pane_, c->is_trainer ? "Battle" : "Ping", 53, by, 50);
        win::button(pane_, "Space", 106, by, 50);
        if (messenger().has_chat(c->node_id)) {
            win::button(pane_, "Delete chat", 0, by + 20, kPaneW);
        }
    }

    void open_im(const Contact& c) {
        ctx_.peer = c.node_id;
        ctx_.router.push(ScreenId::Im);
    }

    void ask_delete(uint32_t id, const std::string& name) {
        if (!messenger().has_chat(id)) {
            toast("No chat with " + name, 1500);
            return;
        }
        ctx_.router.show_modal(std::make_unique<Modal>(
            "Delete chat", "Delete chat with " + name + "?",
            std::vector<std::string>{"Delete", "Cancel"},
            [id, name](int choice) {
                if (choice != 0) return;
                messenger().delete_chat(id);   // tick() sees the new revision and refreshes
                toast("Deleted chat with " + name, 1500);
            }));
    }

    std::string title_;
    std::unique_ptr<win::ListBox> list_;
    lv_obj_t* pane_ = nullptr;
    std::vector<RowRef> refs_;
    bool collapsed_[kGroupCount] = {};
    uint32_t sel_node_ = 0;      // selected contact (when sel_group_ < 0)
    int sel_group_ = -1;         // selected header, or -1
    uint32_t seen_rev_ = 0;
    std::string seen_name_;      // my name the title was made from
    bool seeded_channel_ = false;
};

} // namespace

std::unique_ptr<Screen> make_buddy_screen(Ctx& ctx) {
    return std::make_unique<BuddyScreen>(ctx);
}

} // namespace mm::ui
