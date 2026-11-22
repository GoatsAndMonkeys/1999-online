#include "widgets.h"

#include "skin.h"
#include "softkeys.h"
#include "win.h"

#include "lv_sprites.h"
#include "mm_sprites.h"

#include "BreedingApp.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace mm::ui {

// ── Basic objects ───────────────────────────────────────────────────────────

lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t* panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* o = box(parent, x, y, w, h);
    lv_obj_set_style_bg_color(o, c(color::kPanel), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, c(color::kBorder), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 4, 0);
    return o;
}

lv_obj_t* label(lv_obj_t* parent, const std::string& text, int x, int y,
                const lv_font_t* font, uint32_t col, int width) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font ? font : font_body(), 0);
    lv_obj_set_style_text_color(l, c(col), 0);
    lv_obj_set_pos(l, x, y);
    if (width > 0) {
        // Single line, clipped with "...": DOTS needs a fixed height.
        const lv_font_t* f = font ? font : font_body();
        lv_obj_set_size(l, width, lv_font_get_line_height(f));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_label_set_text(l, text.c_str());
    return l;
}

lv_obj_t* text_block(lv_obj_t* parent, const std::string& text, int x, int y, int width,
                     const lv_font_t* font, uint32_t col) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font ? font : font_body(), 0);
    lv_obj_set_style_text_color(l, c(col), 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, width);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(l, text.c_str());
    return l;
}

void set_text(lv_obj_t* lbl, const std::string& text) {
    if (lbl) lv_label_set_text(lbl, text.c_str());
}

lv_obj_t* chip(lv_obj_t* parent, const std::string& text, int x, int y,
               lv_color_t bg, uint32_t fg) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font_small(), 0);
    lv_obj_set_style_text_color(l, c(fg), 0);
    lv_obj_set_style_bg_color(l, bg, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 3, 0);
    lv_obj_set_style_pad_hor(l, 3, 0);
    lv_obj_set_style_pad_ver(l, 0, 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, text.c_str());
    return l;
}

// ── Bars ────────────────────────────────────────────────────────────────────

lv_obj_t* bar(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t fill) {
    lv_obj_t* b = lv_bar_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, c(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(b, h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, fill, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, h / 2, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(b, 400, LV_PART_MAIN);
    lv_bar_set_range(b, 0, 100);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    return b;
}

void bar_set(lv_obj_t* b, int value, int max, bool animate) {
    if (!b) return;
    max = std::max(1, max);
    lv_bar_set_range(b, 0, max);
    lv_bar_set_value(b, std::clamp(value, 0, max), animate ? LV_ANIM_ON : LV_ANIM_OFF);
}

lv_obj_t* hp_bar(lv_obj_t* parent, int x, int y, int w, int hp, int max_hp) {
    lv_obj_t* b = bar(parent, x, y, w, 4, hp_color(hp, max_hp));
    hp_bar_set(b, hp, max_hp);
    return b;
}

void hp_bar_set(lv_obj_t* b, int hp, int max_hp, bool animate) {
    if (!b) return;
    lv_obj_set_style_bg_color(b, hp_color(hp, max_hp), LV_PART_INDICATOR);
    bar_set(b, hp, max_hp, animate);
}

// ── Sprites ─────────────────────────────────────────────────────────────────

namespace {

// The box and fit of a sprite image live in its user_data so sprite_set() and
// sprite_animate() can re-fit: bit 31 marks it, bits 0..11 box_w, 12..23
// box_h, bit 24 = Frame fit.
constexpr uintptr_t kSpriteTag = uintptr_t{1} << 31;

void* pack_fit(int bw, int bh, SpriteFit fit) {
    const uintptr_t v = kSpriteTag | (static_cast<uintptr_t>(bw) & 0xFFF) |
                        ((static_cast<uintptr_t>(bh) & 0xFFF) << 12) |
                        (fit == SpriteFit::Frame ? uintptr_t{1} << 24 : 0);
    return reinterpret_cast<void*>(v);
}

bool unpack_fit(lv_obj_t* img, int& bw, int& bh, SpriteFit& fit) {
    const auto v = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(img));
    if (!(v & kSpriteTag)) return false;
    bw = static_cast<int>(v & 0xFFF);
    bh = static_cast<int>((v >> 12) & 0xFFF);
    fit = (v >> 24) & 1 ? SpriteFit::Frame : SpriteFit::Crop;
    return true;
}

// Frame fit stands the monster on the bottom of its box; Crop centres it,
// except that a sprite still taller than the box at half size keeps its head
// (top-aligned) and loses its feet.
void apply_fit(lv_obj_t* img) {
    int bw = 0, bh = 0;
    SpriteFit fit = SpriteFit::Crop;
    if (!unpack_fit(img, bw, bh, fit)) return;
    lv_image_align_t al = fit == SpriteFit::Frame ? LV_IMAGE_ALIGN_BOTTOM_MID : LV_IMAGE_ALIGN_CENTER;
    const void* src = lv_image_get_src(img);
    if (fit == SpriteFit::Crop && src && lv_image_src_get_type(src) == LV_IMAGE_SRC_VARIABLE &&
        static_cast<int>(static_cast<const lv_image_dsc_t*>(src)->header.h) > bh)
        al = LV_IMAGE_ALIGN_TOP_MID;
    lv_image_set_inner_align(img, al);
}

}  // namespace

int sprite_scale2(int dex, bool back, int box_w, int box_h, SpriteFit fit, int* out_w, int* out_h) {
    const bool crop = fit == SpriteFit::Crop;
    int w = 0, h = 0;
    for (int s : {mm::sprites::kScale2x, mm::sprites::kScale1x, mm::sprites::kScaleHalf}) {
        if (!mm::sprites::render_size(dex, back, s, crop, w, h)) break;
        if ((w <= box_w && h <= box_h) || s == mm::sprites::kScaleHalf) {
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
            return s;
        }
    }
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    return mm::sprites::kScale1x;
}

lv_obj_t* sprite_box(lv_obj_t* parent, int dex, int x, int y, int box_w, int box_h,
                     bool back, int skin, bool tritan, SpriteFit fit) {
    lv_obj_t* img = lv_image_create(parent);
    lv_obj_set_pos(img, x, y);
    lv_obj_set_size(img, box_w, box_h);
    lv_obj_set_user_data(img, pack_fit(box_w, box_h, fit));
    lv_image_set_antialias(img, false);
    sprite_set(img, dex, back, skin, tritan);
    return img;
}

lv_obj_t* sprite(lv_obj_t* parent, int dex, int x, int y, int scale_256,
                 bool back, int skin, bool tritan) {
    const int px = mm::sprites::kSpriteW * scale_256 / 256;
    return sprite_box(parent, dex, x, y, px, px, back, skin, tritan, SpriteFit::Crop);
}

void sprite_set(lv_obj_t* img, int dex, bool back, int skin, bool tritan) {
    if (!img) return;
    int bw = 0, bh = 0;
    SpriteFit fit = SpriteFit::Crop;
    const lv_image_dsc_t* dsc = nullptr;
    if (dex > 0 && unpack_fit(img, bw, bh, fit)) {
        const int s2 = sprite_scale2(dex, back, bw, bh, fit);
        dsc = mm::sprites::get(dex, back, skin, tritan, s2, fit == SpriteFit::Crop);
    }
    lv_image_set_src(img, dsc);
    apply_fit(img);
}

void sprite_animate(lv_obj_t* img, int slot, int dex, bool back, int skin,
                    uint32_t now_ms, bool tritan) {
    if (!img || !mm::sprites::is_animated(skin)) return;
    int bw = 0, bh = 0;
    SpriteFit fit = SpriteFit::Crop;
    if (!unpack_fit(img, bw, bh, fit)) return;
    const auto phase = static_cast<uint16_t>((now_ms / 120) % 18);
    const int s2 = sprite_scale2(dex, back, bw, bh, fit);
    const lv_image_dsc_t* dsc = mm::sprites::get_animated(slot, dex, back, skin, phase, tritan,
                                                          s2, fit == SpriteFit::Crop);
    if (!dsc) return;
    // Always re-set: the slot's descriptor is reused but its size can change.
    lv_image_set_src(img, dsc);
    apply_fit(img);
    lv_obj_invalidate(img);
}

// ── ListView ────────────────────────────────────────────────────────────────

ListView::ListView(lv_obj_t* parent, int x, int y, int w, int h, int row_h, RowBuilder builder)
    : builder_(std::move(builder)), w_(w), h_(h), row_h_(std::max(8, row_h)) {
    root_ = box(parent, x, y, w, h);
    visible_ = std::max(1, h / row_h_);
    for (int i = 0; i < visible_; ++i) {
        lv_obj_t* row = box(root_, 0, i * row_h_, w - 4, row_h_ - 1);
        lv_obj_set_style_radius(row, 3, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        rows_.push_back(row);
    }
    scroll_ = box(root_, w - 2, 0, 2, 10);
    lv_obj_set_style_bg_color(scroll_, c(color::kDim), 0);
    lv_obj_set_style_bg_opa(scroll_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scroll_, 1, 0);
    refresh();
}

void ListView::set_count(int count) {
    count_ = std::max(0, count);
    sel_ = count_ ? std::clamp(sel_, 0, count_ - 1) : 0;
    select(sel_);
}

void ListView::select(int index) {
    sel_ = count_ ? std::clamp(index, 0, count_ - 1) : 0;
    if (sel_ < top_) top_ = sel_;
    if (sel_ >= top_ + visible_) top_ = sel_ - visible_ + 1;
    top_ = std::clamp(top_, 0, std::max(0, count_ - visible_));
    refresh();
}

void ListView::refresh() {
    for (int i = 0; i < visible_; ++i) {
        lv_obj_t* row = rows_[i];
        lv_obj_clean(row);
        const int idx = top_ + i;
        if (idx >= count_) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        const bool selected = idx == sel_;
        lv_obj_set_style_bg_color(row, c(selected ? color::kPanelHi : color::kPanel), 0);
        lv_obj_set_style_border_width(row, selected ? 1 : 0, 0);
        lv_obj_set_style_border_color(row, c(color::kAccent), 0);
        if (builder_) builder_(row, idx, selected);
    }
    // Scroll indicator.
    if (count_ > visible_) {
        lv_obj_remove_flag(scroll_, LV_OBJ_FLAG_HIDDEN);
        const int track = h_;
        const int thumb = std::max(8, track * visible_ / count_);
        const int max_top = count_ - visible_;
        const int y = max_top ? (track - thumb) * top_ / max_top : 0;
        lv_obj_set_pos(scroll_, w_ - 2, y);
        lv_obj_set_height(scroll_, thumb);
    } else {
        lv_obj_add_flag(scroll_, LV_OBJ_FLAG_HIDDEN);
    }
    if (count_ == 0 && !empty_text_.empty()) {
        if (!empty_) {
            empty_ = label(root_, empty_text_, 0, 0, font_body(), color::kDim);
            lv_obj_center(empty_);
        }
        lv_obj_remove_flag(empty_, LV_OBJ_FLAG_HIDDEN);
    } else if (empty_) {
        lv_obj_add_flag(empty_, LV_OBJ_FLAG_HIDDEN);
    }
}

bool ListView::on_key(const KeyEvent& e) {
    if (count_ == 0) return false;
    switch (e.key) {
        case Key::Up:
            if (sel_ == 0 && e.repeat) return true;
            select(sel_ == 0 ? count_ - 1 : sel_ - 1);
            return true;
        case Key::Down:
            if (sel_ == count_ - 1 && e.repeat) return true;
            select(sel_ == count_ - 1 ? 0 : sel_ + 1);
            return true;
        case Key::Left:
            select(sel_ - visible_);
            return true;
        case Key::Right:
            select(sel_ + visible_);
            return true;
        default:
            return false;
    }
}

void ListView::set_empty_text(const std::string& text) {
    empty_text_ = text;
    if (empty_) set_text(empty_, text);
    refresh();
}

// ── GridNav ─────────────────────────────────────────────────────────────────

bool GridNav::on_key(const KeyEvent& e) {
    if (count <= 0) return false;
    const int before = sel;
    const int rows = (count + cols - 1) / cols;
    int r = sel / cols, col = sel % cols;
    switch (e.key) {
        case Key::Left:  col = (col + cols - 1) % cols; break;
        case Key::Right: col = (col + 1) % cols; break;
        case Key::Up:    r = (r + rows - 1) % rows; break;
        case Key::Down:  r = (r + 1) % rows; break;
        default: return false;
    }
    sel = std::min(count - 1, r * cols + col);
    return sel != before;
}

// ── Toast ───────────────────────────────────────────────────────────────────

namespace {
lv_obj_t* g_toast = nullptr;
lv_timer_t* g_toast_timer = nullptr;

void toast_hide_cb(lv_timer_t*) {
    if (g_toast) lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
    g_toast_timer = nullptr;
}
} // namespace

void toast(const std::string& text, uint32_t ms, uint32_t accent) {
    if (!g_toast || !lv_obj_is_valid(g_toast)) {
        g_toast = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_font(g_toast, font_body(), 0);
        lv_obj_set_style_text_color(g_toast, c(color::kText), 0);
        lv_obj_set_style_bg_color(g_toast, c(color::kBar), 0);
        lv_obj_set_style_bg_opa(g_toast, LV_OPA_90, 0);
        lv_obj_set_style_border_width(g_toast, 1, 0);
        lv_obj_set_style_radius(g_toast, 4, 0);
        lv_obj_set_style_pad_hor(g_toast, 6, 0);
        lv_obj_set_style_pad_ver(g_toast, 3, 0);
        lv_obj_set_width(g_toast, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(g_toast, kScreenW - 16, 0);
        lv_label_set_long_mode(g_toast, LV_LABEL_LONG_MODE_WRAP);
    }
    lv_obj_set_style_border_color(g_toast, c(accent), 0);
    lv_label_set_text(g_toast, text.c_str());
    lv_obj_remove_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(g_toast, LV_ALIGN_BOTTOM_MID, 0, -(kFooterH + 4));
    if (g_toast_timer) lv_timer_delete(g_toast_timer);
    g_toast_timer = lv_timer_create(toast_hide_cb, ms, nullptr);
    lv_timer_set_repeat_count(g_toast_timer, 1);
}

void toast_hide() {
    if (g_toast_timer) {
        lv_timer_delete(g_toast_timer);
        g_toast_timer = nullptr;
    }
    if (g_toast && lv_obj_is_valid(g_toast)) lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
}

// ── Modal ───────────────────────────────────────────────────────────────────

Modal::Modal(const std::string& title, const std::string& body,
             std::vector<std::string> options, Done done)
    : options_(std::move(options)), done_(std::move(done)) {
    root_ = box(lv_layer_top(), 0, 0, kScreenW, kScreenH);
    lv_obj_set_style_bg_color(root_, c(0x000000), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_60, 0);

    // The options are soft keys along the bottom edge (keys 4, 6, 8, each
    // label right over its key); the panel sits centred above them.
    const int w = 240, h = 84;
    lv_obj_t* p = panel(root_, (kScreenW - w) / 2, (kScreenH - kFooterH - h) / 2, w, h);
    lv_obj_set_style_border_color(p, c(color::kAccent), 0);
    label(p, title, 8, 5, font_title(), color::kAccent, w - 16);
    lv_obj_t* b = label(p, body, 8, 25, font_body(), color::kText);
    lv_obj_set_width(b, w - 16);
    lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);

    const Skin& sk = skin();
    bar_ = box(root_, 0, kScreenH - kFooterH, kScreenW, kFooterH);
    win::fill(bar_, sk.hint_a, sk.hint_b);
    paint();
}

Modal::~Modal() {
    if (root_ && lv_obj_is_valid(root_)) lv_obj_delete(root_);
}

// Option i -> soft key slot: every other key from key 4 (up to 3 options),
// so each label may use two keys' width; 4-5 options take adjacent keys.
int Modal::slot_of(int i) const {
    const int n = static_cast<int>(options_.size());
    return n <= 3 ? i * 2 : i;
}

void Modal::paint() {
    if (!bar_) return;
    lv_obj_clean(bar_);
    SoftKeys k;
    const int n = std::min<int>(static_cast<int>(options_.size()), kSoftKeyCount);
    for (int i = 0; i < n; ++i) k.set(slot_of(i), options_[static_cast<size_t>(i)]);
    k.selected = n ? slot_of(sel_) : -1;
    k.left.text = MM_SOFT_BACK;
    softkey_bar(bar_, k, 0, 0, kFooterH, 0);
}

void Modal::close(int choice) {
    if (root_) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    auto done = std::move(done_);
    if (done) done(choice);
}

bool Modal::on_key(const KeyEvent& e) {
    if (!root_) return false;
    const int n = static_cast<int>(options_.size());
    switch (e.key) {
        case Key::Left:  if (n) { sel_ = (sel_ + n - 1) % n; paint(); } break;
        case Key::Right: if (n) { sel_ = (sel_ + 1) % n; paint(); } break;
        case Key::A:     close(n ? sel_ : 0); break;
        case Key::B:     close(-1); break;
        default: {
            const int slot = softkey_slot(e);
            if (slot == kSoftLeft) { close(-1); break; }
            for (int i = 0; i < n && i < kSoftKeyCount; ++i)
                if (slot == slot_of(i)) { close(i); break; }
            break;
        }
    }
    return true;
}

// ── Text helpers ────────────────────────────────────────────────────────────

std::string fmt(const char* f, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

std::string species_name(int dex) {
    if (dex >= 1 && dex <= 151) return breeding::BreedingApp::dexName(static_cast<uint8_t>(dex));
    return fmt("#%03d", dex);
}

std::string title_case(const std::string& s) {
    std::string out = s;
    bool start = true;
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z' && !start) ch = static_cast<char>(ch - 'A' + 'a');
        else if (ch >= 'a' && ch <= 'z' && start) ch = static_cast<char>(ch - 'a' + 'A');
        start = (ch == ' ' || ch == '-' || ch == '.' || ch == '\n');
    }
    return out;
}

} // namespace mm::ui
