#include "mail_chime.h"

#include "settings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace mm {
namespace {

constexpr double kPi = 3.14159265358979323846;

void put_u32(std::string& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void put_u16(std::string& o, uint16_t v) {
    o.push_back(static_cast<char>(v & 0xff));
    o.push_back(static_cast<char>(v >> 8));
}

std::string find_on_path(const char* exe) {
    const char* path = std::getenv("PATH");
    std::string p = path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= p.size()) {
        const size_t end = p.find(':', start);
        const std::string dir = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            const std::string full = dir + "/" + exe;
            if (::access(full.c_str(), X_OK) == 0) return full;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::string();
}

uint32_t steady_ms() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

pid_t g_child = -1;
bool g_played = false;
uint32_t g_last_ms = 0;
std::string g_wav_path;      // written once per run
std::string g_player;        // full path, "" = not found yet
bool g_looked = false;

} // namespace

std::vector<int16_t> mail_chime_pcm(int rate) {
    // A bright rising arpeggio, each note a little bell: fundamental plus a
    // soft octave and a slightly sharp twelfth, quick attack, exponential tail.
    struct Note {
        double freq, start_s, tau_s, gain;
    };
    const Note notes[] = {
        {783.99, 0.000, 0.16, 0.80},    // G5
        {1046.50, 0.095, 0.16, 0.80},   // C6
        {1318.51, 0.190, 0.18, 0.80},   // E6
        {1567.98, 0.285, 0.42, 1.00},   // G6, held
    };
    const double total_s = 1.15;
    const int n = static_cast<int>(total_s * rate);
    std::vector<double> mix(static_cast<size_t>(n), 0.0);
    for (const Note& note : notes) {
        const int first = static_cast<int>(note.start_s * rate);
        for (int i = first; i < n; ++i) {
            const double t = static_cast<double>(i - first) / rate;
            const double env = std::min(1.0, t / 0.004) * std::exp(-t / note.tau_s);
            if (env < 1e-4 && t > 0.01) break;
            const double w = 2.0 * kPi * note.freq * t;
            mix[static_cast<size_t>(i)] +=
                note.gain * env * (std::sin(w) + 0.30 * std::sin(2.0 * w) + 0.12 * std::sin(3.01 * w));
        }
    }
    double peak = 1e-9;
    for (double v : mix) peak = std::max(peak, std::fabs(v));
    // Fade the last 30 ms so the end never clicks.
    const int fade = rate * 30 / 1000;
    std::vector<int16_t> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        double v = mix[static_cast<size_t>(i)] / peak * 0.55;
        if (i > n - fade) v *= static_cast<double>(n - i) / fade;
        out[static_cast<size_t>(i)] = static_cast<int16_t>(std::lround(v * 32767.0));
    }
    return out;
}

std::string mail_chime_wav(int rate) {
    const std::vector<int16_t> pcm = mail_chime_pcm(rate);
    const uint32_t data = static_cast<uint32_t>(pcm.size() * 2);
    std::string o;
    o.reserve(44 + data);
    o += "RIFF";
    put_u32(o, 36 + data);
    o += "WAVEfmt ";
    put_u32(o, 16);
    put_u16(o, 1);                                   // PCM
    put_u16(o, 1);                                   // mono
    put_u32(o, static_cast<uint32_t>(rate));
    put_u32(o, static_cast<uint32_t>(rate) * 2);     // byte rate
    put_u16(o, 2);                                   // block align
    put_u16(o, 16);
    o += "data";
    put_u32(o, data);
    for (int16_t s : pcm) put_u16(o, static_cast<uint16_t>(s));
    return o;
}

bool play_mail_chime() {
    if (!store::enabled()) return false;                       // --shots: silent
    if (!settings().get_bool("mail.sound", true)) return false;

    // Reap the previous chime; never stack two.
    if (g_child > 0) {
        int status = 0;
        if (::waitpid(g_child, &status, WNOHANG) == 0) return false;   // still playing
        g_child = -1;
    }
    const uint32_t now = steady_ms();
    if (g_played && now - g_last_ms < kMailChimeMinGapMs) return false;

    if (!g_looked) {
        g_looked = true;
        for (const char* exe : {"afplay", "pw-play", "paplay", "aplay"}  /* aplay last: HDMI on the Cardputer */) {
            g_player = find_on_path(exe);
            if (!g_player.empty()) break;
        }
    }
    if (g_player.empty()) return false;

    if (g_wav_path.empty()) {
        const char* tmp = std::getenv("TMPDIR");
        std::string dir = tmp && *tmp ? tmp : "/tmp";
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        const std::string path = dir + "/1999online-mail-" + std::to_string(::getuid()) + ".wav";
        const std::string wav = mail_chime_wav();
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        const bool ok = std::fwrite(wav.data(), 1, wav.size(), f) == wav.size();
        std::fclose(f);
        if (!ok) return false;
        g_wav_path = path;
    }

    const std::string base = g_player.substr(g_player.find_last_of('/') + 1);
    std::vector<std::string> args{g_player};
    if (base == "aplay") args.push_back("-q");
    args.push_back(g_wav_path);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, g_player.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return false;
    g_child = pid;
    g_played = true;
    g_last_ms = now;
    return true;
}

} // namespace mm
