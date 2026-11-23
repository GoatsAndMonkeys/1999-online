// ringtone_test.cpp — RTTTL parser / writer, sharing, library file, synth.
//
// Build (from the project root):
//   c++ -std=c++17 -Wall -Wextra -I src/backend tests/backend/ringtone_test.cpp \
//     src/backend/ringtone.cpp src/backend/settings.cpp src/backend/mm_json.cpp -o <out>/ringtone_test
//
// Usage:
//   ringtone_test <scratch_dir>     runs the checks; writes each built-in tune
//                                   to <scratch_dir>/<name>.wav and prints its length.
// Plays nothing and sends nothing.
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

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    setenv("PC1999_CONFIG_DIR", (dir + "/config").c_str(), 1);

    // ── Parser ──────────────────────────────────────────────────────────────
    ring::Tune t;
    std::string err;
    CHECK(ring::parse_rtttl("Test:d=8,o=5,b=120:c,4d#6.,p,16e,2a.4,h,e#", t, &err));
    CHECK(t.name == "Test" && t.notes.size() == 7);
    CHECK(t.notes[0].pitch == 0 && t.notes[0].octave == 5 && t.notes[0].dur == 8);
    CHECK(t.notes[1].pitch == 3 && t.notes[1].octave == 6 && t.notes[1].dur == 4 && t.notes[1].dotted);
    CHECK(t.notes[2].rest());
    CHECK(t.notes[4].pitch == 9 && t.notes[4].octave == 4 && t.notes[4].dotted && t.notes[4].dur == 2);
    CHECK(t.notes[5].pitch == 11);                 // h = b
    CHECK(t.notes[6].pitch == 5);                  // e# = f
    CHECK(ring::note_ms(t.notes[0], 120) == 250);  // eighth at 120 bpm
    CHECK(ring::note_ms(t.notes[1], 120) == 750);  // dotted quarter
    CHECK(!ring::parse_rtttl("hello there", t));
    CHECK(!ring::parse_rtttl("x:d=3:c", t));
    CHECK(!ring::parse_rtttl("x:d=4:", t));
    CHECK(ring::parse_rtttl("NoCtl::c,d,e", t) && t.def_oct == 6 && t.bpm == 63);   // RTTTL defaults
    CHECK(ring::parse_rtttl("  Sp : d = 4 , o = 5 , b = 100 : 8C , D# 6 , P  ", t) && t.notes.size() == 3);

    // ── Round trip ──────────────────────────────────────────────────────────
    for (const ring::Tune& b : ring::builtin()) {
        ring::Tune back;
        const std::string s = ring::to_rtttl(b);
        CHECK(ring::parse_rtttl(s, back));
        CHECK(back.name == b.name && back.bpm == b.bpm && back.notes.size() == b.notes.size());
        bool same = back.notes.size() == b.notes.size();
        for (size_t i = 0; same && i < b.notes.size(); ++i) same = back.notes[i] == b.notes[i];
        CHECK(same);
        CHECK(ring::fits_share(b));
        ring::Tune shared;
        CHECK(ring::parse_shared(ring::share_text(b), shared) && shared.notes.size() == b.notes.size());
    }
    CHECK(ring::builtin().size() == ring::builtin_list().size());

    // ── Sharing detection ───────────────────────────────────────────────────
    ring::Tune got;
    CHECK(!ring::parse_shared("lol: ok: sure", got));
    CHECK(!ring::parse_shared("meet at 10:30: bring snacks", got));
    CHECK(ring::parse_shared("Bleep:d=8,o=6,b=180:c,e,g,c7", got) && got.name == "Bleep");
    CHECK(ring::parse_shared("~RING~ Hi:d=4,o=5,b=120:c,d,e", got) && got.notes.size() == 3);
    CHECK(!ring::parse_shared("~RING~ broken", got));

    // ── Names ───────────────────────────────────────────────────────────────
    CHECK(ring::clean_name("  a:b,c  ") == "abc");
    CHECK(ring::clean_name("abcdefghijklmnopqrstuvwxyz").size() == ring::kMaxNameBytes);

    // ── Library file ────────────────────────────────────────────────────────
    {
        ring::Library lib;
        lib.put(ring::builtin()[0]);
        ring::Tune mine = ring::builtin()[3];
        mine.name = "My \"Beep\"";
        lib.put(mine);
        CHECK(lib.save());
        ring::Library again;
        CHECK(again.load());
        CHECK(again.tunes().size() == 2 && again.tunes()[1].name == "My \"Beep\"");
        CHECK(again.put(mine) == 1 && again.tunes().size() == 2);   // same name: replaced
    }

    // ── Render each built-in tune ───────────────────────────────────────────
    for (size_t i = 0; i < ring::builtin().size(); ++i) {
        const ring::Tune& b = ring::builtin()[i];
        const std::vector<int16_t> pcm = ring::synth(b);
        const double secs = static_cast<double>(pcm.size()) / ring::kSampleRate;
        std::string file = dir + "/" + std::to_string(i) + "_";
        for (char ch : b.name) file += (ch == ' ' ? '_' : ch);
        file += ".wav";
        CHECK(ring::write_wav(file, pcm));
        CHECK(pcm.size() > 0);
        std::printf("%-13s %3zu notes  %5.2f s (timing %5.2f s)  share %3zu B  %s\n", b.name.c_str(),
                    b.notes.size(), secs, ring::duration_ms(b) / 1000.0, ring::share_text(b).size(),
                    ring::builtin_list()[i].credit);
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
