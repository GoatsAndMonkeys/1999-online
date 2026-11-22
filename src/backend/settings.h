// settings.h — what the user chose, kept across restarts.
//
// Two layers, both LVGL-free and plain C++17:
//
//   mm::store::*    the config folder and safe file helpers shared by every
//                   saved file (settings.json, conversations.json, space.json).
//   mm::settings()  a small typed key/value store saved as settings.json.
//
// WHERE: $PC1999_CONFIG_DIR, else $XDG_CONFIG_HOME/1999online, else
// ~/.config/1999online. The folder is created on the first write.
//
// SCREENSHOT MODE: `--shots` (and `--icon`) call store::set_enabled(false)
// before anything else runs, the same switch as GameState::Options::load_files
// / GameState::files_enabled(). While disabled NOTHING here reads or writes a
// file: loads report "no file", writes return false, the values live in memory.
//
// settings.json is one flat JSON object, keys sorted, dotted names:
//
//   { "amp.volume": 80, "ui.skin": "winamp" }
//
// KEYS IN USE (add new ones here so they stay in one place):
//   ui.skin          string  skin id (w95 me xp vista sys7 mac9 beos os2 x11 winamp)
//   ui.sprites       string  monster sprite set: "gen2" (default) | "gen3"
//   amp.volume       int     MeshAmp volume, 0..100
//   amp.kind         string  what was playing last: "radio" | "file" | ...
//   amp.index        int     index of the last station / track in that list
//   amp.station      string  optional: id or URL of the last station (safer than
//                            an index when the list changes)
//   amp.viz_preset   int     visualizer preset index
//   amp.viz_mode     string  optional: visualizer kind
//   amp.shuffle      bool    optional
//   amp.repeat       bool    optional
//
// The amp.* keys are reserved for MeshAmp; wire them like this:
//
//   int vol = mm::settings().get_int("amp.volume", 80);      // on start
//   mm::settings().set_int("amp.volume", vol);               // on change
//
// set_*() is cheap: it only marks the store dirty (and only when the value
// really changed). The file is written from tick() at most once per
// kFlushMs, by flush(), and by the destructor. The Router calls tick().
//
// Not thread-safe: call everything from the UI thread.
#pragma once

#include "mm_json.h"

#include <cstdint>
#include <map>
#include <string>

namespace mm {

namespace store {

// false => no file is read or written anywhere in the app (shots / tests).
void set_enabled(bool on);
bool enabled();

// The config folder, no trailing slash. Re-read from the environment on every
// call so tests can point it somewhere else.
std::string dir();
// dir() + "/" + file
std::string path(const std::string& file);

// Whole file into `out`. false if disabled, missing or unreadable.
bool read_text(const std::string& path, std::string& out);
// Writes `text` to "<path>.tmp.<pid>", fsyncs, then rename()s it over `path`,
// creating the parent folders (0700) first. A reader therefore sees the old
// file or the new one, never half of either. false (and no file) if disabled
// or on any error; the temp file is removed on failure.
bool write_atomic(const std::string& path, const std::string& text);
bool remove_file(const std::string& path);   // false if disabled / not there

// Reads and parses a JSON file whose top level must be an object.
enum class Load { Ok, Missing, Corrupt };
// A corrupt file is moved aside to "<path>.corrupt" (replacing an older one)
// so the next save cannot destroy what the user might still want to repair.
Load load_json(const std::string& path, JsonValue& out);

// "text" -> "\"text\"" with JSON escapes.
std::string quote(const std::string& s);

} // namespace store

class Settings {
public:
    static constexpr uint32_t kFlushMs = 1000;   // at most one write per second

    // "" => store::path("settings.json"), resolved at load / save time.
    explicit Settings(std::string path = std::string());
    ~Settings();                                  // flush()
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    // ── Read ────────────────────────────────────────────────────────────────
    bool has(const std::string& key) const;
    // The default is returned when the key is absent or has another type
    // (a number reads as a bool and a bool as 0/1, like mm::JsonValue).
    std::string get_string(const std::string& key, const std::string& def = std::string()) const;
    int64_t get_int(const std::string& key, int64_t def = 0) const;
    bool get_bool(const std::string& key, bool def = false) const;

    // ── Write (marks dirty only if the value changed) ───────────────────────
    void set_string(const std::string& key, const std::string& value);
    void set_int(const std::string& key, int64_t value);
    void set_bool(const std::string& key, bool value);
    void remove(const std::string& key);

    // ── Files ───────────────────────────────────────────────────────────────
    // Call often (any cadence) with a millisecond clock; writes when dirty and
    // kFlushMs has passed since the first unsaved change.
    void tick(uint32_t now_ms);
    // Writes now if dirty. true if nothing needed writing or the write worked.
    bool flush();
    bool dirty() const { return dirty_; }
    // Forget everything in memory and read the file again (missing / corrupt
    // file => empty store). Called by the constructor.
    store::Load reload();
    const std::string& file() const;              // the path in use
    uint32_t writes() const { return writes_; }   // files written so far (tests)
    std::string to_json() const;

private:
    struct Value {
        enum class Kind : uint8_t { String, Number, Bool } kind = Kind::String;
        std::string s;
        double n = 0.0;
        bool b = false;
        bool operator==(const Value& o) const {
            return kind == o.kind && s == o.s && n == o.n && b == o.b;
        }
    };
    void put(const std::string& key, Value v);

    std::string fixed_path_;
    mutable std::string path_;
    std::map<std::string, Value> values_;
    bool dirty_ = false;
    bool armed_ = false;          // dirty_since_ms_ is valid
    uint32_t dirty_since_ms_ = 0;
    uint32_t writes_ = 0;
};

// The one process-wide store (settings.json in store::dir()).
Settings& settings();

} // namespace mm
