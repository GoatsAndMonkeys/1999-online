#pragma once

#include "lvgl.h"

#include <cstddef>
#include <cstdint>

namespace platform {

constexpr size_t kNavKeyCount = 5;
constexpr uint32_t kSpecialKeyBase = 0x200000U;
constexpr uint32_t kKeyPrintScreen = kSpecialKeyBase | 18U;
constexpr uint32_t kKeyHelp = kSpecialKeyBase | 19U;
constexpr uint32_t kKeyVolumeUp = kSpecialKeyBase | 20U;     // Fn + volume keys on the device
constexpr uint32_t kKeyVolumeDown = kSpecialKeyBase | 21U;
constexpr uint32_t kKeyMute = kSpecialKeyBase | 22U;
constexpr uint32_t kKeyPlayPause = kSpecialKeyBase | 23U;   // Fn + Q / W / E
constexpr uint32_t kKeyPrevSong = kSpecialKeyBase | 24U;
constexpr uint32_t kKeyNextSong = kSpecialKeyBase | 25U;
// The Fn key itself (the keypad driver reports it as KEY_CD; it also switches
// the driver's layer, so Fn + 1..0 arrive as F1..F10).
constexpr uint32_t kKeyFn = kSpecialKeyBase | 26U;
constexpr uint32_t kKeyF1 = kSpecialKeyBase | 30U;          // F1..F10 = kKeyF1 + 0..9
inline int fn_digit(uint32_t key) {                          // Fn + digit -> 1..9, 0 (10); -1 otherwise
    if (key < kKeyF1 || key > kKeyF1 + 9U) return -1;
    return static_cast<int>((key - kKeyF1 + 1U) % 10U);
}

using KeyReleaseListener = void (*)(uint32_t key, const char* key_name, void* user_data);
using GlobalKeyListener = bool (*)(uint32_t key,
                                   const char* key_name,
                                   bool long_pressed,
                                   void* user_data);

void init_key_input(lv_display_t* display);
void attach_key_router(lv_indev_t* indev);
void set_nav_shortcut_mode(bool enabled);
void register_nav_button(size_t index, lv_obj_t* button);
void unregister_nav_button(size_t index, lv_obj_t* button);
void set_key_release_listener(KeyReleaseListener listener, void* user_data);
void clear_key_release_listener(KeyReleaseListener listener, void* user_data);
void set_global_key_listener(GlobalKeyListener listener, void* user_data);
void clear_global_key_listener(GlobalKeyListener listener, void* user_data);
const char* describe_key(uint32_t key);

} // namespace platform
