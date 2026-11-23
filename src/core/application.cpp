#include "application.h"

#include "game_state.h"
#include "router.h"
#include "style.h"
#include "widgets.h"

#include "application_config.h"
#include "asset_manager.h"
#include "audio_player.h"
#include "linux_input.h"
#include "logger.h"
#include "mesh_backend.h"
#include "mesh_link.h"
#include "lv_sprites.h"
#include "mm_sprites.h"
#include "screenshot_service.h"
#include "settings.h"
#include "skin.h"
#include "theme.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if USE_DESKTOP
#include "desktop_simulator_frame.h"
#endif

#if !USE_DESKTOP
#if APP_USE_DRM
#include "src/drivers/display/drm/lv_linux_drm.h"
#else
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#endif
#endif

#ifndef APP_FRAMEBUFFER_DEVICE
#define APP_FRAMEBUFFER_DEVICE "/dev/fb0"
#endif
#ifndef APP_DRM_DEVICE
#define APP_DRM_DEVICE "/dev/dri/card0"
#endif
#ifndef APP_DRM_CONNECTOR_ID
#define APP_DRM_CONNECTOR_ID -1
#endif
#ifndef APP_CONFIG_FILE
#define APP_CONFIG_FILE "monstermesh.conf"
#endif

namespace mm {
namespace {

struct Args {
    std::string shots_dir;
    std::string skin;          // skin id (w95 me xp vista sys7 mac9 beos os2 x11 winamp)
    std::string start_screen;
    std::string icon_path;   // --icon <file.png> <size>: render the launcher icon and exit
    int icon_size = 100;
    bool demo = false;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--shots" && i + 1 < argc) a.shots_dir = argv[++i];
        else if (s == "--screen" && i + 1 < argc) a.start_screen = argv[++i];
        else if (s == "--demo") a.demo = true;
        else if (s == "--skin" && i + 1 < argc) a.skin = argv[++i];
        else if (s == "--icon" && i + 2 < argc) {
            a.icon_path = argv[++i];
            a.icon_size = std::atoi(argv[++i]);
        }
    }
    return a;
}

uint32_t steady_ms() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool screen_from_slug(const std::string& slug, ui::ScreenId& out) {
    for (int i = 0; i < static_cast<int>(ui::ScreenId::Count); ++i) {
        const auto id = static_cast<ui::ScreenId>(i);
        if (slug == ui::screen_slug(id)) {
            out = id;
            return true;
        }
    }
    return false;
}

// Platform key hooks -> Router.
struct KeyHooks {
    ui::Router* router = nullptr;

    static bool on_key(uint32_t key, const char*, bool long_pressed, void* user) {
        auto* self = static_cast<KeyHooks*>(user);
        if (!self || !self->router) return false;
        if (key == platform::kKeyPrintScreen) {
            if (!long_pressed) {
                const auto r = platform::screenshot::capture_active_screen();
                if (r.success) {
                    LOG_INFO("screenshot saved: {}", r.path);
                    ui::toast("Saved " + r.path, 3000, ui::color::kGood);
                } else {
                    ui::toast("Screenshot failed: " + r.error, 3000, ui::color::kBad);
                }
            }
            return true;
        }
        if (key == platform::kKeyVolumeUp || key == platform::kKeyVolumeDown ||
            key == platform::kKeyMute) {
            mm::AudioPlayer& p = mm::audio_player();
            if (key == platform::kKeyMute) {
                if (!long_pressed) p.toggle_mute();
            } else {
                p.set_volume(p.volume() + (key == platform::kKeyVolumeUp ? 5 : -5));
                mm::settings().set_int("amp.volume", p.volume());
            }
            const std::string msg = p.muted() ? std::string("Volume: muted")
                                              : "Volume: " + std::to_string(p.volume()) + "%";
            ui::toast(msg, 1200, ui::color::kInfo);
            return true;
        }
        if (key == platform::kKeyPlayPause || key == platform::kKeyNextSong ||
            key == platform::kKeyPrevSong) {
            if (long_pressed) return true;
            mm::AudioPlayer& p = mm::audio_player();
            using K = mm::AudioPlayer::Kind;
            bool ok = true;
            if (key == platform::kKeyPlayPause) {
                if (p.playing()) {
                    p.stop();
                } else if (p.kind() == K::Station) {
                    ok = p.play_station(p.index());
                } else if (p.kind() == K::Retro) {
                    ok = p.play_retro(p.index());
                } else {
                    ok = p.play_file(std::max(0, p.index()));   // nothing yet: first local file
                }
            } else {
                ok = key == platform::kKeyNextSong ? p.next() : p.prev();
            }
            if (!ok) ui::toast("Nothing to play", 1200, ui::color::kInfo);
            else ui::toast(p.playing() ? "Playing: " + p.title() : std::string("Stopped"),
                           1500, ui::color::kInfo);
            return true;
        }
        // Fn held: the soft-key bar shows the apps; Fn + 3..9 opens one.
        if (key == platform::kKeyFn) {
            if (!long_pressed) self->router->set_app_bar(true);
            return true;
        }
        if (const int d = platform::fn_digit(key); d >= 0) {
            if (!long_pressed) self->router->open_app_key(d);
            return true;
        }
        if (key == platform::kKeyHelp) {
            if (!long_pressed && self->router->top()) {
                ui::toast(self->router->top()->hints(), 3000, ui::color::kInfo);
            }
            return true;
        }
        return self->router->handle_raw_key(key, long_pressed);
    }

    static void on_release(uint32_t key, const char*, void* user) {
        auto* self = static_cast<KeyHooks*>(user);
        if (!self || !self->router) return;
        if (key == platform::kKeyFn) self->router->set_app_bar(false);
        self->router->handle_raw_release(key);
    }
};

#if !USE_DESKTOP
lv_display_t* init_device_display() {
    // Without a delay callback lv_delay_ms() busy-waits, so the main loop's
    // 5 ms pause pinned a core at ~100%. Sleep for real instead.
    lv_delay_set_cb([](uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
#if APP_USE_DRM
    auto* display = lv_linux_drm_create();
    if (!display) return nullptr;
    if (lv_linux_drm_set_file(display, APP_DRM_DEVICE, APP_DRM_CONNECTOR_ID) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }
#else
    auto* display = lv_linux_fbdev_create();
    if (!display) return nullptr;
    if (lv_linux_fbdev_set_file(display, APP_FRAMEBUFFER_DEVICE) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }
#endif
    platform::init_key_input(display);
    return display;
}
#endif

// ── Headless renderer for --shots ───────────────────────────────────────────

void null_flush(lv_display_t* disp, const lv_area_t*, uint8_t*) {
    lv_display_flush_ready(disp);
}

lv_display_t* create_headless_display() {
    lv_tick_set_cb(steady_ms);
    auto* display = lv_display_create(ui::kScreenW, ui::kScreenH);
    static std::vector<uint8_t> buf(ui::kScreenW * ui::kScreenH * 4);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(display, buf.data(), nullptr, buf.size(), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, null_flush);
    return display;
}

void pump(int ms) {
    const uint32_t end = steady_ms() + static_cast<uint32_t>(ms);
    while (static_cast<int32_t>(end - steady_ms()) > 0) {
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int run_shots(ui::Router& router, const std::string& dir, const std::string& only) {
    int n = 0, failed = 0;
    for (int i = 0; i < static_cast<int>(ui::ScreenId::Count); ++i) {
        const auto id = static_cast<ui::ScreenId>(i);
        if (!only.empty() && only != ui::screen_slug(id)) continue;
        router.reset_to_menu();
        if (id != ui::ScreenId::Menu) router.push(id);
        ui::Screen* s = router.top();
        if (!s || router.top_id() != id) {
            LOG_WARN("shots: screen {} did not open", ui::screen_slug(id));
            ++failed;
            continue;
        }
        const int states = std::max(1, s->demo_state_count());
        for (int st = 0; st < states; ++st) {
            ui::toast_hide();
            if (states > 1 || st > 0) {
                s->set_demo_state(st);
                router.rebuild();
            }
            pump(150);
            char file[96];
            std::snprintf(file, sizeof(file), "%02d_%s_%d_%s.png", i,
                          ui::screen_slug(id), st, s->demo_state_name(st));
            const std::string name = dir + "/" + file;
            const auto r = platform::screenshot::capture_active_screen_to(name);
            if (r.success) {
                LOG_INFO("shot: {}", r.path);
                ++n;
            } else {
                LOG_WARN("shot failed {}: {}", name, r.error);
                ++failed;
            }
        }
    }
    // Fn held over MeshAmp: the app launcher on the soft keys.
    if (only.empty() || only == "fn") {
        router.reset_to_menu();
        router.push(ui::ScreenId::Amp);
        router.set_app_bar(true);
        ui::toast_hide();
        pump(150);
        const auto r = platform::screenshot::capture_active_screen_to(dir + "/99_fn_apps.png");
        r.success ? ++n : ++failed;
        router.set_app_bar(false);
    }
    LOG_INFO("shots: {} written, {} failed -> {}", n, failed, dir);
    return failed ? 1 : 0;
}

// Launcher icon (APPLaunch wants 100x100 and 80x80 PNGs): a rounded dark tile
// with the Ditto sprite and a mesh-yellow border. Dev tool for the deb assets.
int render_icon(const std::string& path, int size) {
    static std::vector<uint8_t> buf;
    buf.assign(static_cast<size_t>(size) * size * 4, 0);
    lv_display_t* display = lv_display_create(size, size);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(display, buf.data(), nullptr, buf.size(), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, null_flush);

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
    lv_obj_t* tile = ui::box(scr, 0, 0, size, size);
    lv_obj_set_style_radius(tile, size / 5, 0);
    lv_obj_set_style_bg_color(tile, ui::c(ui::color::kPanel), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tile, ui::c(ui::color::kAccent), 0);
    lv_obj_set_style_border_width(tile, std::max(2, size / 25), 0);
    const int px = size * 3 / 4;                  // box; the sprite takes an integer scale
    ui::sprite_box(tile, 132, (size - px) / 2, (size - px) / 2 - size / 20, px, px);
    ui::label(tile, "MESH", 0, size - size / 4, ui::font_small(), ui::color::kAccent);
    lv_obj_t* l = lv_obj_get_child(tile, -1);
    lv_obj_set_width(l, size);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    pump(100);
    const auto r = platform::screenshot::capture_active_screen_to(path);
    if (!r.success) LOG_ERROR("icon failed: {}", r.error);
    return r.success ? 0 : 1;
}

} // namespace

int Application::run(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    logger::Logger::init();
    logger::Logger::set_tag("1999online");
    lv_init();
    // Screenshot / icon runs must not read or write any saved file (same
    // switch as GameState::Options::load_files below).
    if (!args.shots_dir.empty() || !args.icon_path.empty()) store::set_enabled(false);
    {
        // Sprite set: PC1999_SPRITES=gen2|gen3, then "ui.sprites", then Gen 2.
        sprites::SpriteSet set = sprites::SpriteSet::Gen2;
        const std::string saved = settings().get_string("ui.sprites");
        if (!sprites::parse_set(std::getenv("PC1999_SPRITES"), set))
            sprites::parse_set(saved.c_str(), set);
        sprites::set_active_set(set);
        sprites::clear_cache();
    }
    if (!args.icon_path.empty()) return render_icon(args.icon_path, args.icon_size);

    GameState::Options opt;
    if (args.demo || !args.shots_dir.empty()) {
        opt.force_demo = true;
        opt.enable_ipc = false;
    }
    if (!args.shots_dir.empty()) opt.load_files = false;   // deterministic demo data
    GameState game(opt);

    lv_display_t* display = nullptr;
#if USE_DESKTOP
    std::unique_ptr<app::AssetManager> assets;
    std::unique_ptr<app::DesktopSimulatorFrame> frame;
#endif
    if (!args.shots_dir.empty()) {
        display = create_headless_display();
    } else {
#if USE_DESKTOP
        assets = std::make_unique<app::AssetManager>();
        frame = std::make_unique<app::DesktopSimulatorFrame>(*assets);
        display = frame->display();
#else
        display = init_device_display();
#endif
    }
    if (!display) {
        LOG_ERROR("failed to initialize display");
        return 1;
    }

    view::apply_lvgl_theme(display, true, ui::font_body());
    ui::fonts_init(APP_SOURCE_ASSETS_ROOT);
    {
        // --skin, then PC1999_SKIN, then the skin saved last time.
        const char* want = !args.skin.empty() ? args.skin.c_str() : std::getenv("PC1999_SKIN");
        const std::string saved = settings().get_string("ui.skin");
        if ((!want || !*want) && !saved.empty()) want = saved.c_str();
        const int idx = ui::skin_from_id(want);
        ui::set_skin(idx >= 0 ? idx : 0);
    }

    ui::Router router(game);
    KeyHooks hooks{&router};
    platform::set_global_key_listener(KeyHooks::on_key, &hooks);
    platform::set_key_release_listener(KeyHooks::on_release, &hooks);
    router.start();

    int rc = 0;
    if (!args.shots_dir.empty()) {
        rc = run_shots(router, args.shots_dir, args.start_screen);
    } else {
        ui::ScreenId start{};
        if (!args.start_screen.empty() && screen_from_slug(args.start_screen, start) &&
            start != ui::ScreenId::Menu) {
            router.push(start);
        }
        // The messenger's radio (PC1999_MESH / "mesh.target"); never with --demo.
        MeshBackend::Options mesh_opt;
        mesh_opt.demo = args.demo;
        if (mesh_backend().start(mesh_opt)) {
            LOG_INFO("mesh: connecting to {}", mesh_link().target());
        } else {
            LOG_INFO("mesh: off, messenger in demo mode");
        }
        LOG_INFO("1999 Online started at {}x{} ({})",
                 lv_display_get_horizontal_resolution(display),
                 lv_display_get_vertical_resolution(display),
                 game.demo_mode() ? "demo" : "live");
        while (!router.quit_requested()
#if USE_DESKTOP
               && frame->process_events()
#endif
        ) {
            const uint32_t idle = lv_timer_handler();
            lv_delay_ms(idle < 5 ? idle : 5);
        }
    }

    mesh_backend().stop();
    platform::clear_key_release_listener(KeyHooks::on_release, &hooks);
    platform::clear_global_key_listener(KeyHooks::on_key, &hooks);
    settings().flush();   // no-op in --shots
    return rc;
}

} // namespace mm
