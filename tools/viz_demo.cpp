// viz_demo.cpp — offline check for src/ui/viz.*: renders the presets with the
// synthetic audio source and dumps frames as BMP, times the renderer, and
// exercises the Analyzer. Not part of the app build.
//
//   clang++ -std=c++17 -O2 -Isrc tools/viz_demo.cpp src/ui/viz.cpp src/ui/milk.cpp -o viz_demo
//   viz_demo frames <outdir>     one BMP per preset (frame 120) + sheet.bmp
//   viz_demo bench               ms per frame at 156x60 and 312x121
//   viz_demo analyzer            bass/mid/treb response to a sine + kick
//   viz_demo milk <pack_dir> <list.txt> <outdir> [name-substring ...]
//                                every .milk in the list for 150 frames at
//                                156x60: black / blown-out / frozen / fine
//                                counts, timings, contact sheets; presets
//                                matching a substring are also saved at 312x121
// (add src/ui/milk.cpp to the compile line)
#include "ui/milk.h"
#include "ui/viz.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace mm::viz;

static bool write_bmp(const std::string& path, const uint32_t* px, int w, int h, int scale = 1) {
    const int W = w * scale, H = h * scale;
    const int stride = (W * 3 + 3) & ~3;
    std::vector<uint8_t> out(54 + size_t(stride) * H, 0);
    auto put32 = [&](int off, uint32_t v) { memcpy(&out[off], &v, 4); };
    out[0] = 'B'; out[1] = 'M';
    put32(2, uint32_t(out.size())); put32(10, 54); put32(14, 40);
    put32(18, uint32_t(W)); put32(22, uint32_t(H));
    out[26] = 1; out[28] = 24;
    for (int y = 0; y < H; ++y) {
        uint8_t* row = &out[54 + size_t(stride) * (H - 1 - y)];
        for (int x = 0; x < W; ++x) {
            const uint32_t c = px[(y / scale) * w + (x / scale)];
            row[x * 3 + 0] = uint8_t(c);
            row[x * 3 + 1] = uint8_t(c >> 8);
            row[x * 3 + 2] = uint8_t(c >> 16);
        }
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

struct Stats { double mean, white; };   // mean luma 0..255, fraction of near-white pixels
static Stats stats(const uint32_t* px, int n) {
    double sum = 0; int white = 0;
    for (int i = 0; i < n; ++i) {
        const int r = (px[i] >> 16) & 255, g = (px[i] >> 8) & 255, b = px[i] & 255;
        sum += (r * 3 + g * 6 + b) / 10.0;
        if (r > 240 && g > 240 && b > 240) ++white;
    }
    return {sum / n, double(white) / n};
}

static uint32_t fnv(const uint32_t* px, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; ++i) { h ^= px[i]; h *= 16777619u; }
    return h;
}

static int cmd_frames(const std::string& dir) {
    const int W = 312, H = 121, N = Visualizer::preset_count();
    const int cols = 3, rows = (N + cols - 1) / cols;
    std::vector<uint32_t> sheet(size_t(W * cols) * (H * rows), 0x202020u);
    for (int p = 0; p < N; ++p) {
        uint32_t hash[2] = {0, 0};
        for (int pass = 0; pass < 2; ++pass) {     // twice: must be deterministic
            Visualizer v(W, H);
            v.set_preset(p);
            for (int f = 0; f <= 150; ++f) {
                v.render(fake_audio(f / 30.f));
                if (pass == 0 && (f == 120 || f == 135)) {
                    if (f == 120) {
                        char name[64];
                        snprintf(name, sizeof name, "/preset_%02d.bmp", p);
                        write_bmp(dir + name, v.pixels(), W, H, 2);
                        for (int y = 0; y < H; ++y)
                            memcpy(&sheet[size_t((p / cols) * H + y) * (W * cols) + (p % cols) * W],
                                   v.pixels() + y * W, W * 4);
                    }
                    const Stats s = stats(v.pixels(), W * H);
                    printf("%2d %-20s f%d mean %.1f white %.3f\n", p, Visualizer::preset_name(p), f, s.mean, s.white);
                }
            }
            hash[pass] = fnv(v.pixels(), W * H);
        }
        if (hash[0] != hash[1]) printf("   NOT DETERMINISTIC\n");
    }
    write_bmp(dir + "/sheet.bmp", sheet.data(), W * cols, H * rows);
    return 0;
}

static void bench_size(int W, int H) {
    using clk = std::chrono::steady_clock;
    double worst = 0, total = 0; int frames = 0;
    for (int p = 0; p < Visualizer::preset_count(); ++p) {
        Visualizer v(W, H);
        v.set_preset(p);
        double sum = 0, mx = 0;
        for (int f = 0; f < 300; ++f) {
            const AudioFrame a = fake_audio(f / 30.f);
            const auto t0 = clk::now();
            v.render(a);
            const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
            sum += ms; mx = std::max(mx, ms);
        }
        printf("  %dx%d %-20s avg %.3f ms  max %.3f ms\n", W, H, Visualizer::preset_name(p), sum / 300, mx);
        worst = std::max(worst, sum / 300); total += sum; frames += 300;
    }
    printf("%dx%d: mean %.3f ms, slowest preset avg %.3f ms\n", W, H, total / frames, worst);
}

static int cmd_analyzer() {
    // 11025 Hz: a quiet 1 kHz tone throughout, a 60 Hz kick every 0.5 s, a
    // burst of noise (hi-hat) half way between kicks.
    const int rate = 11025;
    Analyzer an;
    uint32_t seed = 1;
    double clock = 0;
    int pushed = 0;
    float kmax = 0, kmin = 9, hmax = 0;
    for (int f = 0; f < 240; ++f) {
        const float t = f / 30.f;
        const int want = int(t * rate) - pushed;
        std::vector<int16_t> buf(want > 0 ? want : 0);
        for (int i = 0; i < want; ++i, ++pushed) {
            clock = double(pushed) / rate;
            const double ph = std::fmod(clock, 0.5);
            double v = 0.08 * std::sin(2 * M_PI * 1000 * clock);
            v += 0.7 * std::exp(-ph * 18) * std::sin(2 * M_PI * 60 * ph);
            const double hp = std::fmod(clock + 0.25, 0.5);
            seed = seed * 1664525u + 1013904223u;
            // crude high-pass noise: alternate sign
            v += 0.25 * std::exp(-hp * 40) * ((seed >> 16) / 32768.0 - 1.0) * ((pushed & 1) ? 1 : -1);
            buf[i] = int16_t(std::max(-1.0, std::min(1.0, v)) * 32000);
        }
        an.push(buf.data(), buf.size(), rate);
        const AudioFrame a = an.frame(t);
        const double ph = std::fmod(double(t), 0.5);
        if (f >= 120) {
            if (ph < 0.04) kmax = std::max(kmax, a.bass);
            if (ph > 0.35 && ph < 0.45) kmin = std::min(kmin, a.bass);
            if (ph > 0.25 && ph < 0.32) hmax = std::max(hmax, a.treb);
        }
        if (f >= 120 && f < 150) {
            float pk = 0;
            for (float s : a.wave) pk = std::max(pk, std::fabs(s));
            printf("t=%.3f bass %.2f mid %.2f treb %.2f wavepk %.2f spec[0,4,8,12,15] %.2f %.2f %.2f %.2f %.2f\n",
                   t, a.bass, a.mid, a.treb, pk, a.spec[0], a.spec[4], a.spec[8], a.spec[12], a.spec[15]);
        }
    }
    printf("bass on kick (max) %.2f, between kicks (min) %.2f, treb on hat (max) %.2f\n", kmax, kmin, hmax);
    // 44.1 kHz input and then silence.
    Analyzer b;
    std::vector<int16_t> tone(44100);
    for (int i = 0; i < 44100; ++i) tone[i] = int16_t(12000 * std::sin(2 * M_PI * 100 * i / 44100.0));
    AudioFrame a;
    for (int f = 0; f < 30; ++f) { b.push(&tone[f * 1470], 1470, 44100); a = b.frame(f / 30.f); }
    printf("44.1k steady 100 Hz tone: bass %.2f mid %.2f treb %.2f (bass should be ~1)\n", a.bass, a.mid, a.treb);
    for (int f = 30; f < 45; ++f) a = b.frame(f / 30.f);
    printf("after 0.5 s with no pushes: bass %.2f wave[64] %.2f (should be 0)\n", a.bass, a.wave[64]);
    return 0;
}

static int cmd_milk(int argc, char** argv) {
    using clk = std::chrono::steady_clock;
    const std::string pack = argv[2], out = argv[4];
    std::ifstream list(argv[3]);
    std::vector<std::string> files;
    for (std::string line; std::getline(list, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) files.push_back(line);
    }
    int W = 156, H = 60;                           // VIZ_SIZE=312x121 to time another size
    if (const char* e = getenv("VIZ_SIZE")) sscanf(e, "%dx%d", &W, &H);
    const int cols = 6, rows = 8, per = cols * rows;
    std::vector<uint32_t> sheet(size_t(W * cols) * (H * rows), 0x303030u);
    int black = 0, blown = 0, frozen = 0, fine = 0, failed = 0, sheet_no = 0, n = 0, saved = 0;
    double total_ms = 0, worst_avg = 0, worst_frame = 0; std::string worst_name;
    long frames = 0;
    auto flush_sheet = [&] {
        char name[64];
        snprintf(name, sizeof name, "/sheet_%02d.bmp", sheet_no++);
        write_bmp(out + name, sheet.data(), W * cols, H * rows);
        std::fill(sheet.begin(), sheet.end(), 0x303030u);
    };
    std::vector<uint32_t> big_sheet(size_t(312 * 4) * (121 * 4), 0x303030u);
    FILE* log = fopen((out + "/results.txt").c_str(), "w");
    for (const std::string& rel : files) {
        mm::milk::Preset p;
        Visualizer v(W, H);
        if (!mm::milk::load_preset(pack + "/" + rel, p) || !v.load_milk(p)) {
            ++failed;
            if (log) fprintf(log, "LOADFAIL %s\n", rel.c_str());
            continue;
        }
        double sum = 0, mean = 0, white = 0;
        uint32_t h120 = 0;
        for (int f = 0; f <= 150; ++f) {
            const AudioFrame a = fake_audio(f / 30.f);
            const auto t0 = clk::now();
            v.render(a);
            const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
            sum += ms; worst_frame = std::max(worst_frame, ms);
            if (f == 120) h120 = fnv(v.pixels(), W * H);
            if (f == 120 || f == 135 || f == 150) {
                const Stats s = stats(v.pixels(), W * H);
                mean += s.mean / 3; white += s.white / 3;
            }
        }
        total_ms += sum; frames += 151;
        if (sum / 151 > worst_avg) { worst_avg = sum / 151; worst_name = rel; }
        const bool still = fnv(v.pixels(), W * H) == h120;
        const char* cls = mean < 4 ? "BLACK" : (white > 0.6 || mean > 235) ? "BLOWN" : still ? "FROZEN" : "ok";
        if (mean < 4) ++black; else if (white > 0.6 || mean > 235) ++blown; else if (still) ++frozen; else ++fine;
        if (log) fprintf(log, "%-6s mean %5.1f white %.2f %.3f ms  [sheet %02d #%02d] %s\n", cls, mean, white, sum / 151,
                         sheet_no, n % per, p.name.c_str());
        for (int y = 0; y < H; ++y)
            memcpy(&sheet[size_t(((n % per) / cols) * H + y) * (W * cols) + ((n % per) % cols) * W], v.pixels() + y * W, W * 4);
        if (++n % per == 0) flush_sheet();

        for (int k = 5; k < argc; ++k) {
            if (p.name.find(argv[k]) == std::string::npos) continue;
            Visualizer big(312, 121);
            big.load_milk(p);
            for (int f = 0; f <= 150; ++f) big.render(fake_audio(f / 30.f));
            char name[64];
            snprintf(name, sizeof name, "/milk_%02d.bmp", saved++);
            write_bmp(out + name, big.pixels(), 312, 121, 2);
            if (saved <= 16) {                     // also onto a 4x4 sheet
                const int k2 = saved - 1;
                for (int y = 0; y < 121; ++y)
                    memcpy(&big_sheet[size_t((k2 / 4) * 121 + y) * (312 * 4) + (k2 % 4) * 312], big.pixels() + y * 312, 312 * 4);
            }
            printf("saved %s = %s\n", name + 1, p.name.c_str());
            break;
        }
    }
    if (n % per) flush_sheet();
    if (saved) write_bmp(out + "/big_sheet.bmp", big_sheet.data(), 312 * 4, 121 * 4);
    if (log) fclose(log);
    printf("%d presets: %d fine, %d black, %d blown-out, %d frozen (not black, not moving), %d failed to load\n",
           int(files.size()), fine, black, blown, frozen, failed);
    printf("%dx%d: mean %.3f ms/frame, slowest preset avg %.3f ms (%s), slowest single frame %.3f ms\n",
           W, H, total_ms / std::max(1L, frames), worst_avg, worst_name.c_str(), worst_frame);
    return 0;
}

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "frames" && argc > 2) return cmd_frames(argv[2]);
    if (cmd == "bench") { bench_size(156, 60); bench_size(312, 121); return 0; }
    if (cmd == "analyzer") return cmd_analyzer();
    if (cmd == "milk" && argc > 4) return cmd_milk(argc, argv);
    fprintf(stderr, "usage: viz_demo frames <outdir> | bench | analyzer | milk <pack> <list> <outdir> [name...]\n");
    return 2;
}
