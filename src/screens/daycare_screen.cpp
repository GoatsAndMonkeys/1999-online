// daycare_screen.cpp — the mesh daycare: event log, nearby trainers, PvP.
//
// Layout (body 320x138):
//   y  0..15  status strip: own trainer / short name / node id | daycare,
//             nearby count, radio. Replaced by a yellow banner while an
//             incoming challenge is pending (ACCEPT / DECLINE soft keys).
//   Soft keys (keys 4..8 under the display): LOG NEARBY EVENT BEACON HOLLA;
//   ACCEPT DECLINE while a challenge is pending; BATTLE in a party view.
//   y 17..32  tab row: [Log n] [Nearby n] | event total, last beacon result
//   y 34..137 content: Log list, Nearby list, or a neighbour's party.
#include "screens.h"

#include "game_state.h"
#include "router.h"
#include "widgets.h"

#include <chrono>
#include <ctime>
#include <memory>

namespace mm::ui {
namespace {

constexpr int kStripH = 16;
constexpr int kTabY = 17;
constexpr int kTabH = 16;
constexpr int kContentY = 34;
constexpr int kContentH = kBodyH - kContentY;   // 104
constexpr int kListX = 2;
constexpr int kListW = kBodyW - 2;              // rows are kListW - 4 wide
constexpr int kRowH = 34;                       // 3 visible rows
constexpr int kRowW = kListW - 4;

enum class Tab : uint8_t { Log, Nearby };
enum class View : uint8_t { Main, Party };

uint32_t steady_ms32() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() & 0xFFFFFFFFu);
}

std::string ago_secs(long long sec) {
    if (sec < 0) sec = 0;
    if (sec < 45) return "now";
    if (sec < 3600) return fmt("%lldm ago", (sec + 30) / 60);
    if (sec < 86400) return fmt("%lldh ago", sec / 3600);
    return fmt("%lldd ago", sec / 86400);
}

// Neighbour last_seen_ms is the daemon's millis() (monotonic clock on the
// same device). Returns -1 if unknown / implausible.
long long seen_age_sec(uint32_t last_seen_ms) {
    if (last_seen_ms == 0) return -1;
    const uint32_t now = steady_ms32();
    const uint32_t d = now - last_seen_ms;
    if (d > 7u * 86400u * 1000u) return -1;   // wrapped / other clock
    return d / 1000;
}

uint32_t freshness_color(long long age) {
    if (age < 0) return color::kDim;
    if (age < 120) return color::kGood;
    if (age < 900) return color::kWarn;
    return color::kBad;
}

std::string node_str(uint32_t id) { return id ? fmt("!%08x", id) : std::string("!--------"); }

lv_obj_t* flex_row(lv_obj_t* parent, int x, int y, int w, int h, bool align_end, int gap) {
    lv_obj_t* r = box(parent, x, y, w, h);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, align_end ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    return r;
}

lv_obj_t* dot(lv_obj_t* parent, uint32_t col) {
    lv_obj_t* d = box(parent, 0, 0, 6, 6);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, c(col), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    return d;
}

lv_obj_t* right_label(lv_obj_t* parent, const std::string& text, int x, int y, int w,
                      const lv_font_t* f, uint32_t col) {
    lv_obj_t* l = label(parent, text, x, y, f, col, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    return l;
}

class DaycareScreen final : public Screen {
public:
    explicit DaycareScreen(Ctx& ctx) : Screen(ctx) {
        pvp_seen_ = ctx_.game.last_pvp_status();
        if (auto ch = ctx_.game.pending_challenge()) toast_hidden_node_ = ch->node_id;
    }

    const char* title() const override { return "Daycare"; }

    std::string hints() const override {
        if (ctx_.game.pending_challenge()) return "a:accept  d:decline  Tab:switch  Esc:back";
        if (view_ == View::Party) return "Enter:battle  Esc:back";
        if (tab_ == Tab::Log) return "Tab:Nearby  Enter:details  e:event p:beacon h:holla";
        return "Tab:Log  Enter:actions  e:event  p:beacon  h:holla";
    }

    SoftKeys softkeys() const override {
        SoftKeys k;
        k.left.text = MM_SOFT_BACK;
        if (ctx_.game.pending_challenge()) {
            k.set(0, "ACCEPT").set(1, "DECLINE");
        } else if (view_ == View::Party) {
            k.set(0, "BATTLE");
        } else {
            k.set(0, "LOG").set(1, "NEARBY").set(2, "EVENT").set(3, "BEACON").set(4, "HOLLA");
            k.selected = tab_ == Tab::Log ? 0 : 1;
        }
        return k;
    }

    void build(lv_obj_t* body) override {
        rev_ = ctx_.game.revision();
        built_ms_ = steady_ms32();
        list_.reset();
        build_strip(body);
        build_tabs(body);
        if (view_ == View::Party) build_party(body);
        else if (tab_ == Tab::Log) build_log(body);
        else build_nearby(body);
    }

    bool on_key(const KeyEvent& in) override {
        auto& g = ctx_.game;
        // Soft keys: the same actions as the letter keys / Enter.
        KeyEvent e = in;
        const int slot = softkey_slot(in);
        if (slot == kSoftLeft) {
            if (view_ != View::Party) return false;   // Router: back
            e.key = Key::B;
        } else if (slot >= 0 && slot < kSoftKeyCount) {
            static const char kMain[] = {'1', '2', 'e', 'p', 'h'};
            if (g.pending_challenge()) {
                if (slot > 1) return true;
                e.ch = slot == 0 ? 'a' : 'd';
            } else if (view_ == View::Party) {
                if (slot != 0) return true;
                e.key = Key::A;
            } else {
                e.ch = kMain[slot];
            }
        }

        // Incoming challenge: a / d answer it from anywhere on this screen.
        if (e.key == Key::Char && (e.ch == 'a' || e.ch == 'd')) {
            if (auto ch = g.pending_challenge()) {
                if (e.ch == 'a') {
                    const uint32_t node = ch->node_id;
                    g.accept_challenge();
                    pvp_seen_ = g.last_pvp_status();
                    ctx_.battle = BattleRequest{};
                    ctx_.battle.kind = BattleRequest::Kind::Mesh;
                    ctx_.battle.node_id = node;
                    ctx_.router.push(ScreenId::Battle);
                } else {
                    const std::string who = trainer_name(ch->node_id, ch->trainer);
                    g.decline_challenge();
                    pvp_seen_ = g.last_pvp_status();
                    toast("Declined " + who, 1800, color::kDim);
                    ctx_.router.rebuild();
                }
                return true;
            }
        }

        if (e.key == Key::Char) {
            switch (e.ch) {
                case 'e':
                    g.force_event();
                    toast(LV_SYMBOL_REFRESH " Daycare event forced", 1600);
                    return true;
                case 'p':
                    g.force_beacon();
                    toast(LV_SYMBOL_WIFI " Beacon sent", 1600, color::kInfo);
                    return true;
                case 'h':
                    g.hollaback();
                    toast(LV_SYMBOL_LOOP " Hollaback: asking the mesh to re-announce", 2000, color::kInfo);
                    return true;
                case '1': if (slot == 0) set_tab(Tab::Log); return true;      // soft keys only
                case '2': if (slot == 1) set_tab(Tab::Nearby); return true;
                default: break;
            }
        }

        if (view_ == View::Party) {
            if (e.key == Key::B) {
                view_ = View::Main;
                ctx_.router.rebuild();
                return true;
            }
            if (e.key == Key::A) {
                send_challenge(party_node_);
                return true;
            }
            return true;   // swallow arrows/tab in the sub-view
        }

        if (e.key == Key::Tab || ((e.key == Key::Left || e.key == Key::Right) && !e.repeat)) {
            set_tab(tab_ == Tab::Log ? Tab::Nearby : Tab::Log);
            return true;
        }
        if (e.key == Key::Up || e.key == Key::Down) {
            if (list_ && list_->on_key(e)) {
                (tab_ == Tab::Log ? log_sel_ : near_sel_) = list_->selected();
            }
            return true;
        }
        if (e.key == Key::A && list_ && list_->count() > 0) {
            if (tab_ == Tab::Log) open_event(list_->selected());
            else open_neighbor(list_->selected());
            return true;
        }
        return false;
    }

    void tick(uint32_t) override {
        auto& g = ctx_.game;
        // The router toasts "X wants to battle! Open Battle to answer." — on
        // this screen the banner answers it in place, so drop that toast once.
        if (auto ch = g.pending_challenge()) {
            if (ch->node_id != toast_hidden_node_) {
                toast_hidden_node_ = ch->node_id;
                toast_hide();
            }
        } else {
            toast_hidden_node_ = 0;
        }
        if (g.last_pvp_status() != pvp_seen_) {
            pvp_seen_ = g.last_pvp_status();
            if (!pvp_seen_.empty()) toast(pvp_seen_, 2400, color::kInfo);
        }
        // Rebuild on data change, and every 20 s so relative times stay fresh.
        if (g.revision() != rev_ || steady_ms32() - built_ms_ > 20000) ctx_.router.rebuild();
    }

    int demo_state_count() const override { return 4; }
    const char* demo_state_name(int i) const override {
        static const char* const kNames[] = {"log", "nearby", "party", "challenge"};
        return (i >= 0 && i < 4) ? kNames[i] : "main";
    }
    void set_demo_state(int i) override {
        auto& g = ctx_.game;
        view_ = View::Main;
        switch (i) {
            case 0: tab_ = Tab::Log; log_sel_ = 0; break;
            case 1: tab_ = Tab::Nearby; near_sel_ = 0; break;
            case 2: {
                tab_ = Tab::Nearby;
                near_sel_ = 0;
                const auto& nl = g.daycare().neighbor_list;
                if (!nl.empty()) {
                    party_node_ = nl[0].node_id;
                    view_ = View::Party;
                    g.request_neighbor_party(party_node_);
                }
                break;
            }
            case 3:
                tab_ = Tab::Nearby;
                near_sel_ = 0;
                g.simulate_incoming_challenge();
                break;
            default: break;
        }
    }

private:
    // ── Helpers ─────────────────────────────────────────────────────────────
    const Neighbor* find_neighbor(uint32_t node) const {
        for (const auto& n : ctx_.game.daycare().neighbor_list)
            if (n.node_id == node) return &n;
        return nullptr;
    }

    std::string trainer_name(uint32_t node, const std::string& fallback = "") const {
        if (const Neighbor* n = find_neighbor(node)) {
            if (!n->game_name.empty()) return n->game_name;
            if (!n->short_name.empty()) return n->short_name;
        }
        return fallback.empty() ? node_str(node) : fallback;
    }

    void set_tab(Tab t) {
        view_ = View::Main;
        tab_ = t;
        ctx_.router.rebuild();
    }

    void send_challenge(uint32_t node) {
        auto& g = ctx_.game;
        g.challenge(node);
        pvp_seen_ = g.last_pvp_status();   // demo sets this synchronously; we toast ourselves
        toast(LV_SYMBOL_UPLOAD " Challenge sent to " + trainer_name(node), 2200, color::kAccent);
    }

    int event_dex(const DaycareEvent& ev) const {
        if (ev.from_node) {
            if (const Neighbor* n = find_neighbor(ev.from_node)) return n->lead_dex;
            return 0;
        }
        const auto& party = ctx_.game.party();
        if (ev.slot >= 0 && static_cast<size_t>(ev.slot) < party.size()) return party[ev.slot].dex;
        return 0;
    }

    // ── Status strip / challenge banner ─────────────────────────────────────
    void build_strip(lv_obj_t* body) {
        auto& g = ctx_.game;
        if (auto ch = g.pending_challenge()) {
            lv_obj_t* b = box(body, 2, 0, kBodyW - 4, kStripH);
            lv_obj_set_style_bg_color(b, c(color::kAccent), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(b, 3, 0);
            const std::string who = trainer_name(ch->node_id, ch->trainer);
            label(b, LV_SYMBOL_BELL " " + who + " wants to battle!", 5, 1, font_body(), color::kBg, kBodyW - 14);
            return;
        }
        const auto& d = g.daycare();
        lv_obj_t* s = panel(body, 2, 0, kBodyW - 4, kStripH);
        lv_obj_set_style_radius(s, 3, 0);

        lv_obj_t* left = flex_row(s, 4, 0, 124, kStripH - 2, false, 5);
        label(left, d.game_name.empty() ? std::string("TRAINER") : d.game_name, 0, 0, font_small(),
              color::kAccent);
        if (!d.short_name.empty()) label(left, d.short_name, 0, 0, font_small(), color::kText);
        label(left, node_str(d.node_id), 0, 0, font_small(), color::kDim);

        lv_obj_t* right = flex_row(s, 128, 0, kBodyW - 4 - 128 - 6, kStripH - 2, true, 4);
        dot(right, d.active ? color::kGood : color::kDim);
        label(right, d.active ? "daycare active" : "daycare idle", 0, 0, font_small(),
              d.active ? color::kText : color::kDim);
        label(right, fmt(" %d nearby ", d.neighbors), 0, 0, font_small(), color::kText);
        label(right, d.serial_connected ? LV_SYMBOL_WIFI " radio" : LV_SYMBOL_WIFI " no radio", 0, 0,
              font_small(), d.serial_connected ? color::kGood : color::kBad);
    }

    // ── Tabs ────────────────────────────────────────────────────────────────
    lv_obj_t* tab_pill(lv_obj_t* parent, const std::string& text, bool active) {
        lv_obj_t* l = lv_label_create(parent);
        lv_obj_set_style_text_font(l, font_body(), 0);
        lv_obj_set_style_text_color(l, c(active ? color::kBg : color::kDim), 0);
        lv_obj_set_style_bg_color(l, c(active ? color::kAccent : color::kPanel), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(l, c(active ? color::kAccent : color::kBorder), 0);
        lv_obj_set_style_border_width(l, 1, 0);
        lv_obj_set_style_radius(l, 3, 0);
        lv_obj_set_style_pad_hor(l, 7, 0);
        lv_obj_set_style_pad_ver(l, 0, 0);
        lv_label_set_text(l, text.c_str());
        return l;
    }

    void build_tabs(lv_obj_t* body) {
        auto& g = ctx_.game;
        const auto& d = g.daycare();
        lv_obj_t* left = flex_row(body, 2, kTabY, 170, kTabH, false, 4);
        tab_pill(left, fmt("Log %zu", g.events().size()), tab_ == Tab::Log && view_ == View::Main);
        tab_pill(left, fmt("Nearby %zu", d.neighbor_list.size()), tab_ == Tab::Nearby || view_ == View::Party);

        lv_obj_t* right = flex_row(body, 172, kTabY, kBodyW - 172 - 4, kTabH, true, 8);
        if (d.total_events) label(right, fmt("%u events", d.total_events), 0, 0, font_small(), color::kDim);
        if (!d.last_beacon_result.empty()) {
            const bool ok = d.last_beacon_result == "ok";
            label(right, "beacon " + d.last_beacon_result, 0, 0, font_small(),
                  ok ? color::kGood : color::kWarn);
        }
    }

    // ── Log tab ─────────────────────────────────────────────────────────────
    void build_log(lv_obj_t* body) {
        list_ = std::make_unique<ListView>(body, kListX, kContentY, kListW, kContentH, kRowH,
                                           [this](lv_obj_t* row, int i, bool sel) { log_row(row, i, sel); });
        list_->set_empty_text("No daycare events yet.  e: force one");
        list_->set_count(static_cast<int>(ctx_.game.events().size()));
        list_->select(log_sel_);
        log_sel_ = list_->selected();
    }

    void log_row(lv_obj_t* row, int i, bool sel) {
        const auto& evs = ctx_.game.events();
        if (i < 0 || static_cast<size_t>(i) >= evs.size()) return;
        const DaycareEvent& ev = evs[static_cast<size_t>(i)];
        const int dex = event_dex(ev);
        if (dex > 0) sprite_box(row, dex, 1, 0, 32, 32);
        else label(row, ev.from_node ? LV_SYMBOL_ENVELOPE : LV_SYMBOL_BELL, 10, 9, font_title(), color::kDim);

        lv_obj_t* t = text_block(row, ev.text, 36, 4, kRowW - 36 - 50, font_small(),
                                 sel ? color::kText : 0xc8ccd2);
        lv_obj_set_height(t, 2 * lv_font_get_line_height(font_small()));
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);

        if (ev.timestamp) {
            const long long age = static_cast<long long>(time(nullptr) - ev.timestamp);
            right_label(row, ago_secs(age), kRowW - 50, 4, 46, font_small(), color::kDim);
        }
        if (ev.xp > 0) right_label(row, fmt("+%d XP", ev.xp), kRowW - 50, 17, 46, font_small(), color::kGood);
        else if (ev.from_node) right_label(row, "mesh", kRowW - 50, 17, 46, font_small(), color::kInfo);
    }

    void open_event(int i) {
        const auto& evs = ctx_.game.events();
        if (i < 0 || static_cast<size_t>(i) >= evs.size()) return;
        const DaycareEvent& ev = evs[static_cast<size_t>(i)];
        std::string body = ev.text;
        std::string meta;
        if (ev.xp > 0) meta += fmt("+%d XP", ev.xp);
        if (ev.from_node) meta += (meta.empty() ? "" : "  -  ") + std::string("from ") + trainer_name(ev.from_node);
        if (ev.timestamp) {
            meta += (meta.empty() ? "" : "  -  ") +
                    ago_secs(static_cast<long long>(time(nullptr) - ev.timestamp));
        }
        if (!meta.empty()) body += "\n" + meta;
        ctx_.router.show_modal(std::make_unique<Modal>("Daycare event", body,
                                                       std::vector<std::string>{"OK"}, [](int) {}));
    }

    // ── Nearby tab ──────────────────────────────────────────────────────────
    void build_nearby(lv_obj_t* body) {
        list_ = std::make_unique<ListView>(body, kListX, kContentY, kListW, kContentH, kRowH,
                                           [this](lv_obj_t* row, int i, bool sel) { near_row(row, i, sel); });
        list_->set_empty_text("No trainers nearby.  p: send a beacon");
        list_->set_count(static_cast<int>(ctx_.game.daycare().neighbor_list.size()));
        list_->select(near_sel_);
        near_sel_ = list_->selected();
    }

    void near_row(lv_obj_t* row, int i, bool sel) {
        const auto& nl = ctx_.game.daycare().neighbor_list;
        if (i < 0 || static_cast<size_t>(i) >= nl.size()) return;
        const Neighbor& n = nl[static_cast<size_t>(i)];
        if (n.lead_dex > 0) sprite_box(row, n.lead_dex, 1, 0, 32, 32);
        else label(row, LV_SYMBOL_GPS, 10, 9, font_title(), color::kDim);

        const bool challenger = [&] {
            auto ch = ctx_.game.pending_challenge();
            return ch && ch->node_id == n.node_id;
        }();

        lv_obj_t* top = flex_row(row, 36, 2, 190, 16, false, 6);
        label(top, n.game_name.empty() ? n.short_name : n.game_name, 0, 0, font_body(),
              sel ? color::kAccent : color::kText);
        if (!n.short_name.empty() && n.short_name != n.game_name)
            label(top, n.short_name, 0, 0, font_small(), color::kDim);
        if (challenger) label(top, LV_SYMBOL_BELL " challenging", 0, 0, font_small(), color::kAccent);

        std::string sub;
        if (n.lead_dex > 0 || !n.lead_nick.empty()) {
            sub = n.lead_nick.empty() ? species_name(n.lead_dex) : n.lead_nick;
            if (n.lead_level > 0) sub += fmt("  Lv%d", n.lead_level);
        }
        if (n.party_count > 0) sub += (sub.empty() ? "" : "   ") + fmt("%d in party", n.party_count);
        label(row, sub.empty() ? std::string("no party info") : sub, 36, 19, font_small(), color::kDim, 180);

        const long long age = seen_age_sec(n.last_seen_ms);
        lv_obj_t* r1 = flex_row(row, kRowW - 96, 3, 92, 13, true, 3);
        label(r1, LV_SYMBOL_WIFI, 0, 0, font_small(), freshness_color(age));
        label(r1, age < 0 ? std::string("seen ?") : ago_secs(age), 0, 0, font_small(),
              age < 0 ? color::kDim : color::kText);
        right_label(row, node_str(n.node_id), kRowW - 96, 19, 92, font_small(), color::kDim);
    }

    void open_neighbor(int i) {
        const auto& nl = ctx_.game.daycare().neighbor_list;
        if (i < 0 || static_cast<size_t>(i) >= nl.size()) return;
        const Neighbor& n = nl[static_cast<size_t>(i)];
        const uint32_t node = n.node_id;
        std::string title = n.game_name.empty() ? n.short_name : n.game_name;
        if (!n.short_name.empty() && n.short_name != title) title += "  (" + n.short_name + ")";
        std::string body;
        if (n.lead_dex > 0 || !n.lead_nick.empty()) {
            body = (n.lead_nick.empty() ? species_name(n.lead_dex) : n.lead_nick) +
                   fmt(" Lv%d", n.lead_level);
            if (n.party_count > 0) body += fmt("  +%d more", n.party_count - 1);
        }
        const long long age = seen_age_sec(n.last_seen_ms);
        body += (body.empty() ? "" : "\n") + node_str(node) + "   seen " + (age < 0 ? std::string("?") : ago_secs(age));
        ctx_.router.show_modal(std::make_unique<Modal>(
            title, body, std::vector<std::string>{"Battle", "View party", "Cancel"},
            [this, node](int choice) {
                if (choice == 0) {
                    send_challenge(node);
                } else if (choice == 1) {
                    party_node_ = node;
                    view_ = View::Party;
                    ctx_.game.request_neighbor_party(node);
                    ctx_.router.rebuild();
                }
            }));
    }

    // ── Neighbour party sub-view ────────────────────────────────────────────
    void build_party(lv_obj_t* body) {
        auto& g = ctx_.game;
        const Neighbor* n = find_neighbor(party_node_);
        const std::string who = trainer_name(party_node_);
        const auto* party = g.neighbor_party(party_node_);

        std::string head = who + "'s party";
        if (n && !n->short_name.empty() && n->short_name != who) head += "  " + n->short_name;
        label(body, head, 4, kContentY, font_body(), color::kAccent, 200);
        right_label(body, node_str(party_node_), kBodyW - 104, kContentY + 2, 100, font_small(), color::kDim);

        constexpr int kTop = kContentY + 17;
        constexpr int kAreaH = kBodyH - kTop;   // 87
        if (!party || party->empty()) {
            lv_obj_t* p = panel(body, 4, kTop, kBodyW - 8, kAreaH - 2);
            lv_obj_t* l = text_block(p, LV_SYMBOL_REFRESH "  Asking " + who + " for their party...\n"
                                        "It appears here when the mesh answers.",
                                     10, 22, kBodyW - 28, font_body(), color::kDim);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            return;
        }

        constexpr int kCols = 3, kGap = 4;
        constexpr int kCardW = (kBodyW - 8 - kGap * (kCols - 1)) / kCols;   // 101
        constexpr int kCardH = (kAreaH - kGap) / 2 - 1;                    // 40
        const int count = std::min<int>(6, static_cast<int>(party->size()));
        for (int i = 0; i < count; ++i) {
            const PartyMon& m = (*party)[static_cast<size_t>(i)];
            const int x = 4 + (i % kCols) * (kCardW + kGap);
            const int y = kTop + (i / kCols) * (kCardH + kGap);
            lv_obj_t* card = panel(body, x, y, kCardW, kCardH);
            sprite_box(card, m.dex, 1, 3, 32, 32);
            const std::string species = m.species.empty() ? species_name(m.dex) : m.species;
            const std::string name = m.nickname.empty() ? species : m.nickname;
            label(card, name, 35, 3, font_small(), color::kText, kCardW - 38);
            label(card, fmt("Lv%d", m.level), 35, 15, font_small(), color::kAccent);
            if (name != species) label(card, title_case(species), 60, 15, font_small(), color::kDim, kCardW - 63);
            if (m.max_hp) hp_bar(card, 35, 30, kCardW - 41, m.hp, m.max_hp);
        }
        if (count < 4) {
            // Small party: use the empty second row for the battle prompt.
            lv_obj_t* l = label(body, fmt("%d of %d shown.  Enter: battle %s", count,
                                          n && n->party_count > count ? n->party_count : count, who.c_str()),
                                4, kTop + kCardH + kGap + 12, font_small(), color::kDim, kBodyW - 8);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        }
    }

    // ── State ───────────────────────────────────────────────────────────────
    Tab tab_ = Tab::Log;
    View view_ = View::Main;
    int log_sel_ = 0;
    int near_sel_ = 0;
    uint32_t party_node_ = 0;
    std::unique_ptr<ListView> list_;
    uint32_t rev_ = 0;
    uint32_t built_ms_ = 0;
    uint32_t toast_hidden_node_ = 0;
    std::string pvp_seen_;
};

} // namespace

std::unique_ptr<Screen> make_daycare_screen(Ctx& ctx) {
    return std::make_unique<DaycareScreen>(ctx);
}

} // namespace mm::ui
