// beats.h — MeshBeat, a pocket drum machine for 1999 Online.
//
// PATTERN: kTracks drum voices x kSteps sixteenth-note steps (one 4/4 bar).
// Each step is off, on or accented. A pattern also carries a tempo, a swing
// amount (MPC-style 50..75 %: where the off-beat sixteenth falls inside each
// pair), a mute mask and a volume per track.
//
// SOUND: every voice is synthesized, nothing is sampled: a sine kick with a
// pitch drop, noise snare / hats / clap, a sine tom, a two-square bell and a
// pulse bass blip. render() mixes N bars to mono 16-bit PCM whose length is
// exactly N bars, with the tails wrapped round to the start so it loops
// seamlessly; ring::write_wav() makes the .wav.
//
// TEXT FORM (to_text / from_text), used for the library file and sharing:
//
//     B1:Pager Hop:92:62:77767777:00:<64 hex digits>
//
//   version : name : bpm : swing % : volume digit per track : mute mask (hex)
//   : 8 hex digits per track, each digit two steps (hi = first, 0 off / 1 on /
//   2 accent, i.e. digit = a*4 + b). About 110 bytes.
//
// SHARING: share_text() = "~BEAT~ " + to_text(), one mesh text message
// (<= kMaxShareBytes). parse_shared() finds a pattern in a received text.
//
// LIBRARY: presets() are original patterns written for 1999 Online. The user
// has kUserSlots slots in beats.json in the config folder (settings.h,
// mm::store): { "version": 1, "slots": ["<text>" | "", ...] }. Nothing is read
// or written while store::enabled() is false (--shots).
//
// PLAYBACK: LoopPlayer plays a rendered loop by spawning a command-line player
// (afplay, pw-play, paplay, ffplay, mpv, aplay: the first on PATH; aplay last
// because on the Cardputer Zero it opens HDMI). It never blocks: stop() sends
// SIGTERM and the child is reaped later from tick().
//
// LVGL-free, plain C++17. Not thread-safe: call from the UI thread.
#pragma once

#include <sys/types.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mm::beat {

constexpr int kTracks = 8;
constexpr int kSteps = 16;
constexpr int kMinBpm = 60;
constexpr int kMaxBpm = 200;
constexpr int kMinSwing = 50;            // straight
constexpr int kMaxSwing = 75;            // hard shuffle
constexpr int kMaxVol = 9;
constexpr int kDefaultVol = 7;
constexpr int kUserSlots = 4;
constexpr size_t kMaxNameBytes = 18;
constexpr size_t kMaxShareBytes = 200;   // == Messenger::kMaxMessageBytes
constexpr const char* kSharePrefix = "~BEAT~ ";
constexpr int kSampleRate = 22050;

enum Track : uint8_t { Kick, Snare, Hat, OpenHat, Clap, Tom, Bell, Bass };
enum Step : uint8_t { Off = 0, On = 1, Accent = 2 };

const char* track_name(int track);       // "KICK", "SNARE", ... (<= 5 chars)
const char* track_long_name(int track);  // "Kick drum", "Snare", ...

struct Pattern {
    std::string name;
    int bpm = 120;
    int swing = kMinSwing;
    uint8_t mute = 0;                     // bit t: track t muted
    std::array<uint8_t, kTracks> vol{};   // 0..kMaxVol
    std::array<std::array<uint8_t, kSteps>, kTracks> steps{};

    Pattern() { vol.fill(kDefaultVol); }
    bool muted(int t) const { return (mute >> t) & 1; }
    bool empty() const;                   // no step set
    int hits() const;
    void clear();                         // steps only
    void clear_track(int t);
    bool operator==(const Pattern& o) const;
    bool operator!=(const Pattern& o) const { return !(*this == o); }
};

// ── Text ────────────────────────────────────────────────────────────────────
std::string to_text(const Pattern& p);
// false (and `out` untouched) if it is not a pattern.
bool from_text(const std::string& text, Pattern& out);
// Drops ':' and control bytes, trims, caps at kMaxNameBytes.
std::string clean_name(const std::string& name);
// Rows like "x...x...X...x..." ('x' on, 'X' accent, '.' or '-' off), one per
// track in Track order (missing rows stay empty). Used for the presets.
bool from_rows(const std::string& name, int bpm, int swing, const std::vector<std::string>& rows, Pattern& out);

// ── Timing / sound ──────────────────────────────────────────────────────────
double step_ms(int bpm);                  // a sixteenth
uint32_t bar_ms(int bpm);
// Start of step `s` (0..15) inside a bar, swing applied, in ms.
double step_start_ms(const Pattern& p, int s);
// Step sounding at `ms` into a bar (0..15).
int step_at(const Pattern& p, double ms_in_bar);
// Samples in `bars` bars (exactly what render() returns).
size_t loop_samples(int bpm, int bars, int rate = kSampleRate);
// Bars per loop file so it lasts at least ~`min_ms` (1..8).
int loop_bars(int bpm, uint32_t min_ms = 6000);
// Mono PCM of `bars` bars. Muted tracks are silent; accents are louder; the
// open hat is choked by the closed hat.
std::vector<int16_t> render(const Pattern& p, int bars = 1, int rate = kSampleRate);
// One hit of one voice (for auditioning a step).
std::vector<int16_t> render_hit(int track, bool accent, int vol = kDefaultVol, int rate = kSampleRate);

// ── Sharing ─────────────────────────────────────────────────────────────────
std::string share_text(const Pattern& p);
bool fits_share(const Pattern& p);
bool parse_shared(const std::string& message, Pattern& out);

// ── Library ─────────────────────────────────────────────────────────────────
struct Preset {
    const char* name;
    int bpm;
    int swing;
    const char* rows[kTracks];            // see from_rows()
    const char* blurb;
};
const std::vector<Preset>& preset_list();
const std::vector<Pattern>& presets();   // parsed preset_list(), same order

class Library {
public:
    // "" => store::path("beats.json").
    explicit Library(std::string path = std::string());
    bool load();                          // false: missing / corrupt / files disabled
    bool save() const;                    // false: files disabled / error
    // Slot 0..kUserSlots-1; an empty Pattern name means a free slot.
    const Pattern& slot(int i) const { return slots_[static_cast<size_t>(i)]; }
    bool used(int i) const { return !slots_[static_cast<size_t>(i)].name.empty(); }
    void set(int i, const Pattern& p) { slots_[static_cast<size_t>(i)] = p; }
    void erase(int i) { slots_[static_cast<size_t>(i)] = Pattern(); }
    std::string file() const;

private:
    std::string path_;
    std::array<Pattern, kUserSlots> slots_{};
};

// ── Playback ────────────────────────────────────────────────────────────────
class LoopPlayer {
public:
    LoopPlayer() = default;
    ~LoopPlayer();                        // stops (SIGKILL, reaped)
    LoopPlayer(const LoopPlayer&) = delete;
    LoopPlayer& operator=(const LoopPlayer&) = delete;

    // Writes `pcm` to a temp .wav (alternating two names, so a dying player
    // never reads a file being rewritten) and spawns the player. false: no
    // player, write or spawn failed (error()).
    bool play(const std::vector<int16_t>& pcm, int rate = kSampleRate);
    // Starts the file written by the last play() again (a loop restart).
    bool replay();
    void stop();                          // SIGTERM; never waits
    // Reaps finished / stopped players (WNOHANG). Cheap; call every frame.
    void tick();
    bool playing() const { return pid_ > 0; }
    const char* backend_name();
    bool available() { return std::string(backend_name()) != "none"; }
    const std::string& error() const { return error_; }

private:
    bool spawn(const std::string& path);
    void find_backend();
    pid_t pid_ = -1;
    struct Dying {
        pid_t pid;
        int ticks;
    };
    std::vector<Dying> dying_;
    std::string exe_, name_, error_, last_path_;
    bool looked_ = false;
    int flip_ = 0;
};

LoopPlayer& player();

} // namespace mm::beat
