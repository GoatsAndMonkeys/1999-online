// skin.h — 1999 Online skins.
//
// A skin is one row of values: colours, border style, title-bar style, corner
// radii. Every windowed screen draws through ui/win.h, which reads the current
// skin, so a new skin is one more entry in kSkins (skin.cpp) and no new code.
// The legacy MonsterMesh palette (style.h color::k*) is derived from the skin
// too, so the game screens follow it.
#pragma once

#include <cstdint>

namespace mm::ui {

enum class TitleFill : uint8_t {
    Solid,        // title_a
    GradH,        // title_a -> title_b, left to right
    GradV,        // title_a -> title_b, top to bottom
    Stripes,      // title_a background with 1px title_b pinstripes (classic Mac)
};

struct Skin {
    const char* id;
    const char* name;

    uint32_t desk;                 // behind the window
    uint32_t face;                 // window and button face
    uint32_t text;                 // text on face
    uint32_t dim;                  // secondary text
    uint32_t field;                // list / input background
    uint32_t field_text;
    uint32_t sel;                  // selected row
    uint32_t sel_text;

    TitleFill title_fill;
    uint32_t title_a;
    uint32_t title_b;
    uint32_t title_text;
    bool title_center;
    bool title_upper;
    bool title_tab;                // BeOS: the title bar is only as wide as its text
    bool close_left;
    uint32_t close_bg;
    uint32_t close_fg;             // == close_bg: empty close box (classic Mac)
    uint8_t close_radius;

    uint32_t status_a;             // status strip, top to bottom gradient
    uint32_t status_b;
    uint32_t status_text;
    bool status_panels;            // sunken panels (Windows status bar)

    uint32_t hint_a;               // key-hint strip
    uint32_t hint_b;
    uint32_t hint_text;

    bool bevel;                    // true: two-tone 3D edges (hi/lo). false: flat 1px `lo` border
    uint32_t hi;
    uint32_t lo;
    uint8_t radius;                // button corner radius (flat skins)
    uint8_t win_radius;            // window top corner radius

    uint32_t card;                 // info card (tooltip look)
    uint32_t card_text;
    uint32_t card_border;

    uint32_t me;                   // my screen name in chats
    uint32_t them;                 // their screen name
    uint32_t accent;               // logo block

    bool dark;                     // dark face: status colours stay bright
};

int skin_count();
const Skin& skin_at(int index);
const Skin& skin();                // current
int skin_index();
// Sets the current skin and refreshes the legacy palette. The Router rebuilds
// the chrome and every open screen (Router::apply_skin calls this).
void set_skin(int index);
// -1 if unknown.
int skin_from_id(const char* id);

} // namespace mm::ui
