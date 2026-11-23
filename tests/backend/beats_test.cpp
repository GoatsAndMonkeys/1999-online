// beats_test.cpp — MeshBeat patterns: text round trip, sharing, presets,
// timing, WAV render and the library file.
//
// Build (from the project root):
//   c++ -std=c++17 -Wall -Wextra -I src/backend tests/backend/beats_test.cpp \
//     src/backend/beats.cpp src/backend/ringtone.cpp src/backend/settings.cpp \
//     src/backend/mm_json.cpp -o <out>/beats_test
//
// Usage:
//   beats_test <scratch_dir>     runs the checks; writes each preset (2 bars)
//                                to <scratch_dir>/<name>.wav.
// Plays nothing and sends nothing.
#include "beats.h"
#include "ringtone.h"
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace mm;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) ++g_pass;                                                    \
        else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static uint32_t le32(const std::string& b, size_t at) {
    return static_cast<uint8_t>(b[at]) | static_cast<uint8_t>(b[at + 1]) << 8 |
           static_cast<uint8_t>(b[at + 2]) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(b[at + 3])) << 24;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    setenv("PC1999_CONFIG_DIR", (dir + "/config").c_str(), 1);

    // ── Text round trip ─────────────────────────────────────────────────────
    beat::Pattern p;
    p.name = "Test Beat";
    p.bpm = 133;
    p.swing = 61;
    p.mute = 0x24;
    p.vol = {9, 0, 1, 2, 3, 4, 5, 6};
    for (int t = 0; t < beat::kTracks; ++t)
        for (int s = 0; s < beat::kSteps; ++s) p.steps[t][s] = static_cast<uint8_t>((t * 7 + s * 5 + s / 3) % 3);
    const std::string txt = beat::to_text(p);
    beat::Pattern back;
    CHECK(beat::from_text(txt, back));
    CHECK(back == p);
    CHECK(txt.compare(0, 3, "B1:") == 0);
    std::printf("text: %s (%zu bytes)\n", txt.c_str(), txt.size());

    // Share text: prefix, size, decode.
    const std::string share = beat::share_text(p);
    CHECK(share.compare(0, 7, "~BEAT~ ") == 0);
    CHECK(share.size() <= beat::kMaxShareBytes);
    p.name = "Sixteen Chars Nm";                    // longest name still fits
    CHECK(beat::fits_share(p));
    CHECK(beat::share_text(p).size() <= 140);
    beat::Pattern rx;
    CHECK(beat::parse_shared("  " + beat::share_text(p) + "  ", rx) && rx == p);
    CHECK(!beat::parse_shared("hello there", rx));
    CHECK(!beat::parse_shared("~RING~ Tune:d=4,o=5,b=120:c,d,e", rx));
    CHECK(!beat::parse_shared("~BEAT~ B1:x:120:50:77777777:00:zz", rx));      // bad hex
    beat::Pattern blank;
    blank.name = "Empty";
    CHECK(!beat::parse_shared(beat::share_text(blank), rx));                    // no hits: not a beat

    // Bad inputs leave `out` alone.
    beat::Pattern keep = p;
    CHECK(!beat::from_text("B2:x:120:50:77777777:00:", keep) && keep == p);
    CHECK(!beat::from_text("B1:x:fast:50:77777777:00:", keep));
    CHECK(!beat::from_text("B1:x:120:50:7777777a:00:", keep));
    CHECK(!beat::from_text("B1:x:120:50", keep));
    // Lenient: short hex = empty tail tracks; out-of-range tempo is clamped.
    CHECK(beat::from_text("B1:Short:999:10:7:0:c0", keep));
    CHECK(keep.bpm == beat::kMaxBpm && keep.swing == beat::kMinSwing);
    CHECK(keep.steps[0][0] == beat::Accent && keep.steps[0][1] == beat::Off && keep.hits() == 1);
    CHECK(beat::clean_name("  a:b\tc  ") == "abc");
    CHECK(beat::clean_name("abcdefghijklmnopqrstuvwxyz").size() == beat::kMaxNameBytes);

    // ── Presets ─────────────────────────────────────────────────────────────
    CHECK(beat::presets().size() == beat::preset_list().size());
    CHECK(beat::presets().size() >= 5);
    for (const beat::Pattern& pr : beat::presets()) {
        CHECK(!pr.name.empty() && pr.hits() > 8);
        CHECK(pr.bpm >= beat::kMinBpm && pr.bpm <= beat::kMaxBpm);
        beat::Pattern b2;
        CHECK(beat::from_text(beat::to_text(pr), b2) && b2 == pr);
        CHECK(beat::fits_share(pr));
    }
    beat::Pattern rows;
    CHECK(!beat::from_rows("bad", 120, 50, {"x..."}, rows));                     // wrong length
    CHECK(!beat::from_rows("bad", 120, 50, {"x..o............"}, rows));          // bad char
    CHECK(beat::from_rows("ok", 120, 50, {"X-x.............", ""}, rows) && rows.hits() == 2);

    // ── Timing ──────────────────────────────────────────────────────────────
    CHECK(beat::bar_ms(120) == 2000);
    CHECK(beat::step_ms(120) == 125.0);
    beat::Pattern sw;
    sw.bpm = 120;
    sw.swing = 50;
    CHECK(beat::step_start_ms(sw, 1) == 125.0);
    sw.swing = 75;
    CHECK(beat::step_start_ms(sw, 1) == 187.5);            // 3/4 through the pair
    CHECK(beat::step_start_ms(sw, 2) == 250.0);            // even steps don't move
    CHECK(beat::step_at(sw, 0) == 0 && beat::step_at(sw, 180) == 0 && beat::step_at(sw, 190) == 1);
    CHECK(beat::step_at(sw, 1999) == 15);
    CHECK(beat::loop_samples(120, 1, 22050) == 44100);
    CHECK(beat::loop_samples(60, 2, 22050) == 176400);
    CHECK(beat::loop_bars(120) == 3 && beat::loop_bars(60) == 2 && beat::loop_bars(200) == 5);

    // ── Render + WAV ────────────────────────────────────────────────────────
    const beat::Pattern& four = beat::presets()[0];
    std::vector<int16_t> pcm = beat::render(four, 1);
    CHECK(pcm.size() == beat::loop_samples(four.bpm, 1));
    int peak = 0;
    for (int16_t s : pcm) peak = std::max(peak, std::abs(static_cast<int>(s)));
    CHECK(peak > 3000 && peak < 32767);
    beat::Pattern silent = four;
    silent.mute = 0xff;
    bool all_zero = true;
    for (int16_t s : beat::render(silent, 1)) all_zero = all_zero && s == 0;
    CHECK(all_zero);
    CHECK(beat::render(four, 2) == beat::render(four, 2));          // deterministic
    CHECK(!beat::render_hit(beat::Kick, true).empty());

    beat::Pattern at120 = four;
    at120.bpm = 120;
    const std::string wav = dir + "/beat_120.wav";
    CHECK(ring::write_wav(wav, beat::render(at120, 1)));
    {
        std::ifstream f(wav, std::ios::binary);
        const std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(b.size() == 44 + 44100 * 2);                  // 120 bpm: one bar = 2 s at 22.05 kHz
        CHECK(b.compare(0, 4, "RIFF") == 0 && b.compare(8, 8, "WAVEfmt ") == 0 && b.compare(36, 4, "data") == 0);
        CHECK(le32(b, 24) == 22050 && le32(b, 40) == 44100 * 2 && le32(b, 4) == 36 + 44100 * 2);
    }
    for (const beat::Pattern& pr : beat::presets()) {
        std::string fname;
        for (char ch : pr.name) fname += (ch == ' ') ? '_' : ch;
        ring::write_wav(dir + "/" + fname + ".wav", beat::render(pr, 2));
    }

    // ── Library file ────────────────────────────────────────────────────────
    store::set_enabled(true);
    {
        beat::Library lib(dir + "/beats_lib.json");
        lib.set(1, beat::presets()[3]);
        lib.set(3, p);
        CHECK(lib.save());
        beat::Library again(dir + "/beats_lib.json");
        CHECK(again.load());
        CHECK(!again.used(0) && again.used(1) && !again.used(2) && again.used(3));
        CHECK(again.slot(1) == beat::presets()[3] && again.slot(3) == p);
    }
    store::set_enabled(false);
    {
        beat::Library off(dir + "/beats_lib.json");
        CHECK(!off.load());                                 // --shots: nothing read
        CHECK(!off.save());
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
