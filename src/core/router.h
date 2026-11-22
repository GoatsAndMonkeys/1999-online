// router.h — screen stack, persistent header/footer, and key dispatch.
#pragma once

#include "keys.h"
#include "screen.h"

#include "lvgl.h"

#include <memory>
#include <string>
#include <vector>

namespace mm::ui {

class Modal;

class Router {
public:
    explicit Router(GameState& game);
    ~Router();

    // Creates header/footer/body on the active LVGL screen and pushes Menu.
    void start();

    void push(ScreenId id);
    void pop();                       // no-op on the root screen
    void replace(ScreenId id);        // pop + push
    void reset_to_menu();
    // Fn held: the soft-key bar shows the apps instead of the screen's keys.
    void set_app_bar(bool on);
    bool app_bar() const { return app_bar_; }
    // Fn + digit: 3 home, 4..8 Buddy List / Mesh Space / MeshAmp / MonsterMesh /
    // Radio Setup, 9 Skins. Goes back to the app if it is open under the top
    // screen, else opens it over the main menu. false: no app on that key.
    bool open_app_key(int digit);
    void rebuild();                   // clear + rebuild the top screen's body
    void refresh_chrome();            // title / status / hints
    // Switch skin: restyles the chrome and rebuilds every open screen.
    void apply_skin(int index);
    // Short note for the middle of the status strip ("2 new messages"); empty clears it.
    void set_status_note(const std::string& note);

    Screen* top() const;
    ScreenId top_id() const;
    Ctx& ctx() { return ctx_; }

    // A modal on top of everything; keys go to it until it closes.
    void show_modal(std::unique_ptr<Modal> modal);

    // Raw key from the platform layer (press edge). Returns true if consumed.
    bool handle_raw_key(uint32_t raw, bool long_pressed);
    uint32_t nav_raw(uint32_t raw) const;   // F/Z/X/C -> arrows unless typing
    void handle_raw_release(uint32_t raw);

    // Driven by an LVGL timer (~33 ms).
    void tick();

    bool quit_requested() const { return quit_; }

private:
    struct Entry {
        ScreenId id;
        std::unique_ptr<Screen> screen;
        lv_obj_t* body;
    };

    void dispatch(const KeyEvent& e);
    void build_chrome();
    void layout_chrome();
    void update_device(bool force);   // battery / Wi-Fi readouts
    lv_obj_t* make_body(const Screen& s);
    void update_status();
    void update_softkeys();           // soft-key bar or hint text in the footer
    bool sync_skin();                 // wear the top screen's forced skin, or the user's
    static void timer_cb(lv_timer_t* t);

    GameState& game_;
    Ctx ctx_;
    std::vector<Entry> stack_;
    std::unique_ptr<Modal> modal_;
    bool app_bar_ = false;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* header_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* status_dot_ = nullptr;
    lv_obj_t* footer_ = nullptr;
    // Windowed chrome (Screen::windowed()).
    lv_obj_t* strip_ = nullptr;       // status strip
    lv_obj_t* st_left_ = nullptr;
    lv_obj_t* st_mid_ = nullptr;
    lv_obj_t* st_right_ = nullptr;
    lv_obj_t* st_wifi_ = nullptr;     // drawn Wi-Fi bars
    lv_obj_t* st_batt_ = nullptr;     // drawn battery + percent (+ bolt while charging)
    uint32_t last_dev_poll_ = 0;
    int dev_wifi_ = -2, dev_batt_ = -2;
    bool dev_charging_ = false;
    lv_obj_t* frame_ = nullptr;       // window frame + title bar
    lv_obj_t* wtitle_ = nullptr;
    lv_obj_t* wtab_ = nullptr;        // title bar (tab skins: sized to the text)
    std::string status_note_;
    uint32_t last_clock_min_ = 0xffffffff;
    uint32_t last_msg_rev_ = 0xffffffff;
    std::string last_mesh_status_;    // mesh_backend().status_text() shown in the strip
    lv_obj_t* hints_ = nullptr;
    lv_obj_t* softbar_ = nullptr;     // soft-key bar in the footer (replaces hints_)
    SoftKeys soft_;                   // what softbar_ shows
    lv_timer_t* timer_ = nullptr;

    uint32_t last_game_tick_ = 0;
    uint32_t last_status_rev_ = 0;
    bool last_connected_ = false;
    uint32_t seen_challenge_node_ = 0;

    // Key repeat for held directions (same on sim and device).
    uint32_t held_raw_ = 0;
    uint32_t held_since_ = 0;
    uint32_t last_repeat_ = 0;

    bool exit_armed_ = false;
    bool quit_ = false;
    int user_skin_ = -1;              // the skin the user picked (a screen may force another)
};

// Implemented in screens/registry.cpp.
std::unique_ptr<Screen> make_screen(ScreenId id, Ctx& ctx);
const char* screen_slug(ScreenId id);

} // namespace mm::ui
