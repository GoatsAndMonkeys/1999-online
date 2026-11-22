// audio_player.h — MeshAmp's playback backend.
//
// Plays local music files and NTS Radio streams by spawning an external
// player (mpv, else ffplay) with fork/execvp: no shell, no audio library.
// One process at a time; the pid is kept, stopped with SIGTERM and reaped.
//
// Threading: every method is for the UI thread. refresh_stations() does its
// network work on a background thread and hands the result back through a
// mutex-guarded mailbox that tick() drains, so the lists never change under
// the caller between two tick()s.
#pragma once

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mm {

class AudioPlayer {
public:
    enum class State : uint8_t { Stopped, Playing };
    enum class Kind : uint8_t { None, File, Station, Retro };

    struct Station {
        std::string name;        // "NTS 1", "Poolside"
        std::string short_name;  // big readout: "NTS 1", "MIX"
        std::string url;
        bool live = false;       // true: live channel; false: infinite mixtape
        std::string now;         // live channels: current show title ("" if unknown)
        std::string country;     // retro stations: ISO country code ("" if unknown)
    };
    struct Track {
        std::string path;
        std::string title;       // file name without directory and extension
        std::string ext;         // lower-case, no dot: "mp3"
    };

    AudioPlayer();
    ~AudioPlayer();              // stops the child process
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // ── Library ─────────────────────────────────────────────────────────────
    const std::vector<Station>& stations() const { return stations_; }   // NTS Radio
    // 90s / 2000s internet radio from around the world: a built-in list picked
    // from the Radio Browser directory (radio-browser.info), each stream tested.
    const std::vector<Station>& retro() const { return retro_; }
    const std::vector<Track>& files();            // scans on first use
    void rescan_files();                          // re-read the music directory
    const std::string& music_dir() const { return music_dir_; }
    std::string music_dir_label() const;          // "~/Music"
    // Fetch the NTS mixtape list and the live "now playing" titles (curl, on a
    // background thread). Silent on failure; at most one request per minute.
    void refresh_stations();

    // ── Playback ────────────────────────────────────────────────────────────
    bool play_station(int index);                 // false: bad index / no player / spawn failed
    bool play_file(int index);
    bool play_retro(int index);
    void stop();
    bool next();                                  // within the current (or last) list, wraps
    bool prev();
    // Reaps a finished child (auto-advances to the next local file) and applies
    // finished station refreshes. Call a few times per second.
    void tick();

    State state() const { return state_; }
    bool playing() const { return state_ == State::Playing; }
    Kind kind() const { return kind_; }           // what is (or was last) playing
    int index() const { return index_; }          // into files() / stations() / retro(); -1 if none
    std::string title() const;                    // track title, or show / station name
    int elapsed_seconds() const;                  // since the current item started; 0 if stopped

    // "mpv", "ffplay" or "none". Looked up on PATH (again on each play while "none").
    const char* backend_name();
    bool has_backend() { return backend_name()[0] != 'n'; }

    int volume() const { return volume_; }        // 0..100
    // With PipeWire's wpctl on PATH (the device), this is the system output
    // volume and applies at once, to a running player too; the player itself
    // is spawned at full volume. Otherwise it is stored and passed to the
    // player when it is next spawned.
    void set_volume(int v);
    bool system_volume() const { return sysvol_; }
    void toggle_mute();                           // system volume only
    bool muted() const { return muted_; }

    // ── PCM tap (for the visualizer) ────────────────────────────────────────
    // While on, a second process (ffmpeg) decodes what is playing to mono
    // 16-bit PCM at kTapRate and read_pcm() hands out whatever has arrived.
    // It is a separate decode of the same source: in step for files, a few
    // seconds off for radio streams. Without ffmpeg, has_tap() stays false.
    static constexpr int kTapRate = 11025;
    void set_tap(bool on);
    bool has_tap() const { return tap_fd_ >= 0; }
    size_t read_pcm(int16_t* out, size_t max_samples);   // never blocks

    // Bumped whenever anything a screen shows may have changed.
    uint32_t revision() const { return revision_; }

    // Parsers for the NTS public feed (exposed for tests). Return false and
    // leave `out` alone if the document is not what we expect.
    static bool parse_mixtapes(const std::string& json, std::vector<Station>& out);
    static bool parse_live(const std::string& json, std::string& now1, std::string& now2);

private:
    enum class Backend : uint8_t { Unknown, None, Mpv, Ffplay };
    struct Mailbox;

    void find_backend();
    bool spawn(const std::string& target);
    void kill_child();
    void start_tap();
    void stop_tap();
    std::string current_target() const;
    void apply_mailbox();

    std::vector<Station> stations_;
    std::vector<Station> retro_;
    std::vector<Track> files_;
    bool scanned_ = false;
    std::string music_dir_;

    Backend backend_ = Backend::Unknown;
    std::string backend_path_;

    pid_t pid_ = -1;
    bool tap_on_ = false;
    pid_t tap_pid_ = -1;
    int tap_fd_ = -1;
    State state_ = State::Stopped;
    Kind kind_ = Kind::None;
    int index_ = -1;
    std::chrono::steady_clock::time_point started_{};
    int volume_ = 70;
    bool sysvol_ = false;        // wpctl found
    bool sysvol_applied_ = false;
    bool muted_ = false;
    uint32_t revision_ = 1;

    std::shared_ptr<Mailbox> mailbox_;
    std::chrono::steady_clock::time_point last_refresh_{};
    bool refreshed_once_ = false;
};

AudioPlayer& audio_player();

} // namespace mm
