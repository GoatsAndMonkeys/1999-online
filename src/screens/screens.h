// screens.h — factory declarations for every MonsterMesh screen.
//
// Each screen lives in its own screens/<name>_screen.cpp and exposes exactly
// one factory here. registry.cpp maps ScreenId -> factory.
#pragma once

#include "screen.h"

#include <memory>

namespace mm::ui {

std::unique_ptr<Screen> make_menu_screen(Ctx& ctx);
std::unique_ptr<Screen> make_party_screen(Ctx& ctx);
std::unique_ptr<Screen> make_daycare_screen(Ctx& ctx);
std::unique_ptr<Screen> make_breeding_screen(Ctx& ctx);
std::unique_ptr<Screen> make_gym_screen(Ctx& ctx);
std::unique_ptr<Screen> make_battle_screen(Ctx& ctx);
std::unique_ptr<Screen> make_wifi_screen(Ctx& ctx);
// 1999 Online (windowed) screens.
std::unique_ptr<Screen> make_buddy_screen(Ctx& ctx);
std::unique_ptr<Screen> make_im_screen(Ctx& ctx);
std::unique_ptr<Screen> make_space_screen(Ctx& ctx);
std::unique_ptr<Screen> make_amp_screen(Ctx& ctx);
std::unique_ptr<Screen> make_skins_screen(Ctx& ctx);
std::unique_ptr<Screen> make_monster_screen(Ctx& ctx);
std::unique_ptr<Screen> make_radio_screen(Ctx& ctx);

// Generic "coming soon" body used by stubs.
std::unique_ptr<Screen> make_placeholder_screen(Ctx& ctx, const char* title, const char* blurb);
std::unique_ptr<Screen> make_window_placeholder_screen(Ctx& ctx, const char* title, const char* blurb);

} // namespace mm::ui
