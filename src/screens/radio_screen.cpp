// radio_screen.cpp — "Radio Setup": the Meshtastic settings menus.
//
// Left: the sections of mm::radio_config() (User, LoRa, channels, ..., module
// configs, Actions). Right: the fields of the selected section, label on the
// left and value on the right. Tab / Right / Enter moves into the fields,
// Enter edits one (bool: toggles; enum: pick list; number / text: an input
// line under the list), S saves the section, R reverts it, Esc backs out one
// level. Actions ask for confirmation in a small prompt inside the window.
//
// Nothing here reaches a radio yet: radio_config's apply() / run_action() only
// log (see TODO(daemon) in backend/radio_config.cpp), and the toasts say so.
#include "screens.h"

#include "radio_config.h"
#include "router.h"
#include "widgets.h"
#include "win.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace mm::ui {
namespace {

constexpr int kListW = 92;
constexpr int kPaneX = 95;
constexpr int kPaneW = kClientW - kPaneX;      // 217
constexpr int kHeadH = 13;                     // section title line above the fields
constexpr int kFieldsH = kClientH - kHeadH;    // 108
constexpr int kEditH = 42;                     // prompt + input + help under a shortened list
constexpr int kValueMaxW = 104;                // px a value may take in a field row

constexpr int kPromptW = 244;
constexpr int kPromptH = 78;
constexpr int kPromptX = (kClientW - kPromptW) / 2;
constexpr int kPromptY = (kClientH - kPromptH) / 2;

int text_width(const std::string& s) {
    const lv_font_t* f = font_ui();
    int w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const uint32_t next = i + 1 < s.size() ? static_cast<uint8_t>(s[i + 1]) : 0;
        w += lv_font_get_glyph_width(f, static_cast<uint8_t>(s[i]), next);
    }
    return w;
}

// Cut `s` to at most `max_px`, ending in ".." when shortened.
std::string fit(std::string s, int max_px) {
    if (text_width(s) <= max_px) return s;
    while (!s.empty() && text_width(s + "..") > max_px) s.pop_back();
    return s + "..";
}

class RadioScreen final : public Screen {
public:
    explicit RadioScreen(Ctx& ctx) : Screen(ctx) {}

    const char* title() const override { return "Radio Setup"; }
    bool windowed() const override { return true; }

    std::string hints() const override {
        switch (mode_) {
            case Mode::Sections:
                return dirty() ? "Enter:open  S:save  R:revert  Esc:back" : "Enter/Tab:open section  Esc:back";
            case Mode::Fields:
                if (is_actions()) return "Enter:run  Esc:sections";
                return dirty() ? "Enter:edit  S:save  R:revert  Esc:sections" : "Enter:edit  Esc/Tab:sections";
            case Mode::Pick:    return "Up/Down:choose  Enter:set  Esc:cancel";
            case Mode::Edit:    return "Type the value  Enter:set  Esc:cancel";
            case Mode::Confirm: return "Left/Right:choose  Enter:confirm  Esc:cancel";
        }
        return "Esc:back";
    }

    void build(lv_obj_t* body) override {
        body_ = body;
        render();
    }

    bool typing() const override { return mode_ == Mode::Edit; }

    bool on_key(const KeyEvent& e) override {
        if (!body_) return false;
        switch (mode_) {
            case Mode::Sections: return key_sections(e);
            case Mode::Fields:   return key_fields(e);
            case Mode::Pick:     return key_pick(e);
            case Mode::Edit:     return key_edit(e);
            case Mode::Confirm:  return key_confirm(e);
        }
        return false;
    }

    int demo_state_count() const override { return 6; }
    const char* demo_state_name(int i) const override {
        switch (i) {
            case 1:  return "lora";
            case 2:  return "enum";
            case 3:  return "text";
            case 4:  return "module";
            case 5:  return "confirm";
            default: return "sections";
        }
    }
    void set_demo_state(int i) override {
        RadioConfig& rc = radio_config();
        for (int s = 0; s < rc.section_count(); ++s) rc.revert(s);
        auto at = [&](const char* section, const char* field) {
            sec_ = std::max(0, rc.find_section(section));
            fld_ = std::max(0, rc.find_field(sec_, field));
        };
        mode_ = Mode::Fields;
        switch (i) {
            case 1:
                at("lora", "hop_limit");
                rc.set_int(sec_, rc.find_field(sec_, "region"), 1);   // US: shows the unsaved marker
                rc.set_int(sec_, fld_, 5);
                break;
            case 2:
                at("lora", "region");
                mode_ = Mode::Pick;
                pick_ = 3;   // EU_868
                break;
            case 3:
                at("user", "long_name");
                mode_ = Mode::Edit;
                edit_start_ = "Summit Station";
                break;
            case 4:
                at("mqtt", "address");
                break;
            case 5:
                at("actions", "factory_reset_config");
                mode_ = Mode::Confirm;
                confirm_yes_ = false;
                break;
            default:
                at("user", "long_name");
                mode_ = Mode::Sections;
                break;
        }
    }

private:
    enum class Mode : uint8_t { Sections, Fields, Pick, Edit, Confirm };

    const Section& sec() const { return radio_config().section(sec_); }
    const Field& fld() const { return sec().fields[static_cast<size_t>(fld_)]; }
    bool dirty() const { return sec().dirty; }
    bool is_actions() const { return sec().def.kind == SectionKind::Actions; }

    // ── Painting ────────────────────────────────────────────────────────────

    // Rebuild everything for the current mode. Only on build() and when the
    // mode changes; moving around a list repaints just that list.
    void render() {
        const Skin& s = skin();
        lv_obj_clean(body_);
        sections_.reset();
        fields_.reset();
        picks_.reset();
        input_.reset();
        head_ = head_state_ = nullptr;
        fld_ = std::clamp(fld_, 0, std::max(0, static_cast<int>(sec().fields.size()) - 1));

        sections_ = std::make_unique<win::ListBox>(body_, 0, 0, kListW, kClientH);
        sections_->set_active(mode_ == Mode::Sections);
        set_section_rows();

        head_ = win::text(body_, "", kPaneX + 2, 0, s.text, kPaneW - 70, true);
        head_state_ = win::text(body_, "", kPaneX, 0, s.text, 0, false);

        if (mode_ == Mode::Pick) {
            const FieldDef& d = *fld().def;
            picks_ = std::make_unique<win::ListBox>(body_, kPaneX, kHeadH, kPaneW, kFieldsH);
            std::vector<win::Row> rows;
            for (int i = 0; i < d.option_count; ++i) {
                win::Row r;
                r.text = d.options[i].label;
                r.short_text = d.options[i].name;
                // The protobuf name on the right, when both fit.
                if (text_width(r.text) + text_width(d.options[i].name) < kPaneW - 24) r.right = d.options[i].name;
                r.bold = d.options[i].value == fld().ival;
                rows.push_back(r);
            }
            picks_->set_rows(rows);
            picks_->select(pick_);
        } else {
            const bool editing = mode_ == Mode::Edit;
            const int list_h = editing ? kFieldsH - kEditH : kFieldsH;
            fields_ = std::make_unique<win::ListBox>(body_, kPaneX, kHeadH, kPaneW, list_h);
            fields_->set_active(mode_ != Mode::Sections);
            set_field_rows();
            if (editing) build_editor(kHeadH + list_h + 2);
        }
        set_header();
        if (mode_ == Mode::Confirm) build_confirm();
    }

    void set_section_rows() {
        const RadioConfig& rc = radio_config();
        std::vector<win::Row> rows;
        int want = -1;
        SectionKind last = SectionKind::Actions;
        for (int i = 0; i < rc.section_count(); ++i) {
            const Section& s = rc.section(i);
            // Group headers: radio config, module config, admin commands.
            const bool module = s.def.kind == SectionKind::Module;
            const bool admin = s.def.kind == SectionKind::Actions;
            const bool was_module = last == SectionKind::Module;
            if (i == 0 || (module && !was_module) || admin) {
                win::Row h;
                h.header = true;
                h.selectable = false;
                h.dim = true;
                h.text = admin ? "Admin" : module ? "Modules" : "Radio";
                h.tag = -1;
                rows.push_back(h);
            }
            last = s.def.kind;
            win::Row r;
            r.text = s.def.name;
            r.indent = 6;
            r.tag = i;
            if (s.dirty) {
                r.right = "*";
                r.bold = true;
            }
            if (i == sec_) want = static_cast<int>(rows.size());
            rows.push_back(r);
        }
        sections_->set_rows(rows);
        if (want >= 0) sections_->select(want);
    }

    void set_field_rows() {
        const RadioConfig& rc = radio_config();
        const Section& s = sec();
        std::vector<win::Row> rows;
        for (int i = 0; i < static_cast<int>(s.fields.size()); ++i) {
            const Field& f = s.fields[static_cast<size_t>(i)];
            win::Row r;
            r.text = f.def->label;
            r.tag = i;
            r.bold = f.changed();
            if (f.def->type == FieldType::Action) {
                r.right = "run";
            } else {
                std::string v = rc.display(sec_, i);
                if (v.empty()) v = "(empty)";
                r.right = fit(v, kValueMaxW);
            }
            rows.push_back(r);
        }
        fields_->set_rows(rows);
        fields_->select(fld_);
    }

    void set_header() {
        const Section& s = sec();
        std::string title = s.def.title;
        if (mode_ == Mode::Pick) title = std::string(s.def.name) + ": " + fld().def->label;
        lv_label_set_text(head_, title.c_str());
        std::string state;
        if (is_actions()) {
            state = "";
        } else if (mode_ == Mode::Pick) {
            state = fmt("%d choices", fld().def->option_count);
        } else {
            state = s.dirty ? "* unsaved  S: save" : "no changes";
        }
        lv_label_set_text(head_state_, state.c_str());
        lv_obj_set_style_text_color(head_state_, c(s.dirty ? skin().text : skin().dim), 0);
        lv_obj_set_x(head_state_, kClientW - 2 - text_width(state));
    }

    // After a value changed: both lists and the header, without rebuilding.
    void refresh_values() {
        set_section_rows();
        if (fields_) set_field_rows();
        set_header();
    }

    void build_editor(int y) {
        const Skin& s = skin();
        const Field& f = fld();
        const FieldDef& d = *f.def;
        std::string prompt = d.label;
        int max_bytes = d.max_len;
        if (d.type == FieldType::Int) {
            prompt += fmt(" (%lld to %lld%s%s):", static_cast<long long>(d.min), static_cast<long long>(d.max),
                          d.unit[0] ? " " : "", d.unit);
            max_bytes = 11;
        } else {
            prompt += fmt(" (up to %d characters):", d.max_len);
        }
        win::text(body_, prompt, kPaneX + 2, y, s.text, kPaneW - 4);
        input_ = std::make_unique<win::TextInput>(body_, kPaneX, y + 12, kPaneW, 15, max_bytes);
        input_->set_value(edit_start_);
        if (d.help[0]) win::text(body_, d.help, kPaneX + 2, y + 28, s.dim, kPaneW - 4);
    }

    void build_confirm() {
        const Skin& s = skin();
        const FieldDef& d = *fld().def;
        lv_obj_t* p = win::frame(body_, kPromptX, kPromptY, kPromptW, kPromptH, win::Edge::Raised, s.face);
        // A dark rim so the prompt reads as a separate layer over the lists.
        lv_obj_set_style_outline_width(p, 1, 0);
        lv_obj_set_style_outline_color(p, c(s.lo), 0);
        lv_obj_set_style_outline_pad(p, 0, 0);
        win::text(p, std::string(d.label) + "?", 8, 5, s.text, kPromptW - 16, true);
        win::text_wrap(p, d.help, 8, 19, kPromptW - 16, s.text);
        const int bw = 90, by = kPromptH - 22;
        win::button(p, "Yes", kPromptW / 2 - bw - 3, by, bw, confirm_yes_);
        win::button(p, "Cancel", kPromptW / 2 + 3, by, bw, !confirm_yes_);
    }

    // ── Keys ────────────────────────────────────────────────────────────────

    void set_mode(Mode m) {
        mode_ = m;
        render();
    }

    void save() {
        if (is_actions()) return;
        if (!dirty()) {
            toast("Nothing to save", 1200);
            return;
        }
        radio_config().apply(sec_);
        // Honest about what apply() does today.
        toast(std::string(sec().def.name) + ": saved here only, not sent to the radio yet", 2600);
        refresh_values();
    }

    void revert() {
        if (is_actions() || !dirty()) return;
        radio_config().revert(sec_);
        toast(std::string(sec().def.name) + ": changes dropped", 1500);
        refresh_values();
    }

    bool save_keys(const KeyEvent& e) {
        if (e.key != Key::Char) return false;
        if (e.ch == 's') {
            save();
            return true;
        }
        if (e.ch == 'r') {
            revert();
            return true;
        }
        return false;
    }

    bool key_sections(const KeyEvent& e) {
        if (sections_->on_key(e)) {
            const win::Row* r = sections_->selected_row();
            if (r && r->tag >= 0 && r->tag != sec_) {
                sec_ = r->tag;
                fld_ = 0;
                set_field_rows();
                set_header();
            }
            return true;
        }
        if (e.key == Key::Up || e.key == Key::Down) return true;   // at either end
        if (e.key == Key::A || e.key == Key::Tab || e.key == Key::Right) {
            if (!sec().fields.empty()) {
                mode_ = Mode::Fields;
                sections_->set_active(false);
                fields_->set_active(true);
            }
            return true;
        }
        if (e.key == Key::Left) return true;
        return save_keys(e);   // Esc falls through: the router pops the screen
    }

    bool key_fields(const KeyEvent& e) {
        if (fields_->on_key(e)) {
            fld_ = fields_->selected();
            return true;
        }
        if (e.key == Key::Up || e.key == Key::Down || e.key == Key::Right) return true;
        if (e.key == Key::B || e.key == Key::Tab || e.key == Key::Left) {
            mode_ = Mode::Sections;
            sections_->set_active(true);
            fields_->set_active(false);
            return true;
        }
        if (e.key == Key::A) {
            edit_field();
            return true;
        }
        save_keys(e);
        return true;
    }

    void edit_field() {
        if (fld_ < 0 || fld_ >= static_cast<int>(sec().fields.size())) return;
        const Field& f = fld();
        const FieldDef& d = *f.def;
        switch (d.type) {
            case FieldType::Bool:
                radio_config().set_int(sec_, fld_, f.ival ? 0 : 1);
                refresh_values();
                break;
            case FieldType::Enum:
                pick_ = 0;
                for (int i = 0; i < d.option_count; ++i) {
                    if (d.options[i].value == f.ival) pick_ = i;
                }
                set_mode(Mode::Pick);
                break;
            case FieldType::Int:
                edit_start_ = std::to_string(f.ival);
                set_mode(Mode::Edit);
                break;
            case FieldType::Text:
                edit_start_ = f.sval;
                set_mode(Mode::Edit);
                break;
            case FieldType::Action:
                confirm_yes_ = false;   // the safe button is the default
                set_mode(Mode::Confirm);
                break;
        }
    }

    bool key_pick(const KeyEvent& e) {
        if (picks_->on_key(e)) {
            pick_ = picks_->selected();
            return true;
        }
        if (e.key == Key::A) {
            const FieldDef& d = *fld().def;
            const int i = picks_->selected();
            if (i >= 0 && i < d.option_count) radio_config().set_int(sec_, fld_, d.options[i].value);
            set_mode(Mode::Fields);
        } else if (e.key == Key::B || e.key == Key::Left) {
            set_mode(Mode::Fields);
        }
        return true;
    }

    bool key_edit(const KeyEvent& e) {
        const FieldDef& d = *fld().def;
        const uint32_t r = e.raw;
        if (r == LV_KEY_ESC) {
            set_mode(Mode::Fields);
            return true;
        }
        if (r == LV_KEY_ENTER || r == '\r') {
            commit_edit();
            return true;
        }
        if (d.type == FieldType::Int && r >= 32 && r < 127) {
            // Digits only, and a leading minus where the range allows one.
            const bool minus = r == '-' && d.min < 0 && input_->value().empty();
            if (!(r >= '0' && r <= '9') && !minus) return true;
        }
        input_->on_key(e);
        return true;   // everything else is swallowed while editing
    }

    void commit_edit() {
        const FieldDef& d = *fld().def;
        const std::string v = input_->value();
        if (d.type == FieldType::Int) {
            if (v.empty() || v == "-") {
                toast("Type a number, or Esc to cancel", 1500);
                return;
            }
            const long long n = std::strtoll(v.c_str(), nullptr, 10);
            if (n < d.min || n > d.max) {
                toast(fmt("%s: %lld to %lld", d.label, static_cast<long long>(d.min),
                          static_cast<long long>(d.max)),
                      2000);
                return;   // stay in the editor
            }
            radio_config().set_int(sec_, fld_, n);
        } else {
            radio_config().set_text(sec_, fld_, v);
        }
        set_mode(Mode::Fields);
    }

    bool key_confirm(const KeyEvent& e) {
        if (e.key == Key::Left || e.key == Key::Right || e.key == Key::Tab) {
            confirm_yes_ = !confirm_yes_;
            render();
        } else if (e.key == Key::B || (e.key == Key::Char && e.ch == 'n')) {
            set_mode(Mode::Fields);
        } else if (e.key == Key::A) {
            const bool go = confirm_yes_;
            const std::string label = fld().def->label;
            if (go) radio_config().run_action(sec_, fld_);
            set_mode(Mode::Fields);
            // Honest about what run_action() does today.
            if (go) toast(label + ": not sent, no radio link yet", 2600);
        }
        return true;
    }

    lv_obj_t* body_ = nullptr;
    std::unique_ptr<win::ListBox> sections_;
    std::unique_ptr<win::ListBox> fields_;
    std::unique_ptr<win::ListBox> picks_;
    std::unique_ptr<win::TextInput> input_;
    lv_obj_t* head_ = nullptr;
    lv_obj_t* head_state_ = nullptr;

    Mode mode_ = Mode::Sections;
    int sec_ = 0;                 // index into radio_config()
    int fld_ = 0;                 // field of sec_
    int pick_ = 0;                // highlighted option while picking an enum
    bool confirm_yes_ = false;
    std::string edit_start_;      // text the editor opens with
};

} // namespace

std::unique_ptr<Screen> make_radio_screen(Ctx& ctx) {
    return std::make_unique<RadioScreen>(ctx);
}

} // namespace mm::ui
