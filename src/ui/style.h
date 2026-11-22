// style.h — MonsterMesh look for the 320x170 CardputerZero panel.
//
// One dark retro palette, Montserrat built-in fonts only (identical on the
// SDL sim and the device), and the fixed screen geometry every screen lays
// out against. Screens should use these instead of literal colours/sizes.
#pragma once

#include "lvgl.h"

#include <cstdint>

namespace mm::ui {

// ── Geometry ────────────────────────────────────────────────────────────────
constexpr int kScreenW = 320;
constexpr int kScreenH = 170;
constexpr int kHeaderH = 18;                       // title + daemon status
constexpr int kFooterH = 14;                       // key hints
constexpr int kBodyW   = kScreenW;
constexpr int kBodyH   = kScreenH - kHeaderH - kFooterH;   // 138
constexpr int kPad     = 4;

// 1999 Online windowed screens (Screen::windowed() == true): a status strip on
// top, one window with a title bar, and the key-hint strip. They lay out
// against the client area, kClientW x kClientH, with (0,0) at its top-left.
constexpr int kStatusH = 14;
constexpr int kWinX = 2;
constexpr int kWinY = kStatusH + 1;                  // 15
constexpr int kWinW = kScreenW - 2 * kWinX;          // 316
constexpr int kWinH = kScreenH - kWinY - kFooterH - 1;   // 140
constexpr int kTitleH = 15;
constexpr int kClientX = kWinX + 2;                  // 4
constexpr int kClientY = kWinY + 1 + kTitleH + 1;    // 32
constexpr int kClientW = kWinW - 4;                  // 312
constexpr int kClientH = kWinY + kWinH - 2 - kClientY;   // 121

// ── Palette ─────────────────────────────────────────────────────────────────
// Legacy MonsterMesh palette. These are variables, not constants: set_skin()
// (skin.h) repoints them at the current skin. New screens use skin() directly.
namespace color {
inline uint32_t kBg        = 0x101418;   // screen background
inline uint32_t kPanel     = 0x1b2129;   // cards / list rows
inline uint32_t kPanelHi   = 0x2a3440;   // selected row
inline uint32_t kBorder    = 0x323c48;
inline uint32_t kBar       = 0x0b0e11;   // header / footer
inline uint32_t kAccent    = 0xf8d030;   // selection, titles (electric yellow)
inline uint32_t kText      = 0xe8e8e8;
inline uint32_t kDim       = 0x8a94a0;
inline uint32_t kGood      = 0x48d068;   // HP high, online
inline uint32_t kWarn      = 0xf8b030;   // HP mid, demo
inline uint32_t kBad       = 0xe85040;   // HP low, offline
inline uint32_t kInfo      = 0x58a8f8;   // EXP bar, links
inline uint32_t kShiny     = 0xf0c8ff;
} // namespace color

inline lv_color_t c(uint32_t hex) { return lv_color_hex(hex); }

// Gen 1 type colours. Index = GEN1_TYPE_NAMES order in
// ../src/battle/showdown_gen1_typechart.h (also GEN1_BASE_STATS[dex].type1/2):
// 0 Normal 1 Fighting 2 Flying 3 Poison 4 Ground 5 Rock 6 Bird 7 Bug 8 Ghost
// 9 Fire 10 Water 11 Grass 12 Electric 13 Psychic 14 Ice 15 Dragon.
lv_color_t type_color(int gen1_type);
uint32_t type_color_hex(int gen1_type);   // same colour as 0xRRGGBB
const char* type_name(int gen1_type);

// HP bar colour for a fraction 0..1 (green > 50%, yellow > 20%, red).
lv_color_t hp_color(int hp, int max_hp);

// Skin display name + accent colour (skin ids = mm::sprites::SKIN_*).
const char* skin_name(int skin);
lv_color_t skin_color(int skin);

// ── Fonts (built-in Montserrat) ─────────────────────────────────────────────
const lv_font_t* font_small();   // 10 px — dense lists, footer
const lv_font_t* font_body();    // 12 px — default
const lv_font_t* font_title();   // 14 px — headers
const lv_font_t* font_big();     // 18 px — hero numbers / names

// 1999 Online UI font. If a TrueType file is found (env PC1999_FONT, else
// <assets>/fonts/ui.ttf) it is rendered at 11 px through FreeType with a
// synthesized bold; otherwise both fall back to Montserrat 10.
void fonts_init(const char* assets_root);
const lv_font_t* font_ui();
const lv_font_t* font_ui_bold();
const lv_font_t* font_icon();    // 20 px, LVGL symbols for launcher icons

} // namespace mm::ui
