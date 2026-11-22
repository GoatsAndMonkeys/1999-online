// viz.cpp — see viz.h. Engine first, then the presets, then the analyzer.
#include "viz.h"

#include "milk.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace mm::viz {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTau = 6.28318530717959f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Sine good to about 0.1%: enough for wobble, far cheaper than libm on the
// device. Used once per mesh vertex, never per pixel.
inline float fsin(float x) {
    x -= kTau * std::floor(x * (1.f / kTau) + 0.5f);          // -pi..pi
    float y = 1.27323954f * x - 0.405284735f * x * std::fabs(x);
    return 0.225f * (y * std::fabs(y) - y) + y;
}
inline float fcos(float x) { return fsin(x + kPi * 0.5f); }

inline uint32_t hash32(uint32_t n) {
    n = (n ^ 61u) ^ (n >> 16);
    n *= 9u;
    n ^= n >> 4;
    n *= 0x27d4eb2du;
    n ^= n >> 15;
    return n;
}
inline float hash_pm1(uint32_t n) { return (hash32(n) & 0xFFFFu) * (1.f / 32768.f) - 1.f; }

// Hue 0..1 (wraps), saturation 0..1, value 0..1+ -> 0..255 per channel.
void hue_rgb(float h, float s, float v, int& r, int& g, int& b) {
    h -= std::floor(h);
    const float x = h * 6.f;
    const float fr = clampf(std::fabs(x - 3.f) - 1.f, 0.f, 1.f);
    const float fg = clampf(2.f - std::fabs(x - 2.f), 0.f, 1.f);
    const float fb = clampf(2.f - std::fabs(x - 4.f), 0.f, 1.f);
    const float m = 255.f * v;
    r = std::min(255, int(m * (1.f - s + s * fr)));
    g = std::min(255, int(m * (1.f - s + s * fg)));
    b = std::min(255, int(m * (1.f - s + s * fb)));
}

// ── Preset model ────────────────────────────────────────────────────────────

enum WaveMode : int {
    kWaveLine,      // one line through (wave_x, wave_y) at wave_ang
    kWaveCircle,    // closed loop of radius wave_size
    kWaveDual,      // two parallel lines, wave_sep apart
    kWaveDots,      // X/Y scope: sample against a delayed copy of itself
    kWaveBars,      // spectrum bars rising from wave_y
    kWaveSpikes,    // rays from a ring of radius wave_size
    kWaveNone,
};

enum Mirror : int { kMirrorNone, kMirrorH, kMirrorV, kMirrorQuad };

// Screen space for all of this: y runs -1 (top) to 1 (bottom), x runs -A to A
// with A = width / height, so a circle is a circle on any panel shape.
// Motion values are "per frame at 30 fps"; the engine rescales them.
struct Params {
    // Feedback warp.
    float zoom = 1.f;        // > 1 pushes the picture outwards
    float zoomexp = 1.f;     // > 1 makes the zoom stronger towards the edges
    float rot = 0.f;         // radians
    float warp = 0.f;        // wobble amount
    float warp_speed = 1.f;
    float warp_scale = 1.f;  // spatial frequency of the wobble
    float cx = 0.f, cy = 0.f;    // centre of zoom / rotation
    float dx = 0.f, dy = 0.f;    // drift
    float sx = 1.f, sy = 1.f;    // stretch
    float decay = 0.96f;         // brightness kept per frame
    float tint_r = 1.f, tint_g = 1.f, tint_b = 1.f;   // extra per-channel decay
    bool  wrap = false;          // sample past an edge from the opposite edge
    float beat_zoom = 0.f;       // added to zoom on a bass hit
    float darken = 0.f;          // 0..1, soft dark spot at the centre
    int   mirror = kMirrorNone;

    // Waveform.
    int   wave_mode = kWaveLine;
    float wave_hue = 0.f, wave_spread = 0.f;   // spread: hue change along the wave
    float wave_sat = 1.f, wave_bright = 1.f;
    float wave_scale = 0.3f;     // amplitude
    float wave_x = 0.f, wave_y = 0.f;
    float wave_ang = 0.f;
    float wave_size = 0.9f;      // half-length (as a fraction of A) or radius
    float wave_sep = 0.5f;
    float wave_width = 1.f;      // pixels at a 100-pixel-high image

    // A frame painted over the edge of the image (overwrites, not additive).
    float border = 0.f;          // thickness, fraction of the height
    float border_hue = 0.f, border_bright = 1.f;

    // One regular polygon.
    int   shape_sides = 0;       // 0 = none
    float shape_x = 0.f, shape_y = 0.f, shape_rad = 0.2f, shape_ang = 0.f;
    float shape_hue = 0.f, shape_fill = 0.2f, shape_edge = 1.f;

    // Sparks scattered around (cx, cy).
    float stars = 0.f;           // per frame
    float star_spread = 0.3f;
    float star_hue = 0.f, star_hue_range = 1.f;
};

struct Ctx {
    float time, k;                         // k: frames-at-30fps this frame spans
    float bass, mid, treb;
    float bass_att, mid_att, treb_att;     // smoothed
    float beat;                            // 1 on a bass hit, decays
    uint32_t beats, frame;
    float aspect;
};

struct Vtx {
    float x, y, rad, ang;                  // rad: 0 centre .. 1 corner
    float zoom, rot, dx, dy, sx, sy, warp; // start as the frame's values
};

struct Preset {
    const char* name;
    void (*init)(Params&);
    void (*per_frame)(Params&, const Ctx&);
    void (*per_vertex)(Vtx&, const Params&, const Ctx&);   // may be null
};

// ── Presets ─────────────────────────────────────────────────────────────────
// Each is: fixed settings, then what moves every frame, then (optionally) how
// the motion varies across the picture.

const Preset kPresets[] = {
    {"Neon Undertow",
     [](Params& p) {
         p.decay = 0.955f; p.warp = 0.35f; p.darken = 0.55f; p.beat_zoom = 0.05f;
         p.wave_mode = kWaveCircle; p.wave_size = 0.34f; p.wave_scale = 0.22f;
         p.wave_spread = 0.35f; p.wave_width = 1.4f;
     },
     [](Params& p, const Ctx& c) {
         p.rot = 0.022f * fsin(c.time * 0.31f);
         p.cx = p.wave_x = 0.55f * fsin(c.time * 0.23f);
         p.cy = p.wave_y = 0.18f * fsin(c.time * 0.37f + 1.f);
         p.wave_hue = c.time * 0.045f;
         p.wave_size = 0.30f + 0.06f * c.bass_att;
     },
     [](Vtx& v, const Params&, const Ctx&) { v.zoom = 1.035f + 0.05f * v.rad; }},

    {"Glass Cathedral",
     [](Params& p) {
         p.decay = 0.965f; p.zoom = 1.018f; p.mirror = kMirrorQuad;
         p.wave_mode = kWaveLine; p.wave_size = 0.42f; p.wave_scale = 0.3f;
         p.wave_spread = 0.5f; p.wave_width = 1.3f; p.beat_zoom = 0.03f;
         p.shape_sides = 6; p.shape_fill = 0.05f; p.shape_edge = 0.7f;
         p.tint_g = 0.985f;
     },
     [](Params& p, const Ctx& c) {
         p.rot = 0.035f * fsin(c.time * 0.41f);
         p.wave_x = -0.5f * c.aspect + 0.25f * fsin(c.time * 0.29f);
         p.wave_y = -0.45f + 0.2f * fsin(c.time * 0.53f);
         p.wave_ang = c.time * 0.45f;
         p.wave_hue = 0.55f + c.time * 0.03f;
         p.shape_x = -0.22f * c.aspect; p.shape_y = -0.5f;
         p.shape_rad = 0.16f + 0.07f * c.bass_att;
         p.shape_ang = -c.time * 0.6f;
         p.shape_hue = p.wave_hue + 0.45f;
     },
     nullptr},

    {"Dial-Up Aurora",
     [](Params& p) {
         p.decay = 0.982f; p.zoom = 1.004f; p.warp = 2.4f; p.warp_scale = 1.25f;
         p.warp_speed = 0.8f; p.wrap = true;
         p.wave_mode = kWaveDual; p.wave_sep = 0.9f; p.wave_scale = 0.22f;
         p.wave_size = 1.f; p.wave_spread = 0.55f; p.wave_bright = 0.8f;
         p.tint_r = 0.985f;
     },
     [](Params& p, const Ctx& c) {
         p.dx = 0.006f * fsin(c.time * 0.21f);
         p.dy = 0.004f * fsin(c.time * 0.17f + 2.f);
         p.warp = 1.8f + 0.9f * c.mid_att;
         p.wave_hue = 0.33f + 0.22f * fsin(c.time * 0.11f);
         p.wave_sep = 0.7f + 0.3f * fsin(c.time * 0.4f);
     },
     nullptr},

    {"Comet Nursery",
     [](Params& p) {
         p.decay = 0.978f; p.rot = 0.012f; p.darken = 0.3f; p.beat_zoom = 0.05f;
         p.stars = 5.f; p.star_spread = 0.55f; p.star_hue = 0.58f; p.star_hue_range = 0.25f;
         p.wave_mode = kWaveSpikes; p.wave_size = 0.05f; p.wave_scale = 0.22f;
         p.wave_bright = 0.9f; p.wave_spread = 0.12f; p.wave_sat = 0.8f;
     },
     [](Params& p, const Ctx& c) {
         p.cx = p.wave_x = 0.9f * fsin(c.time * 0.19f);
         p.cy = p.wave_y = 0.3f * fsin(c.time * 0.31f + 0.7f);
         p.stars = 4.f + 9.f * c.treb_att;
         p.wave_hue = 0.08f + 0.05f * fsin(c.time * 0.2f);
         p.wave_ang = c.time * 0.3f;
     },
     [](Vtx& v, const Params&, const Ctx&) { v.zoom = 1.02f + 0.07f * v.rad; }},

    {"Cathode Whirlpool",
     [](Params& p) {
         p.decay = 0.972f; p.zoom = 0.968f;
         p.wave_mode = kWaveCircle; p.wave_size = 0.86f; p.wave_scale = 0.16f;
         p.wave_spread = 1.f; p.wave_width = 1.8f; p.beat_zoom = -0.025f;
     },
     [](Params& p, const Ctx& c) {
         p.wave_hue = c.time * 0.08f;
         p.wave_ang = c.time * 0.5f;
         p.sx = 1.f + 0.012f * fsin(c.time * 0.6f);
     },
     [](Vtx& v, const Params&, const Ctx& c) {
         const float q = 1.f - v.rad;
         v.rot = (0.02f + 0.16f * q * q) * (0.75f + 0.25f * fsin(c.time * 0.27f));
     }},

    {"Tidepool Static",
     [](Params& p) {
         p.decay = 0.962f; p.rot = 0.008f;
         p.wave_mode = kWaveDots; p.wave_scale = 0.75f; p.wave_spread = 0.25f;
         p.wave_bright = 1.f; p.tint_r = 0.96f;
     },
     [](Params& p, const Ctx& c) {
         p.wave_hue = 0.42f + 0.1f * fsin(c.time * 0.23f);
         p.wave_scale = 0.55f + 0.2f * c.bass_att;
     },
     [](Vtx& v, const Params&, const Ctx& c) {
         v.zoom = 1.012f + 0.04f * fsin(v.rad * 17.f - c.time * 2.6f);
         v.rot = 0.01f + 0.02f * fsin(v.rad * 9.f + c.time * 0.7f);
     }},

    {"Bassline Skyline",
     [](Params& p) {
         p.decay = 0.95f; p.dy = -0.03f; p.sx = 1.012f; p.warp = 0.7f; p.warp_scale = 2.f;
         p.tint_g = 0.965f; p.tint_b = 0.9f;
         p.wave_mode = kWaveBars; p.wave_y = 0.97f; p.wave_size = 0.96f;
         p.wave_scale = 1.1f; p.wave_spread = 0.16f; p.wave_bright = 0.9f;
     },
     [](Params& p, const Ctx& c) {
         p.wave_hue = 0.98f + 0.04f * fsin(c.time * 0.3f);
         p.dy = -0.022f - 0.012f * c.bass_att;
         p.dx = 0.006f * fsin(c.time * 0.5f);
     },
     nullptr},

    {"Moth Lantern",
     [](Params& p) {
         p.decay = 0.95f; p.zoom = 1.05f; p.beat_zoom = 0.04f;
         p.shape_sides = 5; p.shape_fill = 0.07f; p.shape_edge = 1.f;
         p.wave_mode = kWaveCircle; p.wave_size = 0.1f; p.wave_scale = 0.1f;
         p.wave_bright = 0.5f; p.wave_sat = 0.4f; p.darken = 0.25f;
     },
     [](Params& p, const Ctx& c) {
         p.shape_rad = 0.2f + 0.1f * c.bass_att + 0.12f * c.beat;
         p.shape_ang = c.time * 0.9f;
         p.shape_hue = 0.09f + 0.07f * fsin(c.time * 0.17f);
         p.wave_hue = p.shape_hue + 0.5f;
         p.shape_sides = 3 + int(c.beats / 8 % 4);
     },
     [](Vtx& v, const Params&, const Ctx& c) {
         v.rot = 0.07f * fsin(c.time * 0.45f) + 0.09f * (v.rad - 0.3f);
     }},

    {"Velvet Modem",
     [](Params& p) {
         p.decay = 0.968f; p.mirror = kMirrorH; p.tint_g = 0.955f;
         p.wave_mode = kWaveLine; p.wave_ang = kPi * 0.5f; p.wave_size = 0.4f;
         p.wave_scale = 0.3f; p.wave_spread = 0.3f; p.wave_width = 1.5f;
     },
     [](Params& p, const Ctx& c) {
         p.wave_x = -0.12f * c.aspect;
         p.wave_hue = 0.78f + 0.1f * fsin(c.time * 0.19f);
         p.wave_scale = 0.2f + 0.12f * c.mid_att;
     },
     [](Vtx& v, const Params&, const Ctx& c) {
         v.dx = -0.018f - 0.03f * (1.f - std::fabs(v.y));
         v.dy = 0.016f * fsin(v.x * 2.6f + c.time * 1.1f);
         v.sy = 1.012f;
     }},

    {"Lagoon Lattice",
     [](Params& p) {
         p.decay = 0.975f; p.wrap = true; p.tint_r = 0.95f;
         p.wave_mode = kWaveLine; p.wave_size = 0.7f; p.wave_scale = 0.3f;
         p.wave_spread = 0.2f; p.wave_width = 1.4f;
     },
     [](Params& p, const Ctx& c) {
         p.wave_ang = c.time * 0.21f;
         p.wave_hue = 0.52f + 0.09f * fsin(c.time * 0.13f);
     },
     [](Vtx& v, const Params&, const Ctx& c) {
         v.dx = 0.02f * fsin(v.y * 5.5f + c.time * 0.7f);
         v.dy = 0.02f * fsin(v.x * 5.5f + c.time * 0.9f);
     }},

    {"Solar Clockwork",
     [](Params& p) {
         p.decay = 0.958f; p.rot = 0.018f; p.darken = 0.4f; p.beat_zoom = 0.04f;
         p.wave_mode = kWaveSpikes; p.wave_size = 0.16f; p.wave_scale = 0.3f;
         p.wave_spread = 0.14f; p.tint_b = 0.93f;
     },
     [](Params& p, const Ctx& c) {
         p.wave_hue = 0.02f + 0.05f * fsin(c.time * 0.21f);
         p.wave_ang = -c.time * 0.35f;
         p.wave_size = 0.13f + 0.05f * c.bass_att;
     },
     [](Vtx& v, const Params&, const Ctx& c) {
         v.zoom = 1.035f + 0.045f * fcos(v.ang * 6.f + c.time * 1.4f);
     }},

    {"Midnight Escalator",
     [](Params& p) {
         p.decay = 0.992f; p.zoom = 0.93f; p.border = 0.035f; p.beat_zoom = -0.035f;
         p.wave_mode = kWaveLine; p.wave_size = 0.5f; p.wave_scale = 0.28f;
         p.wave_sat = 0.25f; p.wave_bright = 0.9f; p.wave_width = 1.3f;
     },
     [](Params& p, const Ctx& c) {
         p.rot = 0.03f * fsin(c.time * 0.37f);
         p.border_hue = c.time * 0.21f + 0.35f * fsin(c.time * 5.2f);
         p.border_bright = 0.12f + 0.75f * c.beat;
         p.wave_hue = p.border_hue + 0.5f;
         p.cx = 0.35f * fsin(c.time * 0.29f);
     },
     nullptr},
};

constexpr int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));

// ── Pixel helpers ───────────────────────────────────────────────────────────

inline void add_px(uint32_t* p, int r, int g, int b) {
    const uint32_t c = *p;
    int nr = int((c >> 16) & 0xFF) + r;
    int ng = int((c >> 8) & 0xFF) + g;
    int nb = int(c & 0xFF) + b;
    if (nr > 255) nr = 255;
    if (ng > 255) ng = 255;
    if (nb > 255) nb = 255;
    *p = (uint32_t(nr) << 16) | (uint32_t(ng) << 8) | uint32_t(nb);
}

// Colour over what is there: added (saturating) or alpha-blended.
// a: 0..65536. d: 0..65535, a dither value; rounding by it instead of
// truncating lets very faint strokes (alpha of a percent or less, common in
// MilkDrop presets) still add up to something.
inline void blend_px(uint32_t* p, int r, int g, int b, int a, bool additive, int d) {
    if (additive) {
        add_px(p, (r * a + d) >> 16, (g * a + d) >> 16, (b * a + d) >> 16);
        return;
    }
    const uint32_t c = *p;
    const int dr = int((c >> 16) & 0xFF), dg = int((c >> 8) & 0xFF), db = int(c & 0xFF);
    *p = (uint32_t(dr + (((r - dr) * a + d) >> 16)) << 16) | (uint32_t(dg + (((g - dg) * a + d) >> 16)) << 8) |
         uint32_t(db + (((b - db) * a + d) >> 16));
}

// Previous-frame fetch for a sample whose 2x2 footprint touches an edge.
void fetch_edge(const uint32_t* src, int W, int H, bool wrap, int32_t u, int32_t v,
                uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d,
                uint32_t& fx, uint32_t& fy) {
    int ix = u >> 16, iy = v >> 16;
    int ix1 = ix + 1, iy1 = iy + 1;
    if (wrap) {
        ix %= W;  if (ix < 0) ix += W;
        ix1 %= W; if (ix1 < 0) ix1 += W;
        iy %= H;  if (iy < 0) iy += H;
        iy1 %= H; if (iy1 < 0) iy1 += H;
    } else {
        if (ix < 0) { ix = ix1 = 0; fx = 0; }
        else if (ix >= W - 1) { ix = ix1 = W - 1; fx = 0; }
        if (iy < 0) { iy = iy1 = 0; fy = 0; }
        else if (iy >= H - 1) { iy = iy1 = H - 1; fy = 0; }
    }
    a = src[iy * W + ix];
    b = src[iy * W + ix1];
    c = src[iy1 * W + ix];
    d = src[iy1 * W + ix1];
}

}  // namespace

// ── Engine ──────────────────────────────────────────────────────────────────

struct Visualizer::Impl {
    Visualizer& z;
    const int W, H;
    uint32_t* buf;
    const float A;

    explicit Impl(Visualizer& v)
        : z(v), W(v.w_), H(v.h_), buf(v.cur_.data()), A(float(v.w_) / float(v.h_)) {}

    float px(float x) const { return (x / A + 1.f) * 0.5f * float(W); }
    float py(float y) const { return (y + 1.f) * 0.5f * float(H); }
    float unit() const { return 0.5f * float(H); }           // pixels per 1.0

    // Dither source for blend_px: restarts from the frame number, so output
    // stays a pure function of the inputs.
    uint32_t dith = z.frame_ * 2654435761u + 12345u;
    int dither() { dith = dith * 1664525u + 1013904223u; return int(dith >> 16); }

    uint32_t rnd() { z.lcg_ = z.lcg_ * 1664525u + 1013904223u; return z.lcg_ >> 8; }
    float rnd01() { return float(rnd() & 0xFFFF) * (1.f / 65536.f); }

    // Where every mesh vertex reads the previous frame from.
    void build_mesh(const Params& P, const Ctx& C, const Preset& ps) {
        const float k = C.k;
        const float wt = C.time * P.warp_speed;
        const float ws = P.warp_scale;
        float last_rot = 0.f, cr = 1.f, sr = 0.f;
        const int n = (kGX + 1) * (kGY + 1);
        for (int i = 0; i < n; ++i) {
            Vtx v{z.vx_[i], z.vy_[i], z.vrad_[i], z.vang_[i],
                  P.zoom, P.rot, P.dx, P.dy, P.sx, P.sy, P.warp};
            if (ps.per_vertex) ps.per_vertex(v, P, C);

            float zm = v.zoom;
            if (P.zoomexp != 1.f) zm = std::pow(zm, std::pow(P.zoomexp, v.rad * 2.f - 1.f));
            zm = std::max(0.2f, 1.f + (zm - 1.f) * k);
            const float sxe = std::max(0.2f, 1.f + (v.sx - 1.f) * k);
            const float sye = std::max(0.2f, 1.f + (v.sy - 1.f) * k);

            float qx = (v.x - P.cx) / (zm * sxe);
            float qy = (v.y - P.cy) / (zm * sye);

            if (v.warp != 0.f) {
                const float wa = v.warp * k * 0.012f;
                qx += wa * (fsin(wt * 0.81f + ws * (v.y * 2.1f + v.x * 0.7f)) +
                            fsin(wt * 1.31f - ws * (v.y * 1.3f - v.x * 1.1f) + 1.7f));
                qy += wa * (fsin(wt * 0.67f + ws * (v.x * 1.9f - v.y * 0.8f) + 0.9f) +
                            fsin(wt * 1.13f + ws * (v.x * 1.2f + v.y * 1.5f) + 4.0f));
            }

            const float r = v.rot * k;
            if (r != last_rot) { last_rot = r; cr = std::cos(r); sr = std::sin(r); }
            const float rx = qx * cr - qy * sr + P.cx - v.dx * k;
            const float ry = qx * sr + qy * cr + P.cy - v.dy * k;

            const float su = px(rx), sv = py(ry);
            z.su_[i] = su == su ? clampf(su, -8000.f, 8000.f) : 0.f;
            z.sv_[i] = sv == sv ? clampf(sv, -8000.f, 8000.f) : 0.f;
        }
    }

    // prev -> cur through the mesh, bilinear, with the per-channel decay.
    // This is the only whole-image loop; everything in it is integer.
    void warp(float decay_r, float decay_g, float decay_b, bool wrap) {
        const uint32_t* src = z.prev_.data();
        const uint32_t dr = uint32_t(clampf(decay_r, 0.f, 1.f) * 256.f + 0.5f);
        const uint32_t dg = uint32_t(clampf(decay_g, 0.f, 1.f) * 256.f + 0.5f);
        const uint32_t db = uint32_t(clampf(decay_b, 0.f, 1.f) * 256.f + 0.5f);
        const unsigned Wm = unsigned(W - 1), Hm = unsigned(H - 1);
        int32_t rowu[kGX + 1], rowv[kGX + 1];

        for (int j = 0; j < kGY; ++j) {
            const int y0 = z.ys_[j], y1 = z.ys_[j + 1];
            if (y1 <= y0) continue;
            const float* u0 = &z.su_[j * (kGX + 1)];
            const float* u1 = u0 + (kGX + 1);
            const float* v0 = &z.sv_[j * (kGX + 1)];
            const float* v1 = v0 + (kGX + 1);
            const float inv = 1.f / float(y1 - y0);
            for (int y = y0; y < y1; ++y) {
                const float ty = (float(y - y0) + 0.5f) * inv;
                for (int i = 0; i <= kGX; ++i) {
                    // -0.5: pixel centres to sample indices.
                    rowu[i] = int32_t((u0[i] + (u1[i] - u0[i]) * ty - 0.5f) * 65536.f);
                    rowv[i] = int32_t((v0[i] + (v1[i] - v0[i]) * ty - 0.5f) * 65536.f);
                }
                uint32_t* dst = buf + y * W;
                for (int i = 0; i < kGX; ++i) {
                    const int x0 = z.xs_[i], x1 = z.xs_[i + 1];
                    const int n = x1 - x0;
                    if (n <= 0) continue;
                    const int32_t du = (rowu[i + 1] - rowu[i]) / n;
                    const int32_t dv = (rowv[i + 1] - rowv[i]) / n;
                    int32_t u = rowu[i] + du / 2;
                    int32_t v = rowv[i] + dv / 2;
                    for (int x = x0; x < x1; ++x, u += du, v += dv) {
                        const int ix = u >> 16, iy = v >> 16;
                        uint32_t fx = uint32_t(u >> 8) & 0xFFu;
                        uint32_t fy = uint32_t(v >> 8) & 0xFFu;
                        uint32_t a, b, c, d;
                        if (unsigned(ix) < Wm && unsigned(iy) < Hm) {
                            const uint32_t* p = src + iy * W + ix;
                            a = p[0]; b = p[1]; c = p[W]; d = p[W + 1];
                        } else {
                            fetch_edge(src, W, H, wrap, u, v, a, b, c, d, fx, fy);
                        }
                        if ((a | b | c | d) == 0) { dst[x] = 0; continue; }
                        // Four weights summing to exactly 256.
                        const uint32_t w11 = (fx * fy) >> 8;
                        const uint32_t w01 = fx - w11;
                        const uint32_t w10 = fy - w11;
                        const uint32_t w00 = 256u - fx - fy + w11;
                        const uint32_t rb = (a & 0xFF00FFu) * w00 + (b & 0xFF00FFu) * w01 +
                                            (c & 0xFF00FFu) * w10 + (d & 0xFF00FFu) * w11;
                        const uint32_t g = (a & 0xFF00u) * w00 + (b & 0xFF00u) * w01 +
                                           (c & 0xFF00u) * w10 + (d & 0xFF00u) * w11;
                        // Channels are 8.8 here; the decay takes them back to 8.
                        const uint32_t r8 = ((rb >> 16) * dr) >> 16;
                        const uint32_t g8 = (((g >> 8) & 0xFFFFu) * dg) >> 16;
                        const uint32_t b8 = ((rb & 0xFFFFu) * db) >> 16;
                        dst[x] = (r8 << 16) | (g8 << 8) | b8;
                    }
                }
            }
        }
    }

    void darken_centre(float cx, float cy, float amount, int R) {
        const int x0 = int(px(cx)), y0 = int(py(cy));
        const int str = int(clampf(amount, 0.f, 1.f) * 256.f);
        const int R2 = R * R;
        for (int y = std::max(0, y0 - R); y <= std::min(H - 1, y0 + R); ++y) {
            for (int x = std::max(0, x0 - R); x <= std::min(W - 1, x0 + R); ++x) {
                const int d2 = (x - x0) * (x - x0) + (y - y0) * (y - y0);
                if (d2 >= R2) continue;
                const uint32_t f = uint32_t(256 - str * (R2 - d2) / R2);
                const uint32_t c = buf[y * W + x];
                buf[y * W + x] = ((((c & 0xFF00FFu) * f) >> 8) & 0xFF00FFu) |
                                 ((((c & 0x00FF00u) * f) >> 8) & 0x00FF00u);
            }
        }
    }

    // Additive anti-aliased line, `wd` pixels wide, in pixel coordinates.
    // Covers pixel centres in [start, end) along its long axis, so chained
    // segments do not double up at the joints.
    void line(float xa, float ya, float xb, float yb, int r, int g, int b, float wd,
              int alpha = 65536, bool additive = true) {
        if (!(xa == xa) || !(xb == xb) || !(ya == ya) || !(yb == yb)) return;
        const float adx = std::fabs(xb - xa), ady = std::fabs(yb - ya);
        const bool steep = ady > adx;
        if (steep) { std::swap(xa, ya); std::swap(xb, yb); }
        if (xa > xb) { std::swap(xa, xb); std::swap(ya, yb); }
        const int majmax = steep ? H : W, minmax = steep ? W : H;
        const float len = xb - xa;
        if (!(len < 1e6f) || !(std::fabs(ya) < 1e6f) || !(std::fabs(yb) < 1e6f)) return;
        const float slope = len > 1e-4f ? (yb - ya) / len : 0.f;
        int m0 = int(std::ceil(xa - 0.5f)), m1 = int(std::ceil(xb - 0.5f));
        if (m1 == m0 && len > 0.f) return;       // shorter than a pixel, between centres
        if (m1 == m0) m1 = m0 + 1;               // a dot
        m0 = std::max(m0, 0);
        m1 = std::min(m1, majmax);
        if (m0 >= m1) return;
        const int half = int(wd * 32768.f);      // half width, 16.16
        const float cstart = ya + (float(m0) + 0.5f - xa) * slope;
        if (!(std::fabs(cstart) < 30000.f) || std::fabs(slope) > 1.001f) return;
        int32_t cfx = int32_t(cstart * 65536.f);
        const int32_t sfx = int32_t(slope * 65536.f);
        for (int m = m0; m < m1; ++m, cfx += sfx) {
            const int32_t lo = cfx - half, hi = cfx + half;
            int n0 = lo >> 16, n1 = (hi - 1) >> 16;
            if (n1 < 0 || n0 >= minmax) continue;
            for (int nn = std::max(n0, 0); nn <= std::min(n1, minmax - 1); ++nn) {
                const int32_t top = std::max(lo, nn << 16);
                const int32_t bot = std::min(hi, (nn + 1) << 16);
                const int cov = int((int64_t(bot - top) * alpha) >> 16);   // 0..65536
                if (cov <= 0) continue;
                uint32_t* p = steep ? buf + m * W + nn : buf + nn * W + m;
                blend_px(p, r, g, b, cov, additive, dither());
            }
        }
    }

    void dot(float x, float y, int r, int g, int b, int size, int alpha = 65536, bool additive = true) {
        if (!(std::fabs(x) < 1e6f) || !(std::fabs(y) < 1e6f)) return;
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        for (int yy = y0; yy < y0 + size; ++yy)
            for (int xx = x0; xx < x0 + size; ++xx)
                if (unsigned(xx) < unsigned(W) && unsigned(yy) < unsigned(H))
                    blend_px(buf + yy * W + xx, r, g, b, alpha, additive, dither());
    }

    void add_rect(int x0, int y0, int x1, int y1, int r, int g, int b) {
        x0 = std::max(x0, 0); y0 = std::max(y0, 0);
        x1 = std::min(x1, W); y1 = std::min(y1, H);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) add_px(buf + y * W + x, r, g, b);
    }

    void set_rect(int x0, int y0, int x1, int y1, uint32_t c) {
        x0 = std::max(x0, 0); y0 = std::max(y0, 0);
        x1 = std::min(x1, W); y1 = std::min(y1, H);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) buf[y * W + x] = c;
    }

    void border(const Params& P) {
        const int t = std::max(1, int(P.border * float(H) + 0.5f));
        int r, g, b;
        hue_rgb(P.border_hue, 1.f, clampf(P.border_bright, 0.f, 1.f), r, g, b);
        const uint32_t c = (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
        set_rect(0, 0, W, t, c);
        set_rect(0, H - t, W, H, c);
        set_rect(0, t, t, H - t, c);
        set_rect(W - t, t, W, H - t, c);
    }

    void shape(const Params& P, float wd) {
        const int n = std::min(P.shape_sides, 12);
        if (n < 3) return;
        float vx[12], vy[12];
        for (int i = 0; i < n; ++i) {
            const float a = P.shape_ang + kTau * float(i) / float(n);
            vx[i] = px(P.shape_x + P.shape_rad * std::cos(a));
            vy[i] = py(P.shape_y + P.shape_rad * std::sin(a));
        }
        int r, g, b;
        if (P.shape_fill > 0.f) {
            hue_rgb(P.shape_hue, 0.85f, P.shape_fill, r, g, b);
            float ymin = vy[0], ymax = vy[0];
            for (int i = 1; i < n; ++i) { ymin = std::min(ymin, vy[i]); ymax = std::max(ymax, vy[i]); }
            const int ya = std::max(0, int(std::ceil(ymin - 0.5f)));
            const int yb = std::min(H - 1, int(std::floor(ymax - 0.5f)));
            for (int y = ya; y <= yb; ++y) {
                const float yc = float(y) + 0.5f;
                float xl = 1e9f, xr = -1e9f;
                for (int i = 0; i < n; ++i) {
                    const int j = (i + 1) % n;
                    const float y0 = vy[i], y1 = vy[j];
                    if ((y0 <= yc) == (y1 <= yc)) continue;
                    const float x = vx[i] + (vx[j] - vx[i]) * (yc - y0) / (y1 - y0);
                    xl = std::min(xl, x);
                    xr = std::max(xr, x);
                }
                if (xr > xl) add_rect(int(xl + 0.5f), y, int(xr + 0.5f), y + 1, r, g, b);
            }
        }
        if (P.shape_edge > 0.f) {
            hue_rgb(P.shape_hue, 0.75f, P.shape_edge, r, g, b);
            for (int i = 0; i < n; ++i) {
                const int j = (i + 1) % n;
                line(vx[i], vy[i], vx[j], vy[j], r, g, b, wd);
            }
        }
    }

    void stars(const Params& P, const Ctx& C) {
        const float want = P.stars * C.k;
        int n = int(want);
        if (rnd01() < want - float(n)) ++n;
        const int size = H >= 96 ? 2 : 1;
        for (int i = 0; i < n; ++i) {
            const float a = rnd01() * kTau, d = P.star_spread * std::sqrt(rnd01());
            int r, g, b;
            hue_rgb(P.star_hue + P.star_hue_range * (rnd01() - 0.5f), 0.6f, 0.6f + 0.4f * rnd01(), r, g, b);
            dot(px(P.cx + d * fcos(a)), py(P.cy + d * fsin(a)), r, g, b, size);
        }
    }

    void wave(const Params& P, const AudioFrame& a, float wd) {
        constexpr int N = AudioFrame::kWave;
        float s[N];
        for (int i = 0; i < N; ++i) {            // light smoothing, sanitised
            const float p0 = a.wave[i > 0 ? i - 1 : 0], p1 = a.wave[i], p2 = a.wave[i < N - 1 ? i + 1 : N - 1];
            const float v = 0.25f * p0 + 0.5f * p1 + 0.25f * p2;
            s[i] = v == v ? clampf(v, -1.5f, 1.5f) : 0.f;
        }
        const float ca = std::cos(P.wave_ang), sa = std::sin(P.wave_ang);
        const float bright = clampf(P.wave_bright, 0.f, 1.f);
        int r, g, b;

        switch (P.wave_mode) {
        case kWaveLine:
        case kWaveDual: {
            const int lines = P.wave_mode == kWaveDual ? 2 : 1;
            const float half = P.wave_size * A;
            for (int l = 0; l < lines; ++l) {
                const float off = lines == 2 ? (l ? 0.5f : -0.5f) * P.wave_sep : 0.f;
                float lx = 0.f, ly = 0.f;
                for (int i = 0; i < N; ++i) {
                    const float t = float(i) / float(N - 1) * 2.f - 1.f;
                    const float v = (l ? -s[N - 1 - i] : s[i]) * P.wave_scale + off;
                    const float x = px(P.wave_x + ca * t * half - sa * v);
                    const float y = py(P.wave_y + sa * t * half + ca * v);
                    if (i) {
                        hue_rgb(P.wave_hue + P.wave_spread * (t * 0.5f + (l ? 0.5f : 0.f)),
                                P.wave_sat, bright, r, g, b);
                        line(lx, ly, x, y, r, g, b, wd);
                    }
                    lx = x; ly = y;
                }
            }
            break;
        }
        case kWaveCircle: {
            float lx = 0.f, ly = 0.f, fx0 = 0.f, fy0 = 0.f;
            constexpr int kBlend = 16;           // cross-fade the tail into the head
            for (int i = 0; i <= N; ++i) {
                const int k = i % N;
                float v = s[k];
                if (k >= N - kBlend) {
                    const float m = float(N - k) / float(kBlend);
                    v = v * m + s[0] * (1.f - m);
                }
                const float ang = P.wave_ang + kTau * float(k) / float(N);
                const float rad = P.wave_size + v * P.wave_scale;
                float x = px(P.wave_x + rad * fcos(ang));
                float y = py(P.wave_y + rad * fsin(ang));
                if (i == 0) { fx0 = x; fy0 = y; }
                if (i == N) { x = fx0; y = fy0; }
                if (i) {
                    const float h = std::fabs(float(k) / float(N) * 2.f - 1.f);   // seamless
                    hue_rgb(P.wave_hue + P.wave_spread * h, P.wave_sat, bright, r, g, b);
                    line(lx, ly, x, y, r, g, b, wd);
                }
                lx = x; ly = y;
            }
            break;
        }
        case kWaveDots: {
            const int size = H >= 96 ? 2 : 1;
            for (int i = 0; i < N; ++i) {
                const float vx = s[i], vy = s[(i + 37) % N];
                hue_rgb(P.wave_hue + P.wave_spread * float(i) / float(N), P.wave_sat, bright, r, g, b);
                dot(px(P.wave_x + (ca * vx - sa * vy) * P.wave_scale * 1.6f),
                    py(P.wave_y + (sa * vx + ca * vy) * P.wave_scale), r, g, b, size);
            }
            break;
        }
        case kWaveBars: {
            constexpr int K = AudioFrame::kSpec;
            const float x0 = px(P.wave_x - P.wave_size * A), x1 = px(P.wave_x + P.wave_size * A);
            const float bw = (x1 - x0) / float(K);
            const int gap = bw >= 6.f ? 2 : (bw >= 3.f ? 1 : 0);
            const int base = int(py(P.wave_y));
            for (int k = 0; k < K; ++k) {
                const float lv = a.spec[k] == a.spec[k] ? clampf(a.spec[k], 0.f, 1.f) : 0.f;
                const int top = base - int(lv * P.wave_scale * unit());
                const int xa = int(x0 + bw * float(k)), xb = int(x0 + bw * float(k + 1)) - gap;
                hue_rgb(P.wave_hue + P.wave_spread * float(k) / float(K), P.wave_sat, bright, r, g, b);
                add_rect(xa, top, xb, base, r / 4, g / 4, b / 4);
                add_rect(xa, top - std::max(1, H / 60), xb, top, r, g, b);
            }
            break;
        }
        case kWaveSpikes: {
            constexpr int K = 40;
            for (int k = 0; k < K; ++k) {
                const float lv = std::fabs(s[k * N / K]) + 0.5f * clampf(a.spec[k % AudioFrame::kSpec], 0.f, 1.f);
                const float ang = P.wave_ang + kTau * float(k) / float(K);
                const float c = fcos(ang), sn = fsin(ang);
                const float r0 = P.wave_size, r1 = P.wave_size + (0.04f + lv) * P.wave_scale;
                hue_rgb(P.wave_hue + P.wave_spread * lv, P.wave_sat, bright, r, g, b);
                line(px(P.wave_x + r0 * c), py(P.wave_y + r0 * sn),
                     px(P.wave_x + r1 * c), py(P.wave_y + r1 * sn), r, g, b, wd);
            }
            break;
        }
        default:
            break;
        }
    }

    // ── Extras used by MilkDrop presets ─────────────────────────────────────

    void blend_rect(int x0, int y0, int x1, int y1, int r, int g, int b, int a) {
        x0 = std::max(x0, 0); y0 = std::max(y0, 0);
        x1 = std::min(x1, W); y1 = std::min(y1, H);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) blend_px(buf + y * W + x, r, g, b, a, false, dither());
    }

    // A frame `t0`..`t1` pixels in from each edge (x and y thicknesses given
    // separately), alpha-blended.
    void frame_rect(int ax0, int ay0, int ax1, int ay1, int r, int g, int b, int a) {
        if (a <= 0) return;
        ax1 = std::min(ax1, W / 2); ay1 = std::min(ay1, H / 2);
        if (ax0 >= ax1 && ay0 >= ay1) return;
        blend_rect(ax0, ay0, W - ax0, ay1, r, g, b, a);                 // top
        blend_rect(ax0, H - ay1, W - ax0, H - ay0, r, g, b, a);         // bottom
        blend_rect(ax0, ay1, ax1, H - ay1, r, g, b, a);                 // left
        blend_rect(W - ax1, ay1, W - ax0, H - ay1, r, g, b, a);         // right
    }

    struct TV { float x, y, r, g, b, a, u, v; };   // pixels; colour 0..255; alpha 0..256 (fractions count); u,v: source pixels

    // Gouraud triangle, optionally multiplied by the previous frame (`tex`).
    // Pixel centres on a shared edge go to exactly one of the two triangles.
    void tri(const TV& p0, const TV& p1, const TV& p2, bool additive, const uint32_t* tex) {
        const TV* p[3] = {&p0, &p1, &p2};
        for (const TV* q : p)
            if (!(std::fabs(q->x) < 1e6f) || !(std::fabs(q->y) < 1e6f)) return;
        const float area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
        if (!(std::fabs(area) > 1e-3f)) return;
        const float inv = 1.f / area;
        const float fa[6] = {p0.r, p0.g, p0.b, p0.a, p0.u, p0.v};
        const float fb[6] = {p1.r, p1.g, p1.b, p1.a, p1.u, p1.v};
        const float fc[6] = {p2.r, p2.g, p2.b, p2.a, p2.u, p2.v};
        float gx[6], gy[6];
        for (int k = 0; k < 6; ++k) {
            gx[k] = ((fb[k] - fa[k]) * (p2.y - p0.y) - (fc[k] - fa[k]) * (p1.y - p0.y)) * inv;
            gy[k] = ((fc[k] - fa[k]) * (p1.x - p0.x) - (fb[k] - fa[k]) * (p2.x - p0.x)) * inv;
        }
        const float ymin = std::min(p0.y, std::min(p1.y, p2.y)), ymax = std::max(p0.y, std::max(p1.y, p2.y));
        const int ya = int(std::ceil(std::max(ymin, -1.f) - 0.5f));
        const int yb = int(std::ceil(std::min(ymax, float(H) + 1.f) - 0.5f));
        const int32_t cmax[4] = {255 << 16, 255 << 16, 255 << 16, 256 << 16};
        for (int y = std::max(ya, 0); y < std::min(yb, H); ++y) {
            const float yc = float(y) + 0.5f;
            float xl = 1e9f, xr = -1e9f;
            for (int e = 0; e < 3; ++e) {
                const TV& a = *p[e];
                const TV& b = *p[(e + 1) % 3];
                if ((a.y <= yc) == (b.y <= yc)) continue;
                const float x = a.x + (b.x - a.x) * (yc - a.y) / (b.y - a.y);
                xl = std::min(xl, x);
                xr = std::max(xr, x);
            }
            if (!(xr > xl)) continue;
            const int x0 = std::max(0, int(std::ceil(std::max(xl, -1.f) - 0.5f)));
            const int x1 = std::min(W, int(std::ceil(std::min(xr, float(W) + 1.f) - 0.5f)));
            if (x0 >= x1) continue;
            const float ddx = float(x0) + 0.5f - p0.x, ddy = yc - p0.y;
            int32_t c[4], st[4];
            for (int k = 0; k < 4; ++k) {
                const float hi = k == 3 ? 256.f : 255.f;
                c[k] = int32_t(clampf(fa[k] + gx[k] * ddx + gy[k] * ddy, 0.f, hi) * 65536.f);
                st[k] = int32_t(clampf(gx[k], -256.f, 256.f) * 65536.f);
            }
            float u = fa[4] + gx[4] * ddx + gy[4] * ddy, v = fa[5] + gx[5] * ddx + gy[5] * ddy;
            uint32_t* dst = buf + y * W;
            for (int x = x0; x < x1; ++x) {
                int r = c[0] >> 16, g = c[1] >> 16, b = c[2] >> 16;
                const int a = c[3] >> 8;         // 0..65536
                if (tex) {
                    int tu = int(std::floor(clampf(u, -1e6f, 1e6f))) % W, tv = int(std::floor(clampf(v, -1e6f, 1e6f))) % H;
                    if (tu < 0) tu += W;
                    if (tv < 0) tv += H;
                    const uint32_t t = tex[tv * W + tu];
                    r = (r * int((t >> 16) & 0xFF)) >> 8;
                    g = (g * int((t >> 8) & 0xFF)) >> 8;
                    b = (b * int(t & 0xFF)) >> 8;
                    u += gx[4];
                    v += gx[5];
                }
                if (a > 0) blend_px(dst + x, r, g, b, a, additive, dither());
                for (int k = 0; k < 4; ++k) {
                    c[k] += st[k];
                    if (c[k] < 0) c[k] = 0;
                    else if (c[k] > cmax[k]) c[k] = cmax[k];
                }
            }
        }
    }

    // cur -> disp: video echo (a second, zoomed / flipped copy blended in),
    // then gamma and the colour filters through one 256-entry table.
    void present(float gamma, float echo_alpha, float echo_zoom, int orient,
                 bool brighten, bool darken, bool solarize, bool invert) {
        uint8_t lut[256];
        for (int i = 0; i < 256; ++i) {
            float v = std::min(1.f, float(i) * (1.f / 255.f) * gamma);
            if (brighten) v = v * (2.f - v);
            if (darken) v = v * v;
            if (solarize) v = 2.f * v * (1.f - v);
            if (invert) v = 1.f - v;
            lut[i] = uint8_t(clampf(v, 0.f, 1.f) * 255.f + 0.5f);
        }
        uint32_t* out = z.disp_.data();
        const int n = W * H;
        const int ea = int(clampf(echo_alpha, 0.f, 1.f) * 256.f);
        if (ea <= 0) {
            for (int i = 0; i < n; ++i) {
                const uint32_t c = buf[i];
                out[i] = (uint32_t(lut[(c >> 16) & 0xFF]) << 16) | (uint32_t(lut[(c >> 8) & 0xFF]) << 8) | lut[c & 0xFF];
            }
            return;
        }
        const float iz = 1.f / clampf(echo_zoom, 0.25f, 100.f);
        const float fxs = (orient & 1) ? -iz : iz, fys = (orient & 2) ? -iz : iz;
        const int32_t step = int32_t(fxs * 65536.f);
        const uint32_t na = uint32_t(256 - ea), ua = uint32_t(ea);
        for (int y = 0; y < H; ++y) {
            int sy = int(std::floor(float(H) * 0.5f + (float(y) + 0.5f - float(H) * 0.5f) * fys));
            sy = std::max(0, std::min(H - 1, sy));
            const uint32_t* erow = buf + sy * W;
            const uint32_t* brow = buf + y * W;
            int32_t sx = int32_t((float(W) * 0.5f + (0.5f - float(W) * 0.5f) * fxs) * 65536.f);
            uint32_t* o = out + y * W;
            for (int x = 0; x < W; ++x, sx += step) {
                int ix = sx >> 16;
                if (unsigned(ix) >= unsigned(W)) ix = ix < 0 ? 0 : W - 1;
                const uint32_t c = brow[x], e = erow[ix];
                const uint32_t rb = (((c & 0xFF00FFu) * na + (e & 0xFF00FFu) * ua) >> 8) & 0xFF00FFu;
                const uint32_t g = (((c & 0x00FF00u) * na + (e & 0x00FF00u) * ua) >> 8) & 0x00FF00u;
                o[x] = (uint32_t(lut[rb >> 16]) << 16) | (uint32_t(lut[g >> 8]) << 8) | lut[rb & 0xFF];
            }
        }
    }

    void mirror(int mode) {
        if (mode == kMirrorH || mode == kMirrorQuad) {
            for (int y = 0; y < H; ++y) {
                uint32_t* row = buf + y * W;
                for (int x = 0; x < W / 2; ++x) row[W - 1 - x] = row[x];
            }
        }
        if (mode == kMirrorV || mode == kMirrorQuad) {
            for (int y = 0; y < H / 2; ++y)
                std::memcpy(buf + (H - 1 - y) * W, buf + y * W, size_t(W) * sizeof(uint32_t));
        }
    }
};

Visualizer::Visualizer(int w, int h)
    : w_(std::max(8, std::min(w, 4096))), h_(std::max(8, std::min(h, 4096))) {
    cur_.assign(size_t(w_) * size_t(h_), 0u);
    prev_.assign(size_t(w_) * size_t(h_), 0u);
    out_ = cur_.data();
    const float A = float(w_) / float(h_);
    const float corner = std::sqrt(A * A + 1.f);
    for (int i = 0; i <= kGX; ++i) xs_[i] = int((long long)i * w_ / kGX);
    for (int j = 0; j <= kGY; ++j) ys_[j] = int((long long)j * h_ / kGY);
    for (int j = 0; j <= kGY; ++j) {
        for (int i = 0; i <= kGX; ++i) {
            const int n = j * (kGX + 1) + i;
            const float x = (float(xs_[i]) / float(w_) * 2.f - 1.f) * A;
            const float y = float(ys_[j]) / float(h_) * 2.f - 1.f;
            vx_[n] = x;
            vy_[n] = y;
            vrad_[n] = std::sqrt(x * x + y * y) / corner;
            vang_[n] = std::atan2(y, x);
            su_[n] = float(xs_[i]);
            sv_[n] = float(ys_[j]);
        }
    }
}

int Visualizer::preset_count() { return kPresetCount; }

const char* Visualizer::preset_name(int i) {
    return i >= 0 && i < kPresetCount ? kPresets[i].name : "";
}

void Visualizer::set_preset(int i) {
    i %= kPresetCount;
    if (i < 0) i += kPresetCount;
    preset_ = i;
    milk_.reset();
}

void Visualizer::clear() {
    std::fill(cur_.begin(), cur_.end(), 0u);
    std::fill(prev_.begin(), prev_.end(), 0u);
    std::fill(disp_.begin(), disp_.end(), 0u);
    lcg_ = 0x1999u;
    beat_ = 0.f;
    beat_hold_ = 0.f;
}

void Visualizer::render(const AudioFrame& a) {
    float dt = started_ ? a.time - last_time_ : 1.f / 30.f;
    if (!(dt > 0.f)) dt = 1.f / 30.f;            // also catches NaN and rewinds
    dt = std::min(dt, 0.1f);
    started_ = true;
    last_time_ = a.time;

    bass_ = a.bass == a.bass ? clampf(a.bass, 0.f, 4.f) : 0.f;
    mid_ = a.mid == a.mid ? clampf(a.mid, 0.f, 4.f) : 0.f;
    treb_ = a.treb == a.treb ? clampf(a.treb, 0.f, 4.f) : 0.f;

    // Beat: bass well above its own recent level, at most ~5 a second.
    beat_ *= std::exp(-dt * 7.f);
    beat_hold_ -= dt;
    if (beat_hold_ <= 0.f && bass_ > 1.25f && bass_ > bass_att_ * 1.2f) {
        beat_ = 1.f;
        beat_hold_ = 0.2f;
        ++beats_;
    }
    const float sm = 1.f - std::exp(-dt * 6.f);
    bass_att_ += (bass_ - bass_att_) * sm;
    mid_att_ += (mid_ - mid_att_) * sm;
    treb_att_ += (treb_ - treb_att_) * sm;

    if (milk_) render_milk(a, dt);
    else render_builtin(a, dt);
    ++frame_;
}

void Visualizer::render_builtin(const AudioFrame& a, float dt) {
    Ctx C;
    C.time = a.time;
    C.k = clampf(dt * 30.f, 0.25f, 3.f);         // this frame, in 30 fps frames
    C.bass = bass_; C.mid = mid_; C.treb = treb_;
    C.bass_att = bass_att_; C.mid_att = mid_att_; C.treb_att = treb_att_;
    C.beat = beat_;
    C.beats = beats_;
    C.frame = frame_;
    C.aspect = float(w_) / float(h_);

    const Preset& ps = kPresets[preset_];
    Params P;
    ps.init(P);
    ps.per_frame(P, C);
    P.zoom += P.beat_zoom * beat_;

    cur_.swap(prev_);
    Impl d(*this);
    d.build_mesh(P, C, ps);
    const float dk = std::pow(clampf(P.decay, 0.f, 1.f), C.k);
    d.warp(dk * P.tint_r, dk * P.tint_g, dk * P.tint_b, P.wrap);

    const float wd = std::max(1.f, P.wave_width * float(h_) * 0.01f);
    if (P.darken > 0.f) d.darken_centre(P.cx, P.cy, P.darken * std::min(1.f, C.k), std::max(2, h_ / 7));
    if (P.border > 0.f) d.border(P);
    if (P.shape_sides >= 3) d.shape(P, wd);
    if (P.stars > 0.f) d.stars(P, C);
    d.wave(P, a, wd);
    d.mirror(P.mirror);
    out_ = cur_.data();
}

// ── MilkDrop 1 presets ──────────────────────────────────────────────────────
// The numbers and equations come from a .milk file (ui/milk.h); this is the
// part of MilkDrop that turns them into a picture: the per-vertex warp
// formula, the eight wave modes, borders, motion vectors, custom shapes and
// waves, and the echo / gamma stage in front of the display.

namespace {

// Inputs every MilkDrop program can read.
struct Common {
    static constexpr int kN = 14;
    int idx[kN];
    void bind(milk::Vars& v) {
        static const char* const names[kN] = {"time", "fps", "frame", "progress", "bass", "mid", "treb",
                                              "bass_att", "mid_att", "treb_att", "meshx", "meshy",
                                              "aspectx", "aspecty"};
        for (int i = 0; i < kN; ++i) idx[i] = v.index(names[i]);
    }
    void set(milk::Vars& v, const double* vals) const {
        for (int i = 0; i < kN; ++i) v[idx[i]] = vals[i];
    }
};

void bind_bank(milk::Vars& v, char prefix, int* idx) {       // q1..q8 or t1..t8
    for (int i = 0; i < 8; ++i) idx[i] = v.index(std::string(1, prefix) + char('1' + i));
}

inline float num(double v, float lo, float hi, float fallback) {
    if (!(v == v)) return fallback;
    return v < double(lo) ? lo : (v > double(hi) ? hi : float(v));
}

struct Def { const char* var; const char* key; double def; };

}  // namespace

struct Visualizer::Milk {
    // Per-frame variables that start each frame at the preset's value.
    enum F {
        kZoom, kZoomExp, kRot, kWarp, kCx, kCy, kDx, kDy, kSx, kSy,      // also per vertex
        kDecay, kWaveR, kWaveG, kWaveB, kWaveA, kWaveX, kWaveY, kWaveMystery, kWaveMode,
        kWaveDots, kWaveThick, kWaveAdditive, kWaveBrighten,
        kObSize, kObR, kObG, kObB, kObA, kIbSize, kIbR, kIbG, kIbB, kIbA,
        kMvX, kMvY, kMvDx, kMvDy, kMvL, kMvR, kMvG, kMvB, kMvA,
        kEchoZoom, kEchoAlpha, kEchoOrient, kGamma, kDarkenCenter, kWrap,
        kInvert, kBrighten, kDarken, kSolarize, kFCount
    };
    static constexpr int kVertexVars = 10;

    struct Shape {
        enum { kX, kY, kRad, kAng, kTexAng, kTexZoom, kR, kG, kB, kA, kR2, kG2, kB2, kA2,
               kBorR, kBorG, kBorB, kBorA, kSides, kAdditive, kThick, kTextured, kN };
        bool on = false;
        milk::Vars v;
        milk::Program init, frame;
        Common c;
        int q[8], t[8], i[kN];
        double base[kN], t_init[8];
    };
    struct Wave {
        static constexpr int kMaxPoints = 512;        // MilkDrop's own limit
        static constexpr size_t kStepBudget = 100000; // per_point steps per wave per frame
        bool on = false, spectrum = false, dots = false, thick = false, additive = false;
        int samples = 128, sep = 0;
        float scaling = 1.f, smoothing = 0.5f;
        milk::Vars v;
        milk::Program init, frame, point;
        Common c;
        int q[8], t[8], rgba[4], sample, value1, value2, x, y;
        double base[4], t_init[8];
    };

    std::string name;
    milk::Vars fv, pv;                  // per-frame and per-vertex variable pools
    milk::Program init, frame, pixel;
    int fi[kFCount];
    double base[kFCount];
    Common fc, pc;
    int fq[8], pq[8];
    int px, py, prad, pang, pvert[kVertexVars];
    double q_init[8] = {};
    float warp_speed = 1.f, warp_scale = 1.f, wave_scale = 1.f, wave_smooth = 0.75f;
    float mod_start = 0.75f, mod_end = 0.95f;
    bool mod_alpha = false;
    bool first = true;
    float t0 = 0.f;
    uint32_t rng = 0x4D494C4Bu;
    float ax = 1.f, ay = 1.f;           // MilkDrop's aspect factors (1, 1 = stretch to fit)
    Shape shapes[4];
    Wave waves[4];

    static const Def kF[kFCount];

    void setup(const milk::Preset& p);
    void set_aspect(int w, int h, bool stretch) {
        ax = ay = 1.f;
        if (!stretch) {
            if (w > h) ay = float(h) / float(w);
            else ax = float(w) / float(h);
        }
    }
};

const Def Visualizer::Milk::kF[Visualizer::Milk::kFCount] = {
    {"zoom", "zoom", 1}, {"zoomexp", "fzoomexponent", 1}, {"rot", "rot", 0}, {"warp", "warp", 1},
    {"cx", "cx", 0.5}, {"cy", "cy", 0.5}, {"dx", "dx", 0}, {"dy", "dy", 0}, {"sx", "sx", 1}, {"sy", "sy", 1},
    {"decay", "fdecay", 0.98}, {"wave_r", "wave_r", 1}, {"wave_g", "wave_g", 1}, {"wave_b", "wave_b", 1},
    {"wave_a", "fwavealpha", 0.8}, {"wave_x", "wave_x", 0.5}, {"wave_y", "wave_y", 0.5},
    {"wave_mystery", "fwaveparam", 0}, {"wave_mode", "nwavemode", 0},
    {"wave_usedots", "bwavedots", 0}, {"wave_thick", "bwavethick", 0},
    {"wave_additive", "badditivewaves", 0}, {"wave_brighten", "bmaximizewavecolor", 1},
    {"ob_size", "ob_size", 0.01}, {"ob_r", "ob_r", 0}, {"ob_g", "ob_g", 0}, {"ob_b", "ob_b", 0}, {"ob_a", "ob_a", 0},
    {"ib_size", "ib_size", 0.01}, {"ib_r", "ib_r", 0.25}, {"ib_g", "ib_g", 0.25}, {"ib_b", "ib_b", 0.25}, {"ib_a", "ib_a", 0},
    {"mv_x", "nmotionvectorsx", 12}, {"mv_y", "nmotionvectorsy", 9}, {"mv_dx", "mv_dx", 0}, {"mv_dy", "mv_dy", 0},
    {"mv_l", "mv_l", 0.9}, {"mv_r", "mv_r", 1}, {"mv_g", "mv_g", 1}, {"mv_b", "mv_b", 1}, {"mv_a", "mv_a", 0},
    {"echo_zoom", "fvideoechozoom", 2}, {"echo_alpha", "fvideoechoalpha", 0}, {"echo_orient", "nvideoechoorientation", 0},
    {"gamma", "fgammaadj", 2}, {"darken_center", "bdarkencenter", 0}, {"wrap", "btexwrap", 1},
    {"invert", "binvert", 0}, {"brighten", "bbrighten", 0}, {"darken", "bdarken", 0}, {"solarize", "bsolarize", 0},
};

void Visualizer::Milk::setup(const milk::Preset& p) {
    name = p.name;
    for (int i = 0; i < kFCount; ++i) {
        fi[i] = fv.index(kF[i].var);
        base[i] = p.get(kF[i].key, kF[i].def);
        if (!std::isfinite(base[i])) base[i] = kF[i].def;
    }
    // Very old presets switch motion vectors with a flag instead of mv_a.
    if (p.params.find("mv_a") == p.params.end()) base[kMvA] = p.get("bmotionvectorson", 0) != 0 ? 1.0 : 0.0;
    warp_speed = num(p.get("fwarpanimspeed", 1), -100.f, 100.f, 1.f);
    warp_scale = num(p.get("fwarpscale", 1), -1000.f, 1000.f, 1.f);
    if (std::fabs(warp_scale) < 1e-3f) warp_scale = 1.f;
    wave_scale = num(p.get("fwavescale", 1), -100.f, 100.f, 1.f);
    wave_smooth = num(p.get("fwavesmoothing", 0.75), 0.f, 0.95f, 0.75f);
    mod_alpha = p.get("bmodwavealphabyvolume", 0) != 0;
    mod_start = num(p.get("fmodwavealphastart", 0.75), -10.f, 10.f, 0.75f);
    mod_end = num(p.get("fmodwavealphaend", 0.95), -10.f, 10.f, 0.95f);

    fc.bind(fv);
    bind_bank(fv, 'q', fq);
    init.compile(p.init, fv);
    frame.compile(p.per_frame, fv);

    pc.bind(pv);
    bind_bank(pv, 'q', pq);
    px = pv.index("x"); py = pv.index("y"); prad = pv.index("rad"); pang = pv.index("ang");
    for (int i = 0; i < kVertexVars; ++i) pvert[i] = pv.index(kF[i].var);
    pixel.compile(p.per_pixel, pv);

    static const Def kShape[Shape::kN] = {
        {"x", "x", 0.5}, {"y", "y", 0.5}, {"rad", "rad", 0.1}, {"ang", "ang", 0},
        {"tex_ang", "tex_ang", 0}, {"tex_zoom", "tex_zoom", 1},
        {"r", "r", 1}, {"g", "g", 0}, {"b", "b", 0}, {"a", "a", 1},
        {"r2", "r2", 0}, {"g2", "g2", 1}, {"b2", "b2", 0}, {"a2", "a2", 0},
        {"border_r", "border_r", 1}, {"border_g", "border_g", 1}, {"border_b", "border_b", 1}, {"border_a", "border_a", 0.1},
        {"sides", "sides", 4}, {"additive", "additive", 0}, {"thick", "thickoutline", 0}, {"textured", "textured", 0},
    };
    for (int s = 0; s < 4; ++s) {
        const milk::CustomCode& cc = p.shapes[s];
        Shape& sh = shapes[s];
        sh.on = cc.enabled;
        if (!sh.on) continue;
        for (int i = 0; i < Shape::kN; ++i) {
            sh.i[i] = sh.v.index(kShape[i].var);
            const auto it = cc.params.find(kShape[i].key);
            sh.base[i] = it != cc.params.end() && std::isfinite(it->second) ? it->second : kShape[i].def;
        }
        sh.c.bind(sh.v);
        bind_bank(sh.v, 'q', sh.q);
        bind_bank(sh.v, 't', sh.t);
        sh.init.compile(cc.init, sh.v);
        sh.frame.compile(cc.per_frame, sh.v);
    }
    static const char* const kRgba[4] = {"r", "g", "b", "a"};
    for (int s = 0; s < 4; ++s) {
        const milk::CustomCode& cc = p.waves[s];
        Wave& wv = waves[s];
        wv.on = cc.enabled;
        if (!wv.on) continue;
        auto get = [&](const char* k, double def) {
            const auto it = cc.params.find(k);
            return it != cc.params.end() && std::isfinite(it->second) ? it->second : def;
        };
        wv.samples = int(num(get("samples", 512), 2.f, 512.f, 512.f));
        wv.sep = int(num(get("sep", 0), 0.f, 127.f, 0.f));
        wv.spectrum = get("bspectrum", 0) != 0;
        wv.dots = get("busedots", 0) != 0;
        wv.thick = get("bdrawthick", 0) != 0;
        wv.additive = get("badditive", 0) != 0;
        wv.scaling = num(get("scaling", 1), -100.f, 100.f, 1.f);
        wv.smoothing = num(get("smoothing", 0.5), 0.f, 0.95f, 0.5f);
        for (int i = 0; i < 4; ++i) {
            wv.rgba[i] = wv.v.index(kRgba[i]);
            wv.base[i] = get(kRgba[i], 1);
        }
        wv.sample = wv.v.index("sample");
        wv.value1 = wv.v.index("value1");
        wv.value2 = wv.v.index("value2");
        wv.x = wv.v.index("x");
        wv.y = wv.v.index("y");
        wv.c.bind(wv.v);
        bind_bank(wv.v, 'q', wv.q);
        bind_bank(wv.v, 't', wv.t);
        wv.init.compile(cc.init, wv.v);
        wv.frame.compile(cc.per_frame, wv.v);
        wv.point.compile(cc.per_point, wv.v);
    }
}

Visualizer::~Visualizer() = default;

bool Visualizer::load_milk(const mm::milk::Preset& p) {
    if (p.uses_shaders) return false;
    auto m = std::make_unique<Milk>();
    m->setup(p);
    m->set_aspect(w_, h_, true);
    milk_ = std::move(m);
    if (disp_.empty()) disp_.assign(size_t(w_) * size_t(h_), 0u);
    return true;
}

bool Visualizer::load_milk(const char* path) {
    milk::Preset p;
    if (!path || !milk::load_preset(path, p)) return false;
    return load_milk(p);
}

const char* Visualizer::current_name() const {
    return milk_ ? milk_->name.c_str() : kPresets[preset_].name;
}

void Visualizer::render_milk(const AudioFrame& a, float dt) {
    Milk& M = *milk_;
    const int W = w_, H = h_;
    if (M.first) M.t0 = a.time;

    const double cv[Common::kN] = {
        double(a.time), double(clampf(1.f / dt, 1.f, 120.f)), double(frame_),
        double(clampf((a.time - M.t0) / 30.f, 0.f, 1.f)),
        double(bass_), double(mid_), double(treb_), double(bass_att_), double(mid_att_), double(treb_att_),
        double(kGX), double(kGY), double(M.ax), double(M.ay)};

    // Per frame: variables back to the preset's values, q1..q8 back to what
    // the init code left, then the preset's own equations.
    for (int i = 0; i < Milk::kFCount; ++i) M.fv[M.fi[i]] = M.base[i];
    M.fc.set(M.fv, cv);
    if (M.first) {
        M.init.run(M.fv, M.rng);
        for (int i = 0; i < 8; ++i) M.q_init[i] = M.fv[M.fq[i]];
        for (int i = 0; i < Milk::kFCount; ++i) M.fv[M.fi[i]] = M.base[i];
    }
    for (int i = 0; i < 8; ++i) M.fv[M.fq[i]] = M.q_init[i];
    M.frame.run(M.fv, M.rng);
    double q[8];
    for (int i = 0; i < 8; ++i) q[i] = M.fv[M.fq[i]];
    auto F = [&](int i, float lo, float hi) { return num(M.fv[M.fi[i]], lo, hi, float(M.base[i])); };

    // Per vertex: the per_pixel equations, then MilkDrop's warp formula.
    {
        M.pc.set(M.pv, cv);
        for (int i = 0; i < 8; ++i) M.pv[M.pq[i]] = q[i];
        double vert[Milk::kVertexVars];
        for (int i = 0; i < Milk::kVertexVars; ++i) vert[i] = M.fv[M.fi[i]];
        const bool per_vertex = !M.pixel.empty();
        const float wt = a.time * M.warp_speed, wsi = 1.f / M.warp_scale;
        const float f0 = 11.68f + 4.0f * fcos(wt * 1.413f + 10.f);
        const float f1 = 8.77f + 3.0f * fcos(wt * 1.113f + 7.f);
        const float f2 = 10.54f + 3.0f * fcos(wt * 1.233f + 3.f);
        const float f3 = 11.49f + 4.0f * fcos(wt * 0.933f + 5.f);
        const float ax = M.ax, ay = M.ay;
        float last_rot = 0.f, cr = 1.f, sr = 0.f;
        double out[Milk::kVertexVars];
        for (int i = 0; i < Milk::kVertexVars; ++i) out[i] = vert[i];
        for (int j = 0, n = 0; j <= kGY; ++j) {
            for (int i = 0; i <= kGX; ++i, ++n) {
                const float x = float(xs_[i]) / float(W) * 2.f - 1.f;       // -1..1, y up
                const float y = 1.f - float(ys_[j]) / float(H) * 2.f;
                if (per_vertex) {
                    float ang = std::atan2(y * ay, x * ax);
                    if (ang < 0.f) ang += kTau;
                    M.pv[M.px] = double(x * 0.5f * ax + 0.5f);
                    M.pv[M.py] = double(-y * 0.5f * ay + 0.5f);
                    M.pv[M.prad] = double(std::sqrt(x * x * ax * ax + y * y * ay * ay) * 0.70710678f);
                    M.pv[M.pang] = double(ang);
                    for (int k = 0; k < Milk::kVertexVars; ++k) M.pv[M.pvert[k]] = vert[k];
                    M.pixel.run(M.pv, M.rng);
                    for (int k = 0; k < Milk::kVertexVars; ++k) out[k] = M.pv[M.pvert[k]];
                }
                const float zoom = num(out[Milk::kZoom], -1e4f, 1e4f, 1.f);
                const float zexp = num(out[Milk::kZoomExp], -1e4f, 1e4f, 1.f);
                const float rot = num(out[Milk::kRot], -1e4f, 1e4f, 0.f);
                const float warp = num(out[Milk::kWarp], -1e4f, 1e4f, 0.f);
                const float cx = num(out[Milk::kCx], -1e4f, 1e4f, 0.5f), cy = num(out[Milk::kCy], -1e4f, 1e4f, 0.5f);
                const float dx = num(out[Milk::kDx], -1e4f, 1e4f, 0.f), dy = num(out[Milk::kDy], -1e4f, 1e4f, 0.f);
                float sx = num(out[Milk::kSx], -1e4f, 1e4f, 1.f), sy = num(out[Milk::kSy], -1e4f, 1e4f, 1.f);
                if (std::fabs(sx) < 1e-4f) sx = 1e-4f;
                if (std::fabs(sy) < 1e-4f) sy = 1e-4f;

                float zoom2 = zoom;
                if (zexp != 1.f) {
                    const float rad = std::sqrt(x * x * ax * ax + y * y * ay * ay) * 0.70710678f;
                    zoom2 = std::pow(zoom, std::pow(zexp, rad * 2.f - 1.f));
                }
                if (!(std::fabs(zoom2) > 1e-4f)) zoom2 = 1e-4f;      // also NaN
                const float zi = 1.f / zoom2;
                float u = x * ax * 0.5f * zi + 0.5f;
                float v = -y * ay * 0.5f * zi + 0.5f;
                u = (u - cx) / sx + cx;
                v = (v - cy) / sy + cy;
                if (warp != 0.f) {
                    const float w = warp * 0.0035f;
                    u += w * fsin(wt * 0.333f + wsi * (x * f0 - y * f3));
                    v += w * fcos(wt * 0.375f - wsi * (x * f2 + y * f1));
                    u += w * fcos(wt * 0.753f - wsi * (x * f1 - y * f2));
                    v += w * fsin(wt * 0.825f + wsi * (x * f0 + y * f3));
                }
                if (rot != last_rot) { last_rot = rot; cr = std::cos(rot); sr = std::sin(rot); }
                const float u2 = u - cx, v2 = v - cy;
                u = u2 * cr - v2 * sr + cx - dx;
                v = u2 * sr + v2 * cr + cy - dy;
                u = (u - 0.5f) / ax + 0.5f;
                v = (v - 0.5f) / ay + 0.5f;
                const float su = u * float(W), sv = v * float(H);
                su_[n] = su == su ? clampf(su, -8000.f, 8000.f) : float(xs_[i]);
                sv_[n] = sv == sv ? clampf(sv, -8000.f, 8000.f) : float(ys_[j]);
            }
        }
    }

    cur_.swap(prev_);
    Impl d(*this);
    const float decay = F(Milk::kDecay, 0.f, 1.f);
    d.warp(decay, decay, decay, F(Milk::kWrap, -1e3f, 1e3f) != 0.f);

    auto X = [&](float cx) { return (cx + 1.f) * 0.5f * float(W); };      // clip space (y up) -> pixels
    auto Y = [&](float cy) { return (1.f - cy) * 0.5f * float(H); };
    auto c255 = [](double v) { return int(num(v, 0.f, 1.f, 0.f) * 255.f + 0.5f); };
    auto a16 = [](double v) { return int(num(v, 0.f, 1.f, 0.f) * 65536.f + 0.5f); };     // alpha for blend_px
    const float thin = std::max(1.f, float(H) * 0.01f);
    // MilkDrop drew one-pixel lines on a canvas several hundred pixels high;
    // here a one-pixel line is a far bigger share of the picture and would
    // flood the feedback with light, so thin strokes are drawn fainter on
    // small canvases.
    const float stroke = clampf(float(H) / 150.f, 0.45f, 1.f);
    const float speck = clampf(std::sqrt(float(W) * float(H) / 120000.f), 0.25f, 1.f);   // same, for dot clouds

    // Motion vectors: a grid of short strokes pointing the way the image moves.
    {
        const int nx = int(F(Milk::kMvX, 0.f, 64.f)), ny = int(F(Milk::kMvY, 0.f, 48.f));
        // Dense grids were sparse dots at full size; keep their coverage low.
        const float cell = nx > 0 && ny > 0 ? float(W) / float(nx) * float(H) / float(ny) : 0.f;
        const int ma = int(float(a16(M.fv[M.fi[Milk::kMvA]])) * stroke * clampf(cell / 64.f, 0.05f, 1.f));
        if (ma > 0 && nx > 0 && ny > 0) {
            const float ox = F(Milk::kMvDx, -1.f, 1.f), oy = F(Milk::kMvDy, -1.f, 1.f);
            const float len = F(Milk::kMvL, 0.f, 5.f);
            const int r = c255(M.fv[M.fi[Milk::kMvR]]), g = c255(M.fv[M.fi[Milk::kMvG]]), b = c255(M.fv[M.fi[Milk::kMvB]]);
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    const float fx = (float(i) + 0.5f) / float(nx) + ox, fy = (float(j) + 0.5f) / float(ny) - oy;
                    if (!(fx > 0.f && fx < 1.f && fy > 0.f && fy < 1.f)) continue;
                    const float gx = fx * float(kGX), gy = fy * float(kGY);
                    const int ix = std::min(int(gx), kGX - 1), iy = std::min(int(gy), kGY - 1);
                    const float tx = gx - float(ix), ty = gy - float(iy);
                    const int n = iy * (kGX + 1) + ix;
                    auto lerp2 = [&](const float* s) {
                        return (s[n] * (1.f - tx) + s[n + 1] * tx) * (1.f - ty) +
                               (s[n + kGX + 1] * (1.f - tx) + s[n + kGX + 2] * tx) * ty;
                    };
                    const float px = fx * float(W), py = fy * float(H);
                    float mx = (px - lerp2(su_)) * len, my = (py - lerp2(sv_)) * len;
                    const float l2 = mx * mx + my * my;
                    if (l2 < 1.f) { d.dot(px, py, r, g, b, 1, ma, false); continue; }
                    if (l2 > float(W) * float(W)) continue;
                    d.line(px, py, px + mx, py + my, r, g, b, thin, ma, false);
                }
            }
        }
    }

    // Waveform as the wave code sees it: smoothed, scaled.
    constexpr int N = AudioFrame::kWave;
    float ws[N];
    {
        float prev = 0.f;
        for (int i = 0; i < N; ++i) {
            const float s = a.wave[i] == a.wave[i] ? clampf(a.wave[i], -1.5f, 1.5f) : 0.f;
            prev = i ? prev * M.wave_smooth + s * (1.f - M.wave_smooth) : s;
            ws[i] = prev;
        }
    }

    // Custom shapes.
    for (Milk::Shape& sh : M.shapes) {
        if (!sh.on) continue;
        using S = Milk::Shape;
        for (int i = 0; i < S::kN; ++i) sh.v[sh.i[i]] = sh.base[i];
        sh.c.set(sh.v, cv);
        for (int i = 0; i < 8; ++i) sh.v[sh.q[i]] = q[i];
        if (M.first) {
            for (int i = 0; i < 8; ++i) sh.v[sh.t[i]] = 0.0;
            sh.init.run(sh.v, M.rng);
            for (int i = 0; i < 8; ++i) sh.t_init[i] = sh.v[sh.t[i]];
            for (int i = 0; i < S::kN; ++i) sh.v[sh.i[i]] = sh.base[i];
        }
        for (int i = 0; i < 8; ++i) sh.v[sh.t[i]] = sh.t_init[i];
        sh.frame.run(sh.v, M.rng);
        auto V = [&](int i) { return sh.v[sh.i[i]]; };
        const int sides = int(num(V(S::kSides), 3.f, 48.f, 4.f));
        const float sx = num(V(S::kX), -100.f, 100.f, 0.5f) * 2.f - 1.f;
        const float sy = 1.f - num(V(S::kY), -100.f, 100.f, 0.5f) * 2.f;
        const float rad = num(V(S::kRad), 0.f, 1000.f, 0.1f);
        // A shape under a pixel across would slip between pixel centres:
        // draw it a pixel across and proportionally fainter instead.
        const float rpx = rad * M.ay * 0.5f * float(W), rpy = rad * M.ax * 0.5f * float(H);
        const float kx = rpx < 1.f ? 1.f / std::max(rpx, 1e-3f) : 1.f, ky = rpy < 1.f ? 1.f / std::max(rpy, 1e-3f) : 1.f;
        const float tiny = clampf(1.f / (kx * ky), 0.12f, 1.f);
        const float ang = num(V(S::kAng), -1e5f, 1e5f, 0.f), tang = num(V(S::kTexAng), -1e5f, 1e5f, 0.f);
        float tz = num(V(S::kTexZoom), -1e4f, 1e4f, 1.f);
        if (std::fabs(tz) < 1e-3f) tz = 1e-3f;
        const bool additive = V(S::kAdditive) != 0.0, textured = V(S::kTextured) != 0.0;
        Impl::TV centre{X(sx), Y(sy), float(c255(V(S::kR))), float(c255(V(S::kG))), float(c255(V(S::kB))),
                        num(V(S::kA), 0.f, 1.f, 0.f) * 256.f * tiny, 0.5f * float(W), 0.5f * float(H)};
        Impl::TV rim[49];
        for (int k = 0; k <= sides; ++k) {
            const float t = kTau * float(k % sides) / float(sides) + kPi * 0.25f;
            rim[k] = Impl::TV{X(sx + rad * kx * std::cos(t + ang) * M.ay), Y(sy + rad * ky * std::sin(t + ang) * M.ax),
                              float(c255(V(S::kR2))), float(c255(V(S::kG2))), float(c255(V(S::kB2))),
                              num(V(S::kA2), 0.f, 1.f, 0.f) * 256.f * tiny,
                              (0.5f + 0.5f * std::cos(t + tang) / tz * M.ay) * float(W),
                              (0.5f - 0.5f * std::sin(t + tang) / tz * M.ax) * float(H)};
        }
        if (centre.a > 0.f || rim[0].a > 0.f)
            for (int k = 0; k < sides; ++k)
                d.tri(centre, rim[k], rim[k + 1], additive, textured ? prev_.data() : nullptr);
        const int ba = int(float(a16(V(S::kBorA))) * tiny * stroke);
        if (ba > 0) {
            const int r = c255(V(S::kBorR)), g = c255(V(S::kBorG)), b = c255(V(S::kBorB));
            const float wd = V(S::kThick) != 0.0 ? thin * 2.f : thin;
            for (int k = 0; k < sides; ++k)
                d.line(rim[k].x, rim[k].y, rim[k + 1].x, rim[k + 1].y, r, g, b, wd, ba, additive);
        }
    }

    // Custom waves.
    for (Milk::Wave& wv : M.waves) {
        if (!wv.on) continue;
        for (int i = 0; i < 4; ++i) wv.v[wv.rgba[i]] = wv.base[i];
        wv.c.set(wv.v, cv);
        for (int i = 0; i < 8; ++i) wv.v[wv.q[i]] = q[i];
        if (M.first) {
            for (int i = 0; i < 8; ++i) wv.v[wv.t[i]] = 0.0;
            wv.init.run(wv.v, M.rng);
            for (int i = 0; i < 8; ++i) wv.t_init[i] = wv.v[wv.t[i]];
            for (int i = 0; i < 4; ++i) wv.v[wv.rgba[i]] = wv.base[i];
        }
        for (int i = 0; i < 8; ++i) wv.v[wv.t[i]] = wv.t_init[i];
        wv.frame.run(wv.v, M.rng);
        double rgba[4];
        for (int i = 0; i < 4; ++i) rgba[i] = wv.v[wv.rgba[i]];

        // As many points as the preset asks for, within a budget of program
        // steps: a long per_point program gets fewer points, never fewer than 64.
        const int budget = int(Milk::Wave::kStepBudget / std::max<size_t>(1, wv.point.size()));
        const int n = std::min(std::min(wv.samples, Milk::Wave::kMaxPoints), std::max(64, budget));
        const float mult = wv.scaling * M.wave_scale * (wv.spectrum ? 1.2f : 0.5f);
        const float wd = wv.thick ? thin * 2.f : thin;
        float lx = 0.f, ly = 0.f, sm1 = 0.f, sm2 = 0.f;
        for (int j = 0; j < n; ++j) {
            const float pos = float(j) / float(n - 1);
            float v1, v2;
            if (wv.spectrum) {
                const float sp = pos * float(AudioFrame::kSpec - 1);
                const int k = std::min(int(sp), AudioFrame::kSpec - 2);
                const float s0 = a.spec[k] == a.spec[k] ? a.spec[k] : 0.f, s1 = a.spec[k + 1] == a.spec[k + 1] ? a.spec[k + 1] : 0.f;
                v1 = v2 = clampf(s0 + (s1 - s0) * (sp - float(k)), 0.f, 1.f);
            } else {
                const int k = int(pos * float(N - 1));
                v1 = ws[k];
                v2 = ws[(k + 19 + wv.sep) % N];
            }
            sm1 = j ? sm1 * wv.smoothing + v1 * (1.f - wv.smoothing) : v1;
            sm2 = j ? sm2 * wv.smoothing + v2 * (1.f - wv.smoothing) : v2;
            wv.v[wv.sample] = double(pos);
            wv.v[wv.value1] = double(sm1 * mult);
            wv.v[wv.value2] = double(sm2 * mult);
            wv.v[wv.x] = 0.5 + double(sm1 * mult);
            wv.v[wv.y] = 0.5 + double(sm2 * mult);
            for (int i = 0; i < 4; ++i) wv.v[wv.rgba[i]] = rgba[i];
            wv.point.run(wv.v, M.rng);
            const float x = X((num(wv.v[wv.x], -100.f, 100.f, 0.5f) * 2.f - 1.f) / M.ax);
            const float y = Y((1.f - num(wv.v[wv.y], -100.f, 100.f, 0.5f) * 2.f) / M.ay);
            const int r = c255(wv.v[wv.rgba[0]]), g = c255(wv.v[wv.rgba[1]]), b = c255(wv.v[wv.rgba[2]]);
            const int al = int(float(a16(wv.v[wv.rgba[3]])) * (wv.dots ? speck : stroke));
            if (al > 0) {
                if (wv.dots) d.dot(x - (wv.thick ? 1.f : 0.f), y - (wv.thick ? 1.f : 0.f), r, g, b, wv.thick ? 2 : 1, al, wv.additive);
                else if (j) d.line(lx, ly, x, y, r, g, b, wd, al, wv.additive);
            }
            lx = x; ly = y;
        }
    }

    // The basic waveform, modes 0-7.
    {
        const int mode = int(F(Milk::kWaveMode, 0.f, 7.f));
        float alpha = F(Milk::kWaveA, 0.f, 100.f);
        if (M.mod_alpha && M.mod_end != M.mod_start)
            alpha *= clampf(((bass_ + mid_ + treb_) / 3.f - M.mod_start) / (M.mod_end - M.mod_start), 0.f, 1.f);
        float r = F(Milk::kWaveR, 0.f, 1.f), g = F(Milk::kWaveG, 0.f, 1.f), b = F(Milk::kWaveB, 0.f, 1.f);
        if (F(Milk::kWaveBrighten, -1e3f, 1e3f) != 0.f) {
            const float mx = std::max(r, std::max(g, b));
            if (mx > 0.01f) { r /= mx; g /= mx; b /= mx; }
        }
        const float wx = F(Milk::kWaveX, -10.f, 10.f) * 2.f - 1.f, wy = F(Milk::kWaveY, -10.f, 10.f) * 2.f - 1.f;
        const float mys = F(Milk::kWaveMystery, -100.f, 100.f);
        const bool dots = F(Milk::kWaveDots, -1e3f, 1e3f) != 0.f, thick = F(Milk::kWaveThick, -1e3f, 1e3f) != 0.f;
        const bool additive = F(Milk::kWaveAdditive, -1e3f, 1e3f) != 0.f;
        const float sc = M.wave_scale;
        auto L = [&](int i) { return ws[unsigned(i) % N] * sc; };
        auto R = [&](int i) { return ws[unsigned(i + 19) % N] * sc; };

        float px[2][100], py[2][100];
        int count = 0, strips = 1;
        bool closed = false;
        switch (mode) {
        case 0: {               // a circle whose radius follows the wave
            count = 64;
            for (int i = 0; i < count; ++i) {
                float rad = 0.5f + 0.4f * R(i) + mys;
                const float ang = float(i) / float(count - 1) * kTau + a.time * 0.2f;
                if (i < count / 10) {       // ease the start into what follows the end
                    float mix = float(i) / (float(count) * 0.1f);
                    mix = 0.5f - 0.5f * fcos(mix * kPi);
                    rad = (0.5f + 0.4f * R(i + count) + mys) * (1.f - mix) + rad * mix;
                }
                px[0][i] = rad * fcos(ang) * M.ay + wx;
                py[0][i] = rad * fsin(ang) * M.ax + wy;
            }
            closed = true;
            break;
        }
        case 1:                 // angle from one channel, radius from the other
            count = 96;
            alpha *= 1.25f;
            for (int i = 0; i < count; ++i) {
                const float rad = 0.53f + 0.43f * R(i) + mys;
                const float ang = L(i + 32) * 1.57f + a.time * 2.3f;
                px[0][i] = rad * fcos(ang) * M.ay + wx;
                py[0][i] = rad * fsin(ang) * M.ax + wy;
            }
            break;
        case 2:
        case 3:                 // X/Y scope (3: louder with treble)
            count = 96;
            alpha *= 0.35f;
            if (mode == 3) alpha *= 1.3f * treb_ * treb_;
            for (int i = 0; i < count; ++i) {
                px[0][i] = R(i) * M.ay + wx;
                py[0][i] = L(i + 32) * M.ax + wy;
            }
            break;
        case 4: {               // a horizontal trace with some momentum
            count = 96;
            const float w1 = 0.45f + 0.5f * (mys * 0.5f + 0.5f), w2 = 1.f - w1;
            for (int i = 0; i < count; ++i) {
                float x = -1.f + 2.f * float(i) / float(count) + wx;
                float y = L(i) * 0.47f + wy;
                x += R(i + 25) * 0.44f;
                if (i > 1) {
                    x = x * w2 + w1 * (px[0][i - 1] * 2.f - px[0][i - 2]);
                    y = y * w2 + w1 * (py[0][i - 1] * 2.f - py[0][i - 2]);
                }
                px[0][i] = clampf(x, -50.f, 50.f);
                py[0][i] = clampf(y, -50.f, 50.f);
            }
            break;
        }
        case 5: {               // the two channels multiplied as complex numbers
            count = 96;
            alpha *= 0.35f;
            const float cr = fcos(a.time * 0.3f), sr = fsin(a.time * 0.3f);
            for (int i = 0; i < count; ++i) {
                const float x0 = R(i) * L(i + 32) + L(i) * R(i + 32);
                const float y0 = R(i) * R(i) - L(i + 32) * L(i + 32);
                px[0][i] = (x0 * cr - y0 * sr) * M.ay + wx;
                py[0][i] = (x0 * sr + y0 * cr) * M.ax + wy;
            }
            break;
        }
        default: {              // 6: one line at an angle; 7: two, wave_y apart
            count = 96;
            strips = mode == 7 ? 2 : 1;
            const float ang = 1.57f * mys;
            const float dx = std::cos(ang), dy = std::sin(ang);
            const float bx = wx * std::cos(ang + 1.57f), by = wx * std::sin(ang + 1.57f);
            // The stretch of the line that lies within the screen (and a bit).
            float t0 = -3.f, t1 = 3.f;
            const float lim = 1.1f;
            if (std::fabs(dx) > 1e-4f) {
                const float ta = (-lim - bx) / dx, tb = (lim - bx) / dx;
                t0 = std::max(t0, std::min(ta, tb)); t1 = std::min(t1, std::max(ta, tb));
            }
            if (std::fabs(dy) > 1e-4f) {
                const float ta = (-lim - by) / dy, tb = (lim - by) / dy;
                t0 = std::max(t0, std::min(ta, tb)); t1 = std::min(t1, std::max(ta, tb));
            }
            if (!(t1 > t0)) { count = 0; break; }
            const float sep = mode == 7 ? (wy * 0.5f + 0.5f) * (wy * 0.5f + 0.5f) : 0.f;
            for (int s = 0; s < strips; ++s) {
                for (int i = 0; i < count; ++i) {
                    const float t = t0 + (t1 - t0) * float(i) / float(count - 1);
                    const float off = 0.25f * (s ? R(i) : L(i)) + (s ? -sep : sep);
                    px[s][i] = bx + dx * t - dy * off;
                    py[s][i] = by + dy * t + dx * off;
                }
            }
            break;
        }
        }
        const int al = int(clampf(alpha, 0.f, 1.f) * (dots ? speck : stroke) * 65536.f + 0.5f);
        if (al > 0 && count > 1) {
            const int ir = int(r * 255.f), ig = int(g * 255.f), ib = int(b * 255.f);
            const float wd = thick ? thin * 2.f : thin;
            for (int s = 0; s < strips; ++s) {
                // A wave squeezed into less than a pixel would fall between
                // pixel centres and vanish; it is a dot.
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                for (int i = 0; i < count; ++i) {
                    x0 = std::min(x0, X(px[s][i])); x1 = std::max(x1, X(px[s][i]));
                    y0 = std::min(y0, Y(py[s][i])); y1 = std::max(y1, Y(py[s][i]));
                }
                if (x1 - x0 < 1.5f && y1 - y0 < 1.5f) {
                    const int size = thick ? 2 : 1;
                    d.dot((x0 + x1) * 0.5f - float(size - 1) * 0.5f, (y0 + y1) * 0.5f - float(size - 1) * 0.5f,
                          ir, ig, ib, size, al, additive);
                    continue;
                }
                for (int i = 0; i < count; ++i) {
                    const float x = X(px[s][i]), y = Y(py[s][i]);
                    if (dots) {
                        d.dot(x, y, ir, ig, ib, thick ? 2 : 1, al, additive);
                    } else if (i) {
                        d.line(X(px[s][i - 1]), Y(py[s][i - 1]), x, y, ir, ig, ib, wd, al, additive);
                    }
                }
                if (closed && !dots)
                    d.line(X(px[s][count - 1]), Y(py[s][count - 1]), X(px[s][0]), Y(py[s][0]), ir, ig, ib, wd, al, additive);
            }
        }
    }

    if (F(Milk::kDarkenCenter, -1e3f, 1e3f) != 0.f) d.darken_centre(0.f, 0.f, 0.1f, std::max(2, H / 12));

    // Borders: the outer one against the edge, the inner one just inside it.
    {
        const float os = F(Milk::kObSize, 0.f, 1.f), is = F(Milk::kIbSize, 0.f, 1.f);
        // A border thinner than a pixel is drawn one pixel wide but fainter.
        auto thin_gain = [&](float size) { return clampf(size * 0.5f * float(H), 0.25f, 1.f); };
        const int oa = int(float(a16(M.fv[M.fi[Milk::kObA]])) * thin_gain(os));
        const int ia = int(float(a16(M.fv[M.fi[Milk::kIbA]])) * thin_gain(is));
        auto px_of = [](float size, int dim) { return size > 0.f ? std::max(1, int(size * 0.5f * float(dim) + 0.5f)) : 0; };
        const int ox = px_of(os, W), oy = px_of(os, H);
        if (oa > 0 && os > 0.f)
            d.frame_rect(0, 0, ox, oy, c255(M.fv[M.fi[Milk::kObR]]), c255(M.fv[M.fi[Milk::kObG]]), c255(M.fv[M.fi[Milk::kObB]]), oa);
        if (ia > 0 && is > 0.f)
            d.frame_rect(ox, oy, ox + px_of(is, W), oy + px_of(is, H),
                         c255(M.fv[M.fi[Milk::kIbR]]), c255(M.fv[M.fi[Milk::kIbG]]), c255(M.fv[M.fi[Milk::kIbB]]), ia);
    }

    d.present(F(Milk::kGamma, 0.f, 8.f), F(Milk::kEchoAlpha, 0.f, 1.f), F(Milk::kEchoZoom, 0.25f, 100.f),
              int(F(Milk::kEchoOrient, 0.f, 3.f)),
              F(Milk::kBrighten, -1e3f, 1e3f) != 0.f, F(Milk::kDarken, -1e3f, 1e3f) != 0.f,
              F(Milk::kSolarize, -1e3f, 1e3f) != 0.f, F(Milk::kInvert, -1e3f, 1e3f) != 0.f);
    out_ = disp_.data();
    M.first = false;
}

// ── Analyzer ────────────────────────────────────────────────────────────────

Analyzer::Analyzer() {
    for (int i = 0; i < kFft; ++i) {
        win_[i] = 0.5f - 0.5f * std::cos(kTau * float(i) / float(kFft));
        unsigned r = 0;
        for (int b = 0; b < 8; ++b) r |= ((unsigned(i) >> b) & 1u) << (7 - b);   // 8 = log2(kFft)
        rev_[i] = uint16_t(r);
    }
    for (int i = 0; i < kFft / 2; ++i) {
        cos_[i] = std::cos(kTau * float(i) / float(kFft));
        sin_[i] = std::sin(kTau * float(i) / float(kFft));
    }
    reset();
}

void Analyzer::reset() {
    std::memset(ring_, 0, sizeof ring_);
    wpos_ = last_wpos_ = 0;
    cursor_ = 0.0;
    dec_n_ = 0;
    dec_acc_ = 0;
    started_ = false;
    stall_ = 0.f;
    avg_[0] = avg_[1] = avg_[2] = 0.f;
    spec_peak_ = wave_peak_ = 0.f;
}

void Analyzer::push(const int16_t* mono, size_t n, int sample_rate) {
    if (!mono || !n) return;
    if (sample_rate <= 0) sample_rate = 11025;
    if (sample_rate != in_rate_) {
        in_rate_ = sample_rate;
        dec_ = std::max(1, (sample_rate + 5512) / 11025);
        rate_ = sample_rate / dec_;
        dec_n_ = 0;
        dec_acc_ = 0;
    }
    if (dec_ == 1) {
        for (size_t i = 0; i < n; ++i) ring_[wpos_++ & (kRing - 1)] = mono[i];
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        dec_acc_ += mono[i];
        if (++dec_n_ == dec_) {
            ring_[wpos_++ & (kRing - 1)] = int16_t(dec_acc_ / dec_);
            dec_n_ = 0;
            dec_acc_ = 0;
        }
    }
}

AudioFrame Analyzer::frame(float time) {
    AudioFrame f;
    f.time = time;

    float dt = started_ ? time - last_time_ : 0.f;
    if (!(dt >= 0.f)) dt = 0.f;
    dt = std::min(dt, 0.25f);
    last_time_ = time;

    // Read position: runs with the clock, never ahead of what has been
    // written and never more than 150 ms behind it.
    const double w = double(wpos_);
    if (!started_) cursor_ = w;
    started_ = true;
    cursor_ += double(dt) * double(rate_);
    cursor_ = std::max(cursor_, w - 0.15 * double(rate_));
    cursor_ = std::min(cursor_, w);

    stall_ = wpos_ == last_wpos_ ? stall_ + dt : 0.f;
    last_wpos_ = wpos_;
    if (stall_ > 0.3f || wpos_ == 0) return f;   // silence

    const uint32_t end = uint32_t(cursor_);
    float re[kFft], im[kFft], raw[kFft];
    float peak = 0.f;
    for (int i = 0; i < kFft; ++i) {
        const float v = float(ring_[(end - kFft + i) & (kRing - 1)]) * (1.f / 32768.f);
        raw[i] = v;
        peak = std::max(peak, std::fabs(v));
        re[rev_[i]] = v * win_[i];
        im[rev_[i]] = 0.f;
    }

    // Waveform: the newest 128 samples, levelled so quiet tracks still move.
    wave_peak_ = std::max(peak, wave_peak_ * std::exp(-dt / 2.f));
    const float gain = wave_peak_ > 0.004f ? clampf(0.8f / wave_peak_, 1.f, 8.f) : 1.f;
    for (int i = 0; i < AudioFrame::kWave; ++i)
        f.wave[i] = clampf(raw[kFft - AudioFrame::kWave + i] * gain, -1.f, 1.f);

    // 256-point radix-2 FFT.
    for (int len = 2; len <= kFft; len <<= 1) {
        const int half = len >> 1, step = kFft / len;
        for (int i = 0; i < kFft; i += len) {
            for (int j = 0; j < half; ++j) {
                const float c = cos_[j * step], s = -sin_[j * step];
                const float tr = re[i + j + half] * c - im[i + j + half] * s;
                const float ti = re[i + j + half] * s + im[i + j + half] * c;
                re[i + j + half] = re[i + j] - tr;
                im[i + j + half] = im[i + j] - ti;
                re[i + j] += tr;
                im[i + j] += ti;
            }
        }
    }
    float mag[kFft / 2];
    mag[0] = 0.f;
    for (int i = 1; i < kFft / 2; ++i)
        mag[i] = std::sqrt(re[i] * re[i] + im[i] * im[i]) * (4.f / float(kFft));   // sine at full scale = 1

    const float bin_hz = float(rate_) / float(kFft);
    auto bin = [&](float hz) { return std::max(1, std::min(kFft / 2, int(hz / bin_hz + 0.5f))); };
    auto mean = [&](int lo, int hi) {
        hi = std::max(hi, lo + 1);
        hi = std::min(hi, kFft / 2);
        lo = std::min(lo, hi - 1);
        float s = 0.f;
        for (int i = lo; i < hi; ++i) s += mag[i];
        return s / float(hi - lo);
    };

    // Bands relative to their own long-run level.
    const float nyq = 0.5f * float(rate_);
    const float edges[4] = {30.f, 250.f, 2000.f, nyq};
    float imm[3];
    const float follow = 1.f - std::exp(-dt / 2.5f);
    float* out[3] = {&f.bass, &f.mid, &f.treb};
    for (int b = 0; b < 3; ++b) {
        imm[b] = mean(bin(edges[b]), bin(edges[b + 1]));
        if (avg_[b] <= 0.f) avg_[b] = imm[b];
        avg_[b] += (imm[b] - avg_[b]) * follow;
        *out[b] = avg_[b] > 2e-5f ? clampf(imm[b] / avg_[b], 0.f, 3.f) : 0.f;
    }

    // Spectrum: 16 log-spaced bands, tilted up towards the top, against a
    // slowly falling peak.
    const float f_lo = 50.f, f_hi = nyq * 0.95f;
    const float ratio = std::pow(f_hi / f_lo, 1.f / float(AudioFrame::kSpec));
    float band[AudioFrame::kSpec], top = 0.f, fq = f_lo;
    for (int k = 0; k < AudioFrame::kSpec; ++k, fq *= ratio) {
        band[k] = mean(bin(fq), bin(fq * ratio)) * (1.f + 0.35f * float(k));
        top = std::max(top, band[k]);
    }
    spec_peak_ = std::max(top, spec_peak_ * std::exp(-dt / 4.f));
    if (spec_peak_ > 1e-4f)
        for (int k = 0; k < AudioFrame::kSpec; ++k)
            f.spec[k] = std::sqrt(clampf(band[k] / spec_peak_, 0.f, 1.f));
    return f;
}

// ── Synthetic audio ─────────────────────────────────────────────────────────

AudioFrame fake_audio(float time) {
    AudioFrame f;
    f.time = time;
    const double kBeat = 60.0 / 124.0;           // seconds per beat
    const double t = time > 0.f ? double(time) : 0.0;

    auto pattern = [&](double ts, float& kick, float& snare, float& hat, int& bar_note) {
        const double beat = ts / kBeat;
        const double bi = std::floor(beat);
        const float ph = float(beat - bi);                       // 0..1 within the beat
        const double half = beat * 2.0;
        const float ph2 = float(half - std::floor(half));
        const uint32_t n = uint32_t(bi);
        kick = std::exp(-ph * 6.5f);
        snare = (n & 1u) ? std::exp(-ph * 9.f) : 0.f;
        // Hats on the off-beats, with the odd extra one.
        const bool off = (uint32_t(std::floor(half)) & 1u) != 0;
        const bool extra = (hash32(uint32_t(std::floor(half)) + 77u) & 7u) == 0;
        hat = (off || extra) ? std::exp(-ph2 * 11.f) : 0.f;
        bar_note = int((n / 4u) % 4u);
    };

    float kick, snare, hat;
    int note;
    pattern(t, kick, snare, hat, note);
    const float swell = 0.5f + 0.5f * std::sin(float(std::fmod(t * 0.37, double(kTau))));
    f.bass = 0.45f + 1.7f * kick;
    f.mid = 0.55f + 1.1f * snare + 0.35f * swell;
    f.treb = 0.5f + 1.3f * hat + 0.4f * snare;

    static const float kNotes[4] = {55.f, 55.f, 65.41f, 49.f};
    constexpr double kRate = 11025.0;
    for (int i = 0; i < AudioFrame::kWave; ++i) {
        const double ts = std::max(0.0, t - double(AudioFrame::kWave - 1 - i) / kRate);
        float k2, s2, h2;
        int n2;
        pattern(ts, k2, s2, h2, n2);
        const double beat = ts / kBeat;
        const float tk = float((beat - std::floor(beat)) * kBeat);              // s since the kick
        const float noise = hash_pm1(uint32_t(ts * kRate));
        float v = 0.75f * k2 * std::sin(kTau * (48.f * tk + 5.f * (1.f - std::exp(-tk * 28.f))));
        v += 0.22f * std::sin(float(std::fmod(ts * double(kNotes[n2]) * 2.0, 1.0)) * kTau);
        v += 0.14f * (0.4f + 0.6f * swell) *
             std::sin(float(std::fmod(ts * 330.0, 1.0)) * kTau + 2.f * std::sin(float(std::fmod(ts * 0.5, 1.0)) * kTau));
        v += 0.3f * s2 * noise + 0.16f * h2 * noise;
        f.wave[i] = clampf(v, -1.f, 1.f);
    }

    for (int k = 0; k < AudioFrame::kSpec; ++k) {
        const float x = float(k) / float(AudioFrame::kSpec - 1);
        const float lo = std::max(0.f, 1.f - x * 3.5f), hi = std::max(0.f, x * 2.2f - 1.2f);
        const float md = std::max(0.f, 1.f - std::fabs(x - 0.5f) * 3.f);
        const float wob = 0.5f + 0.5f * std::sin(float(std::fmod(t * (0.9 + 0.31 * k), double(kTau))) + 1.3f * float(k));
        const float v = lo * (0.25f + 0.75f * kick) + md * (0.2f + 0.5f * snare + 0.25f * swell) +
                        hi * (0.12f + 0.7f * hat) + 0.14f * wob;
        f.spec[k] = clampf(v, 0.f, 1.f);
    }
    return f;
}

}  // namespace mm::viz
