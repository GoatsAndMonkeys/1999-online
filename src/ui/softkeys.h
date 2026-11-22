// softkeys.h — choice labels drawn right above the physical number keys.
//
// The Cardputer Zero's top keyboard row sits directly under the display. Like
// M5's own launcher (APPLaunch), a screen offers its choices on keys 4..8 and
// draws each label straight above its key; the position over the key is the
// cue, no key numbers are printed. Keys 3 and 9 sit under the far left and far
// right edges of the display and carry small corner labels (back / extra).
//
// Geometry (M5's simulator frame, src/app/desktop_simulator_frame.h and
// desktop_virtual_keypad.cpp): the display is at frame x = 155, key column c
// (0 = key 1) spans frame x = 8 + c*57 .. +50. In screen coordinates key 4
// spans x 24..74 (centre 49), then every 57 px: 106, 163, 220, 277. Key 3
// ends at x 17, key 9 starts at x 309.
//
// A screen that returns non-empty Screen::softkeys() gets the bar drawn by the
// Router in place of the footer hint strip (the bottom 14 px of the display, in
// every skin, legacy or windowed). Screens still handle the digit keys in
// on_key(); softkey_slot() turns a KeyEvent into a slot.
#pragma once

#include "keys.h"

#include "lvgl.h"

#include <cstdint>
#include <string>

namespace mm::ui {

constexpr int kSoftKeyCount = 5;          // keys 4..8
constexpr int kSoftKeyPitch = 57;         // key column step, px
constexpr int kSoftKeyW = 50;             // key width, px
constexpr int kSoftKeyX0 = 24;            // screen x of key 4's left edge
// Screen x of the centre of key 4 + i (i = 0..4): 49, 106, 163, 220, 277.
constexpr int softkey_center(int i) { return kSoftKeyX0 + kSoftKeyW / 2 + i * kSoftKeyPitch; }
constexpr int kSoftCornerLeftW = 19;      // key 3 shows under x 0..17
constexpr int kSoftCornerRightX = 302;    // key 9 shows under x 309..319 (key 8 ends at 302)

// softkey_slot() results besides 0..4 (keys 4..8).
constexpr int kSoftNone = -1;
constexpr int kSoftLeft = 5;              // key 3
constexpr int kSoftRight = 6;             // key 9

// Corner label for "back" (key 3).
#define MM_SOFT_BACK LV_SYMBOL_LEFT

struct SoftKey {
    std::string text;         // empty: no label (the key does nothing)
    bool enabled = true;      // false: drawn dimmed
    uint32_t color = 0;       // cap colour; 0 = the skin's button face

    bool operator==(const SoftKey& o) const {
        return text == o.text && enabled == o.enabled && color == o.color;
    }
    bool operator!=(const SoftKey& o) const { return !(*this == o); }
};

struct SoftKeys {
    SoftKey key[kSoftKeyCount];   // keys 4..8, left to right
    SoftKey left;                 // key 3: a symbol or <= 2 characters
    SoftKey right;                // key 9: same
    int selected = -1;            // 0..4: the key Enter would press (arrow focus)

    bool empty() const;
    // Sets key[i]; returns *this for chaining.
    SoftKeys& set(int i, const std::string& text, bool enabled = true, uint32_t color = 0);

    bool operator==(const SoftKeys& o) const;
    bool operator!=(const SoftKeys& o) const { return !(*this == o); }
};

// '4'..'8' -> 0..4, '3' -> kSoftLeft, '9' -> kSoftRight, anything else -> kSoftNone.
int softkey_slot(const KeyEvent& e);

// Draws the bar into `parent` as a transparent kScreenW x h box at (x, y).
// `screen_x` is the screen x of the parent's left edge, so the caps land over
// the keys whatever the parent (0 for the Router footer or lv_layer_top()).
// A cap is as wide as its text needs: up to one key pitch, or out to the
// centre of a free neighbouring key on that side (so a label on key 4 with
// key 5 empty may be ~86 px, one on key 6 with 5 and 7 empty ~112 px). It
// stays centred over its key as far as the corners allow. Returns the bar.
lv_obj_t* softkey_bar(lv_obj_t* parent, const SoftKeys& keys, int x, int y, int h, int screen_x = 0);

} // namespace mm::ui
