// ringtone.cpp — see ringtone.h.
#include "ringtone.h"

#include "mm_json.h"
#include "settings.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace mm::ring {
namespace {

const char* const kSharpNames[12] = {"c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b"};
const char* const kUpperNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

bool valid_dur(int d) { return d == 1 || d == 2 || d == 4 || d == 8 || d == 16 || d == 32; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t end = s.find(sep, start);
        out.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

bool read_int(const std::string& s, size_t& i, int& v) {
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    v = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        v = v * 10 + (s[i] - '0');
        if (v > 10000) return false;
        ++i;
    }
    return true;
}

// One RTTTL note token (already trimmed, lower-case).
bool parse_note(const std::string& tok, int def_dur, int def_oct, Note& n) {
    size_t i = 0;
    int dur = def_dur;
    if (i < tok.size() && std::isdigit(static_cast<unsigned char>(tok[i]))) {
        if (!read_int(tok, i, dur) || !valid_dur(dur)) return false;
    }
    if (i >= tok.size()) return false;
    static const int kLetter[8] = {9, 11, 0, 2, 4, 5, 7, 11};   // a b c d e f g h(=b)
    const char ch = tok[i++];
    int pitch;
    if (ch == 'p') pitch = -1;
    else if (ch >= 'a' && ch <= 'h') pitch = kLetter[ch - 'a'];
    else return false;
    bool sharp = false, dotted = false;
    int oct = def_oct;
    if (i < tok.size() && (tok[i] == '#' || tok[i] == '_')) { sharp = true; ++i; }
    if (i < tok.size() && tok[i] == '.') { dotted = true; ++i; }
    if (i < tok.size() && std::isdigit(static_cast<unsigned char>(tok[i]))) {
        if (!read_int(tok, i, oct)) return false;
    }
    if (i < tok.size() && tok[i] == '.') { dotted = true; ++i; }
    if (i != tok.size()) return false;
    if (oct < kMinOctave || oct > kMaxOctave) return false;
    if (pitch >= 0 && sharp) {
        ++pitch;
        if (pitch == 12) {             // b# = c of the next octave
            pitch = 0;
            if (oct < kMaxOctave) ++oct;
        }
    }
    n.pitch = static_cast<int8_t>(pitch);
    n.octave = static_cast<int8_t>(pitch < 0 ? def_oct : oct);
    n.dur = static_cast<uint8_t>(dur);
    n.dotted = dotted;
    return true;
}

std::string note_with(const Note& n, int def_dur, int def_oct) {
    std::string s;
    if (n.dur != def_dur) s += std::to_string(n.dur);
    s += n.rest() ? "p" : kSharpNames[n.pitch];
    if (n.dotted) s += '.';
    if (!n.rest() && n.octave != def_oct) s += std::to_string(n.octave);
    return s;
}

std::string find_on_path(const char* name) {
    const char* path = std::getenv("PATH");
    const std::string dirs = path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos) end = dirs.size();
        std::string dir = dirs.substr(start, end - start);
        if (dir.empty()) dir = ".";
        const std::string full = dir + "/" + name;
        struct stat st {};
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(full.c_str(), X_OK) == 0) return full;
        start = end + 1;
    }
    return {};
}

void put_u32(std::string& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void put_u16(std::string& b, uint16_t v) {
    b.push_back(static_cast<char>(v & 0xff));
    b.push_back(static_cast<char>(v >> 8));
}

} // namespace

// ── Notation ────────────────────────────────────────────────────────────────

bool parse_rtttl(const std::string& text_in, Tune& out, std::string* err) {
    auto fail = [&](const char* why) {
        if (err) *err = why;
        return false;
    };
    const std::string text = trim(text_in);
    const size_t c1 = text.find(':');
    if (c1 == std::string::npos) return fail("no ':' after the name");
    const size_t c2 = text.find(':', c1 + 1);
    if (c2 == std::string::npos) return fail("no ':' after the settings");
    Tune t;
    t.name = clean_name(text.substr(0, c1));
    t.def_dur = 4;
    t.def_oct = 6;                     // RTTTL's defaults when d= / o= are missing
    t.bpm = 63;
    std::string ctl = text.substr(c1 + 1, c2 - c1 - 1);
    for (const std::string& part : split(ctl, ',')) {
        std::string p = trim(part);
        if (p.empty()) continue;
        for (char& ch : p) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const size_t eq = p.find('=');
        if (eq == std::string::npos) return fail("bad setting");
        const std::string key = trim(p.substr(0, eq));
        const std::string val = trim(p.substr(eq + 1));
        size_t i = 0;
        int v = 0;
        if (!read_int(val, i, v) || i != val.size()) {
            if (key == "s" || key == "l") continue;   // style / loop: ignored
            return fail("bad number in settings");
        }
        if (key == "d") {
            if (!valid_dur(v)) return fail("d= must be 1 2 4 8 16 or 32");
            t.def_dur = v;
        } else if (key == "o") {
            if (v < kMinOctave || v > kMaxOctave) return fail("o= out of range");
            t.def_oct = v;
        } else if (key == "b") {
            if (v <= 0) return fail("b= must be > 0");
            t.bpm = std::clamp(v, kMinBpm, kMaxBpm);
        } else if (key != "l" && key != "s") {
            return fail("unknown setting");
        }
    }
    const std::string body = text.substr(c2 + 1);
    for (const std::string& raw : split(body, ',')) {
        std::string tok;
        for (char ch : raw) {
            if (!std::isspace(static_cast<unsigned char>(ch))) tok += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (tok.empty()) continue;
        Note n;
        if (!parse_note(tok, t.def_dur, t.def_oct, n)) return fail("bad note");
        t.notes.push_back(n);
        if (t.notes.size() > 400) return fail("too many notes");
    }
    if (t.notes.empty()) return fail("no notes");
    out = std::move(t);
    return true;
}

std::string to_rtttl(const Tune& t) {
    static const int kDurs[6] = {4, 8, 16, 2, 32, 1};
    std::string best;
    for (int d : kDurs) {
        for (int o = kMinOctave; o <= kMaxOctave; ++o) {
            std::string s = clean_name(t.name) + ":d=" + std::to_string(d) + ",o=" + std::to_string(o) +
                            ",b=" + std::to_string(std::clamp(t.bpm, kMinBpm, kMaxBpm)) + ":";
            for (size_t i = 0; i < t.notes.size(); ++i) {
                if (i) s += ',';
                s += note_with(t.notes[i], d, o);
            }
            if (best.empty() || s.size() < best.size()) best = std::move(s);
        }
    }
    return best;
}

std::string note_text(const Note& n) {
    return note_with(n, 0, -1);
}

std::string pitch_name(const Note& n) {
    if (n.rest()) return "rest";
    return std::string(kUpperNames[n.pitch]) + std::to_string(n.octave);
}

std::string note_label(const Note& n) {
    std::string s = pitch_name(n) + "  1/" + std::to_string(n.dur);
    if (n.dotted) s += " dotted";
    return s;
}

std::string clean_name(const std::string& name) {
    std::string s;
    for (char ch : name) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (ch == ':' || ch == ',' || u < 32 || u == 127) continue;
        s += ch;
    }
    s = trim(s);
    if (s.size() > kMaxNameBytes) {
        s.resize(kMaxNameBytes);
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xc0) == 0x80) s.pop_back();   // no split UTF-8
        if (!s.empty() && static_cast<unsigned char>(s.back()) >= 0xc0) s.pop_back();
        s = trim(s);
    }
    return s;
}

// ── Timing / sound ──────────────────────────────────────────────────────────

uint32_t note_ms(const Note& n, int bpm) {
    bpm = std::clamp(bpm, kMinBpm, kMaxBpm);
    double ms = 240000.0 / bpm / std::max<int>(1, n.dur);   // whole note = 4 beats
    if (n.dotted) ms *= 1.5;
    return static_cast<uint32_t>(ms + 0.5);
}

uint32_t duration_ms(const Tune& t) {
    uint32_t total = 0;
    for (const Note& n : t.notes) total += note_ms(n, t.bpm);
    return total;
}

double frequency(const Note& n) {
    if (n.rest()) return 0.0;
    return 440.0 * std::pow(2.0, (n.midi() - 69) / 12.0);
}

std::vector<int16_t> synth(const Tune& t, int rate, float duty) {
    std::vector<int16_t> pcm;
    if (rate <= 0) return pcm;
    duty = std::clamp(duty, 0.05f, 0.95f);
    pcm.reserve(static_cast<size_t>(duration_ms(t)) * rate / 1000 + rate / 10);
    constexpr double kAmp = 9000.0;
    const int ramp = std::max(1, rate * 3 / 2000);                 // 1.5 ms: no clicks
    for (const Note& n : t.notes) {
        const int len = static_cast<int>(static_cast<int64_t>(note_ms(n, t.bpm)) * rate / 1000);
        if (n.rest()) {
            pcm.insert(pcm.end(), static_cast<size_t>(len), 0);
            continue;
        }
        // A short gap at the end of each note, so repeats are heard as two.
        const int gap = std::min(len / 8, rate * 15 / 1000);
        const int tone = len - gap;
        const double step = frequency(n) / rate;
        double phase = 0.0;
        for (int i = 0; i < tone; ++i) {
            double env = 1.0;
            if (i < ramp) env = static_cast<double>(i) / ramp;
            else if (tone - i < ramp) env = static_cast<double>(tone - i) / ramp;
            const double v = (phase < duty ? 1.0 : -1.0) * kAmp * env;
            pcm.push_back(static_cast<int16_t>(v));
            phase += step;
            if (phase >= 1.0) phase -= std::floor(phase);
        }
        pcm.insert(pcm.end(), static_cast<size_t>(gap), 0);
    }
    pcm.insert(pcm.end(), static_cast<size_t>(rate / 20), 0);      // 50 ms tail
    return pcm;
}

bool write_wav(const std::string& path, const std::vector<int16_t>& pcm, int rate) {
    std::string b;
    const uint32_t data_bytes = static_cast<uint32_t>(pcm.size() * 2);
    b += "RIFF";
    put_u32(b, 36 + data_bytes);
    b += "WAVEfmt ";
    put_u32(b, 16);
    put_u16(b, 1);                                  // PCM
    put_u16(b, 1);                                  // mono
    put_u32(b, static_cast<uint32_t>(rate));
    put_u32(b, static_cast<uint32_t>(rate) * 2);
    put_u16(b, 2);
    put_u16(b, 16);
    b += "data";
    put_u32(b, data_bytes);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(b.data(), static_cast<std::streamsize>(b.size()));
    for (int16_t s : pcm) {
        const char le[2] = {static_cast<char>(s & 0xff), static_cast<char>((s >> 8) & 0xff)};
        f.write(le, 2);
    }
    return static_cast<bool>(f);
}

// ── Sharing ─────────────────────────────────────────────────────────────────

std::string share_text(const Tune& t) {
    return std::string(kSharePrefix) + to_rtttl(t);
}

bool fits_share(const Tune& t) {
    return share_text(t).size() <= kMaxShareBytes;
}

bool parse_shared(const std::string& message, Tune& out) {
    const std::string m = trim(message);
    const std::string tag = trim(kSharePrefix);              // "~RING~"
    if (m.compare(0, tag.size(), tag) == 0) return parse_rtttl(m.substr(tag.size()), out);
    // A bare RTTTL line: "name:d=..,o=..,b=..:notes" with a settings part
    // that really looks like one, so ordinary chat ("lol: ok: sure") is not
    // taken for a tune.
    const size_t c1 = m.find(':');
    if (c1 == std::string::npos || c1 > 40) return false;
    const size_t c2 = m.find(':', c1 + 1);
    if (c2 == std::string::npos) return false;
    const std::string ctl = m.substr(c1 + 1, c2 - c1 - 1);
    if (ctl.find("b=") == std::string::npos && ctl.find("d=") == std::string::npos &&
        ctl.find("o=") == std::string::npos) {
        return false;
    }
    Tune t;
    if (!parse_rtttl(m, t) || t.notes.size() < 3) return false;
    out = std::move(t);
    return true;
}

// ── Library ─────────────────────────────────────────────────────────────────

const std::vector<BuiltinTune>& builtin_list() {
    // Public-domain classics (melody transcribed from the scores) and
    // originals written for 1999 Online. Nothing here is under copyright.
    static const std::vector<BuiltinTune> kList = {
        // Francisco Tarrega, Gran Vals (1902), bars 13-16: the phone-tune motif.
        {"Gran Vals:d=4,o=5,b=200:8e6,8d6,f#,g#,8c#6,8b,d,e,8b,8a,c#,e,2a", "F. Tarrega, 1902"},
        // Beethoven, Bagatelle No. 25 "Fur Elise" (c. 1810), opening phrase.
        {"Fur Elise:d=16,o=5,b=90:e,d#,e,d#,e,b4,d,c,8a4,p,c4,e4,a4,8b4,p,e4,g#4,b4,8c,p,e4,"
         "e,d#,e,d#,e,b4,d,c,8a4,p,c4,e4,a4,8b4,p,e4,c,b4,4a4",
         "L. v. Beethoven, 1810"},
        // Rossini, William Tell Overture (1829), the finale's gallop.
        {"William Tell:d=16,o=4,b=150:b,b,8b,b,b,8b,b,b,8e5,8f#5,8g#5,b,b,8b,b,b,8b,b,b,8g#5,8f#5,8d#5,"
         "b,b,8b,b,b,8b,b,b,8e5,8f#5,8g#5,8e5,8g#5,4b5,8a5,8f#5,4d#5,4e5",
         "G. Rossini, 1829"},
        // Originals.
        {"Dial-Up:d=32,o=6,b=180:a,p,e7,p,c,p,g,p,b,p,8p,d7,p,f,p,a,p,4p,4a5,16p,4f#6,16p,"
         "e7,c7,e7,c7,e7,c7,g7,d7,g7,d7,g7,d7,16p,8c#7,8a#6,8c#7,4f#7",
         "original"},
        {"Pager:d=32,o=7,b=125:e,c,e,c,e,c,8p,e,c,e,c,e,c,8p,e,c,e,c,e,c,4p,e,c,e,c,e,c,8p,e,c,e,c,e,c",
         "original"},
        {"Y2K:d=8,o=6,b=140:4c,4p,4c,4p,4c,4p,2c7,16c7,16c4,16c7,16c4,16g6,16c4,4p,g5,c,e,4g,e,2g",
         "original"},
        {"Sign On:d=16,o=6,b=160:c,e,g,8c7,p,g,8c7", "original"},
    };
    return kList;
}

const std::vector<Tune>& builtin() {
    static const std::vector<Tune> kTunes = [] {
        std::vector<Tune> v;
        for (const BuiltinTune& b : builtin_list()) {
            Tune t;
            if (parse_rtttl(b.rtttl, t)) v.push_back(std::move(t));
        }
        return v;
    }();
    return kTunes;
}

Library::Library(std::string path) : path_(std::move(path)) {}

std::string Library::file() const {
    return path_.empty() ? store::path("ringtones.json") : path_;
}

bool Library::load() {
    JsonValue doc;
    if (store::load_json(file(), doc) != store::Load::Ok) return false;
    std::vector<Tune> tunes;
    for (const JsonValue& v : doc["tunes"].items()) {
        Tune t;
        if (v.is_string() && parse_rtttl(v.str(), t)) tunes.push_back(std::move(t));
    }
    tunes_ = std::move(tunes);
    return true;
}

bool Library::save() const {
    std::string s = "{\n  \"version\": 1,\n  \"tunes\": [";
    for (size_t i = 0; i < tunes_.size(); ++i) {
        s += i ? ",\n    " : "\n    ";
        s += store::quote(to_rtttl(tunes_[i]));
    }
    s += tunes_.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return store::write_atomic(file(), s);
}

int Library::put(const Tune& t) {
    for (size_t i = 0; i < tunes_.size(); ++i) {
        if (tunes_[i].name == t.name) {
            tunes_[i] = t;
            return static_cast<int>(i);
        }
    }
    tunes_.push_back(t);
    return static_cast<int>(tunes_.size()) - 1;
}

// ── Playback ────────────────────────────────────────────────────────────────

Player::~Player() { stop(); }

void Player::find_backend() {
    looked_ = true;
    // PipeWire/Pulse first: on the Cardputer Zero plain aplay opens the HDMI
    // device and fails ("audio open error"), while pw-play reaches the speaker.
    static const char* const kNames[] = {"afplay", "pw-play", "paplay", "ffplay", "mpv", "aplay"};
    for (const char* n : kNames) {
        exe_ = find_on_path(n);
        if (!exe_.empty()) {
            name_ = n;
            return;
        }
    }
    name_.clear();
}

const char* Player::backend_name() {
    if (!looked_ || name_.empty()) find_backend();
    return name_.empty() ? "none" : name_.c_str();
}

std::string Player::wav_path() const {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = "/tmp";
    return (dir / ("1999online-ring-" + std::to_string(getpid()) + ".wav")).string();
}

bool Player::play(const Tune& t) {
    stop();
    error_.clear();
    if (t.notes.empty()) {
        error_ = "No notes to play";
        return false;
    }
    if (std::strcmp(backend_name(), "none") == 0) {
        error_ = "No audio player found (aplay / afplay / ffplay)";
        return false;
    }
    const std::string path = wav_path();
    if (!write_wav(path, synth(t))) {
        error_ = "Could not write " + path;
        return false;
    }
    std::vector<std::string> args = {name_};
    if (name_ == "aplay") args.push_back("-q");
    else if (name_ == "ffplay") args.insert(args.end(), {"-nodisp", "-autoexit", "-loglevel", "quiet"});
    else if (name_ == "mpv") args.insert(args.end(), {"--no-video", "--really-quiet", "--no-terminal"});
    args.push_back(path);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        error_ = "Could not start the player";
        return false;
    }
    if (pid == 0) {
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        execv(exe_.c_str(), argv.data());
        _exit(127);
    }
    pid_ = pid;
    started_ = std::chrono::steady_clock::now();
    return true;
}

void Player::stop() {
    if (pid_ <= 0) return;
    const pid_t pid = pid_;
    pid_ = -1;
    kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 20; ++i) {                       // up to ~200 ms, then insist
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

void Player::tick() {
    if (pid_ <= 0) return;
    int status = 0;
    const pid_t r = waitpid(pid_, &status, WNOHANG);
    if (r == pid_ || (r < 0 && errno != EINTR)) {
        if (r == pid_ && WIFEXITED(status) && WEXITSTATUS(status) == 127) error_ = "Could not run " + name_;
        pid_ = -1;
    }
}

uint32_t Player::elapsed_ms() const {
    if (pid_ <= 0) return 0;
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started_)
                                     .count());
}

Player& player() {
    static Player p;
    return p;
}

} // namespace mm::ring
