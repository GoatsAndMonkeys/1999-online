// softkeys.cpp — see softkeys.h.
#include "softkeys.h"

#include "skin.h"
#include "style.h"
#include "widgets.h"
#include "win.h"

#include <algorithm>

namespace mm::ui {
namespace {

// LVGL symbols (U+F000..U+F8FF, UTF-8 lead byte 0xEF) only exist in the
// built-in Montserrat fonts, not in the skin's UI font.
bool has_symbol(const std::string& s) {
    for (unsigned char ch : s)
        if (ch == 0xEF) return true;
    return false;
}

const lv_font_t* cap_font(const std::string& s, bool bold) {
    if (has_symbol(s)) return font_small();
    return bold ? font_ui_bold() : font_ui();
}

int text_w(const std::string& s, const lv_font_t* f) {
    lv_point_t p{};
    lv_text_get_size(&p, s.c_str(), f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

// One keycap: a raised button-face box with the label centred.
void cap(lv_obj_t* bar, const SoftKey& k, int x, int w, int h, bool selected) {
    const Skin& s = skin();
    uint32_t bg = k.color ? k.color : (selected ? s.sel : s.face);
    uint32_t fg = k.color ? 0x101418 : (selected ? s.sel_text : s.text);
    if (!k.enabled) {
        bg = k.color ? k.color : s.face;
        fg = s.dim == bg ? s.lo : s.dim;
    }
    lv_obj_t* b = win::frame(bar, x, 0, w, h, win::Edge::Raised, bg, s.radius ? std::min<int>(s.radius, 3) : 0);
    if (selected && k.color) {
        // A coloured cap shows focus with an outline in the text colour.
        lv_obj_set_style_outline_width(b, 1, 0);
        lv_obj_set_style_outline_color(b, c(s.text), 0);
        lv_obj_set_style_outline_pad(b, 0, 0);
    }
    lv_obj_t* l = label(b, k.text, 0, 0, cap_font(k.text, selected), fg, w - 4);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
}

} // namespace

bool SoftKeys::empty() const {
    for (const auto& k : key)
        if (!k.text.empty()) return false;
    return left.text.empty() && right.text.empty();
}

SoftKeys& SoftKeys::set(int i, const std::string& text, bool enabled, uint32_t color) {
    if (i >= 0 && i < kSoftKeyCount) key[i] = SoftKey{text, enabled, color};
    return *this;
}

bool SoftKeys::operator==(const SoftKeys& o) const {
    for (int i = 0; i < kSoftKeyCount; ++i)
        if (key[i] != o.key[i]) return false;
    return left == o.left && right == o.right && selected == o.selected;
}

int softkey_slot(const KeyEvent& e) {
    if (e.key != Key::Char) return kSoftNone;
    if (e.ch >= '4' && e.ch <= '8') return static_cast<int>(e.ch - '4');
    if (e.ch == '3') return kSoftLeft;
    if (e.ch == '9') return kSoftRight;
    return kSoftNone;
}

lv_obj_t* softkey_bar(lv_obj_t* parent, const SoftKeys& keys, int x, int y, int h, int screen_x) {
    lv_obj_t* bar = box(parent, x, y, kScreenW, h);
    // Everything below is in screen x; `ox` maps it into the bar.
    const int ox = -screen_x - x;
    auto filled = [&](int i) { return i >= 0 && i < kSoftKeyCount && !keys.key[i].text.empty(); };

    for (int i = 0; i < kSoftKeyCount; ++i) {
        const SoftKey& k = keys.key[i];
        if (k.text.empty()) continue;
        const bool sel = i == keys.selected;
        // Room: half a pitch each side, or a whole pitch toward a free
        // neighbour; never past the corner caps. Neighbours' rooms never overlap.
        const int ctr = softkey_center(i);
        const int lo = std::max(kSoftCornerLeftW + 1, ctr - (filled(i - 1) ? kSoftKeyPitch / 2 : kSoftKeyPitch - 1));
        const int hi = std::min(kSoftCornerRightX - 1, ctr + (filled(i + 1) ? kSoftKeyPitch / 2 : kSoftKeyPitch - 1));
        const int want = text_w(k.text, cap_font(k.text, sel)) + 10;
        const int w = std::clamp(want, std::min(kSoftKeyW - 6, hi - lo - 2), hi - lo - 2);
        const int left = std::clamp(ctr - w / 2, lo + 1, hi - 1 - w);
        cap(bar, k, left + ox, w, h, sel);
    }
    if (!keys.left.text.empty()) cap(bar, keys.left, 0 + ox, kSoftCornerLeftW, h, false);
    if (!keys.right.text.empty())
        cap(bar, keys.right, kSoftCornerRightX + ox, kScreenW - kSoftCornerRightX, h, false);
    return bar;
}

} // namespace mm::ui
