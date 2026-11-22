#include "audio_player.h"

#include "mm_json.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

namespace mm {
namespace {

constexpr size_t kMaxFiles = 500;
constexpr size_t kMaxFeedBytes = 1 << 20;
constexpr int kRefreshMinSeconds = 60;
constexpr const char* kMixtapeBase = "https://stream-mixtape-geo.ntslive.net/";
constexpr const char* kMixtapesFeed = "https://www.nts.live/api/v2/mixtapes";
constexpr const char* kLiveFeed = "https://www.nts.live/api/v2/live";

struct BuiltinMix {
    const char* name;
    const char* path;
};
// From https://www.nts.live/api/v2/mixtapes; refresh_stations() replaces it.
constexpr BuiltinMix kBuiltinMixes[] = {
    {"Poolside", "mixtape4"},      {"Slow Focus", "mixtape"},    {"Low Key", "mixtape2"},
    {"Memory Lane", "mixtape6"},   {"4 To The Floor", "mixtape5"}, {"Island Time", "mixtape21"},
    {"The Tube", "mixtape26"},     {"Sheet Music", "mixtape35"}, {"Feelings", "mixtape27"},
    {"Expansions", "mixtape3"},    {"Rap House", "mixtape22"},   {"Labyrinth", "mixtape31"},
    {"Sweat", "mixtape24"},        {"Otaku", "mixtape36"},       {"The Pit", "mixtape34"},
    {"Field Recordings", "mixtape23"},
};

struct BuiltinRetro {
    const char* name;
    const char* short_name;
    const char* country;
    const char* url;
};
// 90s / 2000s stations picked from the Radio Browser directory
// (https://www.radio-browser.info, tags 90s / 2000s / eurodance / grunge /
// jungle ...). Every stream answered with audio when the list was made
// (2026-11-20); stations come and go, so expect to prune it now and then.
constexpr BuiltinRetro kBuiltinRetro[] = {
    {"Heart 90s", "90s", "GB", "https://media-ssl.musicradio.com/Heart90sMP3"},
    {"Old Skool Rave Tapes", "RAVE", "GB", "https://azuracast.oldskoolravetapes.co.uk/listen/old_skool_rave_tapes/listen"},
    {"Kniteforce Radio", "RAVE", "GB", "https://kniteforce.out.airtime.pro/kniteforce_a"},
    {"90s90s Hits", "90s", "DE", "http://streams.90s90s.de/pop/mp3-192/"},
    {"90s90s Grunge", "90s", "DE", "http://streams.90s90s.de/grunge/mp3-192/"},
    {"90s90s Techno", "90s", "DE", "http://streams.90s90s.de/techno/mp3-192/"},
    {"Sunshine Live 90er", "90s", "DE", "http://stream.sunshine-live.de/90er/mp3-192/"},
    {"RauteMusik 90s", "90s", "DE", "https://90s-high.rautemusik.fm/"},
    {"0N 90s", "90s", "DE", "https://0n-90s.radionetz.de/0n-90s.mp3"},
    {"0N 2000s", "00s", "DE", "https://0n-2000s.radionetz.de/0n-2000s.mp3"},
    {"bigFM 2000er", "00s", "DE", "https://stream.bigfm.de/2000er/aac-128/"},
    {"Technolovers Eurodance", "RETRO", "DE", "https://stream.technolovers.fm/eurodance"},
    {"EuroDance 90", "90s", "FR", "https://stream-eurodance90.fr/radio/8000/128.mp3"},
    {"Hotmix 90", "90s", "FR", "https://streaming.hotmixradio.com/hotmix-90-en-mp3"},
    {"Qmusic 90s & 00s", "90s", "NL", "https://stream.qmusic.nl/90s-00s/aaclow"},
    {"radio.DANCE 90s", "90s", "NL", "https://radiodance.streamingmedia.it/audio.aac"},
    {"NRJ Bulgaria 90s", "90s", "BG", "http://play.global.audio/energy-90s"},
    {"Nostalgia Radio", "RETRO", "RO", "https://live.radionostalgia.ro:8443/nostalgia.aac"},
    {"Retro Radio", "RETRO", "HU", "https://icast.connectmedia.hu/5001/live.mp3"},
    {"Hit FM 2000s", "00s", "MD", "https://radio3.dixi.md/listen/hit_fm_2000s_hits/hitfm2000hit128.aac"},
    {"Radio Dancefloor 90s", "90s", "IT", "http://streaming.radiodancefloor.it:443/"},
    {"Eurodance-X-Press", "RETRO", "AT", "https://secureonair.krone.at/eurodance.aac"},
    {"Radio ROCK Grunge 90s", "90s", "SK", "https://stream.bauermedia.sk/grunge-90s-hi.aac"},
    {"POP 90", "90s", "TR", "http://23613.live.streamtheworld.com/POP90AAC_SC"},
    {"90s Dance by Mix", "90s", "MX", "https://playerservices.streamtheworld.com/api/livestream-redirect/ACIR11_s01AAC.aac"},
    {"Panamericana Retro Rock", "RETRO", "PE", "https://mdstrm.com/audio/6598b728d982060896866b8a/icecast.audio"},
    {"Static 90s/00s Alt Rock", "90s", "US", "https://r.bgp.rodeo/listen/static/radio.mp3"},
    {"West Coast G-Funk", "RETRO", "US", "https://r.bgp.rodeo/listen/west_coast/radio.mp3"},
    {"181.FM Old School", "RETRO", "US", "http://listen.181fm.com/181-oldschool_128k.mp3"},
    {"Splash 90s Dance", "90s", "ES", "https://c6.auracast.net/radio/8090/radio.mp3"},
    {"George FM Club Classics", "RETRO", "NZ", "https://playerservices.streamtheworld.com/api/livestream-redirect/GEORGEFM_CC_S01.aac"},
    {"Chili Classics", "RETRO", "TH", "https://stream.chiliradio.app/chiliclassics"},
    {"Radio Britannia", "RETRO", "AR", "http://01.solumedia.com.ar:8472/stream/;"},
    {"Dance Wave Retro", "RETRO", "", "https://dancewave.online/retrodance.mp3"},
};

AudioPlayer::Station live_station(int n) {
    AudioPlayer::Station s;
    s.name = n == 1 ? "NTS 1" : "NTS 2";
    s.short_name = s.name;
    s.url = n == 1 ? "https://stream-relay-geo.ntslive.net/stream"
                   : "https://stream-relay-geo.ntslive.net/stream2";
    s.live = true;
    return s;
}

AudioPlayer::Station mix_station(const std::string& name, const std::string& url) {
    AudioPlayer::Station s;
    s.name = name;
    s.short_name = "MIX";
    s.url = url;
    return s;
}

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool is_music_ext(const std::string& ext) {
    return ext == "mp3" || ext == "ogg" || ext == "flac" || ext == "wav" || ext == "m4a";
}

// The feed HTML-escapes titles ("YESTERDAY&#039;S NEWS").
std::string decode_entities(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] == '&') {
            const size_t semi = in.find(';', i);
            if (semi != std::string::npos && semi - i <= 8) {
                const std::string ent = in.substr(i + 1, semi - i - 1);
                char rep = 0;
                if (ent == "amp") rep = '&';
                else if (ent == "quot") rep = '"';
                else if (ent == "apos") rep = '\'';
                else if (ent == "lt") rep = '<';
                else if (ent == "gt") rep = '>';
                else if (ent.size() > 1 && ent[0] == '#') {
                    const bool hex = ent[1] == 'x' || ent[1] == 'X';
                    char* end = nullptr;
                    const long v = std::strtol(ent.c_str() + (hex ? 2 : 1), &end, hex ? 16 : 10);
                    if (end && *end == 0 && v >= 32 && v < 127) rep = static_cast<char>(v);
                }
                if (rep) {
                    out.push_back(rep);
                    i = semi + 1;
                    continue;
                }
            }
        }
        out.push_back(in[i++]);
    }
    return out;
}

// Only ever plays what NTS's own stream hosts serve, whatever the feed says.
bool is_nts_stream_url(const std::string& url) {
    static const char* const kPrefix = "https://";
    static const char* const kSuffix = ".ntslive.net";
    if (url.compare(0, 8, kPrefix) != 0) return false;
    const size_t slash = url.find('/', 8);
    if (slash == std::string::npos) return false;
    const std::string host = url.substr(8, slash - 8);
    const size_t n = std::char_traits<char>::length(kSuffix);
    if (host.size() <= n || host.compare(host.size() - n, n, kSuffix) != 0) return false;
    for (char ch : url) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u <= 32 || u >= 127) return false;
    }
    return true;
}

// Fixed command lines only (no caller data reaches the shell).
std::string run_fixed(const char* cmd) {
    std::string out;
    FILE* p = popen(cmd, "r");
    if (!p) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) {
        if (out.size() + n > kMaxFeedBytes) {
            out.clear();
            break;
        }
        out.append(buf, n);
    }
    pclose(p);
    return out;
}

std::string find_on_path(const char* name) {
    const char* path = std::getenv("PATH");
    std::string dirs = path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos) end = dirs.size();
        std::string dir = dirs.substr(start, end - start);
        if (dir.empty()) dir = ".";
        const std::string full = dir + "/" + name;
        struct stat st {};
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(full.c_str(), X_OK) == 0) {
            return full;
        }
        start = end + 1;
    }
    return {};
}

} // namespace

struct AudioPlayer::Mailbox {
    std::mutex mu;
    bool busy = false;
    bool ready = false;
    bool have_mixes = false;
    std::vector<Station> mixes;
    bool have_live = false;
    std::string now1, now2;
};

AudioPlayer::AudioPlayer() : mailbox_(std::make_shared<Mailbox>()) {
    stations_.push_back(live_station(1));
    stations_.push_back(live_station(2));
    for (const BuiltinMix& m : kBuiltinMixes) {
        stations_.push_back(mix_station(m.name, std::string(kMixtapeBase) + m.path));
    }
    for (const BuiltinRetro& r : kBuiltinRetro) {
        Station st;
        st.name = r.name;
        st.short_name = r.short_name;
        st.country = r.country;
        st.url = r.url;
        st.live = true;
        retro_.push_back(std::move(st));
    }
    const char* env = std::getenv("PC1999_MUSIC");
    if (env && *env) {
        music_dir_ = env;
    } else {
        const char* home = std::getenv("HOME");
        music_dir_ = std::string(home && *home ? home : ".") + "/Music";
    }
    sysvol_ = !find_on_path("wpctl").empty();
    if (sysvol_) {
        // "Volume: 0.60" or "Volume: 0.60 [MUTED]"
        const std::string out = run_fixed("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null");
        const size_t at = out.find("Volume:");
        if (at != std::string::npos) {
            const double v = std::strtod(out.c_str() + at + 7, nullptr);
            volume_ = std::clamp(static_cast<int>(v * 100.0 + 0.5), 0, 100);
            muted_ = out.find("MUTED") != std::string::npos;
            sysvol_applied_ = true;
        }
    }
}

AudioPlayer::~AudioPlayer() {
    kill_child();
}

AudioPlayer& audio_player() {
    static AudioPlayer player;
    return player;
}

// ── Library ─────────────────────────────────────────────────────────────────

std::string AudioPlayer::music_dir_label() const {
    const char* home = std::getenv("HOME");
    if (home && *home) {
        const std::string h = home;
        if (music_dir_ == h) return "~";
        if (music_dir_.compare(0, h.size() + 1, h + "/") == 0) return "~" + music_dir_.substr(h.size());
    }
    return music_dir_;
}

const std::vector<AudioPlayer::Track>& AudioPlayer::files() {
    if (!scanned_) rescan_files();
    return files_;
}

void AudioPlayer::rescan_files() {
    namespace fs = std::filesystem;
    const std::string playing_path =
        (kind_ == Kind::File && index_ >= 0 && index_ < static_cast<int>(files_.size()))
            ? files_[index_].path : std::string();

    std::vector<Track> found;
    auto add_file = [&](const fs::directory_entry& e) {
        std::error_code ec;
        if (!e.is_regular_file(ec)) return;
        const fs::path& p = e.path();
        const std::string name = p.filename().string();
        if (name.empty() || name[0] == '.') return;
        std::string ext = lower(p.extension().string());
        if (!ext.empty()) ext.erase(0, 1);
        if (!is_music_ext(ext)) return;
        Track t;
        t.path = p.string();
        t.title = p.stem().string();
        t.ext = ext;
        found.push_back(std::move(t));
    };
    // The directory itself plus one level of sub-directories (album folders).
    // Stop reading well past the cap so a huge library cannot stall the UI.
    const size_t kScanLimit = kMaxFiles * 8;
    std::error_code ec;
    for (fs::directory_iterator it(music_dir_, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && found.size() < kScanLimit; it.increment(ec)) {
        std::error_code ec2;
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (it->is_directory(ec2)) {
            for (fs::directory_iterator sub(it->path(), fs::directory_options::skip_permission_denied, ec2), send;
                 !ec2 && sub != send && found.size() < kScanLimit; sub.increment(ec2)) {
                add_file(*sub);
            }
        } else {
            add_file(*it);
        }
    }
    std::sort(found.begin(), found.end(), [](const Track& a, const Track& b) {
        const std::string la = lower(a.title), lb = lower(b.title);
        return la != lb ? la < lb : a.path < b.path;
    });
    if (found.size() > kMaxFiles) found.resize(kMaxFiles);

    files_ = std::move(found);
    scanned_ = true;
    if (kind_ == Kind::File) {
        index_ = -1;
        for (size_t i = 0; i < files_.size(); ++i) {
            if (files_[i].path == playing_path) index_ = static_cast<int>(i);
        }
    }
    ++revision_;
}

bool AudioPlayer::parse_mixtapes(const std::string& json, std::vector<Station>& out) {
    JsonValue doc;
    if (!json_parse(json, doc) || !doc["results"].is_array()) return false;
    std::vector<Station> mixes;
    for (const JsonValue& r : doc["results"].items()) {
        const std::string title = decode_entities(r.get_string("title"));
        const std::string url = r.get_string("audio_stream_endpoint");
        if (title.empty() || title.size() > 60 || !is_nts_stream_url(url)) continue;
        mixes.push_back(mix_station(title, url));
        if (mixes.size() >= 64) break;
    }
    if (mixes.empty()) return false;
    out = std::move(mixes);
    return true;
}

bool AudioPlayer::parse_live(const std::string& json, std::string& now1, std::string& now2) {
    JsonValue doc;
    if (!json_parse(json, doc) || !doc["results"].is_array()) return false;
    bool any = false;
    for (const JsonValue& r : doc["results"].items()) {
        const std::string ch = r["channel_name"].as_string();
        std::string title = decode_entities(r["now"].get_string("broadcast_title"));
        if (title.size() > 120) title.resize(120);
        if (ch == "1") { now1 = title; any = true; }
        else if (ch == "2") { now2 = title; any = true; }
    }
    return any;
}

void AudioPlayer::refresh_stations() {
    const auto now = std::chrono::steady_clock::now();
    if (refreshed_once_ && now - last_refresh_ < std::chrono::seconds(kRefreshMinSeconds)) return;
    {
        std::lock_guard<std::mutex> lock(mailbox_->mu);
        if (mailbox_->busy) return;
        mailbox_->busy = true;
    }
    refreshed_once_ = true;
    last_refresh_ = now;
    // Detached: the thread owns a reference to the mailbox, so it may outlive
    // the player (curl has a 10 s timeout) without blocking shutdown.
    std::shared_ptr<Mailbox> box = mailbox_;
    try {
        std::thread([box] {
            const std::string mixes_json = run_fixed((std::string("curl -s -m 10 ") + kMixtapesFeed + " 2>/dev/null").c_str());
            const std::string live_json = run_fixed((std::string("curl -s -m 10 ") + kLiveFeed + " 2>/dev/null").c_str());
            std::vector<Station> mixes;
            std::string now1, now2;
            const bool have_mixes = parse_mixtapes(mixes_json, mixes);
            const bool have_live = parse_live(live_json, now1, now2);
            std::lock_guard<std::mutex> lock(box->mu);
            box->have_mixes = have_mixes;
            box->mixes = std::move(mixes);
            box->have_live = have_live;
            box->now1 = std::move(now1);
            box->now2 = std::move(now2);
            box->ready = have_mixes || have_live;
            box->busy = false;
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(mailbox_->mu);
        mailbox_->busy = false;
    }
}

void AudioPlayer::apply_mailbox() {
    std::vector<Station> mixes;
    std::string now1, now2;
    bool have_mixes = false, have_live = false;
    {
        std::lock_guard<std::mutex> lock(mailbox_->mu);
        if (!mailbox_->ready) return;
        mailbox_->ready = false;
        have_mixes = mailbox_->have_mixes;
        have_live = mailbox_->have_live;
        mixes = std::move(mailbox_->mixes);
        now1 = std::move(mailbox_->now1);
        now2 = std::move(mailbox_->now2);
    }
    const std::string playing_url =
        (kind_ == Kind::Station && index_ >= 0 && index_ < static_cast<int>(stations_.size()))
            ? stations_[index_].url : std::string();
    if (have_mixes) {
        Station playing;
        const bool keep = !playing_url.empty();
        if (keep) playing = stations_[index_];
        stations_.resize(2);                       // the two live channels stay put
        for (Station& m : mixes) stations_.push_back(std::move(m));
        if (keep) {
            index_ = -1;
            for (size_t i = 0; i < stations_.size(); ++i) {
                if (stations_[i].url == playing_url) index_ = static_cast<int>(i);
            }
            if (index_ < 0) {                      // dropped from the feed: keep it listed while it plays
                stations_.push_back(playing);
                index_ = static_cast<int>(stations_.size()) - 1;
            }
        }
    }
    if (have_live) {
        stations_[0].now = now1;
        stations_[1].now = now2;
    }
    ++revision_;
}

// ── Playback ────────────────────────────────────────────────────────────────

void AudioPlayer::find_backend() {
    backend_path_ = find_on_path("mpv");
    if (!backend_path_.empty()) {
        backend_ = Backend::Mpv;
        return;
    }
    backend_path_ = find_on_path("ffplay");
    backend_ = backend_path_.empty() ? Backend::None : Backend::Ffplay;
}

const char* AudioPlayer::backend_name() {
    if (backend_ == Backend::Unknown) find_backend();
    switch (backend_) {
        case Backend::Mpv: return "mpv";
        case Backend::Ffplay: return "ffplay";
        default: return "none";
    }
}

bool AudioPlayer::spawn(const std::string& target) {
    kill_child();
    // Look again each time while nothing is installed (the user may fix that).
    if (backend_ == Backend::Unknown || backend_ == Backend::None) find_backend();
    if (backend_ == Backend::None) return false;

    // Everything the child needs is built before fork(): after it, only
    // async-signal-safe calls are allowed in a multi-threaded process.
    std::vector<std::string> args;
    const int vol = sysvol_ ? 100 : volume_;     // system volume does the turning down
    if (backend_ == Backend::Mpv) {
        args = {"mpv", "--no-video", "--really-quiet", "--volume=" + std::to_string(vol),
                "--", target};
    } else {
        args = {"ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet",
                "-volume", std::to_string(vol), "-i", target};
    }
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    const char* exe = backend_path_.c_str();

    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // No terminal: the players read key commands from stdin otherwise.
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        execv(exe, argv.data());
        _exit(127);
    }
    pid_ = pid;
    started_ = std::chrono::steady_clock::now();
    if (tap_on_) start_tap();
    return true;
}

// ── PCM tap ─────────────────────────────────────────────────────────────────

std::string AudioPlayer::current_target() const {
    if (kind_ == Kind::File && index_ >= 0 && index_ < static_cast<int>(files_.size())) return files_[index_].path;
    if (kind_ == Kind::Station && index_ >= 0 && index_ < static_cast<int>(stations_.size())) return stations_[index_].url;
    if (kind_ == Kind::Retro && index_ >= 0 && index_ < static_cast<int>(retro_.size())) return retro_[index_].url;
    return {};
}

void AudioPlayer::set_tap(bool on) {
    if (tap_on_ == on) return;
    tap_on_ = on;
    if (on && state_ == State::Playing) start_tap();
    if (!on) stop_tap();
}

void AudioPlayer::start_tap() {
    stop_tap();
    const std::string target = current_target();
    const std::string ffmpeg = find_on_path("ffmpeg");
    if (target.empty() || ffmpeg.empty()) return;

    // "-re" paces the decode at playing speed; a file joins at the point the
    // player has reached.
    std::vector<std::string> args = {"ffmpeg", "-nostdin", "-loglevel", "quiet", "-re"};
    if (kind_ == Kind::File && elapsed_seconds() > 0) {
        args.push_back("-ss");
        args.push_back(std::to_string(elapsed_seconds()));
    }
    for (const char* a : {"-i", target.c_str(), "-vn", "-ac", "1", "-ar"}) args.emplace_back(a);
    args.push_back(std::to_string(kTapRate));
    for (const char* a : {"-f", "s16le", "pipe:1"}) args.emplace_back(a);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    int fds[2];
    if (pipe(fds) != 0) return;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0) {
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execv(ffmpeg.c_str(), argv.data());
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    tap_pid_ = pid;
    tap_fd_ = fds[0];
}

void AudioPlayer::stop_tap() {
    if (tap_fd_ >= 0) close(tap_fd_);
    tap_fd_ = -1;
    if (tap_pid_ <= 0) return;
    const pid_t pid = tap_pid_;
    tap_pid_ = -1;
    kill(pid, SIGKILL);                      // a decoder with nothing to save
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

size_t AudioPlayer::read_pcm(int16_t* out, size_t max_samples) {
    if (tap_fd_ < 0 || max_samples == 0) return 0;
    const ssize_t n = read(tap_fd_, out, max_samples * sizeof(int16_t));
    if (n > 0) return static_cast<size_t>(n) / sizeof(int16_t);
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) stop_tap();   // decoder gone
    return 0;
}

void AudioPlayer::kill_child() {
    stop_tap();
    if (pid_ <= 0) return;
    const pid_t pid = pid_;
    pid_ = -1;
    kill(pid, SIGTERM);
    // Both players exit promptly on SIGTERM; wait briefly, then insist.
    for (int i = 0; i < 100; ++i) {
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR)) return;
        usleep(5000);
    }
    kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

bool AudioPlayer::play_station(int i) {
    if (i < 0 || i >= static_cast<int>(stations_.size())) return false;
    ++revision_;
    kind_ = Kind::Station;
    index_ = i;
    if (!spawn(stations_[i].url)) {
        state_ = State::Stopped;
        return false;
    }
    state_ = State::Playing;
    return true;
}

bool AudioPlayer::play_retro(int i) {
    if (i < 0 || i >= static_cast<int>(retro_.size())) return false;
    ++revision_;
    kind_ = Kind::Retro;
    index_ = i;
    if (!spawn(retro_[i].url)) {
        state_ = State::Stopped;
        return false;
    }
    state_ = State::Playing;
    return true;
}

bool AudioPlayer::play_file(int i) {
    const std::vector<Track>& list = files();
    if (i < 0 || i >= static_cast<int>(list.size())) return false;
    ++revision_;
    kind_ = Kind::File;
    index_ = i;
    if (!spawn(list[i].path)) {
        state_ = State::Stopped;
        return false;
    }
    state_ = State::Playing;
    return true;
}

void AudioPlayer::stop() {
    kill_child();
    if (state_ != State::Stopped) {
        state_ = State::Stopped;
        ++revision_;
    }
}

bool AudioPlayer::next() {
    if (kind_ == Kind::File && !files_.empty()) {
        return play_file((std::max(index_, -1) + 1) % static_cast<int>(files_.size()));
    }
    if (kind_ == Kind::Station) {
        return play_station((std::max(index_, -1) + 1) % static_cast<int>(stations_.size()));
    }
    if (kind_ == Kind::Retro && !retro_.empty()) {
        return play_retro((std::max(index_, -1) + 1) % static_cast<int>(retro_.size()));
    }
    return false;
}

bool AudioPlayer::prev() {
    if (kind_ == Kind::File && !files_.empty()) {
        const int n = static_cast<int>(files_.size());
        return play_file(index_ <= 0 ? n - 1 : index_ - 1);
    }
    if (kind_ == Kind::Station) {
        const int n = static_cast<int>(stations_.size());
        return play_station(index_ <= 0 ? n - 1 : index_ - 1);
    }
    if (kind_ == Kind::Retro && !retro_.empty()) {
        const int n = static_cast<int>(retro_.size());
        return play_retro(index_ <= 0 ? n - 1 : index_ - 1);
    }
    return false;
}

void AudioPlayer::tick() {
    apply_mailbox();
    if (pid_ <= 0) return;
    int status = 0;
    const pid_t r = waitpid(pid_, &status, WNOHANG);
    if (r == 0 || (r < 0 && errno == EINTR)) return;      // still running
    // Exited (or already reaped elsewhere: ECHILD).
    pid_ = -1;
    stop_tap();
    const bool clean = r > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    const bool instant = std::chrono::steady_clock::now() - started_ < std::chrono::seconds(1);
    state_ = State::Stopped;
    ++revision_;
    // A finished local file moves on to the next one and stops after the last.
    // A player that died at once (bad file, no audio device) does not, or one
    // broken setup would race through the whole list.
    if (kind_ == Kind::File && clean && !instant && index_ >= 0 &&
        index_ + 1 < static_cast<int>(files_.size())) {
        play_file(index_ + 1);
    }
}

std::string AudioPlayer::title() const {
    if (kind_ == Kind::File && index_ >= 0 && index_ < static_cast<int>(files_.size())) {
        return files_[index_].title;
    }
    if (kind_ == Kind::Station && index_ >= 0 && index_ < static_cast<int>(stations_.size())) {
        const Station& s = stations_[index_];
        return s.now.empty() ? s.name : s.now;
    }
    if (kind_ == Kind::Retro && index_ >= 0 && index_ < static_cast<int>(retro_.size())) {
        return retro_[index_].name;
    }
    return {};
}

int AudioPlayer::elapsed_seconds() const {
    if (state_ != State::Playing) return 0;
    const auto d = std::chrono::steady_clock::now() - started_;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(d).count());
}

void AudioPlayer::set_volume(int v) {
    v = std::clamp(v, 0, 100);
    if (v == volume_ && (!sysvol_ || sysvol_applied_)) return;
    volume_ = v;
    if (sysvol_) {
        char cmd[96];
        std::snprintf(cmd, sizeof(cmd), "wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%% 2>/dev/null", v);
        run_fixed(cmd);
        if (muted_ && v > 0) {
            run_fixed("wpctl set-mute @DEFAULT_AUDIO_SINK@ 0 2>/dev/null");
            muted_ = false;
        }
        sysvol_applied_ = true;
    }
    ++revision_;
}

void AudioPlayer::toggle_mute() {
    if (!sysvol_) return;
    muted_ = !muted_;
    run_fixed(muted_ ? "wpctl set-mute @DEFAULT_AUDIO_SINK@ 1 2>/dev/null"
                     : "wpctl set-mute @DEFAULT_AUDIO_SINK@ 0 2>/dev/null");
    ++revision_;
}

} // namespace mm
