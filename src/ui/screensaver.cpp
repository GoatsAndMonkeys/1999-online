// screensaver.cpp — the saver engine and the eight savers (see screensaver.h).
//
// Everything is drawn in software into Canvas::px (XRGB8888). Effects size
// themselves from the canvas height, so the same code fills the 320x170
// display and the ~100x53 monitor preview of the Screen Savers window.
#include "screensaver.h"

#include "settings.h"
#include "style.h"

#include "lvgl.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace mm::ui {
namespace {

// ── 5x8 pixel font (classic 5x7 LCD font, column-major, bit 0 = top) ─────────
constexpr uint8_t kFont[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x56,0x20,0x50}, {0x00,0x08,0x07,0x03,0x00},
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, {0x2A,0x1C,0x7F,0x1C,0x2A}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x80,0x70,0x30,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x00,0x60,0x60,0x00}, {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x72,0x49,0x49,0x49,0x46}, {0x21,0x41,0x49,0x4D,0x33},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x31}, {0x41,0x21,0x11,0x09,0x07},
    {0x36,0x49,0x49,0x49,0x36}, {0x46,0x49,0x49,0x29,0x1E}, {0x00,0x00,0x14,0x00,0x00}, {0x00,0x40,0x34,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14}, {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x59,0x09,0x06},
    {0x3E,0x41,0x5D,0x59,0x4E}, {0x7C,0x12,0x11,0x12,0x7C}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x41,0x3E}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x41,0x51,0x73},
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x1C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, {0x26,0x49,0x49,0x49,0x32},
    {0x03,0x01,0x7F,0x01,0x03}, {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63}, {0x03,0x04,0x78,0x04,0x03}, {0x61,0x59,0x49,0x4D,0x43}, {0x00,0x7F,0x41,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x41,0x7F}, {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x03,0x07,0x08,0x00}, {0x20,0x54,0x54,0x78,0x40}, {0x7F,0x28,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x28},
    {0x38,0x44,0x44,0x28,0x7F}, {0x38,0x54,0x54,0x54,0x18}, {0x00,0x08,0x7E,0x09,0x02}, {0x18,0xA4,0xA4,0x9C,0x78},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x40,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x78,0x04,0x78}, {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
    {0xFC,0x18,0x24,0x24,0x18}, {0x18,0x24,0x24,0x18,0xFC}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x24},
    {0x04,0x04,0x3F,0x44,0x24}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44}, {0x4C,0x90,0x90,0x90,0x7C}, {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x77,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00}, {0x02,0x01,0x02,0x04,0x02},
};

// ── Small helpers ────────────────────────────────────────────────────────────

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x1999u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    int range(int n) { return n > 0 ? static_cast<int>(next() % static_cast<uint32_t>(n)) : 0; }
    float unit() { return static_cast<float>(next() & 0xffffff) / 16777216.f; }        // [0,1)
    float span(float a, float b) { return a + (b - a) * unit(); }
};

uint32_t rgb(int r, int g, int b) {
    r = std::clamp(r, 0, 255); g = std::clamp(g, 0, 255); b = std::clamp(b, 0, 255);
    return 0xff000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}
int ch_r(uint32_t c) { return (c >> 16) & 0xff; }
int ch_g(uint32_t c) { return (c >> 8) & 0xff; }
int ch_b(uint32_t c) { return c & 0xff; }

// c * f / 256 per channel.
uint32_t shade(uint32_t c, int f) {
    return rgb(ch_r(c) * f / 256, ch_g(c) * f / 256, ch_b(c) * f / 256);
}
// a -> b by t/256.
uint32_t mix(uint32_t a, uint32_t b, int t) {
    return rgb(ch_r(a) + (ch_r(b) - ch_r(a)) * t / 256, ch_g(a) + (ch_g(b) - ch_g(a)) * t / 256,
               ch_b(a) + (ch_b(b) - ch_b(a)) * t / 256);
}
// h 0..359, full saturation and value.
uint32_t hue(int h) {
    h = ((h % 360) + 360) % 360;
    const int x = (h % 60) * 255 / 60;
    switch (h / 60) {
        case 0: return rgb(255, x, 0);
        case 1: return rgb(255 - x, 255, 0);
        case 2: return rgb(0, 255, x);
        case 3: return rgb(0, 255 - x, 255);
        case 4: return rgb(x, 0, 255);
        default: return rgb(255, 0, 255 - x);
    }
}

struct Canvas {
    int w = 0, h = 0;
    std::vector<uint32_t> px;

    void clear(uint32_t c) { std::fill(px.begin(), px.end(), c); }
    void pset(int x, int y, uint32_t c) {
        if (static_cast<unsigned>(x) < static_cast<unsigned>(w) && static_cast<unsigned>(y) < static_cast<unsigned>(h))
            px[static_cast<size_t>(y) * w + x] = c;
    }
    void rect(int x, int y, int rw, int rh, uint32_t c) {
        const int x0 = std::max(0, x), y0 = std::max(0, y);
        const int x1 = std::min(w, x + rw), y1 = std::min(h, y + rh);
        for (int yy = y0; yy < y1; ++yy) std::fill(&px[static_cast<size_t>(yy) * w + x0], &px[static_cast<size_t>(yy) * w + x0] + std::max(0, x1 - x0), c);
    }
    void line(int x0, int y0, int x1, int y1, uint32_t c) {
        const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
        const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (int guard = 0; guard < 2048; ++guard) {
            pset(x0, y0, c);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    // Every pixel loses 1/2^k of its brightness.
    void fade(int k) {
        const uint32_t mask = (0xffu >> k) * 0x010101u;
        for (uint32_t& p : px) p = 0xff000000u | ((p & 0xffffffu) - ((p >> k) & mask));
    }
    // Shaded ball lit from the top left (pipe joints).
    void ball(int cx, int cy, int r, uint32_t base) {
        const int hx = cx - r / 3, hy = cy - r / 3;
        const int rr = r * r;
        for (int y = -r; y <= r; ++y) {
            for (int x = -r; x <= r; ++x) {
                if (x * x + y * y > rr + r / 2) continue;
                const int dx = cx + x - hx, dy = cy + y - hy;
                const int d = dx * dx + dy * dy;                    // 0 .. ~(1.4r)^2
                int f = 300 - d * 220 / std::max(1, rr);
                uint32_t col = shade(base, std::clamp(f, 70, 256));
                if (d * 6 < rr) col = mix(col, 0xffffffffu, 150);    // specular spot
                pset(cx + x, cy + y, col);
            }
        }
    }
    // One character; scale >= 1; mirror flips it left-right (rain glyphs).
    void glyph(char ch, int x, int y, int scale, uint32_t c, bool mirror = false) {
        const unsigned u = static_cast<unsigned char>(ch);
        const uint8_t* g = kFont[(u >= 32 && u < 127) ? u - 32 : '?' - 32];
        for (int col = 0; col < 5; ++col) {
            const uint8_t bits = g[mirror ? 4 - col : col];
            for (int row = 0; row < 8; ++row) {
                if (bits & (1u << row)) {
                    if (scale == 1) pset(x + col, y + row, c);
                    else rect(x + col * scale, y + row * scale, scale, scale, c);
                }
            }
        }
    }
    void text(const std::string& s, int x, int y, int scale, uint32_t c) {
        for (char ch : s) { glyph(ch, x, y, scale, c); x += 6 * scale; }
    }
    static int text_w(const std::string& s, int scale) {
        return s.empty() ? 0 : static_cast<int>(s.size()) * 6 * scale - scale;
    }
};

// ── Effects ──────────────────────────────────────────────────────────────────

class Effect {
public:
    Effect(Canvas& cv, uint32_t seed) : cv_(cv), rng_(seed), s_(static_cast<float>(cv.h) / 170.f) {}
    virtual ~Effect() = default;
    virtual void step() = 0;
    virtual void set_text(const std::string&) {}
    virtual void arrange_corner() {}

protected:
    // Pixel size scaled to the canvas (at least `min`).
    int px(float full, int min = 1) const { return std::max(min, static_cast<int>(std::lround(full * s_))); }
    Canvas& cv_;
    Rng rng_;
    float s_;
    uint32_t t_ = 0;   // steps so far
};

// Warp-speed starfield: stars rush out of the centre, streaking as they near.
class Starfield final : public Effect {
public:
    Starfield(Canvas& cv, uint32_t seed) : Effect(cv, seed) {
        stars_.resize(static_cast<size_t>(std::max(24, cv.w * cv.h / 420)));
        for (auto& st : stars_) respawn(st, rng_.span(0.05f, 1.f));
    }
    void step() override {
        cv_.clear(0xff000000u);
        const float cx = cv_.w * 0.5f, cy = cv_.h * 0.5f, f = cv_.w * 0.11f;
        for (auto& st : stars_) {
            const float pz = st.z;
            st.z -= 0.017f;
            if (st.z <= 0.02f) { respawn(st, 1.f); continue; }
            const int x0 = static_cast<int>(cx + st.x * f / pz), y0 = static_cast<int>(cy + st.y * f / pz);
            const int x1 = static_cast<int>(cx + st.x * f / st.z), y1 = static_cast<int>(cy + st.y * f / st.z);
            if (x1 < -4 || x1 > cv_.w + 4 || y1 < -4 || y1 > cv_.h + 4) { respawn(st, 1.f); continue; }
            const int b = static_cast<int>((1.f - st.z) * 255.f);
            const uint32_t col = rgb(b * 3 / 4 + 40, b * 3 / 4 + 40, b + 60);
            cv_.line(x0, y0, x1, y1, shade(col, 150));
            cv_.pset(x1, y1, col);
            if (st.z < 0.3f && s_ > 0.6f) {
                cv_.pset(x1 + 1, y1, col); cv_.pset(x1, y1 + 1, col); cv_.pset(x1 + 1, y1 + 1, col);
            }
        }
        ++t_;
    }

private:
    struct Star { float x, y, z; };
    void respawn(Star& st, float z) {
        st.x = rng_.span(-1.f, 1.f) * 1.9f;
        st.y = rng_.span(-1.f, 1.f);
        if (std::fabs(st.x) + std::fabs(st.y) < 0.05f) st.x = 0.3f;
        st.z = z;
    }
    std::vector<Star> stars_;
};

// Shaded pipes that grow across a grid, turning at ball joints.
class Pipes final : public Effect {
public:
    Pipes(Canvas& cv, uint32_t seed) : Effect(cv, seed) {
        cell_ = px(12.f, 4);
        r_ = std::max(1, cell_ / 3);
        gw_ = cv.w / cell_;
        gh_ = cv.h / cell_;
        ox_ = (cv.w - gw_ * cell_) / 2 + cell_ / 2;
        oy_ = (cv.h - gh_ * cell_) / 2 + cell_ / 2;
        restart();
    }
    void step() override {
        ++t_;
        if (fading_ > 0) {
            cv_.fade(2);
            if (--fading_ == 0) restart();
            return;
        }
        grow();   // one segment per step: ~20 cells a second
    }

private:
    static constexpr int kDx[4] = {1, 0, -1, 0};
    static constexpr int kDy[4] = {0, 1, 0, -1};

    uint8_t& used(int x, int y) { return used_[static_cast<size_t>(y) * gw_ + x]; }
    bool free_cell(int x, int y) { return x >= 0 && y >= 0 && x < gw_ && y < gh_ && !used(x, y); }
    int cx(int gx) const { return ox_ + gx * cell_; }
    int cy(int gy) const { return oy_ + gy * cell_; }

    void restart() {
        cv_.clear(0xff000000u);
        used_.assign(static_cast<size_t>(gw_) * gh_, 0);
        filled_ = 0;
        pipes_ = 0;
        new_pipe();
    }
    void new_pipe() {
        static constexpr uint32_t kColors[] = {0xffd83030, 0xff30c040, 0xff3060e0, 0xffe0c020,
                                               0xff20c0c8, 0xffc040c0, 0xffe08020, 0xffc8c8d0};
        for (int tries = 0; tries < 60; ++tries) {
            const int x = rng_.range(gw_), y = rng_.range(gh_);
            if (!free_cell(x, y)) continue;
            gx_ = x; gy_ = y;
            dir_ = rng_.range(4);
            color_ = kColors[(pipes_ + rng_.range(3)) % 8];
            used(x, y) = 1;
            ++filled_;
            ++pipes_;
            cv_.ball(cx(x), cy(y), r_ + std::max(1, r_ / 2), color_);
            return;
        }
        fading_ = 12;   // no room left
    }
    void grow() {
        if (fading_ > 0) return;
        int d = dir_;
        if (rng_.range(100) < 22) d = (dir_ + (rng_.range(2) ? 1 : 3)) % 4;
        if (!free_cell(gx_ + kDx[d], gy_ + kDy[d])) {
            const int order[3] = {dir_, (dir_ + 1) % 4, (dir_ + 3) % 4};
            d = -1;
            const int first = rng_.range(3);
            for (int i = 0; i < 3; ++i) {
                const int cand = order[(first + i) % 3];
                if (free_cell(gx_ + kDx[cand], gy_ + kDy[cand])) { d = cand; break; }
            }
        }
        if (d < 0) {   // boxed in: cap it and start another pipe
            cv_.ball(cx(gx_), cy(gy_), r_ + 1, color_);
            if (pipes_ >= 9 || filled_ * 100 >= gw_ * gh_ * 55) fading_ = 12;
            else new_pipe();
            return;
        }
        const bool turned = d != dir_;
        const int nx = gx_ + kDx[d], ny = gy_ + kDy[d];
        segment(cx(gx_), cy(gy_), cx(nx), cy(ny), d % 2 == 1);
        if (turned) cv_.ball(cx(gx_), cy(gy_), r_ + 1, color_);
        gx_ = nx; gy_ = ny; dir_ = d;
        used(nx, ny) = 1;
        ++filled_;
    }
    // A straight run between two cell centres, shaded across its width.
    void segment(int x0, int y0, int x1, int y1, bool vertical) {
        for (int o = -r_; o <= r_; ++o) {
            const float t = static_cast<float>(o) / (static_cast<float>(r_) + 0.5f);
            int f = static_cast<int>(70.f + 186.f * std::sqrt(std::max(0.f, 1.f - t * t)));
            uint32_t col = shade(color_, f);
            if (o == -(r_ + 1) / 2) col = mix(col, 0xffffffffu, 110);   // highlight stripe
            if (vertical) cv_.line(x0 + o, y0, x1 + o, y1, col);
            else cv_.line(x0, y0 + o, x1, y1 + o, col);
        }
    }

    int cell_, r_, gw_, gh_, ox_, oy_;
    std::vector<uint8_t> used_;
    int gx_ = 0, gy_ = 0, dir_ = 0;
    uint32_t color_ = 0;
    int filled_ = 0, pipes_ = 0, fading_ = 0;
};

// Two bouncing quadrilaterals trailing echoes of themselves, colours drifting.
class Polygons final : public Effect {
public:
    Polygons(Canvas& cv, uint32_t seed) : Effect(cv, seed) {
        for (auto& sh : shapes_) {
            for (auto& v : sh.v) {
                v.x = rng_.span(0.f, static_cast<float>(cv.w - 1));
                v.y = rng_.span(0.f, static_cast<float>(cv.h - 1));
                v.vx = rng_.span(1.6f, 3.6f) * s_ * (rng_.range(2) ? 1.f : -1.f);
                v.vy = rng_.span(1.2f, 3.0f) * s_ * (rng_.range(2) ? 1.f : -1.f);
            }
            sh.hue = rng_.range(360);
        }
        for (int i = 0; i < kHist; ++i) record();
    }
    void step() override {
        ++t_;
        for (auto& sh : shapes_) {
            for (auto& v : sh.v) {
                v.x += v.vx; v.y += v.vy;
                if (v.x < 0) { v.x = -v.x; v.vx = -v.vx; }
                if (v.y < 0) { v.y = -v.y; v.vy = -v.vy; }
                const float mx = static_cast<float>(cv_.w - 1), my = static_cast<float>(cv_.h - 1);
                if (v.x > mx) { v.x = 2 * mx - v.x; v.vx = -v.vx; }
                if (v.y > my) { v.y = 2 * my - v.y; v.vy = -v.vy; }
            }
            sh.hue += 2;
        }
        record();
        cv_.clear(0xff000000u);
        for (int e = kEchoes - 1; e >= 0; --e) {
            const int slot = (head_ - 1 - e * kGap + kHist * 4) % kHist;
            for (int i = 0; i < 2; ++i) {
                const uint32_t col = shade(hue(shapes_[i].hue - e * 6), 256 - e * 200 / kEchoes);
                const auto& p = hist_[slot][i];
                for (int k = 0; k < 4; ++k) {
                    const int n = (k + 1) % 4;
                    cv_.line(p[k].x, p[k].y, p[n].x, p[n].y, col);
                }
            }
        }
    }

private:
    struct Vtx { float x, y, vx, vy; };
    struct Shape { Vtx v[4]; int hue = 0; };
    struct Pt { int x, y; };
    static constexpr int kEchoes = 5, kGap = 4, kHist = kEchoes * kGap + 1;
    void record() {
        for (int i = 0; i < 2; ++i)
            for (int k = 0; k < 4; ++k)
                hist_[head_][i][k] = {static_cast<int>(shapes_[i].v[k].x), static_cast<int>(shapes_[i].v[k].y)};
        head_ = (head_ + 1) % kHist;
    }
    Shape shapes_[2];
    Pt hist_[kHist][2][4] = {};
    int head_ = 0;
};

// The "1999 Online" logo drifting and bouncing; it changes colour on every
// wall and flashes on a perfect corner hit.
class Logo final : public Effect {
public:
    Logo(Canvas& cv, uint32_t seed) : Effect(cv, seed) {
        big_ = px(4.f);
        small_ = std::max(1, big_ / 2);
        lw_ = std::max(Canvas::text_w("1999", big_), Canvas::text_w("ONLINE", small_)) + 2 * big_;
        lh_ = 8 * big_ + big_ + 2 + 8 * small_;
        x_ = rng_.span(0.f, static_cast<float>(cv.w - lw_));
        y_ = rng_.span(0.f, static_cast<float>(cv.h - lh_));
        const float v = std::max(0.6f, 1.7f * s_);
        vx_ = rng_.range(2) ? v : -v;
        vy_ = rng_.range(2) ? v : -v;
        color_ = rng_.range(kColors);
    }
    void step() override {
        ++t_;
        x_ += vx_; y_ += vy_;
        bool hx = false, hy = false;
        const float mx = static_cast<float>(cv_.w - lw_), my = static_cast<float>(cv_.h - lh_);
        if (x_ <= 0.f) { x_ = 0.f; vx_ = std::fabs(vx_); hx = true; }
        if (x_ >= mx) { x_ = mx; vx_ = -std::fabs(vx_); hx = true; }
        if (y_ <= 0.f) { y_ = 0.f; vy_ = std::fabs(vy_); hy = true; }
        if (y_ >= my) { y_ = my; vy_ = -std::fabs(vy_); hy = true; }
        if (hx) last_x_ = t_;
        if (hy) last_y_ = t_;
        if (hx || hy) {
            color_ = (color_ + 1 + rng_.range(kColors - 1)) % kColors;
            // Both walls within a step of each other: a corner. Party.
            if (last_x_ && last_y_ && (last_x_ > last_y_ ? last_x_ - last_y_ : last_y_ - last_x_) <= 1) {
                flash_ = 30;
                last_x_ = last_y_ = 0;
            }
        }
        draw();
    }
    void arrange_corner() override {
        // Heading for the bottom-right corner, arriving in 6 steps.
        const float v = std::fabs(vx_);
        vx_ = vy_ = v;
        x_ = static_cast<float>(cv_.w - lw_) - v * 6.f;
        y_ = static_cast<float>(cv_.h - lh_) - v * 6.f;
    }

private:
    static constexpr int kColors = 7;
    static uint32_t palette(int i) {
        static constexpr uint32_t k[kColors] = {0xffff4040, 0xff40ff60, 0xff4080ff, 0xffffe040,
                                                0xffff60ff, 0xff40ffff, 0xffff9020};
        return k[i % kColors];
    }
    void draw() {
        const uint32_t col = palette(color_);
        uint32_t bg = 0xff000000u;
        if (flash_ > 0) {
            --flash_;
            bg = shade(hue(static_cast<int>(t_) * 40), 60 + (flash_ % 4 < 2 ? 50 : 0));
        }
        cv_.clear(bg);
        if (flash_ > 0 && cv_.h > 80) {   // confetti
            Rng r(t_ * 7919u);
            for (int i = 0; i < 60; ++i) {
                const int x = r.range(cv_.w), y = r.range(cv_.h);
                cv_.rect(x, y, 2, 2, hue(r.range(360)));
            }
        }
        const int x = static_cast<int>(x_), y = static_cast<int>(y_);
        const int bw = Canvas::text_w("1999", big_);
        cv_.text("1999", x + (lw_ - bw) / 2 + 1, y + 1, big_, shade(col, 90));   // drop shadow
        cv_.text("1999", x + (lw_ - bw) / 2, y, big_, col);
        const int uy = y + 8 * big_ + big_ / 2;
        cv_.rect(x, uy, lw_, std::max(1, big_ / 2), col);
        const int sw = Canvas::text_w("ONLINE", small_);
        cv_.text("ONLINE", x + (lw_ - sw) / 2, uy + 2 + big_ / 2, small_, 0xffffffffu);
        if (flash_ > 0 && cv_.h > 80) {
            const std::string msg = "PERFECT CORNER!";
            const int sc = small_;
            // In the half of the screen the logo is not in.
            const int my = (y + lh_ / 2 < cv_.h / 2 ? cv_.h * 3 / 4 : cv_.h / 4) - 4 * sc;
            cv_.text(msg, (cv_.w - Canvas::text_w(msg, sc)) / 2, my, sc, 0xffffffffu);
        }
    }
    int big_, small_, lw_, lh_;
    float x_, y_, vx_, vy_;
    int color_;
    uint32_t last_x_ = 0, last_y_ = 0;
    int flash_ = 0;
};

// Big text crawling right to left, one pass at a time, rainbow letters.
class Marquee final : public Effect {
public:
    Marquee(Canvas& cv, uint32_t seed) : Effect(cv, seed) {
        scale_ = px(4.f);
        text_ = saver_marquee_text();
        pending_ = text_;
        new_pass();
        x_ = static_cast<float>(cv.w) * 0.35f;
    }
    void set_text(const std::string& t) override {
        pending_ = t;
        if (t_ <= 1) adopt();   // nothing shown yet: use it now
    }
    void step() override {
        ++t_;
        x_ -= std::max(1.f, 4.f * s_);
        if (x_ < -static_cast<float>(Canvas::text_w(text_, scale_))) new_pass();
        cv_.clear(0xff000030u);
        // A thin ticker rule above and below the line, like a lobby sign.
        const int th = 8 * scale_;
        cv_.rect(0, y_ - scale_ - 1, cv_.w, 1, 0xff202060u);
        cv_.rect(0, y_ + th + scale_, cv_.w, 1, 0xff202060u);
        int x = static_cast<int>(x_);
        for (size_t i = 0; i < text_.size(); ++i, x += 6 * scale_) {
            if (x > cv_.w || x + 6 * scale_ < 0) continue;
            const uint32_t col = hue(static_cast<int>(i) * 24 + static_cast<int>(t_) * 6);
            cv_.glyph(text_[i], x + 1, y_ + 1, scale_, 0xff000000u);
            cv_.glyph(text_[i], x, y_, scale_, mix(col, 0xffffffffu, 60));
        }
    }

private:
    void adopt() {
        text_ = pending_.empty() ? std::string(saver_default_marquee()) : pending_;
        for (char& ch : text_) if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126) ch = '?';
    }
    void new_pass() {
        adopt();
        x_ = static_cast<float>(cv_.w);
        const int th = 8 * scale_;
        const int lo = cv_.h / 8, hi = std::max(lo + 1, cv_.h - cv_.h / 8 - th);
        y_ = lo + rng_.range(hi - lo);
    }
    int scale_;
    std::string text_, pending_;
    float x_ = 0.f;
    int y_ = 0;
};

// Columns of green glyphs raining down, bright heads, fading tails.
class Rain final : public Effect {
public:
    Rain(Canvas& cv, uint32_t seed) : Effect(cv, seed) {
        sc_ = 1;
        cw_ = 6 * sc_;
        chh_ = 9 * sc_;
        cols_.resize(static_cast<size_t>(cv.w / cw_));
        rows_ = cv.h / chh_ + 1;
        ox_ = (cv.w - static_cast<int>(cols_.size()) * cw_) / 2;
        cv.clear(0xff000000u);
        for (auto& c : cols_) { reset(c); c.y = rng_.span(-static_cast<float>(rows_), static_cast<float>(rows_)); }
    }
    void step() override {
        ++t_;
        cv_.fade(3);
        for (size_t i = 0; i < cols_.size(); ++i) {
            Col& c = cols_[i];
            const int prev = static_cast<int>(c.y);
            c.y += c.speed;
            const int row = static_cast<int>(c.y);
            const int x = ox_ + static_cast<int>(i) * cw_;
            if (row != prev) {
                if (prev >= 0 && prev < rows_) cell(x, prev, 0xff20e050u);
                if (row >= 0 && row < rows_) cell(x, row, 0xffd8ffd8u);
            }
            // Now and then a glyph in the tail flips.
            if (rng_.range(14) == 0) {
                const int r = row - 2 - rng_.range(8);
                if (r >= 0 && r < rows_) cell(x, r, 0xff109030u);
            }
            if (row > rows_ + 22) reset(c);
        }
    }

private:
    struct Col { float y = 0.f, speed = 0.5f; };
    void reset(Col& c) {
        c.y = -static_cast<float>(rng_.range(rows_ * 2));
        c.speed = rng_.span(0.35f, 1.0f);
    }
    void cell(int x, int row, uint32_t col) {
        static const char kGlyphs[] = "0123456789ABCDEFHKLMNPRTUVXYZ<>=+*:$#%&@";
        cv_.rect(x, row * chh_, cw_, chh_, 0xff000000u);
        const char g = kGlyphs[rng_.range(static_cast<int>(sizeof(kGlyphs) - 1))];
        cv_.glyph(g, x, row * chh_, sc_, col, rng_.range(3) != 0);
    }
    int sc_, cw_, chh_, rows_, ox_;
    std::vector<Col> cols_;
};

// Colourful 3.5" floppies and buzzing pagers flying across a dusk sky.
class Flyers final : public Effect {
public:
    Flyers(Canvas& cv, uint32_t seed) : Effect(cv, seed) {
        sky_.resize(cv.px.size());
        for (int y = 0; y < cv.h; ++y) {
            const uint32_t row = mix(0xff080830u, 0xff40185au, y * 256 / std::max(1, cv.h));
            std::fill(&sky_[static_cast<size_t>(y) * cv.w], &sky_[static_cast<size_t>(y) * cv.w] + cv.w, row);
        }
        Rng sr(seed ^ 0xabcdu);
        for (int i = 0; i < cv.w * cv.h / 300; ++i)
            sky_[static_cast<size_t>(sr.range(cv.h)) * cv.w + sr.range(cv.w)] = shade(0xffffffffu, 90 + sr.range(120));
        things_.resize(cv.h > 100 ? 11 : 6);
        for (auto& t : things_) spawn(t, true);
        std::sort(things_.begin(), things_.end(), [](const Thing& a, const Thing& b) { return a.d < b.d; });
    }
    void step() override {
        ++t_;
        std::memcpy(cv_.px.data(), sky_.data(), sky_.size() * sizeof(uint32_t));
        for (auto& t : things_) {
            t.x += t.vx;
            t.y += t.vy;
            t.phase += 0.25f;
            const int sz = size_of(t);
            if (t.x < -2 * sz || t.y > cv_.h + sz) spawn(t, false);
            const int bob = static_cast<int>(std::sin(t.phase) * 2.f * s_);
            if (t.pager) {
                const int buzz = (t_ / 2 + static_cast<uint32_t>(t.x)) % 2 ? std::max(1, px(1.f)) : 0;
                pager(static_cast<int>(t.x) + buzz, static_cast<int>(t.y) + bob, sz, t);
            } else {
                floppy(static_cast<int>(t.x), static_cast<int>(t.y) + bob, sz, t.color);
            }
        }
    }

private:
    struct Thing {
        float x, y, vx, vy, d, phase;
        bool pager;
        uint32_t color;
        int code;
    };
    int size_of(const Thing& t) const { return std::max(6, static_cast<int>(26.f * t.d * s_)); }
    void spawn(Thing& t, bool anywhere) {
        static constexpr uint32_t kDisk[] = {0xff2850c8, 0xffc82838, 0xff28a048, 0xffe0b020,
                                             0xff7038b0, 0xff303038, 0xffe06010, 0xff20a0b0};
        static const float kDepth[] = {0.55f, 0.75f, 1.0f};
        t.d = kDepth[rng_.range(3)];
        t.pager = rng_.range(10) < 3;
        t.color = kDisk[rng_.range(8)];
        t.code = rng_.range(5);
        t.phase = rng_.span(0.f, 6.28f);
        const float sp = (1.0f + 1.8f * t.d) * s_;
        t.vx = -sp;
        t.vy = sp * 0.42f;
        if (anywhere) {
            t.x = rng_.span(0.f, static_cast<float>(cv_.w));
            t.y = rng_.span(-static_cast<float>(cv_.h) * 0.3f, static_cast<float>(cv_.h));
        } else if (rng_.range(3) == 0) {   // in over the top edge
            t.x = rng_.span(cv_.w * 0.3f, static_cast<float>(cv_.w) + 20.f * s_);
            t.y = -static_cast<float>(size_of(t)) - 2.f;
        } else {                          // in from the right
            t.x = static_cast<float>(cv_.w) + 2.f;
            t.y = rng_.span(-static_cast<float>(cv_.h) * 0.3f, static_cast<float>(cv_.h) * 0.7f);
        }
    }
    void floppy(int x, int y, int s, uint32_t body) {
        const int notch = std::max(1, s / 8);
        for (int r = 0; r < s; ++r) {
            const int cut = r < notch ? notch - r : 0;    // chamfered top-right corner
            cv_.rect(x, y + r, s - cut, 1, r == 0 ? mix(body, 0xffffffffu, 70) : body);
        }
        cv_.rect(x + s - 1, y + notch, 1, s - notch, shade(body, 150));   // right edge in shadow
        cv_.rect(x, y + s - 1, s, 1, shade(body, 150));
        // Metal shutter with its window.
        const int shw = s * 11 / 20, shx = x + (s - shw) / 2 + s / 12;
        const int shh = s * 7 / 20;
        cv_.rect(shx, y, shw, shh, 0xffb8bcc8u);
        cv_.rect(shx, y + shh - 1, shw, 1, 0xff70747cu);
        if (s >= 10) cv_.rect(shx + shw * 6 / 10, y + shh / 5, std::max(1, shw / 5), shh * 3 / 5, 0xff404048u);
        // Label with a scribble.
        const int lx = x + s / 8, lw = s - s / 4, ly = y + s / 2, lh = s / 2 - std::max(1, s / 12);
        cv_.rect(lx, ly, lw, lh, 0xfff4f0e0u);
        if (s >= 12) {
            cv_.rect(lx + 2, ly + lh / 3, lw * 2 / 3, 1, 0xff3048a0u);
            cv_.rect(lx + 2, ly + lh * 2 / 3, lw / 2, 1, 0xff3048a0u);
        }
    }
    void pager(int x, int y, int s, const Thing& t) {
        static const char* kCodes[] = {"143", "911", "411", "1999", "07734"};
        const int w = s * 17 / 10, h = s * 17 / 20;
        cv_.rect(x + 1, y, w - 2, h, 0xff24262cu);
        cv_.rect(x, y + 1, w, h - 2, 0xff24262cu);
        cv_.rect(x + 1, y, w - 2, 1, 0xff50545cu);                 // top highlight
        cv_.rect(x + w - std::max(2, s / 6), y - std::max(1, s / 10), std::max(2, s / 8), std::max(1, s / 10), 0xff24262cu);   // belt clip
        const int lx = x + w / 10, ly = y + h / 5, lw = w * 13 / 20, lh = h * 9 / 20;
        cv_.rect(lx, ly, lw, lh, 0xff9cc08au);
        const std::string code = kCodes[t.code];
        if (lh >= 8 && Canvas::text_w(code, 1) <= lw - 2)
            cv_.text(code, lx + lw - 1 - Canvas::text_w(code, 1), ly + (lh - 7) / 2, 1, 0xff1c3018u);
        const int bx = x + w * 82 / 100;
        cv_.rect(bx, y + h / 3, std::max(1, w / 12), std::max(1, h / 4), 0xff808890u);   // button
        if ((t_ / 3) % 2) cv_.rect(bx, y + h * 2 / 3 + 1, std::max(1, w / 14), std::max(1, w / 14), 0xffff4030u);   // blinking LED
    }
    std::vector<uint32_t> sky_;
    std::vector<Thing> things_;
};

// Nothing at all, the most 1999 saver of them all.
class Blank final : public Effect {
public:
    Blank(Canvas& cv, uint32_t seed) : Effect(cv, seed) { cv.clear(0xff000000u); }
    void step() override { ++t_; }
};

struct SaverInfo {
    const char* id;
    const char* name;
};
constexpr SaverInfo kSavers[] = {
    {"stars", "Starfield Warp"},
    {"pipes", "3D Pipes"},
    {"poly", "Mystic Polygons"},
    {"logo", "Bouncing Logo"},
    {"marquee", "Marquee"},
    {"rain", "Digital Rain"},
    {"flyers", "Flying Floppies"},
    {"blank", "Blank Screen"},
};
constexpr int kSaverCount = static_cast<int>(sizeof(kSavers) / sizeof(kSavers[0]));

std::unique_ptr<Effect> make_effect(int which, Canvas& cv, uint32_t seed) {
    switch (which) {
        case 0: return std::make_unique<Starfield>(cv, seed);
        case 1: return std::make_unique<Pipes>(cv, seed);
        case 2: return std::make_unique<Polygons>(cv, seed);
        case 3: return std::make_unique<Logo>(cv, seed);
        case 4: return std::make_unique<Marquee>(cv, seed);
        case 5: return std::make_unique<Rain>(cv, seed);
        case 6: return std::make_unique<Flyers>(cv, seed);
        default: return std::make_unique<Blank>(cv, seed);
    }
}

constexpr const char* kKeyId = "saver.id";
constexpr const char* kKeyWait = "saver.wait";
constexpr const char* kKeyText = "saver.text";
constexpr int kDefaultWait = 5;

// ── Full-screen state ────────────────────────────────────────────────────────
struct FullScreen {
    std::unique_ptr<SaverRenderer> r;
    lv_obj_t* root = nullptr;
    lv_obj_t* img = nullptr;
    lv_image_dsc_t dsc{};
    uint32_t last_step = 0;
    bool frozen = false;
    uint32_t last_activity = 0;
    bool clock_started = false;
};
FullScreen& fs() {
    static FullScreen f;
    return f;
}

void show_full(std::unique_ptr<SaverRenderer> r, bool frozen) {
    saver_stop();
    FullScreen& f = fs();
    f.r = std::move(r);
    f.frozen = frozen;
    f.last_step = lv_tick_get();

    f.root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(f.root);
    lv_obj_set_pos(f.root, 0, 0);
    lv_obj_set_size(f.root, kScreenW, kScreenH);
    lv_obj_set_style_bg_color(f.root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(f.root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(f.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(f.root, LV_OBJ_FLAG_CLICKABLE);

    f.dsc = lv_image_dsc_t{};
    f.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    f.dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
    f.dsc.header.w = static_cast<uint32_t>(f.r->width());
    f.dsc.header.h = static_cast<uint32_t>(f.r->height());
    f.dsc.header.stride = static_cast<uint32_t>(f.r->width() * 4);
    f.dsc.data = reinterpret_cast<const uint8_t*>(f.r->pixels());
    f.dsc.data_size = static_cast<uint32_t>(f.r->width() * f.r->height() * 4);
    f.img = lv_image_create(f.root);
    lv_image_set_src(f.img, &f.dsc);
    lv_obj_set_pos(f.img, 0, 0);
}

} // namespace

// ── SaverRenderer ────────────────────────────────────────────────────────────

struct SaverRenderer::Impl {
    int which = 0;
    Canvas cv;
    std::unique_ptr<Effect> fx;
};

SaverRenderer::SaverRenderer(int which, int w, int h, uint32_t seed) : p_(std::make_unique<Impl>()) {
    p_->which = std::clamp(which, 0, kSaverCount - 1);
    p_->cv.w = std::max(8, w);
    p_->cv.h = std::max(8, h);
    p_->cv.px.assign(static_cast<size_t>(p_->cv.w) * p_->cv.h, 0xff000000u);
    p_->fx = make_effect(p_->which, p_->cv, seed);
    p_->fx->step();   // never show an empty first frame
}
SaverRenderer::~SaverRenderer() = default;
void SaverRenderer::step() { p_->fx->step(); }
void SaverRenderer::set_text(const std::string& t) { p_->fx->set_text(t); }
void SaverRenderer::arrange_corner() { p_->fx->arrange_corner(); }
const uint32_t* SaverRenderer::pixels() const { return p_->cv.px.data(); }
int SaverRenderer::width() const { return p_->cv.w; }
int SaverRenderer::height() const { return p_->cv.h; }
int SaverRenderer::which() const { return p_->which; }

// ── Catalogue / settings ─────────────────────────────────────────────────────

int saver_count() { return kSaverCount; }
const char* saver_name(int which) { return kSavers[std::clamp(which, 0, kSaverCount - 1)].name; }
const char* saver_id(int which) { return kSavers[std::clamp(which, 0, kSaverCount - 1)].id; }
int saver_from_id(const char* id) {
    if (!id) return -1;
    for (int i = 0; i < kSaverCount; ++i)
        if (std::strcmp(kSavers[i].id, id) == 0) return i;
    return -1;
}

int saver_selected() {
    const int i = saver_from_id(mm::settings().get_string(kKeyId, "stars").c_str());
    return i < 0 ? 0 : i;
}
void saver_set_selected(int which) { mm::settings().set_string(kKeyId, saver_id(which)); }
int saver_wait_minutes() {
    return static_cast<int>(std::clamp<int64_t>(mm::settings().get_int(kKeyWait, kDefaultWait), 0, 240));
}
void saver_set_wait_minutes(int minutes) { mm::settings().set_int(kKeyWait, std::clamp(minutes, 0, 240)); }
const char* saver_default_marquee() {
    return "Welcome to 1999 Online!   You've got mesh!   Party like it's 1999   *** Y2K ready ***";
}
std::string saver_marquee_text() {
    const std::string t = mm::settings().get_string(kKeyText, "");
    return t.empty() ? std::string(saver_default_marquee()) : t;
}
void saver_set_marquee_text(const std::string& text) {
    if (text.empty() || text == saver_default_marquee()) mm::settings().remove(kKeyText);
    else mm::settings().set_string(kKeyText, text);
}

// ── Full screen ──────────────────────────────────────────────────────────────

void saver_start(int which) {
    if (which < 0) which = saver_selected();
    const uint32_t seed = lv_tick_get() * 2654435761u + 0x1999u;
    show_full(std::make_unique<SaverRenderer>(which, kScreenW, kScreenH, seed), false);
}

void saver_start_frozen(int which, uint32_t seed, int steps, int variant) {
    auto r = std::make_unique<SaverRenderer>(which, kScreenW, kScreenH, seed);
    if (variant == 1) r->arrange_corner();
    for (int i = 0; i < steps; ++i) r->step();
    show_full(std::move(r), true);
}

void saver_stop() {
    FullScreen& f = fs();
    if (f.root) lv_obj_delete(f.root);
    f.root = nullptr;
    f.img = nullptr;
    f.r.reset();
    f.frozen = false;
    f.last_activity = lv_tick_get();   // waking counts as activity
}

bool saver_active() { return fs().root != nullptr; }

void saver_note_activity(uint32_t now_ms) {
    FullScreen& f = fs();
    f.last_activity = now_ms;
    f.clock_started = true;
}

void saver_tick(uint32_t now_ms) {
    FullScreen& f = fs();
    if (!f.clock_started) {
        f.clock_started = true;
        f.last_activity = now_ms;
    }
    if (!f.root) {
        const int wait = saver_wait_minutes();
        if (wait > 0 && now_ms - f.last_activity >= static_cast<uint32_t>(wait) * 60000u) saver_start(-1);
        return;
    }
    if (f.frozen || !f.r) return;
    if (now_ms - f.last_step < SaverRenderer::kStepMs) return;
    // Catch up at most two steps after a stall, then resync.
    int n = 0;
    while (now_ms - f.last_step >= SaverRenderer::kStepMs && n < 2) {
        f.r->step();
        f.last_step += SaverRenderer::kStepMs;
        ++n;
    }
    if (now_ms - f.last_step >= SaverRenderer::kStepMs) f.last_step = now_ms;
    lv_obj_invalidate(f.img);
}

} // namespace mm::ui
