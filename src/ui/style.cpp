#include <vector>
#include "style.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace mm::ui {

uint32_t type_color_hex(int t) {
    static constexpr uint32_t kColors[16] = {
        0xa8a878, 0xc03028, 0xa890f0, 0xa040a0, 0xe0c068, 0xb8a038, 0x68a090, 0xa8b820,
        0x705898, 0xf08030, 0x6890f0, 0x78c850, 0xf8d030, 0xf85888, 0x98d8d8, 0x7038f8,
    };
    return (t >= 0 && t < 16) ? kColors[t] : 0x68a090;
}

lv_color_t type_color(int t) { return c(type_color_hex(t)); }

const char* type_name(int t) {
    static constexpr const char* kNames[16] = {
        "NORMAL", "FIGHT", "FLYING", "POISON", "GROUND", "ROCK", "BIRD", "BUG",
        "GHOST", "FIRE", "WATER", "GRASS", "ELECTR", "PSYCHC", "ICE", "DRAGON",
    };
    return (t >= 0 && t < 16) ? kNames[t] : "???";
}

lv_color_t hp_color(int hp, int max_hp) {
    if (max_hp <= 0) return c(color::kDim);
    if (hp * 2 > max_hp) return c(color::kGood);
    if (hp * 5 > max_hp) return c(color::kWarn);
    return c(color::kBad);
}

const char* skin_name(int skin) {
    static constexpr const char* kNames[12] = {
        "Regular", "Shiny", "Pink", "Rainbow",
        "Dark", "Dark Shiny", "Dark Pink", "Dark Rainbow",
        "Blackout", "Blackout Shiny", "Blackout Pink", "Blackout Rainbow",
    };
    return (skin >= 0 && skin < 12) ? kNames[skin] : "?";
}

lv_color_t skin_color(int skin) {
    switch (skin % 4) {
        case 1: return c(color::kAccent);   // shiny
        case 2: return c(0xf890c8);         // pink
        case 3: return c(0x80e0f8);         // rainbow
        default: return skin >= 4 ? c(0xa088c8) : c(color::kDim);
    }
}

namespace {
lv_font_t* g_ui = nullptr;
lv_font_t* g_ui_bold = nullptr;
}

void fonts_init(const char* assets_root) {
#if LV_USE_FREETYPE
    if (g_ui) return;
    std::string path;
    // First readable of: PC1999_FONT, the app's own ui.ttf (source tree or
    // installed), then a plain system sans. None: the built-in font is used.
    std::vector<std::string> tries;
    if (const char* env = std::getenv("PC1999_FONT")) tries.emplace_back(env);
    if (assets_root && *assets_root) tries.push_back(std::string(assets_root) + "/fonts/ui.ttf");
    tries.emplace_back("/usr/share/1999online/fonts/ui.ttf");
    tries.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    tries.emplace_back("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
    tries.emplace_back("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf");
    for (const std::string& t : tries) {
        if (FILE* f = std::fopen(t.c_str(), "rb")) {
            std::fclose(f);
            path = t;
            break;
        }
    }
    if (path.empty()) return;
    g_ui = lv_freetype_font_create(path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 11,
                                   LV_FREETYPE_FONT_STYLE_NORMAL);
    g_ui_bold = lv_freetype_font_create(path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 11,
                                        LV_FREETYPE_FONT_STYLE_BOLD);
#else
    (void)assets_root;
#endif
}

const lv_font_t* font_ui()      { return g_ui ? g_ui : &lv_font_montserrat_10; }
const lv_font_t* font_ui_bold() { return g_ui_bold ? g_ui_bold : font_ui(); }
const lv_font_t* font_icon()    { return &lv_font_montserrat_20; }

const lv_font_t* font_small() { return &lv_font_montserrat_10; }
const lv_font_t* font_body()  { return &lv_font_montserrat_12; }
const lv_font_t* font_title() { return &lv_font_montserrat_14; }
const lv_font_t* font_big()   { return &lv_font_montserrat_18; }

} // namespace mm::ui
