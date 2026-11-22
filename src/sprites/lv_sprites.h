#pragma once
// lv_sprites — LVGL 9.5 wrapper around mm_sprites: hands out lv_image_dsc_t
// (LV_COLOR_FORMAT_ARGB8888) for use with lv_image_set_src().
//
// The pixels are ALREADY at their final size: mm::sprites::render() applies
// the integer scale (half / 1x / 2x) and the optional crop, so the lv_image
// must be shown unscaled (lv_image scale 256). Read the size from
// dsc->header.w/h; it depends on the sprite set, the side, the scale and crop.
//
// Lifetime: descriptors from get() live in a bounded LRU (96 entries; a 1x
// Gen 2 front is ~12 KB, a half-size icon ~3 KB). A descriptor stays valid
// until it is evicted (96 newer distinct sprites requested) or clear_cache()
// is called; on eviction the entry is also dropped from LVGL's image cache.
// Re-call get() whenever you (re)build a screen instead of holding pointers
// indefinitely.
//
// Descriptors from get_animated() live in one of 4 persistent per-slot buffers
// and stay valid forever (the pointer for a slot never changes).

#include "lvgl.h"
#include <cstdint>

namespace mm::sprites {

// Cached per (dex, back, skin, tritan, scale2, crop). `scale2` is
// mm::sprites::kScaleHalf / kScale1x / kScale2x (1 / 2 / 4); `crop` trims to
// the sprite's opaque bounding box. For animated (rainbow) skins this returns
// a frozen phase-0 frame; use get_animated() to animate. nullptr on bad dex /
// decode failure.
const lv_image_dsc_t* get(int dex, bool back, int skin, bool tritan = false,
                          int scale2 = 2, bool crop = false);

// Re-decodes into the persistent buffer for `slot` (0..3) at `phase`.
// Already calls lv_image_cache_drop(dsc) on the slot's descriptor; the caller
// must still lv_obj_invalidate() the lv_image showing it (calling
// lv_image_cache_drop again is harmless). nullptr on bad slot/dex.
const lv_image_dsc_t* get_animated(int slot, int dex, bool back, int skin,
                                   uint16_t phase, bool tritan = false,
                                   int scale2 = 2, bool crop = false);

constexpr int kAnimSlots = 4;

// Drop every cached descriptor (also from LVGL's image cache). Invalidates all
// pointers previously returned by get(); slot descriptors stay valid.
void clear_cache();

}  // namespace mm::sprites
