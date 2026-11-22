// lv_sprites — see lv_sprites.h.
//
// LVGL 9 ARGB8888 is stored B,G,R,A in memory, which is exactly a
// little-endian uint32_t 0xAARRGGBB as produced by mm::sprites::decode_into.

#include "lv_sprites.h"
#include "mm_sprites.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <cstring>
#include <iterator>
#include <list>
#include <unordered_map>

namespace mm::sprites {

namespace {

constexpr size_t kCacheMax = 96;

struct Entry {
    uint32_t key = 0;
    lv_image_dsc_t dsc;
    Image img;              // owned pixels; dsc.data points into img.argb
};

struct Slot {
    lv_image_dsc_t dsc;
    Image img;
    bool init = false;
};

// Front of list = most recently used. std::list keeps Entry addresses stable.
std::list<Entry>& lru() { static std::list<Entry> l; return l; }
std::unordered_map<uint32_t, std::list<Entry>::iterator>& index() {
    static std::unordered_map<uint32_t, std::list<Entry>::iterator> m; return m;
}
Slot* slots() { static Slot s[kAnimSlots]; return s; }

inline uint32_t make_key(int dex, bool back, int skin, bool tritan, int scale2, bool crop) {
    return ((uint32_t)dex << 12) | ((uint32_t)(scale2 & 7) << 8) | ((crop ? 1u : 0u) << 7) |
           ((uint32_t)(skin & 0x1F) << 2) | ((back ? 1u : 0u) << 1) | (tritan ? 1u : 0u);
}

void fill_dsc(lv_image_dsc_t& d, const Image& img) {
    std::memset(&d, 0, sizeof(d));
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = LV_COLOR_FORMAT_ARGB8888;
    d.header.flags = 0;
    d.header.w = (uint32_t)img.w;
    d.header.h = (uint32_t)img.h;
    d.header.stride = (uint32_t)img.w * 4;
    d.data_size = (uint32_t)img.w * img.h * 4;
    d.data = reinterpret_cast<const uint8_t*>(img.argb.data());
}

}  // namespace

const lv_image_dsc_t* get(int dex, bool back, int skin, bool tritan, int scale2, bool crop) {
    if (dex < 1 || dex > 386) return nullptr;
    if (skin < 0 || skin >= SKIN_COUNT) skin = SKIN_REGULAR;
    const uint32_t key = make_key(dex, back, skin, tritan, scale2, crop);

    auto& l = lru();
    auto& m = index();
    auto it = m.find(key);
    if (it != m.end()) {
        if (it->second != l.begin()) l.splice(l.begin(), l, it->second);
        return &it->second->dsc;
    }

    // Reuse the LRU tail when full, else allocate a new node.
    if (l.size() >= kCacheMax) {
        auto last = std::prev(l.end());
        lv_image_cache_drop(&last->dsc);
        m.erase(last->key);
        l.splice(l.begin(), l, last);
    } else {
        l.emplace_front();
    }
    Entry& e = l.front();
    if (!render(dex, back, skin, tritan, 0, scale2, crop, e.img) || e.img.w <= 0) {
        l.pop_front();
        return nullptr;
    }
    e.key = key;
    fill_dsc(e.dsc, e.img);
    m[key] = l.begin();
    return &e.dsc;
}

const lv_image_dsc_t* get_animated(int slot, int dex, bool back, int skin,
                                   uint16_t phase, bool tritan, int scale2, bool crop) {
    if (slot < 0 || slot >= kAnimSlots) return nullptr;
    Slot& s = slots()[slot];
    if (s.init) lv_image_cache_drop(&s.dsc);
    if (!render(dex, back, skin, tritan, phase, scale2, crop, s.img) || s.img.w <= 0) return nullptr;
    fill_dsc(s.dsc, s.img);      // size or buffer may have changed
    s.init = true;
    lv_image_cache_drop(&s.dsc);
    return &s.dsc;
}

void clear_cache() {
    for (auto& e : lru()) lv_image_cache_drop(&e.dsc);
    lru().clear();
    index().clear();
    for (int i = 0; i < kAnimSlots; ++i)
        if (slots()[i].init) lv_image_cache_drop(&slots()[i].dsc);
}

}  // namespace mm::sprites
