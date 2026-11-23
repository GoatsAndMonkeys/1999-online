// map_screen.cpp — MeshMap: the nodes that share a position, plotted on a
// paper street-map panel in the style of the 1999 web map sites.
//
// No tiles and no internet: the panel is a metric grid (grid step = the scale
// bar's length) with a zoom ladder, a north arrow and a scale bar. Every node
// with a position is a pin with its short name; mine is the blue one.
//
// Keys: arrows pan, + / - (also = / _) zoom, Enter selects the next pin and
// shows its info card (distance and bearing from me, last heard, SNR), H fits
// all pins again, Esc closes the card, then leaves.
//
// Data: mm::mesh_link().nodes() once the messenger is live (a radio has been
// Ready), else mm::mesh_demo_nodes(). Nothing here transmits.
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

constexpr double kPi = 3.14159265358979323846;
constexpr double kMPerDegLat = 110574.0;
constexpr double kMPerDegLon = 111320.0;   // at the equator; times cos(lat)

// Zoom levels, metres per pixel (fine to coarse).
constexpr double kMpp[] = {2, 3, 5, 7, 10, 15, 20, 30, 40, 60, 80, 120, 160, 240, 320, 480,
                           640, 960, 1300, 2000, 2600, 4000, 6000, 9000};
constexpr int kZooms = static_cast<int>(sizeof kMpp / sizeof *kMpp);

constexpr int kMapW = kClientW - 2;     // inside the sunken edge
constexpr int kMapH = kClientH - 2;
constexpr int kPanPx = 40;
constexpr int kCardW = 138, kCardH = 54;

struct Paper {
    uint32_t bg, minor, major, ink, label_bg;
};

Paper paper() {
    const Skin& s = skin();
    if (s.dark) return {0x1c2318, 0x262f20, 0x34432b, 0xb4c49a, 0x1c2318};
    return {0xf2ead2, 0xe5dabd, 0xcfbf96, 0x5c4a28, 0xfbf6e6};
}

constexpr uint32_t kPinRed = 0xd23a2a, kPinMe = 0x2c5fd6, kPinEdge = 0x3a1c14, kPinSel = 0xf4c81c;

lv_obj_t* rect(lv_obj_t* parent, int x, int y, int w, int h, uint32_t col, lv_opa_t opa = LV_OPA_COVER) {
    lv_obj_t* o = box(parent, x, y, w, h);
    lv_obj_set_style_bg_color(o, c(col), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    return o;
}

uint32_t now_s() { return static_cast<uint32_t>(std::time(nullptr)); }

std::string ago_text(uint32_t then) {
    if (then == 0) return "never";
    const uint32_t now = now_s();
    const uint32_t s = now > then ? now - then : 0;
    if (s < 45) return "just now";
    if (s < 3600) return fmt("%u min ago", (s + 30) / 60);
    if (s < 86400) return fmt("%u hr ago", s / 3600);
    return fmt("%u days ago", s / 86400);
}

std::string dist_text(double m) {
    if (m < 1000) return fmt("%d m", static_cast<int>(std::lround(m)));
    if (m < 10000) return fmt("%.1f km", m / 1000.0);
    return fmt("%d km", static_cast<int>(std::lround(m / 1000.0)));
}

// Great-circle distance (m) and initial bearing (degrees) a -> b.
void dist_bearing(double lat1, double lon1, double lat2, double lon2, double& dist, double& brg) {
    const double r = kPi / 180.0;
    const double p1 = lat1 * r, p2 = lat2 * r, dp = (lat2 - lat1) * r, dl = (lon2 - lon1) * r;
    const double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    dist = 6371000.0 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    brg = std::fmod(std::atan2(y, x) / r + 360.0, 360.0);
}

const char* compass_point(double brg) {
    static const char* const k[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return k[static_cast<int>(std::floor((brg + 22.5) / 45.0)) % 8];
}

std::string node_label(const MeshNode& n) {
    if (!n.short_name.empty()) return n.short_name;
    if (!n.long_name.empty()) return n.long_name.substr(0, 4);
    return MeshLink::node_id_text(n.num).substr(5);
}

std::string node_name(const MeshNode& n) {
    if (!n.long_name.empty()) return n.long_name;
    if (!n.short_name.empty()) return n.short_name;
    return MeshLink::node_id_text(n.num);
}

// A round grid step (1, 2, 5 x 10^n metres) at least `px` pixels long.
double grid_step(double mpp, int px) {
    const double want = mpp * px;
    double base = std::pow(10.0, std::floor(std::log10(want)));
    for (double m : {1.0, 2.0, 5.0, 10.0}) {
        if (base * m >= want) return base * m;
    }
    return base * 10;
}

class MapScreen final : public Screen {
public:
    explicit MapScreen(Ctx& ctx) : Screen(ctx) {
        load_nodes();
        fit();
    }

    const char* title() const override { return "MeshMap"; }
    bool windowed() const override { return true; }
    std::string hints() const override {
        if (pins_.empty()) return "Esc:back";
        return card_ ? "Enter:next  +/-:zoom  arrows:pan  H:fit  Esc:close"
                     : "Enter:pick  +/-:zoom  arrows:pan  H:fit  Esc:back";
    }

    void build(lv_obj_t* body) override {
        const Paper p = paper();
        lv_obj_t* edge = win::frame(body, 0, 0, kClientW, kClientH, win::Edge::Sunken, p.bg);
        map_ = box(edge, 1, 1, kMapW, kMapH);
        paint();
    }

    void tick(uint32_t now_ms) override {
        if (demo_data_ || !map_) return;
        const MeshLink& l = mesh_link();
        if (l.nodes_revision() != seen_rev_ || now_ms - painted_ms_ > 30000) {
            painted_ms_ = now_ms;
            const bool had = !pins_.empty();
            load_nodes();
            if (!had && !pins_.empty()) fit();
            paint();
        }
    }

    bool on_key(const KeyEvent& e) override {
        if (!map_) return false;
        if (e.key == Key::B && card_) {
            card_ = false;
            paint();
            return true;
        }
        if (pins_.empty()) return false;
        switch (e.key) {
            case Key::Up:    pan(0, -kPanPx); return true;
            case Key::Down:  pan(0, kPanPx); return true;
            case Key::Left:  pan(-kPanPx, 0); return true;
            case Key::Right: pan(kPanPx, 0); return true;
            case Key::A:
            case Key::Tab:
                select_next();
                return true;
            case Key::Char:
                if (e.ch == '+' || e.ch == '=') return zoom(-1);
                if (e.ch == '-' || e.ch == '_') return zoom(1);
                if (e.ch == 'h' || e.ch == '0') {
                    fit();
                    paint();
                    return true;
                }
                return false;
            default:
                return false;
        }
    }

    int demo_state_count() const override { return 4; }
    const char* demo_state_name(int i) const override {
        switch (i) {
            case 1:  return "card";
            case 2:  return "zoomed";
            case 3:  return "empty";
            default: return "main";
        }
    }
    void set_demo_state(int i) override {
        demo_data_ = true;
        empty_demo_ = i == 3;
        load_nodes();
        fit();
        card_ = false;
        sel_ = -1;
        if (i == 1) {
            select_num(0xa3f29c01);   // KD2ABC-m
        } else if (i == 2) {
            // Close in on home: me and the nearby gym, with the card on the gym.
            select_num(0x7c11d402);   // Ridge_Gym
            zoom_ = std::max(0, zoom_ - 4);
            if (me_ >= 0 && sel_ >= 0) {
                // Both pins in view, left of the card.
                cx_ = (pins_[me_].x + pins_[sel_].x) / 2 + (kCardW / 2.0 - 10) * kMpp[zoom_];
                cy_ = (pins_[me_].y + pins_[sel_].y) / 2 + 10 * kMpp[zoom_];
            }
        }
    }

private:
    struct Pin {
        MeshNode node;
        double x = 0, y = 0;      // metres east / north of the reference point
    };

    bool demo() const { return demo_data_ || messenger().demo(); }

    void load_nodes() {
        std::vector<MeshNode> nodes;
        if (demo()) {
            nodes = mesh_demo_nodes(now_s());
            if (empty_demo_) {
                for (MeshNode& n : nodes) n.has_position = false;
            }
        } else {
            nodes = mesh_link().nodes();
            seen_rev_ = mesh_link().nodes_revision();
        }
        heard_ = static_cast<int>(nodes.size());
        const uint32_t keep = sel_ >= 0 && sel_ < static_cast<int>(pins_.size()) ? pins_[sel_].node.num : 0;
        pins_.clear();
        me_ = -1;
        for (const MeshNode& n : nodes) {
            if (!n.has_position) continue;
            Pin p;
            p.node = n;
            pins_.push_back(p);
        }
        // Me last, so my pin is drawn on top.
        std::stable_partition(pins_.begin(), pins_.end(), [](const Pin& p) { return !p.node.is_me; });
        if (!pins_.empty() && !have_ref_) {
            // The reference point stays put once chosen so the view does not
            // jump when nodes move; cos(lat) of a far-off node is close enough.
            double lat = 0, lon = 0;
            for (const Pin& p : pins_) { lat += p.node.latitude; lon += p.node.longitude; }
            ref_lat_ = lat / pins_.size();
            ref_lon_ = lon / pins_.size();
            have_ref_ = true;
        }
        const double kx = kMPerDegLon * std::cos(ref_lat_ * kPi / 180.0);
        sel_ = -1;
        for (int i = 0; i < static_cast<int>(pins_.size()); ++i) {
            Pin& p = pins_[i];
            double dlon = p.node.longitude - ref_lon_;
            if (dlon > 180) dlon -= 360;
            if (dlon < -180) dlon += 360;
            p.x = dlon * kx;
            p.y = (p.node.latitude - ref_lat_) * kMPerDegLat;
            if (p.node.is_me) me_ = i;
            if (keep && p.node.num == keep) sel_ = i;
        }
        if (sel_ < 0) card_ = false;
    }

    // Centre on all pins at the closest zoom that shows them all.
    void fit() {
        if (pins_.empty()) {
            cx_ = cy_ = 0;
            zoom_ = 6;
            return;
        }
        double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
        for (const Pin& p : pins_) {
            x0 = std::min(x0, p.x); x1 = std::max(x1, p.x);
            y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
        }
        cx_ = (x0 + x1) / 2;
        cy_ = (y0 + y1) / 2;
        // Room for the pin heads and labels, and the controls at the sides.
        const double w = kMapW - 80, h = kMapH - 34;
        zoom_ = kZooms - 1;
        for (int z = 0; z < kZooms; ++z) {
            if ((x1 - x0) / kMpp[z] <= w && (y1 - y0) / kMpp[z] <= h) {
                zoom_ = std::max(z, 1);
                break;
            }
        }
    }

    int sx(double x) const { return static_cast<int>(std::lround(kMapW / 2.0 + (x - cx_) / kMpp[zoom_])); }
    int sy(double y) const { return static_cast<int>(std::lround(kMapH / 2.0 - (y - cy_) / kMpp[zoom_])); }

    void pan(int dx, int dy) {
        cx_ += dx * kMpp[zoom_];
        cy_ -= dy * kMpp[zoom_];
        paint();
    }

    bool zoom(int dir) {
        const int z = std::clamp(zoom_ + dir, 0, kZooms - 1);
        if (z != zoom_) {
            zoom_ = z;
            paint();
        }
        return true;
    }

    void select_num(uint32_t num) {
        for (int i = 0; i < static_cast<int>(pins_.size()); ++i) {
            if (pins_[i].node.num == num) {
                sel_ = i;
                card_ = true;
                keep_visible();
            }
        }
    }

    // Next pin, west to east (me included), then show its card.
    void select_next() {
        std::vector<int> order(pins_.size());
        for (int i = 0; i < static_cast<int>(order.size()); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return pins_[a].x < pins_[b].x; });
        int pos = -1;
        for (int i = 0; i < static_cast<int>(order.size()); ++i) {
            if (order[i] == sel_) pos = i;
        }
        sel_ = order[(pos + 1) % order.size()];
        if (!card_) {
            // First Enter: start on me if I am on the map.
            if (pos < 0 && me_ >= 0) sel_ = me_;
        }
        card_ = true;
        keep_visible();
        paint();
    }

    // Pan so the selected pin is clear of the card and the controls.
    void keep_visible() {
        if (sel_ < 0) return;
        const Pin& p = pins_[sel_];
        const int x = sx(p.x), y = sy(p.y);
        const bool under_card = x > kMapW - kCardW - 8 && y > kMapH - kCardH - 8;
        if (x < 30 || x > kMapW - 34 || y < 16 || y > kMapH - 20 || under_card) {
            cx_ = p.x + (kCardW / 2.0 - 20) * kMpp[zoom_];   // a little left of centre
            cy_ = p.y;
        }
    }

    void paint() {
        if (!map_) return;
        lv_obj_clean(map_);
        const Paper pp = paper();
        const double mpp = kMpp[zoom_];
        const double step = grid_step(mpp, 44);

        // Grid: major lines every step, minor halfway.
        const double half = step / 2;
        const double left = cx_ - kMapW / 2.0 * mpp, right = cx_ + kMapW / 2.0 * mpp;
        const double bottom = cy_ - kMapH / 2.0 * mpp, top = cy_ + kMapH / 2.0 * mpp;
        for (double gx = std::floor(left / half) * half; gx <= right; gx += half) {
            const bool major = std::fmod(std::fabs(gx) + half / 2, step) < half;
            rect(map_, sx(gx), 0, 1, kMapH, major ? pp.major : pp.minor);
        }
        for (double gy = std::floor(bottom / half) * half; gy <= top; gy += half) {
            const bool major = std::fmod(std::fabs(gy) + half / 2, step) < half;
            rect(map_, 0, sy(gy), kMapW, 1, major ? pp.major : pp.minor);
        }

        if (pins_.empty()) {
            paint_controls(pp, step);
            paint_empty();
            return;
        }

        // Pins first (selected last, on top), then the labels, placed in
        // priority order (selected, me, the rest) where they hit nothing.
        taken_.clear();
        for (int i = 0; i < static_cast<int>(pins_.size()); ++i) {
            if (i != sel_) paint_pin(pins_[i], false, pp);
        }
        if (sel_ >= 0) paint_pin(pins_[sel_], card_, pp);
        if (sel_ >= 0) paint_label(pins_[sel_], card_, pp);
        if (me_ >= 0 && me_ != sel_) paint_label(pins_[me_], false, pp);
        for (int i = 0; i < static_cast<int>(pins_.size()); ++i) {
            if (i != sel_ && i != me_) paint_label(pins_[i], false, pp);
        }

        paint_controls(pp, step);
        if (card_ && sel_ >= 0) paint_card();
        else paint_coords(pp);
    }

    void paint_pin(const Pin& p, bool selected, const Paper& pp) {
        const int x = sx(p.x), y = sy(p.y);
        if (x < -40 || x > kMapW + 4 || y < -4 || y > kMapH + 14) return;
        const bool me = p.node.is_me;
        // Shadow, stem, head.
        rect(map_, x - 1, y, 4, 1, pp.ink, LV_OPA_40);
        rect(map_, x, y - 4, 1, 4, kPinEdge);
        const int hs = selected ? 9 : 7;
        lv_obj_t* head = rect(map_, x - hs / 2, y - 4 - hs, hs, hs, selected ? kPinSel : (me ? kPinMe : kPinRed));
        lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(head, 1, 0);
        lv_obj_set_style_border_color(head, c(kPinEdge), 0);
        rect(head, hs / 2 - 2, 1, 2, 2, 0xffffff, LV_OPA_70);   // glint
        taken_.push_back({x - hs / 2, y - 4 - hs, x + hs / 2 + 1, y + 1});
    }

    static bool hits(const lv_area_t& a, const lv_area_t& b) {
        return a.x1 < b.x2 && b.x1 < a.x2 && a.y1 < b.y2 && b.y1 < a.y2;
    }

    void paint_label(const Pin& p, bool selected, const Paper& pp) {
        const int x = sx(p.x), y = sy(p.y);
        if (x < -40 || x > kMapW + 4 || y < -4 || y > kMapH + 14) return;
        const Skin& s = skin();
        const int hs = selected ? 9 : 7;
        lv_obj_t* l = label(map_, p.node.is_me ? "You" : node_label(p.node), 0, 0, font_small(),
                            selected ? s.sel_text : pp.ink);
        lv_obj_set_style_bg_color(l, c(selected ? s.sel : pp.label_bg), 0);
        lv_obj_set_style_bg_opa(l, selected ? LV_OPA_COVER : LV_OPA_80, 0);
        lv_obj_set_style_pad_hor(l, 1, 0);
        lv_obj_set_style_border_width(l, 1, 0);
        lv_obj_set_style_border_color(l, c(selected ? s.sel : pp.major), 0);
        lv_obj_update_layout(l);
        const int w = lv_obj_get_width(l), h = lv_obj_get_height(l);
        const int head_y = y - 4 - hs;
        // Right of the head, left of it, above, below.
        const int cand[][2] = {
            {x + hs / 2 + 2, head_y - 1},
            {x - hs / 2 - 2 - w, head_y - 1},
            {x - w / 2, head_y - h - 1},
            {x - w / 2, y + 2},
            {x + hs / 2 + 2, head_y - h + 4},
            {x + hs / 2 + 2, head_y + 6},
        };
        int best = 0;
        for (int i = 0; i < 6; ++i) {
            const lv_area_t a{cand[i][0], cand[i][1], cand[i][0] + w, cand[i][1] + h};
            bool clear = true;
            for (const lv_area_t& t : taken_) clear = clear && !hits(a, t);
            if (clear) { best = i; break; }
        }
        lv_obj_set_pos(l, cand[best][0], cand[best][1]);
        taken_.push_back({cand[best][0], cand[best][1], cand[best][0] + w, cand[best][1] + h});
    }

    void paint_controls(const Paper& pp, double step) {
        const Skin& s = skin();
        // Zoom ladder, top left: [+], one notch per level, [-].
        const int zx = 5, zy = 4, bw = 13;
        lv_obj_t* plus = win::frame(map_, zx, zy, bw, bw, win::Edge::Raised, s.face, s.radius);
        lv_obj_center(label(plus, "+", 0, 0, font_small(), s.text, 0));
        const int ly = zy + bw + 2, notch = 2, lh = kZooms * notch + 1;
        rect(map_, zx + bw / 2 - 1, ly, 3, lh, pp.major);
        for (int z = 0; z < kZooms; z += 2) rect(map_, zx + 3, ly + z * notch + 1, bw - 6, 1, pp.ink, LV_OPA_50);
        win::frame(map_, zx + 1, ly + zoom_ * notch - 1, bw - 2, 4, win::Edge::Raised, s.face, 1);
        lv_obj_t* minus = win::frame(map_, zx, ly + lh + 2, bw, bw, win::Edge::Raised, s.face, s.radius);
        lv_obj_center(label(minus, "-", 0, 0, font_small(), s.text, 0));

        // North arrow, top right: a needle in a ring with an N above it.
        const int nx = kMapW - 22, ny = 12, r = 9;
        lv_obj_t* ring = rect(map_, nx - r, ny - r + 2, 2 * r + 1, 2 * r + 1, pp.label_bg, LV_OPA_80);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, c(pp.ink), 0);
        for (int i = 0; i < 6; ++i) rect(map_, nx - (i + 1) / 2 + 0, ny - 4 + i, (i + 1) / 2 * 2 + 1, 1, kPinRed);
        for (int i = 0; i < 6; ++i) rect(map_, nx - (5 - i + 1) / 2, ny + 2 + i, (5 - i + 1) / 2 * 2 + 1, 1, pp.ink);
        rect(map_, nx - r + 2, ny + 2, 2 * r - 3, 1, pp.ink, LV_OPA_40);
        lv_obj_t* n = label(map_, "N", 0, 0, font_ui_bold(), pp.ink, 0);
        lv_obj_update_layout(n);
        lv_obj_set_pos(n, nx - lv_obj_get_width(n) / 2 + 1, ny + r + 3);

        // Scale bar, bottom left: one grid step, in two halves.
        const int len = static_cast<int>(std::lround(step / kMpp[zoom_]));
        const int bx = 6, by = kMapH - 8;
        rect(map_, bx - 1, by - 1, len + 2, 5, pp.ink);
        rect(map_, bx, by, len / 2, 3, pp.label_bg);
        rect(map_, bx + len / 2, by, len - len / 2, 3, pp.ink);
        lv_obj_t* sl = label(map_, dist_text(step), bx, by - 13, font_small(), pp.ink, 0);
        lv_obj_set_style_bg_color(sl, c(pp.bg), 0);
        lv_obj_set_style_bg_opa(sl, LV_OPA_70, 0);
    }

    // Centre coordinates, bottom right, like the readout under a map.
    void paint_coords(const Paper& pp) {
        const double kx = kMPerDegLon * std::cos(ref_lat_ * kPi / 180.0);
        const double lat = ref_lat_ + cy_ / kMPerDegLat, lon = ref_lon_ + cx_ / kx;
        const std::string t = fmt("%.4f%c %.4f%c", std::fabs(lat), lat >= 0 ? 'N' : 'S', std::fabs(lon), lon >= 0 ? 'E' : 'W');
        lv_obj_t* l = label(map_, t, 0, 0, font_small(), pp.ink, 0);
        lv_obj_set_style_bg_color(l, c(pp.bg), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_70, 0);
        lv_obj_update_layout(l);
        lv_obj_set_pos(l, kMapW - lv_obj_get_width(l) - 4, kMapH - lv_obj_get_height(l) - 2);
    }

    void paint_card() {
        const Skin& s = skin();
        const Pin& p = pins_[sel_];
        const MeshNode& n = p.node;
        const int x0 = kMapW - kCardW - 3, y0 = kMapH - kCardH - 3;
        lv_obj_t* cd = win::card(map_, x0, y0, kCardW, kCardH);
        const int tx = 4, tw = kCardW - 8, lh = 12;
        int y = 1;

        std::string tag;
        if (!n.is_me && n.hops_away >= 0) tag = n.hops_away == 0 ? "direct" : fmt("%d hop%s", n.hops_away, n.hops_away == 1 ? "" : "s");
        int tag_w = 0;
        if (!tag.empty()) {
            lv_obj_t* t = win::text(cd, tag, 0, y, s.card_text, 0, false);
            lv_obj_update_layout(t);
            tag_w = lv_obj_get_width(t);
            lv_obj_set_x(t, kCardW - 4 - tag_w);
        }
        win::text(cd, n.is_me ? node_name(n) + " (you)" : node_name(n), tx, y, s.card_text, tw - tag_w - 4, true);
        y += lh + 1;
        auto line = [&](const std::string& t) {
            win::text(cd, t, tx, y, s.card_text, tw);
            y += lh;
        };
        if (n.is_me) {
            line(fmt("%.4f, %.4f", n.latitude, n.longitude));
            line(n.altitude ? fmt("Altitude %d m", n.altitude) : std::string("Altitude unknown"));
            line("Fix " + ago_text(n.position_time));
            return;
        }
        if (me_ >= 0) {
            const MeshNode& m = pins_[me_].node;
            double d = 0, b = 0;
            dist_bearing(m.latitude, m.longitude, n.latitude, n.longitude, d, b);
            line(fmt("%s %s (%d\xC2\xB0) of you", dist_text(d).c_str(), compass_point(b), static_cast<int>(std::lround(b)) % 360));
        } else {
            line("Your position: unknown");
        }
        line("Heard " + ago_text(n.last_heard));
        std::string sig = n.last_heard ? fmt("SNR %.1f dB", static_cast<double>(n.snr)) : std::string("SNR -");
        if (n.battery > 100) sig += ", powered";
        else if (n.battery >= 0) sig += fmt(", batt %d%%", n.battery);
        line(sig);
    }

    void paint_empty() {
        const Skin& s = skin();
        const int w = 200, h = 52;
        lv_obj_t* cd = win::card(map_, (kMapW - w) / 2, (kMapH - h) / 2, w, h);
        win::text(cd, "No positions yet", 6, 3, s.card_text, w - 12, true);
        win::text(cd, "Nodes appear here once they", 6, 17, s.card_text, w - 12);
        win::text(cd, "share their GPS position.", 6, 29, s.card_text, w - 12);
        lv_obj_t* t = win::text(cd, heard_ == 0 ? std::string("No nodes heard yet.")
                                                : fmt("%d node%s heard, none with a fix.", heard_, heard_ == 1 ? "" : "s"),
                                6, 40, s.card_text, w - 12);
        lv_obj_set_y(t, 39);
    }

    lv_obj_t* map_ = nullptr;
    std::vector<lv_area_t> taken_;   // pin heads and labels already placed
    std::vector<Pin> pins_;
    int me_ = -1, sel_ = -1, heard_ = 0;
    bool card_ = false;
    bool demo_data_ = false, empty_demo_ = false;
    bool have_ref_ = false;
    double ref_lat_ = 0, ref_lon_ = 0;
    double cx_ = 0, cy_ = 0;       // view centre, metres from the reference point
    int zoom_ = 6;
    uint32_t seen_rev_ = 0, painted_ms_ = 0;
};

} // namespace

std::unique_ptr<Screen> make_map_screen(Ctx& ctx) {
    return std::make_unique<MapScreen>(ctx);
}

} // namespace mm::ui
