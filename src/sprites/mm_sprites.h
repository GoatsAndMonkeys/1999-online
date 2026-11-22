#pragma once
// mm_sprites — LVGL-free, SDL-free decoder for the monster sprites, with the
// 12 colour "skins" (breeding::Skin 0..11) and the optional tritanopia palette
// transform. Two sprite sets, chosen globally (set_active_set):
//
//   Gen2 (default)  Game Boy Color era, 4 colours, 2bpp: 56x56 fronts
//                   (Gen2ColorIcons.h) and 48x48 backs (Gen2BackIcons.h),
//                   dex 1..151 only.
//   Gen3            64x64 4bpp-indexed (Gen3Front4bpp.h / Gen3Back4bpp.h),
//                   dex 1..386.
//
// With Gen2 active, a dex above 151 (or any Gen 2 decode failure) silently
// falls back to the Gen 3 sprite, so always use the size a call reports.
//
// Port of monster_mesh_pi/src/terminal/Gen2SpriteCache.cpp: the recolour math
// is kept identical so sprites match the terminal app pixel-for-pixel.

#include <cstdint>
#include <vector>

namespace mm::sprites {

// Skin ids — identical order to breeding::Skin and Gen2SpriteCache VAR_*.
enum : int {
    SKIN_REGULAR = 0, SKIN_SHINY, SKIN_PINK, SKIN_RAINBOW,
    SKIN_DARK, SKIN_DARK_SHINY, SKIN_DARK_PINK, SKIN_DARK_RAINBOW,
    SKIN_BLACKOUT, SKIN_BLACKOUT_SHINY, SKIN_BLACKOUT_PINK, SKIN_BLACKOUT_RAINBOW,
    SKIN_COUNT
};

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> argb;   // 0xAARRGGBB per pixel, row-major; transparent -> 0x00000000
};

// ── Sprite set ──────────────────────────────────────────────────────────────
enum class SpriteSet { Gen2, Gen3 };
SpriteSet active_set();
// Changing the set invalidates every cached bound; lv_sprites callers must
// also call mm::sprites::clear_cache() (lv_sprites.h).
void set_active_set(SpriteSet s);
// "gen2" / "gen3" (case-insensitive, also "2" / "3" / "gbc" / "gba").
bool parse_set(const char* text, SpriteSet& out);
const char* set_id(SpriteSet s);
// The set that really draws `dex` right now (Gen3 when dex > 151).
SpriteSet set_for(int dex);
// Native frame of `dex` in the active set: 56 (Gen 2 front), 48 (Gen 2 back)
// or 64 (Gen 3). Sprites are square.
int native_size(int dex, bool back);

// Decode sprite `dex` (1..386), front or back, with `skin` (0..11; out of range
// -> Regular) applied. `phase` animates the rainbow skins (same units as
// Gen2SpriteCache: hue offset = phase*20 degrees; ignored for other skins).
// `tritan` additionally runs the colours through the Machado tritanopia matrix.
// Returns false on bad dex / decode failure (out is left empty).
bool decode(int dex, bool back, int skin, bool tritan, uint16_t phase, Image& out);

// Same as decode() but writes into a caller-owned buffer of 64*64 uint32_t
// (0xAARRGGBB), packed row-major at the REPORTED width (56, 48 or 64).
// Avoids allocation for per-frame rainbow re-decodes.
bool decode_into(int dex, bool back, int skin, bool tritan, uint16_t phase,
                 uint32_t* out, int* out_w = nullptr, int* out_h = nullptr);

// Largest native sprite (the Gen 3 frame); buffers for decode_into are this big.
constexpr int kSpriteW = 64;
constexpr int kSpriteH = 64;

// ── Pixel-perfect scaling ───────────────────────────────────────────────────
// Sprites are only ever drawn at an integer scale. `scale2` counts half
// steps: 1 = exact half, 2 = 1x, 4 = 2x.
constexpr int kScaleHalf = 1, kScale1x = 2, kScale2x = 4;

// Bounding box of the opaque pixels in the native frame (cached per dex/side
// for the active set). false on bad dex or an empty sprite.
struct Bounds { int x = 0, y = 0, w = 0, h = 0; };
bool bounds(int dex, bool back, Bounds& out);

// decode() + optional crop to bounds() + integer scale. 2x repeats pixels;
// half folds each 2x2 block into ONE of its own colours (no blending): the
// block is transparent when fewer than two of its pixels are opaque, else it
// takes the most common opaque colour, darkest on a tie, which keeps the
// outline. `out` is resized to the result.
bool render(int dex, bool back, int skin, bool tritan, uint16_t phase,
            int scale2, bool crop, Image& out);
// Size render() would produce (no decode beyond the cached bounds).
bool render_size(int dex, bool back, int scale2, bool crop, int& w, int& h);

// True for the position/phase-dependent rainbow skins (Rainbow, Dark-Rainbow,
// Blackout-Rainbow) that must be re-decoded each animation frame.
bool is_animated(int skin);

}  // namespace mm::sprites
