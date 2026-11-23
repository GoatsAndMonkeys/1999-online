// beats.cpp — see beats.h.
#include "beats.h"

#include "mm_json.h"
#include "ringtone.h"
#include "settings.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace mm::beat {
namespace {

constexpr double kPi = 3.14159265358979323846;

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

bool parse_int(const std::string& s_in, int& v) {
    const std::string s = trim(s_in);
    if (s.empty() || s.size() > 4) return false;
    v = 0;
    for (char ch : s) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        v = v * 10 + (ch - '0');
    }
    return true;
}

int hex_val(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
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

// Deterministic white noise, so a render is the same every time.
struct Noise {
    uint32_t s = 0x1999u;
    float next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s) / 2147483648.0f - 1.0f;
    }
};

// ── Voices ──────────────────────────────────────────────────────────────────
// Each voice is one hit at full scale (about -1..1), synthesized once per
// sample rate and cached.
using Voice = std::vector<float>;

Voice make_voice(int track, int rate) {
    const double r = rate;
    auto len = [&](double sec) { return static_cast<size_t>(sec * r); };
    Voice v;
    Noise nz;
    nz.s = 0x1999u + static_cast<uint32_t>(track) * 7919u;
    switch (track) {
        case Kick: {                         // sine, pitch dropping 155 -> 45 Hz
            v.resize(len(0.34));
            double ph = 0;
            for (size_t i = 0; i < v.size(); ++i) {
                const double t = i / r;
                const double f = 45.0 + 110.0 * std::exp(-t / 0.028);
                ph += 2 * kPi * f / r;
                double s = std::sin(ph) * std::exp(-t / 0.11);
                s += nz.next() * 0.35 * std::exp(-t / 0.0012);   // beater click
                v[i] = static_cast<float>(s);
            }
            break;
        }
        case Snare: {                        // two body tones + bright noise
            v.resize(len(0.24));
            float prev = 0;
            for (size_t i = 0; i < v.size(); ++i) {
                const double t = i / r;
                const float n = nz.next();
                const float hp = n - prev;
                prev = n;
                double s = std::sin(2 * kPi * 185 * t) * 0.55 * std::exp(-t / 0.045) +
                           std::sin(2 * kPi * 330 * t) * 0.3 * std::exp(-t / 0.028) +
                           hp * 0.45 * std::exp(-t / 0.065);
                v[i] = static_cast<float>(s);
            }
            break;
        }
        case Hat:
        case OpenHat: {                      // high-passed noise + a metallic edge
            const bool open = track == OpenHat;
            v.resize(len(open ? 0.42 : 0.07));
            const double tau = open ? 0.13 : 0.016;
            static const double kMetal[4] = {3312.0, 4791.0, 5917.0, 7203.0};
            float p1 = 0, p2 = 0;
            for (size_t i = 0; i < v.size(); ++i) {
                const double t = i / r;
                const float n = nz.next();
                const float d1 = n - p1;     // two differences: a steep high-pass
                const float d2 = d1 - p2;
                p1 = n;
                p2 = d1;
                double m = 0;
                for (double f : kMetal) m += std::fmod(t * f, 1.0) < 0.5 ? 0.25 : -0.25;
                const double s = (d2 * 0.32 + m * 0.18) * std::exp(-t / tau);
                v[i] = static_cast<float>(s);
            }
            break;
        }
        case Clap: {                         // three quick bursts, then a tail
            v.resize(len(0.24));
            // Resonant band-pass around 1.2 kHz (state variable filter).
            const double fc = 2 * std::sin(kPi * 1200.0 / r);
            const double q = 0.55;
            double lo = 0, bp = 0;
            for (size_t i = 0; i < v.size(); ++i) {
                const double t = i / r;
                const double n = nz.next();
                const double hi = n - lo - q * bp;
                bp += fc * hi;
                lo += fc * bp;
                double env = 0;
                for (double t0 : {0.0, 0.010, 0.021}) {
                    if (t >= t0) env = std::max(env, std::exp(-(t - t0) / 0.0045));
                }
                if (t >= 0.028) env = std::max(env, 0.8 * std::exp(-(t - 0.028) / 0.06));
                v[i] = static_cast<float>(bp * 1.4 * env);
            }
            break;
        }
        case Tom: {                          // sine, 190 -> 110 Hz
            v.resize(len(0.3));
            double ph = 0;
            for (size_t i = 0; i < v.size(); ++i) {
                const double t = i / r;
                const double f = 110.0 + 80.0 * std::exp(-t / 0.05);
                ph += 2 * kPi * f / r;
                v[i] = static_cast<float>(std::sin(ph) * std::exp(-t / 0.095) * 0.9);
            }
            break;
        }
        case Bell: {                         // two detuned squares, a pinged decay
            v.resize(len(0.26));
            double lo = 0;
            for (size_t i = 0; i < v.size(); ++i) {
                const double t = i / r;
                const double a = std::fmod(t * 562.0, 1.0) < 0.5 ? 0.5 : -0.5;
                const double b = std::fmod(t * 841.0, 1.0) < 0.5 ? 0.5 : -0.5;
                const double x = a + b;
                lo += (x - lo) * 0.35;       // soften the edges
                const double env = 0.65 * std::exp(-t / 0.018) + 0.35 * std::exp(-t / 0.11);
                v[i] = static_cast<float>(lo * env * 0.8);
            }
            break;
        }
        case Bass:
        default: {                           // 25 % pulse at G2 through a closing low-pass
            v.resize(len(0.2));
            double ph = 0, lo = 0;
            const double f = 98.0;
            for (size_t i = 0; i < v.size(); ++i) {
                const double t = i / r;
                ph += f / r;
                if (ph >= 1) ph -= 1;
                const double x = ph < 0.25 ? 1.0 : -0.33;
                const double cut = 0.04 + 0.30 * std::exp(-t / 0.03);
                lo += (x - lo) * cut;
                const double env = std::min(1.0, t / 0.002) * std::exp(-t / 0.075);
                v[i] = static_cast<float>(lo * env * 0.9);
            }
            break;
        }
    }
    return v;
}

const Voice& voice(int track, int rate) {
    struct Cache {
        int rate = 0;
        std::array<Voice, kTracks> v;
    };
    static Cache cache;
    if (cache.rate != rate) {
        for (int t = 0; t < kTracks; ++t) cache.v[static_cast<size_t>(t)] = make_voice(t, rate);
        cache.rate = rate;
    }
    return cache.v[static_cast<size_t>(std::clamp(track, 0, kTracks - 1))];
}

// Base level per voice, so a pattern at the default volumes is balanced.
constexpr float kBase[kTracks] = {1.0f, 0.8f, 0.55f, 0.45f, 0.7f, 0.75f, 0.45f, 0.6f};

float hit_gain(int track, bool accent, int vol) {
    return kBase[track] * (static_cast<float>(vol) / kDefaultVol) * (accent ? 1.0f : 0.68f) * 0.6f;
}

int16_t to_pcm(float x) {
    const float y = std::tanh(x) * 0.92f;            // soft clip
    return static_cast<int16_t>(std::lround(std::clamp(y, -1.0f, 1.0f) * 32767.0f));
}

} // namespace

// ── Pattern ─────────────────────────────────────────────────────────────────

const char* track_name(int t) {
    static const char* const kNames[kTracks] = {"KICK", "SNARE", "HAT", "OPEN", "CLAP", "TOM", "BELL", "BASS"};
    return t >= 0 && t < kTracks ? kNames[t] : "?";
}

const char* track_long_name(int t) {
    static const char* const kNames[kTracks] = {"Kick drum", "Snare", "Closed hat", "Open hat",
                                                "Hand clap", "Low tom",  "Cow bell",   "Bass blip"};
    return t >= 0 && t < kTracks ? kNames[t] : "?";
}

bool Pattern::empty() const { return hits() == 0; }

int Pattern::hits() const {
    int n = 0;
    for (const auto& row : steps)
        for (uint8_t s : row) n += s != Off;
    return n;
}

void Pattern::clear() {
    for (auto& row : steps) row.fill(Off);
}

void Pattern::clear_track(int t) {
    if (t >= 0 && t < kTracks) steps[static_cast<size_t>(t)].fill(Off);
}

bool Pattern::operator==(const Pattern& o) const {
    return name == o.name && bpm == o.bpm && swing == o.swing && mute == o.mute && vol == o.vol && steps == o.steps;
}

// ── Text ────────────────────────────────────────────────────────────────────

std::string clean_name(const std::string& name) {
    std::string s;
    for (char ch : name) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (ch == ':' || u < 32 || u == 127) continue;
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

std::string to_text(const Pattern& p) {
    static const char kHex[] = "0123456789abcdef";
    std::string s = "B1:" + clean_name(p.name) + ":" + std::to_string(std::clamp(p.bpm, kMinBpm, kMaxBpm)) + ":" +
                    std::to_string(std::clamp(p.swing, kMinSwing, kMaxSwing)) + ":";
    for (uint8_t v : p.vol) s += static_cast<char>('0' + std::min<int>(v, kMaxVol));
    s += ':';
    s += kHex[(p.mute >> 4) & 0xf];
    s += kHex[p.mute & 0xf];
    s += ':';
    for (const auto& row : p.steps) {
        for (int i = 0; i < kSteps; i += 2) {
            const int a = std::min<int>(row[static_cast<size_t>(i)], Accent);
            const int b = std::min<int>(row[static_cast<size_t>(i + 1)], Accent);
            s += kHex[a * 4 + b];
        }
    }
    return s;
}

bool from_text(const std::string& text_in, Pattern& out) {
    const std::vector<std::string> f = split(trim(text_in), ':');
    if (f.size() != 7 || trim(f[0]) != "B1") return false;
    Pattern p;
    p.name = clean_name(f[1]);
    int bpm = 0, swing = 0;
    if (!parse_int(f[2], bpm) || !parse_int(f[3], swing)) return false;
    p.bpm = std::clamp(bpm, kMinBpm, kMaxBpm);
    p.swing = std::clamp(swing, kMinSwing, kMaxSwing);
    const std::string vol = trim(f[4]);
    if (vol.size() > static_cast<size_t>(kTracks)) return false;
    for (size_t i = 0; i < vol.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(vol[i]))) return false;
        p.vol[i] = static_cast<uint8_t>(vol[i] - '0');
    }
    const std::string mute = trim(f[5]);
    if (mute.empty() || mute.size() > 2) return false;
    int m = 0;
    for (char ch : mute) {
        const int h = hex_val(ch);
        if (h < 0) return false;
        m = m * 16 + h;
    }
    p.mute = static_cast<uint8_t>(m);
    const std::string hex = trim(f[6]);
    if (hex.size() > static_cast<size_t>(kTracks * kSteps / 2)) return false;
    for (size_t i = 0; i < hex.size(); ++i) {
        const int h = hex_val(hex[i]);
        if (h < 0) return false;
        const size_t t = i / (kSteps / 2);
        const size_t s = (i % (kSteps / 2)) * 2;
        p.steps[t][s] = static_cast<uint8_t>(std::min(h / 4, 2));
        p.steps[t][s + 1] = static_cast<uint8_t>(std::min(h % 4, 2));
    }
    out = std::move(p);
    return true;
}

bool from_rows(const std::string& name, int bpm, int swing, const std::vector<std::string>& rows, Pattern& out) {
    if (rows.size() > static_cast<size_t>(kTracks)) return false;
    Pattern p;
    p.name = clean_name(name);
    p.bpm = std::clamp(bpm, kMinBpm, kMaxBpm);
    p.swing = std::clamp(swing, kMinSwing, kMaxSwing);
    for (size_t t = 0; t < rows.size(); ++t) {
        const std::string& r = rows[t];
        if (r.empty()) continue;
        if (r.size() != static_cast<size_t>(kSteps)) return false;
        for (size_t s = 0; s < r.size(); ++s) {
            switch (r[s]) {
                case 'x': p.steps[t][s] = On; break;
                case 'X': p.steps[t][s] = Accent; break;
                case '.': case '-': p.steps[t][s] = Off; break;
                default: return false;
            }
        }
    }
    out = std::move(p);
    return true;
}

// ── Timing / sound ──────────────────────────────────────────────────────────

double step_ms(int bpm) { return 15000.0 / std::clamp(bpm, kMinBpm, kMaxBpm); }

uint32_t bar_ms(int bpm) { return static_cast<uint32_t>(std::lround(step_ms(bpm) * kSteps)); }

double step_start_ms(const Pattern& p, int s) {
    const double st = step_ms(p.bpm);
    const double swing = std::clamp(p.swing, kMinSwing, kMaxSwing) / 100.0;
    double t = s * st;
    if (s & 1) t += 2.0 * st * (swing - 0.5);
    return t;
}

int step_at(const Pattern& p, double ms) {
    const double st = step_ms(p.bpm);
    int k = std::clamp(static_cast<int>(ms / st), 0, kSteps - 1);
    if ((k & 1) && ms < step_start_ms(p, k)) --k;
    return k;
}

size_t loop_samples(int bpm, int bars, int rate) {
    bpm = std::clamp(bpm, kMinBpm, kMaxBpm);
    return static_cast<size_t>(std::llround(std::max(1, bars) * 240.0 * rate / bpm));
}

int loop_bars(int bpm, uint32_t min_ms) {
    const uint32_t bar = std::max<uint32_t>(1, bar_ms(bpm));
    return std::clamp(static_cast<int>((min_ms + bar - 1) / bar), 1, 8);
}

std::vector<int16_t> render(const Pattern& p, int bars, int rate) {
    bars = std::max(1, bars);
    const size_t n = loop_samples(p.bpm, bars, rate);
    std::vector<float> mix(n, 0.0f);
    if (n == 0 || rate <= 0) return {};
    const double per_ms = rate / 1000.0;
    const double bar = 240000.0 / std::clamp(p.bpm, kMinBpm, kMaxBpm);

    // Hit start positions (samples) per track over the whole loop.
    std::array<std::vector<size_t>, kTracks> starts;
    std::array<std::vector<uint8_t>, kTracks> kinds;
    for (int t = 0; t < kTracks; ++t) {
        for (int b = 0; b < bars; ++b) {
            for (int s = 0; s < kSteps; ++s) {
                const uint8_t k = p.steps[static_cast<size_t>(t)][static_cast<size_t>(s)];
                if (k == Off) continue;
                const size_t at = static_cast<size_t>(std::lround((b * bar + step_start_ms(p, s)) * per_ms)) % n;
                starts[static_cast<size_t>(t)].push_back(at);
                kinds[static_cast<size_t>(t)].push_back(k);
            }
        }
    }
    // Circular distance from `a` to the next start in `v` after it (n if none but itself).
    auto next_after = [n](const std::vector<size_t>& v, size_t a) {
        size_t best = n;
        for (size_t x : v) {
            const size_t d = x > a ? x - a : x + n - a;
            if (d > 0 && d < best) best = d;
        }
        return best;
    };
    const size_t fade = static_cast<size_t>(std::max(1, rate / 500));   // 2 ms at a choke
    for (int t = 0; t < kTracks; ++t) {
        if (p.muted(t)) continue;
        const Voice& v = voice(t, rate);
        const auto& st = starts[static_cast<size_t>(t)];
        for (size_t h = 0; h < st.size(); ++h) {
            const size_t a = st[h];
            size_t cut = next_after(st, a);                       // one voice per track
            if (t == OpenHat && !p.muted(Hat)) cut = std::min(cut, next_after(starts[Hat], a));   // choke
            const size_t len = std::min(v.size(), cut);
            const float g = hit_gain(t, kinds[static_cast<size_t>(t)][h] == Accent, p.vol[static_cast<size_t>(t)]);
            for (size_t i = 0; i < len; ++i) {
                float x = v[i] * g;
                if (len < v.size() && len - i <= fade) x *= static_cast<float>(len - i) / fade;
                mix[(a + i) % n] += x;
            }
        }
    }
    std::vector<int16_t> pcm(n);
    for (size_t i = 0; i < n; ++i) pcm[i] = to_pcm(mix[i]);
    return pcm;
}

std::vector<int16_t> render_hit(int track, bool accent, int vol, int rate) {
    const Voice& v = voice(track, rate);
    const float g = hit_gain(std::clamp(track, 0, kTracks - 1), accent, std::max(vol, 3));
    std::vector<int16_t> pcm(v.size() + static_cast<size_t>(rate / 20), 0);
    for (size_t i = 0; i < v.size(); ++i) pcm[i] = to_pcm(v[i] * g);
    return pcm;
}

// ── Sharing ─────────────────────────────────────────────────────────────────

std::string share_text(const Pattern& p) { return std::string(kSharePrefix) + to_text(p); }

bool fits_share(const Pattern& p) { return share_text(p).size() <= kMaxShareBytes; }

bool parse_shared(const std::string& message, Pattern& out) {
    const std::string m = trim(message);
    const std::string tag = trim(kSharePrefix);                // "~BEAT~"
    if (m.compare(0, tag.size(), tag) != 0) return false;
    Pattern p;
    if (!from_text(m.substr(tag.size()), p) || p.empty()) return false;
    if (p.name.empty()) p.name = "Shared beat";
    out = std::move(p);
    return true;
}

// ── Library ─────────────────────────────────────────────────────────────────

const std::vector<Preset>& preset_list() {
    // Original patterns written for 1999 Online. Rows: KICK SNARE HAT OPEN
    // CLAP TOM BELL BASS; 'x' hit, 'X' accent.
    static const std::vector<Preset> kList = {
        {"Four on the Floor", 124, 50,
         {"X...x...X...x...", "................", "x..xx..xx..xx..x", "..x...x...x...x.",
          "....X.......X...", "................", "................", "..x...x...x..xx."},
         "club thump, offbeat hats"},
        {"Y2K Breaks", 132, 54,
         {"X.........X.x...", "....X..x.x..X..x", "x.x.x.x.x.x.x..x", "..............x.",
          "................", "...........x....", "................", "x.........x.x..."},
         "chopped break, ghost snares"},
        {"Dialup Funk", 100, 58,
         {"X..x..x...X..x..", "....X.......X...", "x.xxx.x.x.xxx.xx", ".......x........",
          "............x...", "..............xx", "..x.....x.x.....", "x..x..x...x..x.x"},
         "busy hats, bell answers"},
        {"Pager Hop", 90, 62,
         {"X......x..X.....", "....X.......X...", "x.x.x.x.x.x.x.x.", "................",
          "....x.......x...", "................", "..........x.....", "X......x..x....."},
         "lazy swing, head nod"},
        {"Mall Rat", 160, 50,
         {"X.x.....X.x.....", "....X.......X...", "x.x.x.x.x.x.x...", "..............x.",
          "................", "............x.xx", "................", "x.x.x.x.x.x.x.x."},
         "fast eighths, tom fill"},
        {"Screensaver", 76, 56,
         {"X.......x.x.....", "........X.......", "..x.......x...x.", "......x.........",
          "........x.......", "...x..........x.", "x...............", "x.......x..x...."},
         "slow and spacey"},
        {"Modem Electro", 116, 50,
         {"X.....x...X.....", "....X.......X...", "x.xxx.xxx.xxx.xx", "................",
          "....x.......x...", "................", "..x..x....x..x..", "x..x..x...x..x.."},
         "robot bell, syncopated bass"},
    };
    return kList;
}

const std::vector<Pattern>& presets() {
    static const std::vector<Pattern> kPatterns = [] {
        std::vector<Pattern> v;
        for (const Preset& pr : preset_list()) {
            std::vector<std::string> rows;
            for (const char* r : pr.rows) rows.emplace_back(r ? r : "");
            Pattern p;
            if (from_rows(pr.name, pr.bpm, pr.swing, rows, p)) v.push_back(std::move(p));
        }
        return v;
    }();
    return kPatterns;
}

Library::Library(std::string path) : path_(std::move(path)) {}

std::string Library::file() const { return path_.empty() ? store::path("beats.json") : path_; }

bool Library::load() {
    JsonValue doc;
    if (store::load_json(file(), doc) != store::Load::Ok) return false;
    std::array<Pattern, kUserSlots> slots{};
    const auto& items = doc["slots"].items();
    for (size_t i = 0; i < items.size() && i < static_cast<size_t>(kUserSlots); ++i) {
        Pattern p;
        if (!items[i].is_string() || items[i].str().empty() || !from_text(items[i].str(), p)) continue;
        if (p.name.empty()) p.name = "Untitled";
        slots[i] = std::move(p);
    }
    slots_ = std::move(slots);
    return true;
}

bool Library::save() const {
    std::string s = "{\n  \"version\": 1,\n  \"slots\": [";
    for (int i = 0; i < kUserSlots; ++i) {
        s += i ? ",\n    " : "\n    ";
        s += store::quote(used(i) ? to_text(slots_[static_cast<size_t>(i)]) : std::string());
    }
    s += "\n  ]\n}\n";
    return store::write_atomic(file(), s);
}

// ── Playback ────────────────────────────────────────────────────────────────

LoopPlayer::~LoopPlayer() {
    if (pid_ > 0) dying_.push_back({pid_, 0});
    pid_ = -1;
    for (const Dying& d : dying_) {                    // exiting: make sure, then reap
        kill(d.pid, SIGKILL);
        int st = 0;
        while (waitpid(d.pid, &st, 0) < 0 && errno == EINTR) {}
    }
    dying_.clear();
}

void LoopPlayer::find_backend() {
    looked_ = true;
    // PipeWire/Pulse before aplay: on the Cardputer Zero plain aplay opens the
    // HDMI device and fails, while pw-play reaches the speaker.
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

const char* LoopPlayer::backend_name() {
    if (!looked_ || name_.empty()) find_backend();
    return name_.empty() ? "none" : name_.c_str();
}

bool LoopPlayer::play(const std::vector<int16_t>& pcm, int rate) {
    stop();
    error_.clear();
    if (pcm.empty()) {
        error_ = "Nothing to play";
        return false;
    }
    if (!available()) {
        error_ = "No audio player found (pw-play / paplay / aplay)";
        return false;
    }
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = "/tmp";
    flip_ ^= 1;
    const std::string path =
        (dir / ("1999online-beat-" + std::to_string(getpid()) + "-" + std::to_string(flip_) + ".wav")).string();
    if (!ring::write_wav(path, pcm, rate)) {
        error_ = "Could not write " + path;
        return false;
    }
    last_path_ = path;
    return spawn(path);
}

bool LoopPlayer::replay() {
    if (last_path_.empty()) return false;
    stop();
    return spawn(last_path_);
}

bool LoopPlayer::spawn(const std::string& path) {
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
    return true;
}

void LoopPlayer::stop() {
    if (pid_ <= 0) return;
    kill(pid_, SIGTERM);
    dying_.push_back({pid_, 0});
    pid_ = -1;
}

void LoopPlayer::tick() {
    if (pid_ > 0) {
        int status = 0;
        const pid_t r = waitpid(pid_, &status, WNOHANG);
        if (r == pid_ || (r < 0 && errno != EINTR)) {
            if (r == pid_ && WIFEXITED(status) && WEXITSTATUS(status) == 127) error_ = "Could not run " + name_;
            pid_ = -1;
        }
    }
    for (size_t i = 0; i < dying_.size();) {
        int status = 0;
        const pid_t r = waitpid(dying_[i].pid, &status, WNOHANG);
        if (r == dying_[i].pid || (r < 0 && errno != EINTR)) {
            dying_.erase(dying_.begin() + static_cast<long>(i));
            continue;
        }
        if (++dying_[i].ticks == 15) kill(dying_[i].pid, SIGKILL);   // ~0.5 s and still there
        ++i;
    }
}

LoopPlayer& player() {
    static LoopPlayer p;
    return p;
}

} // namespace mm::beat
