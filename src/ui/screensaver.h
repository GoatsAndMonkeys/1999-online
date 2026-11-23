// screensaver.h — 1999 Online screen savers.
//
// Eight original takes on the classic late-90s savers, software-rendered into
// a 32-bit pixel buffer that LVGL shows through one lv_image (no objects are
// created or destroyed per frame). The simulation runs on a fixed 50 ms step
// (20 fps), so a saver looks the same on the device and in the simulator.
//
// Two ways to use it:
//   * Full screen: saver_start() puts a 320x170 saver on lv_layer_top() above
//     everything; saver_tick() animates it; saver_stop() removes it.
//   * Anywhere else (the Screen Savers window's monitor preview): a
//     SaverRenderer of any size; call step() and show pixels().
//
// Settings (settings.json): "saver.id" (selected saver id), "saver.wait"
// (idle minutes before the saver starts, 0 = never), "saver.text" (marquee).
//
// ── Router hook ─────────────────────────────────────────────────────────────
// Router::handle_raw_key(), first thing (before long-press handling):
//     if (saver_active()) { saver_stop(); return true; }   // any key wakes, and is swallowed
//     saver_note_activity(lv_tick_get());
// Router::tick():
//     saver_tick(now);   // animates a running saver, starts one after the idle wait
// A key that wakes the saver is eaten: it does not reach the screen. Its
// long-press / release events may still arrive afterwards; they are harmless
// (handle_raw_release only clears key repeat).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace mm::ui {

// ── Catalogue ──────────────────────────────────────────────────────────────
int saver_count();
const char* saver_name(int which);     // "Starfield"
const char* saver_id(int which);       // "stars" (stored in settings)
int saver_from_id(const char* id);     // -1 if unknown

// ── Settings (persisted) ───────────────────────────────────────────────────
int saver_selected();
void saver_set_selected(int which);
int saver_wait_minutes();              // 0 = off
void saver_set_wait_minutes(int minutes);
std::string saver_marquee_text();
void saver_set_marquee_text(const std::string& text);
const char* saver_default_marquee();

// ── Full screen ────────────────────────────────────────────────────────────
// which < 0: the selected saver. Restarts if one is already running.
void saver_start(int which = -1);
void saver_stop();
bool saver_active();
// Call every UI tick (any cadence). Steps the running saver at 20 fps; with no
// saver running, starts the selected one once now - last activity >= wait.
void saver_tick(uint32_t now_ms);
// A key was pressed (resets the idle clock).
void saver_note_activity(uint32_t now_ms);
// Headless screenshots: starts `which` with a fixed seed, runs `steps` steps
// and holds that frame (saver_tick() leaves it alone). variant 1 on the logo
// saver: lined up for a perfect corner hit (the flash).
void saver_start_frozen(int which, uint32_t seed, int steps, int variant = 0);

// ── Off-screen renderer ────────────────────────────────────────────────────
class SaverRenderer {
public:
    // w x h pixels; the effect scales its sizes to h / 170.
    SaverRenderer(int which, int w, int h, uint32_t seed);
    ~SaverRenderer();
    SaverRenderer(const SaverRenderer&) = delete;
    SaverRenderer& operator=(const SaverRenderer&) = delete;

    void step();                         // one 50 ms simulation step
    void set_text(const std::string& t); // marquee (takes effect on the next pass)
    void arrange_corner();               // logo: head for a corner (demo)
    const uint32_t* pixels() const;      // w*h XRGB8888, row-major
    int width() const;
    int height() const;
    int which() const;

    static constexpr uint32_t kStepMs = 50;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace mm::ui
