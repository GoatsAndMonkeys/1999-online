// widgets.h — small LVGL building blocks shared by every MonsterMesh screen.
//
// All helpers create plain lv_obj children with the MonsterMesh style and no
// LVGL focus/group behaviour: screens drive selection themselves from
// Screen::on_key(), which keeps the keyboard model identical on the SDL sim
// and the device.
#pragma once

#include "keys.h"
#include "style.h"

#include "lvgl.h"

#include <functional>
#include <string>
#include <vector>

namespace mm::ui {

// ── Basic objects ───────────────────────────────────────────────────────────
// Bare container: no padding, no border, no scrollbars, transparent.
lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h);
// Rounded dark card with a 1px border.
lv_obj_t* panel(lv_obj_t* parent, int x, int y, int w, int h);
// Label; width <= 0 means LV_SIZE_CONTENT (no wrap). With a fixed width it is
// ONE line, clipped with "...". Use text_block() for wrapped text.
lv_obj_t* label(lv_obj_t* parent, const std::string& text, int x, int y,
                const lv_font_t* font = nullptr, uint32_t color = color::kText, int width = 0);
// Wrapped multi-line text of a fixed width (height grows with content).
lv_obj_t* text_block(lv_obj_t* parent, const std::string& text, int x, int y, int width,
                     const lv_font_t* font = nullptr, uint32_t color = color::kText);
void set_text(lv_obj_t* lbl, const std::string& text);
// Small coloured pill, e.g. a type or skin badge. Returns the pill.
lv_obj_t* chip(lv_obj_t* parent, const std::string& text, int x, int y,
               lv_color_t bg, uint32_t fg = 0x101418);

// ── Bars ────────────────────────────────────────────────────────────────────
// Horizontal bar (lv_bar) with a dark track. Range 0..max.
lv_obj_t* bar(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t fill);
void bar_set(lv_obj_t* b, int value, int max, bool animate = false);
// HP bar: colour follows hp_color(). Optionally a "hp/max" label to its right
// (pass a label from hp_label or nullptr).
lv_obj_t* hp_bar(lv_obj_t* parent, int x, int y, int w, int hp, int max_hp);
void hp_bar_set(lv_obj_t* b, int hp, int max_hp, bool animate = false);

// ── Sprites ─────────────────────────────────────────────────────────────────
// Monster sprites are pixel art and are ONLY drawn at an integer scale:
// 2x, 1x, or exactly half. Callers ask for a box in pixels; the sprite gets
// the largest of those scales that fits and is centred in the box, no
// smoothing. Native 1x is 56px (front) / 48px (back) with the default Gen 2
// set, 64px with Gen 3 (and for dex > 151), see mm::sprites::SpriteSet.
enum class SpriteFit {
    // Trim to the monster's own bounding box first, then fit. Best for icons:
    // a small monster is not shrunk for the empty margin around it.
    Crop,
    // Fit the whole native frame (56 / 48 / 64 px), bottom-aligned in the box
    // so monsters stand on the same line. Use for battle scenes; give a box of
    // at least 64x64 (1x) or 128x128 (2x) so the Gen 3 fallback fits too.
    Frame,
};
// The lv_image is sized to the box, top-left at (x, y); a sprite larger than
// the box even at half size is clipped to it (Crop keeps the head: a sprite
// too tall is top-aligned). Bad dex shows nothing.
// Sizes to remember (Gen 2 set): 56x56 holds any front at 1x, 28x28 any front
// at half; nearly every Gen 2 monster fills 48..56 px of its frame, so a box
// under 56 px means half size.
lv_obj_t* sprite_box(lv_obj_t* parent, int dex, int x, int y, int box_w, int box_h,
                     bool back = false, int skin = 0, bool tritan = false,
                     SpriteFit fit = SpriteFit::Crop);
// Compatibility: `scale_256` of the old 64px sprite (128 = 32px, 256 = 64px,
// 384 = 96px ...). Same as sprite_box() with a square box of 64*scale_256/256
// px and SpriteFit::Crop; the scale itself is never applied to the pixels.
lv_obj_t* sprite(lv_obj_t* parent, int dex, int x, int y, int scale_256 = 256,
                 bool back = false, int skin = 0, bool tritan = false);
// Swap the monster shown by an image from sprite_box()/sprite(); keeps its
// box and fit, re-picks the integer scale.
void sprite_set(lv_obj_t* img, int dex, bool back = false, int skin = 0, bool tritan = false);
// Integer scale sprite_box() would pick, in half steps (1 = half, 2 = 1x,
// 4 = 2x), and the pixel size the monster is drawn at.
int sprite_scale2(int dex, bool back, int box_w, int box_h, SpriteFit fit = SpriteFit::Crop,
                  int* out_w = nullptr, int* out_h = nullptr);

// Animates a rainbow-skin sprite (no-op for other skins). Call from
// Screen::tick(); `slot` is one of mm::sprites::kAnimSlots (0..3) and must be
// unique among sprites animating at the same time.
void sprite_animate(lv_obj_t* img, int slot, int dex, bool back, int skin,
                    uint32_t now_ms, bool tritan = false);

// ── List view ───────────────────────────────────────────────────────────────
// Keyboard-driven, virtualised vertical list: only the visible rows exist as
// LVGL objects; they are refilled when the selection scrolls. Rows have a
// fixed height. The row builder is called with an EMPTY row object (already
// sized, styled, and highlighted if selected) and fills it with children.
class ListView {
public:
    using RowBuilder = std::function<void(lv_obj_t* row, int index, bool selected)>;

    ListView(lv_obj_t* parent, int x, int y, int w, int h, int row_h, RowBuilder builder);

    void set_count(int count);            // keeps selection in range; refreshes
    int count() const { return count_; }
    int selected() const { return sel_; }
    void select(int index);               // clamps; scrolls into view; refreshes
    void refresh();                       // rebuild visible rows (data changed)

    // Up/Down move (wraps), Left/Right page. Returns true if consumed.
    bool on_key(const KeyEvent& e);

    lv_obj_t* obj() const { return root_; }
    // Optional text drawn centred when count == 0.
    void set_empty_text(const std::string& text);

private:
    lv_obj_t* root_ = nullptr;
    lv_obj_t* empty_ = nullptr;
    lv_obj_t* scroll_ = nullptr;          // thin scroll indicator
    std::vector<lv_obj_t*> rows_;
    RowBuilder builder_;
    int w_ = 0, h_ = 0, row_h_ = 0, visible_ = 0;
    int count_ = 0, sel_ = 0, top_ = 0;
    std::string empty_text_;
};

// ── Menu grid ───────────────────────────────────────────────────────────────
// Selection helper for an N-column grid of items (no LVGL objects).
struct GridNav {
    int cols = 1;
    int count = 0;
    int sel = 0;
    bool on_key(const KeyEvent& e);       // arrows move; returns true if sel changed
};

// ── Toast (on lv_layer_top, auto-hides) ─────────────────────────────────────
void toast(const std::string& text, uint32_t ms = 2200, uint32_t accent = color::kAccent);
void toast_hide();

// ── Modal (on lv_layer_top) ────────────────────────────────────────────────
// A centred dialog with a title, body text and up to 3 options. The caller
// drives it: create, then forward keys to on_key(); it calls `done(choice)`
// (-1 = cancelled with B) and deletes itself.
// Options are soft keys (see softkeys.h): keys 4/6/8 pick, key 3 cancels;
// Left/Right + Enter still work, Esc cancels (choice -1).
class Modal {
public:
    using Done = std::function<void(int choice)>;
    Modal(const std::string& title, const std::string& body,
          std::vector<std::string> options, Done done);
    ~Modal();
    bool on_key(const KeyEvent& e);       // always consumes
    bool open() const { return root_ != nullptr; }

private:
    void close(int choice);
    void paint();
    int slot_of(int i) const;             // option -> soft key slot (0 = key 4)
    lv_obj_t* root_ = nullptr;
    lv_obj_t* bar_ = nullptr;             // soft-key strip along the bottom edge
    std::vector<std::string> options_;
    int sel_ = 0;
    Done done_;
};

// ── Text helpers ────────────────────────────────────────────────────────────
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
// Species name for a national dex number ("BULBASAUR"; Gen 2/3 dex use the
// Gen 3 table name when known, else "#252").
std::string species_name(int dex);
// "Pikachu" style (first letter upper, rest lower).
std::string title_case(const std::string& s);

} // namespace mm::ui
