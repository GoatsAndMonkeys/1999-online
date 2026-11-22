// sprite_dump — standalone check for mm::sprites (no LVGL/SDL).
// Renders a contact sheet: rows = dex list, columns = skins (front), then the
// same skins for the back sprite, plus tritan and rainbow-phase samples.
// Output: binary PPM composited over a checkerboard (shows transparency).
//
// Build (from monster_mesh_pi/):
//   clang -c -O2 src/terminal/puff.c -o $OUT/puff.o
//   clang++ -std=c++17 -O2 -Isrc/terminal -Isrc/shared -Icardputer/src/sprites \
//     cardputer/src/sprites/mm_sprites.cpp cardputer/tests/sprites/sprite_dump.cpp \
//     $OUT/puff.o -o $OUT/sprite_dump
//   $OUT/sprite_dump $OUT/sheet.ppm

#include "mm_sprites.h"
#include <cstdio>
#include <vector>

using namespace mm::sprites;

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "sheet.ppm";
    const int dexes[] = {25, 6, 150, 384};
    struct Col { bool back; int skin; bool tritan; uint16_t phase; };
    std::vector<Col> cols;
    for (int s : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}) cols.push_back({false, s, false, 0});
    for (int s : {0, 1, 2, 3, 4, 8}) cols.push_back({true, s, false, 0});
    cols.push_back({false, 0, true, 0});   // tritan regular
    cols.push_back({false, 1, true, 0});   // tritan shiny
    cols.push_back({false, 3, false, 6});  // rainbow, later phase
    cols.push_back({false, 3, false, 12});

    const int S = 2, cell = 64 * S + 4;
    const int W = cell * (int)cols.size(), H = cell * (int)(sizeof(dexes) / sizeof(dexes[0]));
    std::vector<uint8_t> rgb((size_t)W * H * 3, 40);
    int fails = 0;

    // Sanity: bad dex must fail; is_animated.
    Image bad;
    if (decode(0, false, 0, false, 0, bad) || decode(387, false, 0, false, 0, bad)) {
        std::fprintf(stderr, "bad dex did not fail\n"); ++fails;
    }
    if (!is_animated(3) || !is_animated(7) || !is_animated(11) || is_animated(0) || is_animated(2)) {
        std::fprintf(stderr, "is_animated wrong\n"); ++fails;
    }

    for (int r = 0; r < (int)(sizeof(dexes) / sizeof(dexes[0])); ++r) {
        for (int c = 0; c < (int)cols.size(); ++c) {
            Image img;
            if (!decode(dexes[r], cols[c].back, cols[c].skin, cols[c].tritan, cols[c].phase, img)) {
                std::fprintf(stderr, "decode failed dex %d col %d\n", dexes[r], c); ++fails; continue;
            }
            int opaque = 0, partial = 0;
            for (int y = 0; y < img.h * S; ++y)
                for (int x = 0; x < img.w * S; ++x) {
                    uint32_t p = img.argb[(y / S) * img.w + (x / S)];
                    uint8_t a = p >> 24;
                    uint8_t chk = (((x / 8) + (y / 8)) & 1) ? 200 : 235;
                    uint8_t R = a ? (p >> 16) & 0xFF : chk;
                    uint8_t G = a ? (p >> 8) & 0xFF : chk;
                    uint8_t B = a ? p & 0xFF : chk;
                    size_t o = ((size_t)(r * cell + 2 + y) * W + (c * cell + 2 + x)) * 3;
                    rgb[o] = R; rgb[o + 1] = G; rgb[o + 2] = B;
                }
            for (uint32_t p : img.argb) { uint8_t a = p >> 24; opaque += a == 255; partial += (a && a != 255); }
            if (opaque == 0 || opaque == img.w * img.h || partial) {
                std::fprintf(stderr, "suspicious alpha dex %d col %d: opaque=%d partial=%d\n",
                             dexes[r], c, opaque, partial);
                ++fails;
            }
        }
    }
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::perror(path); return 2; }
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);

    // Decode every dex front+back to make sure the whole blob is sound.
    for (int d = 1; d <= 386; ++d)
        for (int b = 0; b < 2; ++b) {
            Image img;
            if (!decode(d, b, 0, false, 0, img)) { std::fprintf(stderr, "full sweep fail dex %d back %d\n", d, b); ++fails; }
        }
    std::printf("wrote %s (%dx%d), failures=%d\n", path, W, H, fails);
    return fails ? 1 : 0;
}
