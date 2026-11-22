// win.h — skinned building blocks for 1999 Online windowed screens.
//
// Everything here reads the current skin (skin.h): never hard-code a colour in
// a windowed screen. Coordinates are relative to the parent, usually the
// kClientW x kClientH client area a windowed Screen::build() receives.
// Like widgets.h, nothing uses LVGL focus: screens drive selection from
// on_key() and repaint.
#pragma once

#include "keys.h"
#include "skin.h"
#include "style.h"

#include "lvgl.h"

#include <string>
#include <vector>

namespace mm::ui::win {

enum class Edge : uint8_t { None, Raised, Sunken, Line };

// Filled box with a skinned edge. Bevel skins draw two-tone 3D edges; flat
// skins draw a 1px border (with `radius` for Raised). Children may use the
// full box; keep them 1-2px inside the edge.
lv_obj_t* frame(lv_obj_t* parent, int x, int y, int w, int h, Edge edge, uint32_t bg, int radius = 0);

// Vertical or horizontal 2-colour fill on any object (a == b: solid).
void fill(lv_obj_t* obj, uint32_t a, uint32_t b, bool horizontal = false);

// Text in the UI font. width <= 0: natural size. width > 0: one line, clipped
// with "...". Default colour is the skin's text colour.
lv_obj_t* text(lv_obj_t* parent, const std::string& s, int x, int y, int width = 0);
lv_obj_t* text(lv_obj_t* parent, const std::string& s, int x, int y, uint32_t color,
               int width = 0, bool bold = false);
// Wrapped, multi-line.
lv_obj_t* text_wrap(lv_obj_t* parent, const std::string& s, int x, int y, int width, uint32_t color);

// Push button, 16px tall by default. `focused` marks the one Enter activates.
lv_obj_t* button(lv_obj_t* parent, const std::string& label, int x, int y, int w,
                 bool focused = false, int h = 16);

// Sunken field (list / text box background). Returns the box; draw inside it.
lv_obj_t* field(lv_obj_t* parent, int x, int y, int w, int h);
// Tooltip-style info card. Returns the box.
lv_obj_t* card(lv_obj_t* parent, int x, int y, int w, int h);
// Small tab row for in-window tabs; returns the tab strip height used (14).
int tabs(lv_obj_t* parent, int x, int y, const std::vector<std::string>& names, int active);
// Horizontal meter (HP, volume, progress): sunken track, `value` of `max`.
lv_obj_t* meter(lv_obj_t* parent, int x, int y, int w, int h, int value, int max, uint32_t color);

// One line of an in-field list.
struct Row {
    std::string text;
    std::string short_text;   // used when `text` does not fit (group headers)
    std::string right;        // right-aligned detail
    int indent = 0;           // px
    bool header = false;      // bold, not indented
    bool dim = false;
    bool bold = false;
    uint32_t color = 0;       // 0: field text colour
    int tag = 0;              // caller data
    bool selectable = true;
};

// Keyboard-driven list inside a sunken field: Up/Down move (skipping
// non-selectable rows), scrolls to keep the selection visible.
class ListBox {
public:
    ListBox(lv_obj_t* parent, int x, int y, int w, int h);

    void set_rows(std::vector<Row> rows);     // keeps the selection index if still valid
    const std::vector<Row>& rows() const { return rows_; }
    int selected() const { return sel_; }     // -1 if empty
    const Row* selected_row() const;
    void select(int index);
    bool on_key(const KeyEvent& e);           // true if the selection moved
    void set_active(bool active);             // inactive: selection drawn dim (pane without focus)
    void repaint();

private:
    lv_obj_t* box_ = nullptr;
    std::vector<Row> rows_;
    int w_ = 0, h_ = 0, row_h_ = 13, visible_ = 0;
    int sel_ = -1, top_ = 0;
    bool active_ = true;
};

// Single-line text input inside a sunken field. Feed it every KeyEvent; it
// reads KeyEvent::raw so punctuation and capitals type normally. Enter, Esc,
// Tab and real arrow keys are NOT consumed (the screen handles them).
class TextInput {
public:
    TextInput(lv_obj_t* parent, int x, int y, int w, int h = 15, int max_bytes = 200);
    bool on_key(const KeyEvent& e);           // true if consumed (typed or erased)
    const std::string& value() const { return value_; }
    void set_value(const std::string& v);
    void set_placeholder(const std::string& p);

private:
    void repaint();
    lv_obj_t* label_ = nullptr;
    std::string value_, placeholder_;
    int w_ = 0, max_ = 200;
};

} // namespace mm::ui::win
