// ringtone.h — one-packet ringtones for the Ringtones screen.
//
// NOTATION: RTTTL, the Nokia "Ring Tone Text Transfer Language":
//
//     Gran Vals:d=4,o=5,b=180:8e6,8d6,f#,g#,8c#6,8b,d,e,8b,8a,c#,e,2a
//
//   name : d=default duration, o=default octave, b=tempo (quarter notes per
//   minute) : notes. A note is [duration]letter[#][.][octave][.] with letters
//   c d e f g a b (h is read as b) and p for a rest; durations 1 2 4 8 16 32
//   (whole .. 32nd), "." makes it dotted (x1.5). Octaves are scientific pitch
//   (a4 = 440 Hz), 3..8 accepted. Parsing is lenient about spaces and case.
//
// SOUND: synth() renders a square / pulse wave, like a 1999 phone buzzer, to
// mono 16-bit PCM; write_wav() puts that in a .wav file. Player plays such a
// file by spawning a command-line player (aplay, paplay, afplay, ffplay, mpv:
// the first one on PATH), one at a time, like AudioPlayer does for MeshAmp.
//
// SHARING: a tune travels as one mesh text message, share_text():
//
//     "~RING~ Gran Vals:d=4,o=5,b=180:8e6,8d6,..."
//
// at most kMaxShareBytes (Messenger::kMaxMessageBytes) bytes. parse_shared()
// finds a tune in a received text: the ~RING~ prefix, or a bare RTTTL line
// someone pasted from elsewhere.
//
// LIBRARY: builtin() are public-domain classics and original beeps. The user's
// own tunes are kept in ringtones.json in the config folder (settings.h,
// mm::store): { "version": 1, "tunes": ["<rtttl>", ...] }. Nothing is read or
// written while store::enabled() is false (--shots).
//
// LVGL-free, plain C++17. Not thread-safe: call from the UI thread.
#pragma once

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace mm::ring {

constexpr int kMinOctave = 3;
constexpr int kMaxOctave = 8;
constexpr int kMinBpm = 25;
constexpr int kMaxBpm = 400;
constexpr size_t kMaxNameBytes = 16;
constexpr size_t kMaxShareBytes = 200;        // == Messenger::kMaxMessageBytes / kMaxTextBytes
constexpr const char* kSharePrefix = "~RING~ ";
constexpr int kSampleRate = 22050;

struct Note {
    int8_t pitch = 0;      // 0..11 = C..B; -1 = rest
    int8_t octave = 5;     // kMinOctave..kMaxOctave (ignored for a rest)
    uint8_t dur = 4;       // 1, 2, 4, 8, 16, 32
    bool dotted = false;

    bool rest() const { return pitch < 0; }
    int midi() const { return rest() ? -1 : (octave + 1) * 12 + pitch; }
    bool operator==(const Note& o) const {
        return pitch == o.pitch && (rest() || octave == o.octave) && dur == o.dur && dotted == o.dotted;
    }
};

struct Tune {
    std::string name;      // <= kMaxNameBytes, no ':' or ','
    int def_dur = 4;       // RTTTL "d=" when writing (chosen by to_rtttl if 0)
    int def_oct = 5;       // RTTTL "o="
    int bpm = 120;         // RTTTL "b="
    std::vector<Note> notes;
};

// ── Notation ────────────────────────────────────────────────────────────────
// false (and `out` untouched) if it is not RTTTL; `err` gets a short reason.
bool parse_rtttl(const std::string& text, Tune& out, std::string* err = nullptr);
// Compact RTTTL: picks the d= / o= that make the string shortest.
std::string to_rtttl(const Tune& t);
// One note as RTTTL with no defaults: "8c#6.", "4p".
std::string note_text(const Note& n);
// "C#5", "rest".
std::string pitch_name(const Note& n);
// Note name in the UI: "F#5 1/8." etc.
std::string note_label(const Note& n);
// Clean a name for RTTTL (drops ':' ',' and control bytes, trims, caps length).
std::string clean_name(const std::string& name);

// ── Timing / sound ──────────────────────────────────────────────────────────
uint32_t note_ms(const Note& n, int bpm);
uint32_t duration_ms(const Tune& t);
double frequency(const Note& n);              // Hz; 0 for a rest
// duty: 0.5 square (default), 0.25 / 0.125 thinner pulse.
std::vector<int16_t> synth(const Tune& t, int rate = kSampleRate, float duty = 0.5f);
bool write_wav(const std::string& path, const std::vector<int16_t>& pcm, int rate = kSampleRate);

// ── Sharing ─────────────────────────────────────────────────────────────────
std::string share_text(const Tune& t);
bool fits_share(const Tune& t);               // share_text() <= kMaxShareBytes
// true if `message` carries a tune (prefix, or a bare RTTTL line).
bool parse_shared(const std::string& message, Tune& out);

// ── Library ─────────────────────────────────────────────────────────────────
struct BuiltinTune {
    const char* rtttl;
    const char* credit;    // "Tarrega, 1902" / "original"
};
const std::vector<BuiltinTune>& builtin_list();
const std::vector<Tune>& builtin();           // parsed builtin_list(), same order

class Library {
public:
    // "" => store::path("ringtones.json").
    explicit Library(std::string path = std::string());
    bool load();                              // false: missing / corrupt / files disabled
    bool save() const;                        // false: files disabled / error
    std::vector<Tune>& tunes() { return tunes_; }
    const std::vector<Tune>& tunes() const { return tunes_; }
    // Replaces a tune of the same name, else appends. Returns its index.
    int put(const Tune& t);
    std::string file() const;

private:
    std::string path_;
    std::vector<Tune> tunes_;
};

// ── Playback ────────────────────────────────────────────────────────────────
class Player {
public:
    Player() = default;
    ~Player();                                // stops
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Renders `t` to a temp .wav and starts the player. false: no player on
    // PATH, nothing to play, or the write / spawn failed (see error()).
    bool play(const Tune& t);
    void stop();
    // Reaps a finished player. Call a few times per second.
    void tick();
    bool playing() const { return pid_ > 0; }
    uint32_t elapsed_ms() const;              // since play(); 0 if stopped
    // "aplay", "afplay", ... or "none" (looked up again while "none").
    const char* backend_name();
    const std::string& error() const { return error_; }
    std::string wav_path() const;

private:
    void find_backend();
    pid_t pid_ = -1;
    std::string exe_, name_ = "", error_;
    bool looked_ = false;
    std::chrono::steady_clock::time_point started_{};
};

Player& player();

} // namespace mm::ring
