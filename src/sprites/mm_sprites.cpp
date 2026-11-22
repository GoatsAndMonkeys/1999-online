// mm_sprites — see mm_sprites.h. Recolour math ported verbatim from
// monster_mesh_pi/src/terminal/Gen2SpriteCache.cpp.
//
// NOTE: the Gen3*4bpp.h and Gen2*Icons.h headers define large `static const`
// arrays; this must be the ONLY translation unit in the app that includes them.

#include "mm_sprites.h"

#include "Gen3Front4bpp.h"   // monster_mesh_pi/src/terminal
#include "Gen3Back4bpp.h"
#include "Gen2ColorIcons.h"  // Gen 2 fronts, 56x56 2bpp, dex 1..151
#include "Gen2BackIcons.h"   // Gen 2 backs,  48x48 2bpp, dex 1..151
#include "BreedingGenetics.h" // monster_mesh_pi/src/shared (only for static_asserts)

extern "C" {
#include "puff.h"
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

namespace mm::sprites {

static_assert(int(SKIN_REGULAR) == int(breeding::SKIN_REGULAR) &&
              int(SKIN_RAINBOW) == int(breeding::SKIN_RAINBOW) &&
              int(SKIN_DARK) == int(breeding::SKIN_DARK) &&
              int(SKIN_DARK_RAINBOW) == int(breeding::SKIN_DARK_RAINBOW) &&
              int(SKIN_BLACKOUT) == int(breeding::SKIN_BLACKOUT) &&
              int(SKIN_BLACKOUT_RAINBOW) == int(breeding::SKIN_BLACKOUT_RAINBOW),
              "mm::sprites skin ids must match breeding::Skin");
static_assert(GEN3_FRONT_4BPP_W == kSpriteW && GEN3_FRONT_4BPP_H == kSpriteH &&
              GEN3_BACK_4BPP_W == kSpriteW && GEN3_BACK_4BPP_H == kSpriteH,
              "sprite blobs are expected to be 64x64");
static_assert(GEN2_COLOR_W == 56 && GEN2_COLOR_H == 56 && GEN2_BACK_W == 48 && GEN2_BACK_H == 48,
              "Gen 2 sprite blobs are expected to be 56x56 / 48x48");

namespace {

struct GammaLut {
    uint8_t v[256];
    GammaLut() {
        for (int i = 0; i < 256; ++i) {
            float f = powf(i / 255.0f, 2.1f) * 255.0f + 0.5f;
            v[i] = (uint8_t)(f > 255.0f ? 255 : (int)f);
        }
    }
};
const GammaLut& gamma_lut() { static const GammaLut lut; return lut; }

// ── luma-preserving recolour helpers (operate on 0..255 RGB) ─────────────────
inline uint8_t luma(int r, int g, int b) { return (uint8_t)((77*r + 150*g + 29*b) >> 8); }

void hueLuma(int deg, int sat /*0..255*/, int Y, uint8_t &or_, uint8_t &og, uint8_t &ob) {
    float h = (deg % 360) / 60.0f; int i = (int)h; float f = h - i; float s = sat / 255.0f;
    const float v = 255.0f;
    float p = v*(1-s), q = v*(1-s*f), t = v*(1-s*(1-f)); float r, g, b;
    switch (i % 6) {
        case 0: r=v; g=t; b=p; break;  case 1: r=q; g=v; b=p; break;  case 2: r=p; g=v; b=t; break;
        case 3: r=p; g=q; b=v; break;  case 4: r=t; g=p; b=v; break;  default: r=v; g=p; b=q; break;
    }
    int yb = (77*(int)r + 150*(int)g + 29*(int)b) >> 8; if (yb < 1) yb = 1;
    float sc = (float)Y / yb;
    int R=(int)(r*sc), G=(int)(g*sc), B=(int)(b*sc);
    or_ = R>255?255:R; og = G>255?255:G; ob = B>255?255:B;
}

inline int satLite(int Y, int baseSat) {
    if (Y <= 175) return baseSat;
    int pct = 90 * (Y - 175) / 80;               // 0..90
    int s = baseSat - baseSat * pct / 100;
    return s < 15 ? 15 : s;
}

// True red hue? (matches firmware Dark gate: sat>=0.45, hue in [-18deg,+10deg])
bool isRed(int r, int g, int b) {
    int mx = r>g ? (r>b?r:b) : (g>b?g:b);
    int mn = r<g ? (r<b?r:b) : (g<b?g:b);
    int chroma = mx - mn;
    if (mx == 0 || mx != r || chroma*100 < 45*mx) return false;   // red-dominant, sat>=0.45
    int gb = g - b;
    return gb*6 <= chroma && (b-g)*10 <= 3*chroma;                // hue in [-18,+10]
}

// Apply a skin to one opaque pixel (r,g,b in/out). i is the pixel index.
void applyVariant(int variant, int i, int w, int h, uint16_t phase,
                  uint8_t &r, uint8_t &g, uint8_t &b) {
    const uint8_t* lut = gamma_lut().v;
    switch (variant) {
        case SKIN_REGULAR: case SKIN_SHINY: default:
            break;                                    // palette already chosen
        case SKIN_RAINBOW: {
            int d = (i / w) + (i % w);
            int hue = ((d * 306) / (2 * (h - 1)) + phase * 20) % 360;
            uint8_t Y = luma(r, g, b);
            hueLuma(hue, satLite(Y, 242), Y, r, g, b);
            break;
        }
        case SKIN_PINK: {
            uint8_t Y = luma(r, g, b);
            hueLuma(329, satLite(Y, 217), Y, r, g, b);
            break;
        }
        case SKIN_DARK: case SKIN_DARK_SHINY: {
            if (isRed(r, g, b)) break;                // keep the red
            uint8_t y = lut[luma(r, g, b)];
            r = g = b = y;
            break;
        }
        case SKIN_DARK_PINK: {
            if (isRed(r, g, b)) {                     // red -> pink, keep brightness
                uint8_t Y = luma(r, g, b);
                hueLuma(329, satLite(Y, 217), Y, r, g, b);
            } else {
                uint8_t y = lut[luma(r, g, b)];
                r = g = b = y;
            }
            break;
        }
        case SKIN_DARK_RAINBOW: {
            if (isRed(r, g, b)) {                     // red -> animated rainbow glow
                int d = (i / w) + (i % w);
                int hue = ((d * 306) / (2 * (h - 1)) + phase * 20) % 360;
                uint8_t Y = luma(r, g, b);
                hueLuma(hue, satLite(Y, 242), Y, r, g, b);
            } else {
                uint8_t y = lut[luma(r, g, b)];
                r = g = b = y;
            }
            break;
        }
        // Blackout ("double dark") — near-black grayscale silhouette (~40% luma);
        // Pink/Rainbow still tint the red pixels, heavily dimmed.
        case SKIN_BLACKOUT: case SKIN_BLACKOUT_SHINY: {
            uint8_t y = (uint8_t)(lut[luma(r, g, b)] * 2 / 5);
            r = g = b = y;
            break;
        }
        case SKIN_BLACKOUT_PINK: {
            if (isRed(r, g, b)) {
                uint8_t Y = (uint8_t)(luma(r, g, b) * 2 / 5);
                hueLuma(329, satLite(Y, 217), Y, r, g, b);
            } else {
                r = g = b = (uint8_t)(lut[luma(r, g, b)] * 2 / 5);
            }
            break;
        }
        case SKIN_BLACKOUT_RAINBOW: {
            if (isRed(r, g, b)) {
                int d = (i / w) + (i % w);
                int hue = ((d * 306) / (2 * (h - 1)) + phase * 20) % 360;
                uint8_t Y = (uint8_t)(luma(r, g, b) * 2 / 5);
                hueLuma(hue, satLite(Y, 242), Y, r, g, b);
            } else {
                r = g = b = (uint8_t)(lut[luma(r, g, b)] * 2 / 5);
            }
            break;
        }
    }
}

// Machado (2009) tritanopia transform at full severity, clamped to [0,255].
inline void applyTritan(uint8_t &r, uint8_t &g, uint8_t &b) {
    float R = r, G = g, B = b;
    float nr = 1.255528f * R - 0.076749f * G - 0.178779f * B;
    float ng = -0.078411f * R + 0.930809f * G + 0.147602f * B;
    float nb =  0.004733f * R + 0.691367f * G + 0.303900f * B;
    auto cl = [](float v) -> uint8_t {
        return v <= 0.0f ? 0 : (v >= 255.0f ? 255 : (uint8_t)(v + 0.5f));
    };
    r = cl(nr); g = cl(ng); b = cl(nb);
}

inline void expand565(uint16_t c, uint8_t& rr, uint8_t& gg, uint8_t& bb) {
    int r = ((c >> 11) & 0x1F) << 3;  r |= r >> 5;    // 5->8 bit expand
    int g = ((c >> 5)  & 0x3F) << 2;  g |= g >> 6;
    int b = (c & 0x1F) << 3;          b |= b >> 5;
    rr = (uint8_t)r; gg = (uint8_t)g; bb = (uint8_t)b;
}

inline uint32_t pack(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

}  // namespace

bool is_animated(int skin) {
    return skin == SKIN_RAINBOW || skin == SKIN_DARK_RAINBOW || skin == SKIN_BLACKOUT_RAINBOW;
}

// Flood fill from the frame's border: every pixel equal to `from` that is
// 4-connected to the edge becomes `to`. Pixels of the same value enclosed by
// the monster are left alone (white inside a sprite stays white).
static void clear_border_fill(uint8_t* idx, int w, int h, uint8_t from, uint8_t to) {
    if (from == to || w > kSpriteW || h > kSpriteH) return;
    uint16_t stack[kSpriteW * kSpriteH];
    int sp = 0;
    auto seed = [&](int p) { if (idx[p] == from) { idx[p] = to; stack[sp++] = (uint16_t)p; } };
    for (int x = 0; x < w; ++x) { seed(x); seed((h - 1) * w + x); }
    for (int y = 0; y < h; ++y) { seed(y * w); seed(y * w + w - 1); }
    while (sp > 0) {
        const int p = stack[--sp], x = p % w, y = p / w;
        if (x > 0) seed(p - 1);
        if (x < w - 1) seed(p + 1);
        if (y > 0) seed(p - w);
        if (y < h - 1) seed(p + w);
    }
}

// ── Gen 3: 64x64, 16 colours, index 0 = transparent ─────────────────────────
static bool decode_gen3(int dex, bool back, int skin, bool tritan, uint16_t phase,
                        uint32_t* out, int* out_w, int* out_h) {
    const int w = kSpriteW, h = kSpriteH;

    const uint32_t *offs = back ? kGen3Back4bppOffsets : kGen3Front4bppOffsets;
    const uint8_t  *blob = back ? kGen3Back4bppDeflate : kGen3Front4bppDeflate;
    const bool useShiny = (skin == SKIN_SHINY || skin == SKIN_DARK_SHINY ||
                           skin == SKIN_BLACKOUT_SHINY);
    const uint16_t *pal = back
        ? (useShiny ? kGen3BackPalShiny[dex]  : kGen3BackPalNormal[dex])
        : (useShiny ? kGen3FrontPalShiny[dex] : kGen3FrontPalNormal[dex]);
    uint32_t start = offs[dex], end = offs[dex + 1];
    if (end <= start) return false;

    uint8_t idxbuf[kSpriteW * kSpriteH / 2];   // 2048 B
    unsigned long destlen = (unsigned long)(w * h / 2), srclen = end - start;
    if (puff(idxbuf, &destlen, blob + start, &srclen) != 0) return false;
    if (destlen != (unsigned long)(w * h / 2)) return false;

    // Non-rainbow skins are position-independent: build a 16-entry effective
    // palette once (565 expand -> skin recolour -> tritan) and blit by index.
    // Rainbow skins depend on pixel position, so they recolour per pixel.
    const bool posDep = is_animated(skin);
    uint32_t eff[16];
    if (!posDep) {
        for (int k = 0; k < 16; ++k) {
            uint8_t rr, gg, bb;
            expand565(pal[k], rr, gg, bb);
            applyVariant(skin, 0, w, h, phase, rr, gg, bb);
            if (tritan) applyTritan(rr, gg, bb);
            eff[k] = pack(rr, gg, bb);
        }
    }

    // A few blobs in the shared data (front #1 and #44, back #93) never use
    // index 0: their page is a WHITE palette entry, so they drew as a sprite
    // on an opaque white square. For such a fully opaque frame, treat the
    // corner colour as the page and clear what is connected to the border.
    uint8_t idx[kSpriteW * kSpriteH];
    bool has_clear = false;
    for (int i = 0; i < w * h; ++i) {
        idx[i] = (i & 1) ? (idxbuf[i >> 1] & 0x0F) : (idxbuf[i >> 1] >> 4);
        has_clear |= idx[i] == 0;
    }
    if (!has_clear) clear_border_fill(idx, w, h, idx[0], 0);

    for (int i = 0; i < w * h; ++i) {
        if (idx[i] == 0) { out[i] = 0; continue; }
        if (!posDep) { out[i] = eff[idx[i]]; continue; }
        uint8_t rr, gg, bb;
        expand565(pal[idx[i]], rr, gg, bb);
        applyVariant(skin, i, w, h, phase, rr, gg, bb);
        if (tritan) applyTritan(rr, gg, bb);
        out[i] = pack(rr, gg, bb);
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return true;
}

// ── Gen 2: 56x56 front / 48x48 back, 4 colours ──────────────────────────────
// Format (monster_mesh_pi/tools/gen2_sprite_bake.py): per-dex 4-colour RGB565
// palette sorted lightest (0) to darkest (3); 2bpp, 4 px/byte, leftmost pixel
// in the top bits; raw DEFLATE per sprite. Index 0 is BOTH the page behind
// the monster and any white inside it, so only the index-0 pixels connected
// to the frame's edge are made transparent (flood fill from the border).
//
// Shiny: the Gen 2 headers carry one palette. The shiny one is derived from
// the Gen 3 pair for the same dex and side: each Gen 2 colour finds the Gen 3
// normal colour closest in hue (both compared at the Gen 2 colour's
// brightness) and takes that entry's shiny colour, again at its own
// brightness. So the shading of the Gen 2 art is kept and only the hue moves.
static void to_luma(int r, int g, int b, int Y, int out[3]) {
    const int y0 = luma(r, g, b);
    if (Y <= y0) {                                   // shade towards black
        const int d = y0 < 1 ? 1 : y0;
        out[0] = r * Y / d; out[1] = g * Y / d; out[2] = b * Y / d;
    } else {                                         // tint towards white
        const int d = 255 - y0 < 1 ? 1 : 255 - y0, t = Y - y0;
        out[0] = r + (255 - r) * t / d; out[1] = g + (255 - g) * t / d; out[2] = b + (255 - b) * t / d;
    }
    for (int c = 0; c < 3; ++c) out[c] = out[c] < 0 ? 0 : (out[c] > 255 ? 255 : out[c]);
}

static void gen2_shiny(int dex, bool back, uint8_t rgb[4][3]) {
    const uint16_t* pn = back ? kGen3BackPalNormal[dex] : kGen3FrontPalNormal[dex];
    const uint16_t* ps = back ? kGen3BackPalShiny[dex]  : kGen3FrontPalShiny[dex];
    for (int k = 0; k < 4; ++k) {
        const int Y = luma(rgb[k][0], rgb[k][1], rgb[k][2]);
        int best = -1; long bestd = 0;
        for (int j = 1; j < 16; ++j) {                 // 0 is the transparent slot
            if (pn[j] == 0xF81F) continue;             // unused palette slot
            uint8_t r, g, b;
            expand565(pn[j], r, g, b);
            int n[3];
            to_luma(r, g, b, Y, n);
            const long dr = n[0] - rgb[k][0], dg = n[1] - rgb[k][1], db = n[2] - rgb[k][2];
            const long dy = luma(r, g, b) - Y;
            const long d = 4 * (2 * dr * dr + 4 * dg * dg + 3 * db * db) + dy * dy;
            if (best < 0 || d < bestd) { best = j; bestd = d; }
        }
        if (best < 0) continue;
        uint8_t sr, sg, sb;
        expand565(ps[best], sr, sg, sb);
        int o[3];
        to_luma(sr, sg, sb, Y, o);
        for (int c = 0; c < 3; ++c) rgb[k][c] = (uint8_t)o[c];
    }
}

static bool decode_gen2(int dex, bool back, int skin, bool tritan, uint16_t phase,
                        uint32_t* out, int* out_w, int* out_h) {
    if (dex < 1 || dex > 151) return false;
    const int w = back ? (int)GEN2_BACK_W : (int)GEN2_COLOR_W;
    const int h = back ? (int)GEN2_BACK_H : (int)GEN2_COLOR_H;
    const int n = w * h;
    const uint32_t* offs = back ? kGen2BackDeflateOffsets : kGen2ColorDeflateOffsets;
    const uint8_t*  blob = back ? kGen2BackDeflate : kGen2ColorDeflate;
    const uint16_t* pal  = back ? kGen2BackPalettes[dex] : kGen2ColorPalettes[dex];
    const uint32_t start = offs[dex], end = offs[dex + 1];
    if (end <= start) return false;

    uint8_t packed[56 * 56 / 4];
    unsigned long destlen = (unsigned long)(n / 4), srclen = end - start;
    if (puff(packed, &destlen, blob + start, &srclen) != 0) return false;
    if (destlen != (unsigned long)(n / 4)) return false;

    // 0..3 = palette index; kOut = background reached from the border.
    constexpr uint8_t kOut = 4;
    uint8_t idx[56 * 56];
    for (int i = 0; i < n; ++i) idx[i] = (packed[i >> 2] >> (6 - 2 * (i & 3))) & 3;

    clear_border_fill(idx, w, h, 0, kOut);

    uint8_t rgb[4][3];
    for (int k = 0; k < 4; ++k) expand565(pal[k], rgb[k][0], rgb[k][1], rgb[k][2]);
    if (skin == SKIN_SHINY || skin == SKIN_DARK_SHINY || skin == SKIN_BLACKOUT_SHINY)
        gen2_shiny(dex, back, rgb);

    const bool posDep = is_animated(skin);
    uint32_t eff[4];
    if (!posDep) {
        for (int k = 0; k < 4; ++k) {
            uint8_t rr = rgb[k][0], gg = rgb[k][1], bb = rgb[k][2];
            applyVariant(skin, 0, w, h, phase, rr, gg, bb);
            if (tritan) applyTritan(rr, gg, bb);
            eff[k] = pack(rr, gg, bb);
        }
    }
    bool any = false;
    for (int i = 0; i < n; ++i) {
        const uint8_t k = idx[i];
        if (k == kOut) { out[i] = 0; continue; }
        any = true;
        if (!posDep) { out[i] = eff[k]; continue; }
        uint8_t rr = rgb[k][0], gg = rgb[k][1], bb = rgb[k][2];
        applyVariant(skin, i, w, h, phase, rr, gg, bb);
        if (tritan) applyTritan(rr, gg, bb);
        out[i] = pack(rr, gg, bb);
    }
    if (!any) return false;                    // blank blob: let Gen 3 draw it
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return true;
}

// ── Sprite set ──────────────────────────────────────────────────────────────
namespace {
SpriteSet g_set = SpriteSet::Gen2;
// Cached opaque bounds, [dex][back] = {x, y, w, h}; w == 0 => not computed,
// w == 255 => known empty / bad.
uint8_t g_bounds[387][2][4];
}  // namespace

SpriteSet active_set() { return g_set; }

void set_active_set(SpriteSet s) {
    if (s == g_set) return;
    g_set = s;
    std::memset(g_bounds, 0, sizeof(g_bounds));
}

bool parse_set(const char* text, SpriteSet& out) {
    if (!text) return false;
    std::string t;
    for (const char* p = text; *p; ++p)
        if (!std::isspace((unsigned char)*p)) t += (char)std::tolower((unsigned char)*p);
    if (t == "gen2" || t == "2" || t == "gbc" || t == "g2") { out = SpriteSet::Gen2; return true; }
    if (t == "gen3" || t == "3" || t == "gba" || t == "g3") { out = SpriteSet::Gen3; return true; }
    return false;
}

const char* set_id(SpriteSet s) { return s == SpriteSet::Gen2 ? "gen2" : "gen3"; }

bool decode_into(int dex, bool back, int skin, bool tritan, uint16_t phase,
                 uint32_t* out, int* out_w, int* out_h) {
    if (!out || dex < 1 || dex > 386) return false;
    if (skin < 0 || skin >= SKIN_COUNT) skin = SKIN_REGULAR;
    if (g_set == SpriteSet::Gen2 &&
        decode_gen2(dex, back, skin, tritan, phase, out, out_w, out_h))
        return true;
    return decode_gen3(dex, back, skin, tritan, phase, out, out_w, out_h);
}

bool bounds(int dex, bool back, Bounds& out) {
    if (dex < 1 || dex > 386) return false;
    uint8_t* c = g_bounds[dex][back ? 1 : 0];
    if (c[2] == 0) {
        uint32_t px[kSpriteW * kSpriteH];
        int w = 0, h = 0;
        c[2] = 255;
        if (decode_into(dex, back, SKIN_REGULAR, false, 0, px, &w, &h)) {
            int x0 = w, y0 = h, x1 = -1, y1 = -1;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (px[y * w + x] >> 24) {
                        x0 = std::min(x0, x); x1 = std::max(x1, x);
                        y0 = std::min(y0, y); y1 = std::max(y1, y);
                    }
            if (x1 >= x0) {
                c[0] = (uint8_t)x0; c[1] = (uint8_t)y0;
                c[2] = (uint8_t)(x1 - x0 + 1); c[3] = (uint8_t)(y1 - y0 + 1);
            }
        }
    }
    if (c[2] == 255) return false;
    out.x = c[0]; out.y = c[1]; out.w = c[2]; out.h = c[3];
    return true;
}

SpriteSet set_for(int dex) {
    if (g_set == SpriteSet::Gen3) return SpriteSet::Gen3;
    // A Gen 2 blob that fails to decode also falls back; bounds() knows.
    if (dex < 1 || dex > 151) return SpriteSet::Gen3;
    return kGen2ColorDeflateOffsets[dex + 1] > kGen2ColorDeflateOffsets[dex]
               ? SpriteSet::Gen2 : SpriteSet::Gen3;
}

int native_size(int dex, bool back) {
    if (set_for(dex) == SpriteSet::Gen3) return kSpriteW;
    return back ? (int)GEN2_BACK_W : (int)GEN2_COLOR_W;
}

static inline int scaled(int v, int scale2) { return scale2 == kScaleHalf ? (v + 1) / 2 : v * scale2 / 2; }

bool render_size(int dex, bool back, int scale2, bool crop, int& w, int& h) {
    if (scale2 != kScaleHalf && scale2 != kScale1x && scale2 != kScale2x) return false;
    Bounds b;
    if (!bounds(dex, back, b)) return false;
    if (!crop) { b.w = b.h = native_size(dex, back); }
    w = scaled(b.w, scale2);
    h = scaled(b.h, scale2);
    return true;
}

bool render(int dex, bool back, int skin, bool tritan, uint16_t phase,
            int scale2, bool crop, Image& out) {
    out.w = out.h = 0;
    if (scale2 != kScaleHalf && scale2 != kScale1x && scale2 != kScale2x) return false;
    uint32_t src[kSpriteW * kSpriteH];
    int sw = 0, sh = 0;
    if (!decode_into(dex, back, skin, tritan, phase, src, &sw, &sh)) return false;
    Bounds b{0, 0, sw, sh};
    if (crop && !bounds(dex, back, b)) return false;
    const int w = scaled(b.w, scale2), h = scaled(b.h, scale2);
    out.argb.resize((size_t)w * h);
    uint32_t* dst = out.argb.data();
    auto at = [&](int x, int y) -> uint32_t {      // crop-relative, 0 outside
        return (x < b.w && y < b.h) ? src[(b.y + y) * sw + (b.x + x)] : 0u;
    };
    if (scale2 == kScale1x) {
        for (int y = 0; y < h; ++y)
            std::memcpy(dst + (size_t)y * w, src + (b.y + y) * sw + b.x, (size_t)w * 4);
    } else if (scale2 == kScale2x) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) dst[(size_t)y * w + x] = at(x / 2, y / 2);
    } else {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const uint32_t p[4] = {at(2 * x, 2 * y), at(2 * x + 1, 2 * y),
                                       at(2 * x, 2 * y + 1), at(2 * x + 1, 2 * y + 1)};
                int opaque = 0, bestn = 0, besty = 0;
                uint32_t best = 0;
                for (int i = 0; i < 4; ++i) {
                    if (!(p[i] >> 24)) continue;
                    ++opaque;
                    int cnt = 0;
                    for (int j = 0; j < 4; ++j) cnt += (p[j] == p[i]);
                    const int yv = luma((p[i] >> 16) & 255, (p[i] >> 8) & 255, p[i] & 255);
                    if (cnt > bestn || (cnt == bestn && yv < besty)) { bestn = cnt; besty = yv; best = p[i]; }
                }
                dst[(size_t)y * w + x] = opaque >= 2 ? best : 0u;
            }
        }
    }
    out.w = w; out.h = h;
    return true;
}

bool decode(int dex, bool back, int skin, bool tritan, uint16_t phase, Image& out) {
    out.argb.resize((size_t)kSpriteW * kSpriteH);
    if (!decode_into(dex, back, skin, tritan, phase, out.argb.data(), &out.w, &out.h)) {
        out = Image{};
        return false;
    }
    out.argb.resize((size_t)out.w * out.h);
    return true;
}

}  // namespace mm::sprites
