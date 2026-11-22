// skins_screen.cpp — skin picker: list on the left, live sample on the right.
// Moving the selection applies the skin immediately; Esc keeps it.
#include "screens.h"

#include "router.h"
#include "widgets.h"
#include "win.h"

namespace mm::ui {
namespace {

class SkinsScreen final : public Screen {
public:
    explicit SkinsScreen(Ctx& ctx) : Screen(ctx) {}

    const char* title() const override { return "Skins"; }
    bool windowed() const override { return true; }
    std::string hints() const override { return "Up/Down:try a skin  Enter/Esc:keep it"; }

    void build(lv_obj_t* body) override {
        const Skin& s = skin();
        win::text(body, "Choose a skin:", 1, 1);
        list_ = std::make_unique<win::ListBox>(body, 0, 15, 128, kClientH - 15);
        std::vector<win::Row> rows;
        for (int i = 0; i < skin_count(); ++i) {
            win::Row r;
            r.text = skin_at(i).name;
            rows.push_back(r);
        }
        list_->set_rows(rows);
        list_->select(skin_index());

        // Sample: the pieces every screen is made of.
        const int x = 134;
        win::text(body, "Sample:", x, 1);
        win::card(body, x, 15, 176, 42);
        win::text(body, std::string(s.name), x + 5, 18, s.card_text, 166, true);
        win::text(body, "KD2ABC-m: 2 hops, SNR 6.5", x + 5, 30, s.card_text, 166);
        win::text(body, "at the summit brb", x + 5, 42, s.card_text, 166);
        lv_obj_t* f = win::field(body, x, 61, 176, 29);
        win::text(f, "xXmeshKid03Xx:", 3, 2, s.me, 0, true);
        win::text(f, "KD2ABC-m:", 3, 14, s.them, 0, true);
        win::text(f, "hi", 92, 2, s.field_text);
        win::text(f, "hey!!", 66, 14, s.field_text);
        win::button(body, "OK", x, 96, 56, true);
        win::button(body, "Cancel", x + 60, 96, 56);
        win::button(body, "Apply", x + 120, 96, 56);
    }

    bool on_key(const KeyEvent& e) override {
        if (list_ && list_->on_key(e)) {
            const int pick = list_->selected();
            if (pick != skin_index()) ctx_.router.apply_skin(pick);   // rebuilds this screen
            return true;
        }
        if (e.key == Key::A) {
            toast(std::string("Skin: ") + skin().name, 1200);
            ctx_.router.pop();
            return true;
        }
        return false;
    }

private:
    std::unique_ptr<win::ListBox> list_;
};

} // namespace

std::unique_ptr<Screen> make_skins_screen(Ctx& ctx) {
    return std::make_unique<SkinsScreen>(ctx);
}

} // namespace mm::ui
