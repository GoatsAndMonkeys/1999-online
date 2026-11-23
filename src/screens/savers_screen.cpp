// savers_screen.cpp — Screen Savers, after the Win98 Display Properties tab:
// the saver list on the left; on the right a little monitor running the
// selected saver in miniature, the idle wait and the marquee text.
//
// Up/Down pick a saver (saved at once), Left/Right change the wait, Enter /
// PREVIEW runs it full screen until any key, Tab / TEXT edits the marquee.
#include "screens.h"

#include "router.h"
#include "screensaver.h"
#include "widgets.h"
#include "win.h"

#include <algorithm>
#include <cstdio>

namespace mm::ui {
namespace {

constexpr int kListW = 112;
constexpr int kRightX = kListW + 6;
constexpr int kRightW = kClientW - kRightX;        // 194
constexpr int kBezelW = 112, kBezelH = 66;
constexpr int kTubeW = 100, kTubeH = 53;           // 320x170 at ~0.31
constexpr int kWaits[] = {0, 1, 2, 3, 5, 10, 15, 20, 30, 45, 60};
constexpr int kWaitCount = static_cast<int>(sizeof(kWaits) / sizeof(kWaits[0]));
constexpr int kDemoSaver = 6;                      // settings shot: the floppies in the preview

class SaversScreen final : public Screen {
public:
    explicit SaversScreen(Ctx& ctx) : Screen(ctx) {}
    ~SaversScreen() override {
        if (owns_full_) saver_stop();
    }

    const char* title() const override { return "Screen Savers"; }
    bool windowed() const override { return true; }
    bool typing() const override { return editing_; }

    std::string hints() const override {
        if (editing_) return "Type the marquee text  Enter:OK  Esc:cancel";
        return "Up/Dn:saver  L/R:wait  Enter:preview  Tab:text";
    }

    SoftKeys softkeys() const override {
        SoftKeys k;
        if (editing_) return k;   // digits type
        k.left.text = MM_SOFT_BACK;
        k.set(0, "WAIT-", wait_index() > 0).set(1, "WAIT+", wait_index() < kWaitCount - 1);
        k.set(2, "PREVIEW").set(3, "TEXT");
        return k;
    }

    void build(lv_obj_t* body) override {
        const Skin& s = skin();
        win::text(body, "Screen Saver:", 1, 1);
        list_ = std::make_unique<win::ListBox>(body, 0, 15, kListW, kClientH - 15);
        std::vector<win::Row> rows;
        for (int i = 0; i < saver_count(); ++i) {
            win::Row r;
            r.text = saver_name(i);
            rows.push_back(r);
        }
        list_->set_rows(rows);
        list_->select(sel_ < 0 ? saver_selected() : sel_);
        list_->set_active(!editing_);
        sel_ = list_->selected();

        // The monitor: bezel, tube, power light, neck and foot.
        const int mx = kRightX + (kRightW - kBezelW) / 2;
        lv_obj_t* bezel = win::frame(body, mx, 0, kBezelW, kBezelH, win::Edge::Raised, s.face, 3);
        lv_obj_t* tube = win::frame(bezel, (kBezelW - kTubeW) / 2 - 2, 3, kTubeW + 4, kTubeH + 4,
                                    win::Edge::Sunken, 0x000000);
        lv_obj_t* led = box(bezel, kBezelW - 12, kBezelH - 7, 4, 2);
        lv_obj_set_style_bg_color(led, c(0x30d040), 0);
        lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
        win::frame(body, mx + kBezelW / 2 - 14, kBezelH, 28, 4, win::Edge::Raised, s.face);
        win::frame(body, mx + kBezelW / 2 - 32, kBezelH + 4, 64, 5, win::Edge::Raised, s.face, 2);

        ensure_preview();
        pdsc_ = lv_image_dsc_t{};
        pdsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        pdsc_.header.cf = LV_COLOR_FORMAT_XRGB8888;
        pdsc_.header.w = kTubeW;
        pdsc_.header.h = kTubeH;
        pdsc_.header.stride = kTubeW * 4;
        pdsc_.data = reinterpret_cast<const uint8_t*>(preview_->pixels());
        pdsc_.data_size = kTubeW * kTubeH * 4;
        pimg_ = lv_image_create(tube);
        lv_image_set_src(pimg_, &pdsc_);
        lv_obj_set_pos(pimg_, 2, 2);

        // Wait: [ 5 ] minutes            [Preview]
        const int ry = 81;
        win::text(body, "Wait:", kRightX, ry + 2);
        lv_obj_t* wf = win::field(body, kRightX + 32, ry, 34, 15);
        const int wait = saver_wait_minutes();
        char buf[16];
        if (wait > 0) std::snprintf(buf, sizeof(buf), "%d", wait);
        else std::snprintf(buf, sizeof(buf), "Off");
        lv_obj_t* wl = win::text(wf, buf, 0, 0, s.field_text);
        lv_obj_align(wl, LV_ALIGN_CENTER, 0, 0);
        win::text(body, wait == 1 ? "minute" : (wait > 0 ? "minutes" : "never"), kRightX + 70, ry + 2,
                  s.dim);
        win::button(body, "Preview", kClientW - 62, ry, 62, !editing_);

        // Marquee text.
        const int ty = 102;
        win::text(body, "Text:", kRightX, ty + 2, editing_ ? s.text : s.dim);
        if (editing_) {
            input_ = std::make_unique<win::TextInput>(body, kRightX + 32, ty, kRightW - 32, 15, 120);
            input_->set_value(draft_);
        } else {
            input_.reset();
            lv_obj_t* tf = win::field(body, kRightX + 32, ty, kRightW - 32, 15);
            win::text(tf, saver_marquee_text(), 3, 1, sel_ == 4 ? s.field_text : s.dim, kRightW - 32 - 6);
        }
    }

    void tick(uint32_t now_ms) override {
        // Our own full-screen preview animates even before the Router calls
        // saver_tick() itself (a second call in the same tick is a no-op).
        if (owns_full_ && saver_active()) saver_tick(now_ms);
        if (demo_ >= 0 || !preview_ || !pimg_ || saver_active()) return;   // frozen, or the big one is up
        if (now_ms - last_step_ < SaverRenderer::kStepMs) return;
        last_step_ = now_ms;
        preview_->step();
        lv_obj_invalidate(pimg_);
    }

    void on_hide() override {
        if (owns_full_) { saver_stop(); owns_full_ = false; }
    }

    bool on_key(const KeyEvent& in) override {
        // A full-screen preview: any key ends it (the Router does this too
        // once it has the idle hook; doing it here keeps Preview self-contained).
        if (saver_active()) {
            saver_stop();
            owns_full_ = false;
            return true;
        }
        if (editing_) return edit_key(in);

        KeyEvent e = in;
        switch (softkey_slot(in)) {
            case 0: e.key = Key::Left; break;
            case 1: e.key = Key::Right; break;
            case 2: e.key = Key::A; break;
            case 3: e.key = Key::Tab; break;
            default: break;
        }
        if (e.key == Key::Char && e.ch == 'p') e.key = Key::A;
        if (e.key == Key::Char && e.ch == 't') e.key = Key::Tab;

        if (list_ && (e.key == Key::Up || e.key == Key::Down) && list_->on_key(e)) {
            sel_ = list_->selected();
            saver_set_selected(sel_);
            preview_.reset();
            ctx_.router.rebuild();   // new preview, marquee field emphasis
            return true;
        }
        if (e.key == Key::Left || e.key == Key::Right) {
            const int i = std::clamp(wait_index() + (e.key == Key::Right ? 1 : -1), 0, kWaitCount - 1);
            saver_set_wait_minutes(kWaits[i]);
            saver_note_activity(lv_tick_get());
            ctx_.router.rebuild();
            return true;
        }
        if (e.key == Key::A) {
            saver_start(sel_);
            owns_full_ = true;
            return true;
        }
        if (e.key == Key::Tab) {
            editing_ = true;
            draft_ = saver_marquee_text();
            ctx_.router.rebuild();
            return true;
        }
        return false;
    }

    // settings, one full-screen frame per saver, the logo's corner flash, text editing.
    int demo_state_count() const override { return 1 + saver_count() + 2; }
    const char* demo_state_name(int i) const override {
        static std::string name;
        if (i <= 0) return "settings";
        if (i == saver_count() + 1) return "full_logo_corner";
        if (i > saver_count() + 1) return "edit_text";
        name = std::string("full_") + saver_id(i - 1);
        return name.c_str();
    }
    void set_demo_state(int i) override {
        demo_ = i;
        saver_stop();
        owns_full_ = false;
        editing_ = false;
        if (i <= 0 || i > saver_count() + 1) {
            const bool edit = i > 0;
            sel_ = edit ? 4 : kDemoSaver;           // marquee / floppies in the preview
            editing_ = edit;
            draft_ = "Meet me at the summit at 5! *** KD2ABC-m";
            preview_.reset();
            return;
        }
        // A deterministic mid-animation frame of each saver, full screen.
        const bool corner = i == saver_count() + 1;
        const int which = corner ? 3 : i - 1;
        const int steps = corner ? 7 : (which == 1 ? 170 : 90);
        saver_start_frozen(which, 0x1999u + static_cast<uint32_t>(which) * 77u, steps, corner ? 1 : 0);
        owns_full_ = true;
    }

private:
    int wait_index() const {
        const int w = saver_wait_minutes();
        int best = 0;
        for (int i = 0; i < kWaitCount; ++i)
            if (kWaits[i] <= w) best = i;
        return best;
    }

    void ensure_preview() {
        if (preview_ && preview_->which() == sel_) return;
        preview_ = std::make_unique<SaverRenderer>(sel_, kTubeW, kTubeH, 0x5eedu + static_cast<uint32_t>(sel_));
        if (editing_) preview_->set_text(draft_);
        if (demo_ >= 0) for (int f = 0; f < 120; ++f) preview_->step();
    }

    bool edit_key(const KeyEvent& e) {
        if (e.key == Key::A || e.key == Key::Tab) {
            draft_ = input_ ? input_->value() : draft_;
            saver_set_marquee_text(draft_);
            editing_ = false;
            if (sel_ == 4) preview_.reset();   // restart the marquee with the new words
            ctx_.router.rebuild();
            return true;
        }
        if (input_ && input_->on_key(e)) {
            draft_ = input_->value();
            if (preview_) preview_->set_text(draft_);   // the next pass shows it
            return true;
        }
        if (e.key == Key::B) {   // Esc (or Backspace on an empty field): cancel
            editing_ = false;
            ctx_.router.rebuild();
            return true;
        }
        return true;   // swallow everything else while typing
    }

    std::unique_ptr<win::ListBox> list_;
    std::unique_ptr<win::TextInput> input_;
    std::unique_ptr<SaverRenderer> preview_;
    lv_image_dsc_t pdsc_{};
    lv_obj_t* pimg_ = nullptr;
    uint32_t last_step_ = 0;
    int sel_ = -1;
    int demo_ = -1;
    bool owns_full_ = false;
    bool editing_ = false;
    std::string draft_;
};

} // namespace

std::unique_ptr<Screen> make_savers_screen(Ctx& ctx) {
    return std::make_unique<SaversScreen>(ctx);
}

} // namespace mm::ui
