// keys.h — logical keys for MonsterMesh screens.
//
// The CardputerZero has a 46-key keyboard. Arrows arrive either as real
// KEY_UP/DOWN/LEFT/RIGHT (Fn+;./, on the device keymap) or as the bare
// ; . , / characters (original Cardputer layout), so both map to the same
// logical direction. Screens only ever see a KeyEvent.
#pragma once

#include "lvgl.h"

#include <cstdint>

namespace mm {

enum class Key : uint8_t {
    None,
    Up,
    Down,
    Left,
    Right,
    A,      // Enter / Space — confirm
    B,      // Esc / Backspace / ` — back / cancel
    Tab,    // Tab — cycle focus / switch pane
    Char,   // any other printable key; see KeyEvent::ch (lower-case)
};

struct KeyEvent {
    Key key = Key::None;
    uint32_t ch = 0;      // printable character for Key::Char (and the raw char otherwise, if any)
    bool repeat = false;  // auto-repeat while a direction is held
    uint32_t raw = 0;     // untranslated key: text fields read this (case kept; ';' is ';')
};

inline KeyEvent translate_key(uint32_t raw) {
    KeyEvent e;
    e.raw = raw;
    e.ch = (raw >= 32 && raw < 127) ? raw : 0;
    switch (raw) {
        case LV_KEY_UP:    case ';': e.key = Key::Up; break;
        case LV_KEY_DOWN:  case '.': e.key = Key::Down; break;
        case LV_KEY_LEFT:  case ',': e.key = Key::Left; break;
        case LV_KEY_RIGHT: case '/': e.key = Key::Right; break;
        case LV_KEY_ENTER /* '\n' */: case '\r': case ' ': e.key = Key::A; break;
        case LV_KEY_ESC: case LV_KEY_BACKSPACE /* 8 */: case '`': e.key = Key::B; break;
        case LV_KEY_NEXT /* '\t' */: e.key = Key::Tab; break;
        default:
            if (e.ch) {
                e.key = Key::Char;
                if (e.ch >= 'A' && e.ch <= 'Z') e.ch = e.ch - 'A' + 'a';
            }
            break;
    }
    return e;
}

// The CardputerZero prints its arrows on F/Z/X/C (Fn gives real arrow keys).
// Outside text fields the Router treats those letters as the arrows.
inline uint32_t arrow_letter(uint32_t raw) {
    switch (raw) {
        case 'f': case 'F': return LV_KEY_UP;
        case 'x': case 'X': return LV_KEY_DOWN;
        case 'z': case 'Z': return LV_KEY_LEFT;
        case 'c': case 'C': return LV_KEY_RIGHT;
        default: return raw;
    }
}

inline bool is_direction(Key k) {
    return k == Key::Up || k == Key::Down || k == Key::Left || k == Key::Right;
}

} // namespace mm
