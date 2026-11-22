#include "win.h"

#include "widgets.h"

#include <algorithm>

namespace mm::ui::win {
namespace {

void line(lv_obj_t* parent, int x, int y, int w, int h, uint32_t col) {
    lv_obj_t* l = box(parent, x, y, w, h);
    lv_obj_set_style_bg_color(l, c(col), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
}

} // namespace

void fill(lv_obj_t* obj, uint32_t a, uint32_t b, bool horizontal) {
    lv_obj_set_style_bg_color(obj, c(a), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    if (a != b) {
        lv_obj_set_style_bg_grad_color(obj, c(b), 0);
        lv_obj_set_style_bg_grad_dir(obj, horizontal ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_VER, 0);
    }
}

lv_obj_t* frame(lv_obj_t* parent, int x, int y, int w, int h, Edge edge, uint32_t bg, int radius) {
    const Skin& s = skin();
    lv_obj_t* o = box(parent, x, y, w, h);
    lv_obj_set_style_bg_color(o, c(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    if (edge == Edge::None) return o;
    if (s.bevel && edge != Edge::Line) {
        const bool raised = edge == Edge::Raised;
        const uint32_t tl = raised ? s.hi : s.lo, br = raised ? s.lo : s.hi;
        line(o, 0, 0, w, 1, tl);
        line(o, 0, 0, 1, h, tl);
        line(o, 0, h - 1, w, 1, br);
        line(o, w - 1, 0, 1, h, br);
    } else {
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, c(s.lo), 0);
        if (edge == Edge::Raised && radius > 0) {
            lv_obj_set_style_radius(o, radius, 0);
            lv_obj_set_style_clip_corner(o, true, 0);
        }
    }
    return o;
}

lv_obj_t* text(lv_obj_t* parent, const std::string& s, int x, int y, uint32_t color, int width, bool bold) {
    return label(parent, s, x, y, bold ? font_ui_bold() : font_ui(), color, width);
}

lv_obj_t* text(lv_obj_t* parent, const std::string& s, int x, int y, int width) {
    return text(parent, s, x, y, skin().text, width, false);
}

lv_obj_t* text_wrap(lv_obj_t* parent, const std::string& s, int x, int y, int width, uint32_t color) {
    return text_block(parent, s, x, y, width, font_ui(), color);
}

lv_obj_t* button(lv_obj_t* parent, const std::string& lbl, int x, int y, int w, bool focused, int h) {
    const Skin& s = skin();
    lv_obj_t* b = frame(parent, x, y, w, h, Edge::Raised, s.face, s.radius);
    if (focused) {
        lv_obj_set_style_outline_width(b, 1, 0);
        lv_obj_set_style_outline_color(b, c(s.text), 0);
        lv_obj_set_style_outline_pad(b, 0, 0);
    }
    lv_obj_t* l = text(b, lbl, 0, 0, s.text, 0, focused);
    lv_obj_center(l);
    return b;
}

lv_obj_t* field(lv_obj_t* parent, int x, int y, int w, int h) {
    return frame(parent, x, y, w, h, Edge::Sunken, skin().field);
}

lv_obj_t* card(lv_obj_t* parent, int x, int y, int w, int h) {
    const Skin& s = skin();
    lv_obj_t* o = box(parent, x, y, w, h);
    lv_obj_set_style_bg_color(o, c(s.card), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, c(s.card_border), 0);
    return o;
}

int tabs(lv_obj_t* parent, int x, int y, const std::vector<std::string>& names, int active) {
    const Skin& s = skin();
    int cx = x;
    for (int i = 0; i < static_cast<int>(names.size()); ++i) {
        const bool on = i == active;
        lv_obj_t* probe = text(parent, names[i], 0, 0, s.text, 0, on);
        lv_obj_update_layout(probe);
        const int tw = lv_obj_get_width(probe) + 12;
        lv_obj_delete(probe);
        lv_obj_t* t = frame(parent, cx, on ? y : y + 1, tw, on ? 14 : 13, Edge::Raised, s.face, s.radius);
        lv_obj_t* l = text(t, names[i], 0, 0, on ? s.text : s.dim, 0, on);
        lv_obj_center(l);
        cx += tw + 1;
    }
    return 14;
}

lv_obj_t* meter(lv_obj_t* parent, int x, int y, int w, int h, int value, int max, uint32_t color) {
    lv_obj_t* track = field(parent, x, y, w, h);
    max = std::max(1, max);
    const int inner = (w - 2) * std::clamp(value, 0, max) / max;
    if (inner > 0) {
        lv_obj_t* f = box(track, 1, 1, inner, h - 2);
        lv_obj_set_style_bg_color(f, c(color), 0);
        lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    }
    return track;
}

// ── ListBox ─────────────────────────────────────────────────────────────────

ListBox::ListBox(lv_obj_t* parent, int x, int y, int w, int h) : w_(w), h_(h) {
    box_ = field(parent, x, y, w, h);
    row_h_ = lv_font_get_line_height(font_ui()) + 1;
    if (row_h_ < 12) row_h_ = 12;
    visible_ = std::max(1, (h - 2) / row_h_);
}

const Row* ListBox::selected_row() const {
    return (sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) ? &rows_[sel_] : nullptr;
}

void ListBox::set_rows(std::vector<Row> rows) {
    rows_ = std::move(rows);
    const int n = static_cast<int>(rows_.size());
    if (n == 0) {
        sel_ = -1;
    } else {
        sel_ = std::clamp(sel_, 0, n - 1);
        if (!rows_[sel_].selectable) {
            int found = -1;
            for (int i = sel_; i < n && found < 0; ++i) if (rows_[i].selectable) found = i;
            for (int i = sel_; i >= 0 && found < 0; --i) if (rows_[i].selectable) found = i;
            sel_ = found;
        }
    }
    repaint();
}

void ListBox::select(int index) {
    const int n = static_cast<int>(rows_.size());
    if (index < 0 || index >= n || !rows_[index].selectable) return;
    sel_ = index;
    repaint();
}

void ListBox::set_active(bool active) {
    if (active_ == active) return;
    active_ = active;
    repaint();
}

bool ListBox::on_key(const KeyEvent& e) {
    if (e.key != Key::Up && e.key != Key::Down) return false;
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return false;
    const int step = e.key == Key::Down ? 1 : -1;
    for (int i = sel_ + step; i >= 0 && i < n; i += step) {
        if (rows_[i].selectable) {
            sel_ = i;
            repaint();
            return true;
        }
    }
    return false;
}

void ListBox::repaint() {
    const Skin& s = skin();
    // Keep the bevel lines (first 4 children on bevel skins); drop the rows.
    const int keep = s.bevel ? 4 : 0;
    while (static_cast<int>(lv_obj_get_child_count(box_)) > keep) {
        lv_obj_delete(lv_obj_get_child(box_, -1));
    }
    const int n = static_cast<int>(rows_.size());
    if (sel_ >= 0) {
        if (sel_ < top_) top_ = sel_;
        if (sel_ >= top_ + visible_) top_ = sel_ - visible_ + 1;
        // Show a group header above the first row when there is room.
        if (top_ > 0 && sel_ == top_ && !rows_[top_ - 1].selectable) --top_;
    }
    top_ = std::clamp(top_, 0, std::max(0, n - visible_));
    for (int v = 0; v < visible_ && top_ + v < n; ++v) {
        const Row& r = rows_[top_ + v];
        const bool sel = top_ + v == sel_;
        const int y = 1 + v * row_h_;
        uint32_t fg = r.color ? r.color : (r.dim ? s.dim : s.field_text);
        if (sel) {
            lv_obj_t* hl = box(box_, 1, y, w_ - 2, row_h_);
            lv_obj_set_style_bg_color(hl, c(active_ ? s.sel : s.dim), 0);
            lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, 0);
            fg = s.sel_text;
        }
        const int x = 3 + (r.header ? 0 : r.indent);
        int right_w = 0;
        if (!r.right.empty()) {
            lv_obj_t* rl = text(box_, r.right, 0, y, sel ? fg : s.dim, 0, false);
            lv_obj_update_layout(rl);
            right_w = lv_obj_get_width(rl) + (n > visible_ ? 7 : 4);   // clear the scroll thumb
            lv_obj_set_x(rl, w_ - right_w);
        }
        const int avail = w_ - x - 2 - right_w;
        const bool bold = r.header || r.bold;
        std::string shown = r.text;
        if (!r.short_text.empty()) {
            lv_obj_t* probe = text(box_, r.text, 0, 0, fg, 0, bold);
            lv_obj_update_layout(probe);
            if (lv_obj_get_width(probe) > avail) shown = r.short_text;
            lv_obj_delete(probe);
        }
        text(box_, shown, x, y, fg, avail, bold);
    }
    // Scroll hint: a 2px thumb on the right edge when rows are hidden.
    if (n > visible_) {
        const int track = h_ - 2;
        const int th = std::max(6, track * visible_ / n);
        const int ty = 1 + (track - th) * top_ / std::max(1, n - visible_);
        lv_obj_t* thumb = box(box_, w_ - 3, ty, 2, th);
        lv_obj_set_style_bg_color(thumb, c(s.dim), 0);
        lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
    }
}

// ── TextInput ───────────────────────────────────────────────────────────────

TextInput::TextInput(lv_obj_t* parent, int x, int y, int w, int h, int max_bytes)
    : w_(w), max_(max_bytes) {
    lv_obj_t* f = field(parent, x, y, w, h);
    label_ = text(f, "", 3, (h - lv_font_get_line_height(font_ui())) / 2, skin().field_text, 0, false);
    repaint();
}

void TextInput::set_value(const std::string& v) {
    value_ = v.substr(0, static_cast<size_t>(max_));
    repaint();
}

void TextInput::set_placeholder(const std::string& p) {
    placeholder_ = p;
    repaint();
}

bool TextInput::on_key(const KeyEvent& e) {
    const uint32_t r = e.raw;
    if (r == LV_KEY_BACKSPACE || r == LV_KEY_DEL) {
        if (value_.empty()) return false;   // let the screen treat it as "back"
        value_.pop_back();
        repaint();
        return true;
    }
    if (r >= 32 && r < 127) {
        if (static_cast<int>(value_.size()) < max_) value_.push_back(static_cast<char>(r));
        repaint();
        return true;
    }
    return false;
}

void TextInput::repaint() {
    const Skin& s = skin();
    if (value_.empty() && !placeholder_.empty()) {
        lv_obj_set_style_text_color(label_, c(s.dim), 0);
        lv_label_set_text(label_, placeholder_.c_str());
        lv_obj_set_x(label_, 3);
        return;
    }
    lv_obj_set_style_text_color(label_, c(s.field_text), 0);
    const std::string shown = value_ + "|";
    lv_label_set_text(label_, shown.c_str());
    lv_obj_update_layout(label_);
    // Keep the caret in view: slide the text left when it is wider than the box.
    const int over = lv_obj_get_width(label_) - (w_ - 6);
    lv_obj_set_x(label_, over > 0 ? 3 - over : 3);
}

} // namespace mm::ui::win
