// viz.h — a MilkDrop-style music visualizer engine (software, no GPU).
//
// The classic feedback pipeline, on the CPU: every frame the previous image
// is warped through a coarse mesh (zoom, rotation, translation, stretch and a
// "warp" wobble, computed per mesh vertex and interpolated per pixel in fixed
// point), faded by a decay, and then the waveform, a few shapes and a slowly
// cycling colour are drawn on top additively. A preset is a row of parameters
// plus two small functions (per frame, per vertex) that modulate them from
// time and the bass / mid / treble levels. All presets are original and live
// in viz.cpp; there is no expression parser and no preset file format.
//
// Standard library only: no LVGL, no SDL. The caller owns the display; this
// hands back a w*h block of 0x00RRGGBB pixels.
//
//   mm::viz::Visualizer viz(156, 60);
//   mm::viz::Analyzer   an;
//   ...each frame:
//   an.push(pcm, n, 11025);                 // whatever PCM arrived
//   viz.render(an.frame(now_seconds));      // or viz.render(fake_audio(now))
//   blit(viz.pixels(), viz.width(), viz.height());
//
// The same engine can also be driven by a real MilkDrop 1 preset file (.milk,
// read by ui/milk.h): see Visualizer::load_milk().
//
// Nothing here is thread-safe: use one Visualizer / Analyzer from one thread.
// Output is deterministic: the same sequence of AudioFrames gives the same
// pixels (the only randomness is an internal seeded LCG).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mm::milk {
struct Preset;
}

namespace mm::viz {

// One frame's worth of audio, as the visualizer wants it.
struct AudioFrame {
    static constexpr int kWave = 128;
    static constexpr int kSpec = 16;

    float wave[kWave] = {};   // -1..1, mono, oldest first, most recent samples
    float spec[kSpec] = {};   // 0..1, log-spaced bands, low to high (used by
                              // the bar / spike wave modes; zeros are fine)
    float bass = 0.f;         // ~0..3 relative to recent loudness, 1 = average
    float mid  = 0.f;
    float treb = 0.f;
    float time = 0.f;         // seconds, monotonic; drives all animation
};

class Visualizer {
public:
    // Any size >= 8x8 works; cost is linear in w*h. Allocates once, here.
    Visualizer(int w, int h);
    ~Visualizer();

    static int preset_count();
    static const char* preset_name(int i);   // "" if out of range

    // Out-of-range indices wrap. The image is kept, so the old picture melts
    // into the new preset rather than cutting to black.
    // Also leaves MilkDrop mode (see load_milk).
    void set_preset(int i);
    int preset() const { return preset_; }   // the built-in last selected

    // Drive the engine from a MilkDrop 1 preset instead of a built-in one:
    // its per_frame_init / per_frame / per_pixel equations, the eight basic
    // wave modes, borders, motion vectors, custom shapes and custom waves,
    // video echo, gamma and the brighten / darken / solarize / invert filters.
    // Returns false, changing nothing, for a preset that needs pixel shaders
    // (MilkDrop 2) or a file that cannot be read. The image is kept.
    //
    // Where this differs from MilkDrop on purpose:
    //  - the preset's square picture is stretched to the canvas (as MilkDrop 1
    //    did), so circles become ellipses on a wide panel;
    //  - motion is per frame, as in MilkDrop, so it is slower at low frame
    //    rates (the built-ins are frame-rate independent);
    //  - lines, dots and hairline borders are drawn fainter on small canvases,
    //    where one pixel is a far larger share of the picture;
    //  - there is one (mono) channel: "right" is the same wave a little later;
    //  - a custom wave with a long per_point program gets fewer points (a
    //    budget of program steps per frame), to bound the cost.
    bool load_milk(const mm::milk::Preset& p);
    bool load_milk(const char* path);
    bool is_milk() const { return milk_ != nullptr; }

    // Name of whatever is running: the .milk preset's, or the built-in's.
    const char* current_name() const;

    // Advance one frame. Motion is scaled by the time since the last call
    // (taken from a.time), so it looks the same at 15 fps as at 30.
    void render(const AudioFrame& a);

    // Black out the feedback buffer and restart the internal random sequence.
    void clear();

    const uint32_t* pixels() const { return out_; }   // w*h, 0x00RRGGBB
    int width() const { return w_; }
    int height() const { return h_; }

private:
    struct Impl;                         // drawing helpers, in viz.cpp
    struct Milk;                         // a loaded .milk preset and its state
    friend struct Impl;

    void render_builtin(const AudioFrame& a, float dt);
    void render_milk(const AudioFrame& a, float dt);

    static constexpr int kGX = 32;       // warp mesh cells across
    static constexpr int kGY = 12;       // and down

    int w_, h_;
    int preset_ = 0;
    std::vector<uint32_t> cur_, prev_;   // feedback pair, swapped each frame
    std::vector<uint32_t> disp_;         // MilkDrop mode: cur_ after echo / gamma
    const uint32_t* out_ = nullptr;      // what pixels() hands out
    std::unique_ptr<Milk> milk_;

    // Mesh: where each vertex sits on screen (static) and where it samples
    // the previous frame from (per frame), in pixels.
    int   xs_[kGX + 1], ys_[kGY + 1];
    float vx_[(kGX + 1) * (kGY + 1)], vy_[(kGX + 1) * (kGY + 1)];     // -A..A, -1..1
    float vrad_[(kGX + 1) * (kGY + 1)], vang_[(kGX + 1) * (kGY + 1)];
    float su_[(kGX + 1) * (kGY + 1)], sv_[(kGX + 1) * (kGY + 1)];

    // Running state.
    bool     started_ = false;
    float    last_time_ = 0.f;
    float    bass_ = 0.f, mid_ = 0.f, treb_ = 0.f;          // sanitised inputs
    float    bass_att_ = 1.f, mid_att_ = 1.f, treb_att_ = 1.f;
    float    beat_ = 0.f;                // 1 on a bass hit, decays to 0
    float    beat_hold_ = 0.f;           // refractory timer, seconds
    uint32_t beats_ = 0;
    uint32_t frame_ = 0;
    uint32_t lcg_ = 0x1999u;
};

// Turns raw mono PCM into AudioFrames: a 256-point FFT at about 11 kHz gives
// the three bands, each divided by its own running average (so 1.0 means "as
// loud as lately", like MilkDrop's bass/mid/treb), plus a 16-band spectrum
// and a lightly gain-levelled waveform.
class Analyzer {
public:
    Analyzer();

    // Append samples. Any rate works; rates above ~16 kHz are averaged down to
    // roughly 11 kHz first. Large bursts are fine: frame() plays them out in
    // step with the clock, up to 150 ms behind the newest sample.
    void push(const int16_t* mono, size_t n, int sample_rate);

    // The frame for `time` (seconds, the same clock you give the Visualizer).
    // If nothing has been pushed for 0.3 s the output falls to silence.
    AudioFrame frame(float time);

    void reset();

private:
    static constexpr int kRing = 4096;   // power of two
    static constexpr int kFft = 256;

    int16_t  ring_[kRing];
    uint32_t wpos_ = 0;                  // total samples written
    double   cursor_ = 0.0;              // read position, trails wpos_
    int      rate_ = 11025;              // rate of the stored samples
    int      in_rate_ = 0, dec_ = 1, dec_n_ = 0;
    int32_t  dec_acc_ = 0;

    bool     started_ = false;
    float    last_time_ = 0.f;
    uint32_t last_wpos_ = 0;
    float    stall_ = 0.f;               // seconds without new samples
    float    avg_[3] = {0.f, 0.f, 0.f};  // long-run band levels
    float    spec_peak_ = 0.f;
    float    wave_peak_ = 0.f;

    float    win_[kFft];
    float    cos_[kFft / 2], sin_[kFft / 2];
    uint16_t rev_[kFft];
};

// A stand-in for when there is no PCM: a looping four-on-the-floor pattern
// (kick, off-beat hats, a snare on two and four, a bass note) as a pure
// function of time.
AudioFrame fake_audio(float time);

}  // namespace mm::viz
