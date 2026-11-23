// hood_screen.cpp — Mesh Neighborhood: every node the radio knows, as a
// Win9x Network Neighborhood "Details" view (icon, name, signal bars, hops,
// last heard, battery), sortable by column.
//
// Enter opens a node's Properties pane with two probes:
//   Ping        an empty PRIVATE_APP packet with want_ack; the reply is the
//               far node's own ack (no chat text is sent, nothing shows up
//               in anyone's inbox).
//   Traceroute  TRACEROUTE_APP with want_response; the reply lists the
//               relays both ways with the SNR of every hop.
// The result prints in a DOS-prompt box, like `ping` / `tracert` in 1999.
//
// Demo mode (no radio, --shots) uses mm::mesh_demo_nodes() and fakes probe
// results after a short delay; it never touches the radio.
#include "screens.h"

#include "mesh_link.h"
#include "messenger.h"
#include "router.h"
#include "widgets.h"
#include "win.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

namespace mm::ui {
namespace {

constexpr int kRowH = 13;
constexpr int kHeadH = 13;
constexpr int kRows = (kClientH - 2 - kHeadH) / kRowH;   // visible rows

// Column x positions inside the list field (and the header).
constexpr int kColIcon = 2, kColName = 17, kColSig = 128, kColHops = 172, kColHeard = 204, kColBatt = 268;
constexpr int kListW = kClientW - 2;

enum Sort { kByName, kBySignal, kByHops, kByHeard, kSorts };
const char* const kSortName[kSorts] = {"name", "signal", "hops", "last heard"};

// DOS box colours, the same in every skin.
constexpr uint32_t kDosBg = 0x000000, kDosText = 0xc0c0c0, kDosHi = 0xffffff, kDosDim = 0x808080;

constexpr uint32_t kDemoDelayMs = 1800;

uint32_t now_s() { return static_cast<uint32_t>(std::time(nullptr)); }

lv_obj_t* rect(lv_obj_t* parent, int x, int y, int w, int h, uint32_t col) {
    lv_obj_t* o = box(parent, x, y, w, h);
    lv_obj_set_style_bg_color(o, c(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

std::string name_of(const MeshNode& n) {
    if (!n.long_name.empty()) return n.long_name;
    if (!n.short_name.empty()) return n.short_name;
    return MeshLink::node_id_text(n.num);
}

std::string short_of(const MeshNode& n) {
    if (!n.short_name.empty()) return n.short_name;
    if (!n.long_name.empty()) return n.long_name.substr(0, 4);
    return MeshLink::node_id_text(n.num).substr(5);
}

std::string heard_short(uint32_t then) {
    if (then == 0) return "-";
    const uint32_t now = now_s();
    const uint32_t s = now > then ? now - then : 0;
    if (s < 60) return "now";
    if (s < 3600) return fmt("%u min", s / 60);
    if (s < 86400) return fmt("%u hr", s / 3600);
    return fmt("%u day%s", s / 86400, s / 86400 == 1 ? "" : "s");
}

std::string heard_long(uint32_t then) {
    if (then == 0) return "never";
    const std::string s = heard_short(then);
    return s == "now" ? "just now" : s + " ago";
}

std::string batt_text(int b) {
    if (b > 100) return "AC";
    if (b >= 0) return fmt("%d%%", b);
    return "-";
}

// 0..4 bars from the SNR of the last packet heard.
int bars_of(const MeshNode& n) {
    if (n.last_heard == 0) return 0;
    if (n.snr >= 5) return 4;
    if (n.snr >= 0) return 3;
    if (n.snr >= -7) return 2;
    if (n.snr >= -13) return 1;
    return 0;
}

std::string snr_text(float snr) {
    if (snr <= MeshProbe::kNoSnr + 1) return "? dB";
    return fmt("%+.1f dB", static_cast<double>(snr));
}

const char* routing_error(int e) {
    switch (e) {
        case 1:  return "No route to node.";
        case 2:  return "Refused (NAK).";
        case 3:  return "Request timed out.";
        case 4:  return "Radio not connected.";
        case 5:  return "No answer after retries.";
        case 6:  return "No channel for that node.";
        case 8:  return "No response from node.";
        case 9:  return "Duty cycle limit reached.";
        default: return nullptr;
    }
}

// Little Win9x "computer" icon: monitor with a screen, and a base.
void draw_computer(lv_obj_t* p, int x, int y, bool me, bool dim) {
    const uint32_t edge = 0x202020;
    const uint32_t screen = dim ? 0x808080 : (me ? 0x2a7a3a : 0x1a5ab0);
    lv_obj_t* mon = rect(p, x, y, 11, 8, 0xc8c8c0);
    lv_obj_set_style_border_width(mon, 1, 0);
    lv_obj_set_style_border_color(mon, c(edge), 0);
    rect(mon, 1, 1, 7, 4, screen);
    rect(p, x + 4, y + 8, 3, 1, edge);
    rect(p, x + 2, y + 9, 7, 2, 0xc8c8c0);
    rect(p, x + 2, y + 10, 7, 1, edge);
}

void draw_bars(lv_obj_t* p, int x, int y, int n, uint32_t on, uint32_t off) {
    for (int i = 0; i < 4; ++i) {
        const int h = 3 + i * 2;
        rect(p, x + i * 4, y + 9 - h, 3, h, i < n ? on : off);
    }
}

class HoodScreen final : public Screen {
public:
    explicit HoodScreen(Ctx& ctx) : Screen(ctx) { load(); }

    const char* title() const override { return "Mesh Neighborhood"; }
    bool windowed() const override { return true; }
    std::string hints() const override {
        if (detail_) return "P:ping  T:traceroute  Left/Right+Enter  Esc:back";
        return std::string("Enter:properties  Tab:sort by ") + kSortName[(sort_ + 1) % kSorts] + "  Esc:back";
    }

    void build(lv_obj_t* body) override {
        root_ = box(body, 0, 0, kClientW, kClientH);
        paint();
    }

    void on_show() override { note(); }
    void on_hide() override { ctx_.router.set_status_note(""); }

    void tick(uint32_t now_ms) override {
        if (!root_) return;
        now_ms_ = now_ms;
        bool dirty = false;
        if (!demo()) {
            const MeshLink& l = mesh_link();
            if (l.nodes_revision() != nodes_rev_) {
                load();
                dirty = true;
            }
            if (probe_id_ && l.probes_revision() != probes_rev_) {
                probes_rev_ = l.probes_revision();
                if (const MeshProbe* p = l.probe(probe_id_)) result_ = *p;
                dirty = true;
            }
        } else if (fake_at_ && now_ms - fake_at_ >= kDemoDelayMs) {
            fake_at_ = 0;
            result_ = mesh_demo_probe(result_.to, result_.kind);
            dirty = true;
        }
        // Blink the DOS cursor while a probe is out.
        if (blink_ms_ == 0) blink_ms_ = now_ms;
        if (detail_ && have_result_ && result_.status == MeshProbe::Status::Waiting && now_ms - blink_ms_ >= 500) {
            blink_ms_ = now_ms;
            blink_ = !blink_;
            dirty = true;
        }
        if (dirty) paint();
    }

    bool on_key(const KeyEvent& e) override {
        if (!root_) return false;
        return detail_ ? detail_key(e) : list_key(e);
    }

    int demo_state_count() const override { return 6; }
    const char* demo_state_name(int i) const override {
        static const char* const k[] = {"main", "by-signal", "properties", "traceroute", "ping", "waiting"};
        return i >= 0 && i < 6 ? k[i] : "main";
    }
    void set_demo_state(int i) override {
        demo_state_ = true;
        sort_ = i == 1 ? kBySignal : kByName;
        detail_ = false;
        have_result_ = false;
        fake_at_ = 0;
        focus_ = 0;
        load();
        sel_ = 1;
        top_ = 0;
        if (i >= 2) {
            const uint32_t who = i == 4 ? 0xba5e5701u : 0xa3f29c01u;   // base_stn (direct) / KD2ABC-m (2 hops)
            select_num(who);
            detail_ = true;
            if (i == 3) {
                focus_ = 1;
                result_ = mesh_demo_probe(who, MeshProbe::Kind::Traceroute);
                have_result_ = true;
            } else if (i == 4) {
                result_ = mesh_demo_probe(who, MeshProbe::Kind::Ping);
                have_result_ = true;
            } else if (i == 5) {
                focus_ = 1;
                result_ = MeshProbe{};
                result_.to = who;
                result_.kind = MeshProbe::Kind::Traceroute;
                result_.status = MeshProbe::Status::Waiting;
                have_result_ = true;
                blink_ = true;
            }
        }
    }

private:
    bool demo() const { return demo_state_ || messenger().demo(); }

    void load() {
        const uint32_t keep = sel_ >= 0 && sel_ < static_cast<int>(nodes_.size()) ? nodes_[sel_].num : 0;
        if (demo()) {
            nodes_ = mesh_demo_nodes(now_s());
        } else {
            nodes_ = mesh_link().nodes();
            nodes_rev_ = mesh_link().nodes_revision();
        }
        sort_nodes();
        sel_ = nodes_.empty() ? -1 : std::min(std::max(sel_, 0), static_cast<int>(nodes_.size()) - 1);
        if (keep) select_num(keep);
        if (detail_ && (sel_ < 0 || nodes_[sel_].num != detail_num_)) {
            if (!select_num(detail_num_)) detail_ = false;
        }
        note();
    }

    void note() {
        if (!root_) return;
        int heard = 0;
        for (const MeshNode& n : nodes_) heard += !n.is_me && n.last_heard != 0;
        ctx_.router.set_status_note(fmt("%d node%s", heard, heard == 1 ? "" : "s"));
    }

    void sort_nodes() {
        auto key = [this](const MeshNode& a, const MeshNode& b) {
            if (a.is_me != b.is_me) return a.is_me;          // this PC first, always
            const std::string an = name_of(a), bn = name_of(b);
            auto by_name = [&] {
                return std::lexicographical_compare(an.begin(), an.end(), bn.begin(), bn.end(), [](char x, char y) {
                    return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
                });
            };
            switch (sort_) {
                case kBySignal: {
                    const bool ha = a.last_heard != 0, hb = b.last_heard != 0;
                    if (ha != hb) return ha;
                    if (a.snr != b.snr) return a.snr > b.snr;
                    return by_name();
                }
                case kByHops: {
                    const int ha = a.hops_away < 0 ? 99 : a.hops_away, hb = b.hops_away < 0 ? 99 : b.hops_away;
                    if (ha != hb) return ha < hb;
                    return by_name();
                }
                case kByHeard:
                    if (a.last_heard != b.last_heard) return a.last_heard > b.last_heard;
                    return by_name();
                default:
                    return by_name();
            }
        };
        std::stable_sort(nodes_.begin(), nodes_.end(), key);
    }

    bool select_num(uint32_t num) {
        for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
            if (nodes_[i].num == num) {
                sel_ = i;
                detail_num_ = num;
                scroll_to_sel();
                return true;
            }
        }
        return false;
    }

    void scroll_to_sel() {
        if (sel_ < top_) top_ = sel_;
        if (sel_ >= top_ + kRows) top_ = sel_ - kRows + 1;
        top_ = std::max(top_, 0);
    }

    const MeshNode* selected() const {
        return sel_ >= 0 && sel_ < static_cast<int>(nodes_.size()) ? &nodes_[sel_] : nullptr;
    }

    const MeshNode* find(uint32_t num) const {
        for (const MeshNode& n : nodes_)
            if (n.num == num) return &n;
        return nullptr;
    }

    std::string hop_name(uint32_t num) const {
        if (num == 0xffffffffu) return "(unknown relay)";
        if (const MeshNode* n = find(num)) return name_of(*n);
        return MeshLink::node_id_text(num);
    }

    // ── Keys ────────────────────────────────────────────────────────────────

    bool list_key(const KeyEvent& e) {
        const int n = static_cast<int>(nodes_.size());
        switch (e.key) {
            case Key::Up:
                if (sel_ > 0) --sel_;
                break;
            case Key::Down:
                if (sel_ + 1 < n) ++sel_;
                break;
            case Key::Left:
                sel_ = std::max(0, sel_ - kRows);
                break;
            case Key::Right:
                sel_ = std::min(n - 1, sel_ + kRows);
                break;
            case Key::Tab:
                cycle_sort();
                return true;
            case Key::Char:
                if (e.ch == 's') {
                    cycle_sort();
                    return true;
                }
                return false;
            case Key::A:
                if (const MeshNode* s = selected()) {
                    detail_ = true;
                    detail_num_ = s->num;
                    have_result_ = false;
                    probe_id_ = 0;
                    fake_at_ = 0;
                    focus_ = 0;
                    ctx_.router.rebuild();   // new hints
                }
                return true;
            default:
                return false;
        }
        scroll_to_sel();
        paint();
        return true;
    }

    void cycle_sort() {
        const uint32_t keep = selected() ? selected()->num : 0;
        sort_ = (sort_ + 1) % kSorts;
        sort_nodes();
        if (keep) select_num(keep);
        ctx_.router.rebuild();   // hints name the next sort key
    }

    bool detail_key(const KeyEvent& e) {
        switch (e.key) {
            case Key::B:
                detail_ = false;
                ctx_.router.rebuild();
                return true;
            case Key::Left:
            case Key::Up:
                focus_ = 0;
                paint();
                return true;
            case Key::Right:
            case Key::Down:
            case Key::Tab:
                focus_ = e.key == Key::Tab ? 1 - focus_ : 1;
                paint();
                return true;
            case Key::A:
                run(focus_ == 0 ? MeshProbe::Kind::Ping : MeshProbe::Kind::Traceroute);
                return true;
            case Key::Char:
                if (e.ch == 'p') {
                    focus_ = 0;
                    run(MeshProbe::Kind::Ping);
                    return true;
                }
                if (e.ch == 't') {
                    focus_ = 1;
                    run(MeshProbe::Kind::Traceroute);
                    return true;
                }
                return true;
            default:
                return true;
        }
    }

    void run(MeshProbe::Kind kind) {
        const MeshNode* n = selected();
        if (!n || n->is_me) return;
        if (have_result_ && result_.status == MeshProbe::Status::Waiting) {
            toast("Still waiting for the last one...", 1500);
            return;
        }
        result_ = MeshProbe{};
        result_.to = n->num;
        result_.kind = kind;
        result_.status = MeshProbe::Status::Waiting;
        have_result_ = true;
        blink_ = true;
        blink_ms_ = now_ms_;
        if (demo()) {
            fake_at_ = std::max<uint32_t>(now_ms_, 1);
        } else {
            MeshLink& l = mesh_link();
            probe_id_ = kind == MeshProbe::Kind::Ping ? l.send_ping(n->num) : l.send_traceroute(n->num);
            probes_rev_ = l.probes_revision();
            if (probe_id_ == 0) {
                result_.status = MeshProbe::Status::Failed;
                result_.error = -1;   // could not send
            } else if (const MeshProbe* p = l.probe(probe_id_)) {
                result_ = *p;
            }
        }
        paint();
    }

    // ── Painting ────────────────────────────────────────────────────────────

    void paint() {
        if (!root_) return;
        lv_obj_clean(root_);
        if (detail_) paint_detail();
        else paint_list();
    }

    void paint_list() {
        const Skin& s = skin();
        lv_obj_t* f = win::field(root_, 0, 0, kClientW, kClientH);

        // Column headers: raised buttons, the sort column marked with a little
        // down arrow like Explorer.
        struct Col { const char* name; int x, w; int sort; };
        const Col cols[] = {
            {"Name", 0, kColSig - 1, kByName},
            {"Signal", kColSig - 1, kColHops - kColSig, kBySignal},
            {"Hops", kColHops - 1, kColHeard - kColHops, kByHops},
            {"Heard", kColHeard - 1, kColBatt - kColHeard, kByHeard},
            {"Batt", kColBatt - 1, kListW - kColBatt + 1, -1},
        };
        for (const Col& col : cols) {
            lv_obj_t* h = win::frame(f, col.x, 0, col.w, kHeadH, win::Edge::Raised, s.face);
            lv_obj_t* t = win::text(h, col.name, col.x == 0 ? kColName - 1 : 3, 0, s.text);
            lv_obj_set_y(t, -1);
            if (col.sort == sort_) {
                // ▼ drawn as three rows of pixels at the right of the header.
                const int ax = col.w - 10, ay = 4;
                for (int i = 0; i < 3; ++i) rect(h, ax + i, ay + i, 5 - 2 * i, 1, s.text);
            }
        }

        if (nodes_.empty()) {
            win::text(f, "Nobody in the neighborhood yet.", kColName, kHeadH + 6, s.dim);
            win::text(f, demo() ? "" : "Nodes appear as the radio hears them.", kColName, kHeadH + 20, s.dim);
            return;
        }

        for (int r = 0; r < kRows; ++r) {
            const int i = top_ + r;
            if (i >= static_cast<int>(nodes_.size())) break;
            const MeshNode& n = nodes_[i];
            const bool on = i == sel_;
            const int y = kHeadH + 1 + r * kRowH;
            const bool stale = !n.is_me && n.last_heard == 0;
            const uint32_t fg = on ? s.sel_text : (stale ? s.dim : s.field_text);
            draw_computer(f, kColIcon, y + 1, n.is_me, stale);
            if (on) {
                // Explorer highlights the label only (plus the dotted focus box).
                rect(f, kColName - 2, y, kListW - kColName, kRowH, s.sel);
            }
            win::text(f, name_of(n), kColName, y - 1, fg, kColSig - kColName - 4, n.is_me);   // me: bold
            if (!n.is_me) {
                draw_bars(f, kColSig + 2, y + 1, bars_of(n), on ? s.sel_text : (s.dark ? 0x58d858 : 0x188818),
                          on ? s.sel : (s.dark ? 0x404848 : 0xb0b0b0));
                if (on) {
                    // Empty bars need to show on the selection colour too.
                    for (int b = bars_of(n); b < 4; ++b) {
                        const int h = 3 + b * 2;
                        lv_obj_t* o = rect(f, kColSig + 2 + b * 4, y + 1 + 9 - h, 3, h, s.sel);
                        lv_obj_set_style_border_width(o, 1, 0);
                        lv_obj_set_style_border_color(o, c(s.sel_text), 0);
                        lv_obj_set_style_border_opa(o, LV_OPA_50, 0);
                    }
                }
                win::text(f, n.hops_away < 0 ? std::string("-") : n.hops_away == 0 ? std::string("0") : fmt("%d", n.hops_away),
                          kColHops + 2, y - 1, fg);
                win::text(f, heard_short(n.last_heard), kColHeard + 2, y - 1, fg);
            } else {
                win::text(f, "-", kColHops + 2, y - 1, fg);
                win::text(f, "-", kColHeard + 2, y - 1, fg);
            }
            win::text(f, batt_text(n.battery), kColBatt + 2, y - 1, fg);
        }

        // Scroll thumb when the list is longer than the view.
        const int total = static_cast<int>(nodes_.size());
        if (total > kRows) {
            const int th = kClientH - 2 - kHeadH, x = kListW - 4;
            rect(f, x, kHeadH, 4, th, s.dark ? 0x303030 : 0xd8d8d8);
            const int bh = std::max(8, th * kRows / total);
            const int by = kHeadH + (th - bh) * top_ / std::max(1, total - kRows);
            rect(f, x, by, 4, bh, s.dim);
        }
    }

    void paint_detail() {
        const Skin& s = skin();
        const MeshNode* np = selected();
        if (!np) return;
        const MeshNode& n = *np;

        // Properties card on the left.
        const int cw = 150;
        lv_obj_t* cd = win::card(root_, 0, 0, cw, kClientH);
        draw_computer(cd, 4, 4, n.is_me, false);
        win::text(cd, name_of(n), 19, 1, s.card_text, cw - 24, true);
        int y = 16;
        auto row = [&](const char* k, const std::string& v) {
            win::text(cd, k, 4, y, s.card_text);
            win::text(cd, v, 50, y, s.card_text, cw - 56);
            y += 12;
        };
        row("ID", MeshLink::node_id_text(n.num) + "  " + short_of(n));
        if (n.is_me) {
            row("Role", "this radio");
        } else {
            row("Signal", n.last_heard ? fmt("%.1f dB SNR", static_cast<double>(n.snr)) : std::string("-"));
            row("Hops", n.hops_away < 0 ? std::string("unknown") : n.hops_away == 0 ? std::string("direct") : fmt("%d", n.hops_away));
            row("Heard", heard_long(n.last_heard));
        }
        row("Battery", n.battery > 100 ? std::string("plugged in") : n.battery >= 0 ? fmt("%d%%", n.battery) : std::string("unknown"));
        row("Position", n.has_position ? fmt("%.3f, %.3f", n.latitude, n.longitude) : std::string("not shared"));
        if (!n.is_me) {
            const int bars = bars_of(n);
            draw_bars(cd, cw - 24, 3, bars, 0x188818, 0xb0b0b0);
        }

        // Buttons and the DOS box on the right.
        const int rx = cw + 4, rw = kClientW - rx;
        const bool can = !n.is_me;
        lv_obj_t* b1 = win::button(root_, "Ping", rx, 0, 52, can && focus_ == 0);
        lv_obj_t* b2 = win::button(root_, "Traceroute", rx + 56, 0, rw - 56, can && focus_ == 1);
        if (!can) {
            lv_obj_set_style_opa(b1, LV_OPA_50, 0);
            lv_obj_set_style_opa(b2, LV_OPA_50, 0);
        }
        paint_dos(rx, 19, rw, kClientH - 19, n);
    }

    void paint_dos(int x, int y, int w, int h, const MeshNode& n) {
        lv_obj_t* fr = win::frame(root_, x, y, w, h, win::Edge::Sunken, kDosBg);
        lv_obj_t* d = box(fr, 2, 1, w - 4, h - 3);
        lv_obj_set_style_clip_corner(d, true, 0);
        const int lh = 11;
        int ly = 0;
        auto put = [&](const std::string& t, int tx = 0, uint32_t col = kDosText, int tw = 0) {
            return label(d, t, tx, ly, font_small(), col, tw);
        };
        auto nl = [&] { ly += lh; };
        const std::string target = short_of(n);

        if (n.is_me) {
            put("C:\\MESH>"); nl();
            put("That is this radio."); nl();
            put("Pick another node"); nl();
            put("to ping or trace."); nl();
            return;
        }
        if (!have_result_) {
            put("MESH-DOS Version 6.99", 0, kDosDim); nl();
            put("(C) 1999 Pocket Mesh Co.", 0, kDosDim); nl();
            nl();
            put("C:\\MESH>_"); nl();
            nl();
            put("P = ping, T = traceroute", 0, kDosDim);
            return;
        }

        const MeshProbe& r = result_;
        const bool trace = r.kind == MeshProbe::Kind::Traceroute;
        put(std::string("C:\\MESH>") + (trace ? "tracert " : "ping ") + target, 0, kDosHi);
        nl();

        if (r.status == MeshProbe::Status::Waiting) {
            put(trace ? "Tracing route to" : "Pinging"); nl();
            put(MeshLink::node_id_text(n.num) + "..."); nl();
            if (!demo() && r.sent_ms) {
                put(fmt("%u s", static_cast<unsigned>((now_ms_ > r.sent_ms ? now_ms_ - r.sent_ms : 0) / 1000)), 0, kDosDim);
                nl();
            }
            put(blink_ ? "_" : " ");
            return;
        }
        if (r.status == MeshProbe::Status::Failed) {
            if (r.error < 0) {
                put("Could not send:"); nl();
                put("radio busy or offline.");
                return;
            }
            const char* why = routing_error(r.error);
            put(why ? why : fmt("Failed, error %d.", r.error).c_str());
            nl();
            if (r.error == 3 || r.error == 8) {
                put(trace ? "Node may be out of range," : "No reply. Node may be", 0, kDosDim); nl();
                put(trace ? "or asleep." : "asleep or out of range.", 0, kDosDim);
            }
            return;
        }

        if (!trace) {
            put("Reply from " + MeshLink::node_id_text(n.num)); nl();
            put(fmt("  time=%ums", r.rtt_ms())); nl();
            put(r.hops < 0 ? std::string("  hops=?") : fmt("  hops=%d", r.hops)); nl();
            put(r.snr <= MeshProbe::kNoSnr + 1 ? std::string("  SNR=?") : fmt("  SNR=%.2f dB", static_cast<double>(r.snr))); nl();
            nl();
            put("Ping complete.", 0, kDosDim);
            return;
        }

        // Hop list: the relays, then the target itself, with the SNR each hop
        // was heard at; then the way back (if the firmware recorded it).
        const int cn = 13, cs = w - 4 - 44;
        auto hop = [&](int i, const std::string& who, float snr) {
            put(fmt("%d", i), 0);
            put(who, cn, kDosText, cs - cn - 2);
            put(snr_text(snr), cs);
            nl();
        };
        const int hops = static_cast<int>(r.route.size());
        for (int i = 0; i <= hops; ++i) {
            const std::string who = i < hops ? hop_name(r.route[i]) : name_of(n);
            const float snr = i < static_cast<int>(r.snr_towards.size()) ? r.snr_towards[i] : MeshProbe::kNoSnr;
            hop(i + 1, who, snr);
        }
        if (!r.snr_back.empty() || !r.route_back.empty()) {
            std::string back = "back:";
            for (uint32_t b : r.route_back) {
                const MeshNode* bn = find(b);
                back += " " + (bn ? short_of(*bn) : hop_name(b));
            }
            if (r.route_back.empty()) back += " direct";
            if (!r.snr_back.empty()) back += "  " + snr_text(r.snr_back.back());
            put(back, 0, kDosDim, w - 6);
            nl();
        }
        put(fmt("Trace complete, %.1f s.", r.rtt_ms() / 1000.0));
    }

    lv_obj_t* root_ = nullptr;
    std::vector<MeshNode> nodes_;
    int sel_ = 0, top_ = 0, sort_ = kByName;
    bool detail_ = false;
    uint32_t detail_num_ = 0;
    int focus_ = 0;                 // 0 Ping, 1 Traceroute
    MeshProbe result_;
    bool have_result_ = false;
    uint32_t probe_id_ = 0, probes_rev_ = 0, nodes_rev_ = 0;
    uint32_t fake_at_ = 0;          // demo: when the fake probe went out
    uint32_t now_ms_ = 0, blink_ms_ = 0;
    bool blink_ = true;
    bool demo_state_ = false;
};

} // namespace

std::unique_ptr<Screen> make_hood_screen(Ctx& ctx) {
    return std::make_unique<HoodScreen>(ctx);
}

} // namespace mm::ui
