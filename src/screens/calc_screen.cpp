// calc_screen.cpp — Calculator: a 1990s desktop calculator (Windows 95 calc.exe
// layout) in a skinned window. LCD-style readout, beveled button grid, memory
// register, and a Tab-toggled Scientific panel.
//
// Semantics are the classic immediate-execution ones: 2 + 3 * 4 = 20, an
// operator right after another replaces it, "5 + =" is 10, repeated = repeats
// the last operation (2 + 3 = = = gives 5, 8, 11), divide by zero and invalid
// input show an error until C / CE / Backspace / a digit.
//
// Keys: typing() is true, so the Router does NOT turn F/Z/X/C into arrows here
// (c is Clear, x is multiply); the real arrow keys (Fn+; . , / on the device)
// move the highlight. Bare ; . , / are typed, not arrows: '.' and ',' are the
// decimal point, '/' divides. Enter presses the highlighted button once the
// arrows have been used, otherwise it is '='. Backspace edits; Esc leaves.
//
// The Calc class is plain C++; define MM_CALC_LOGIC_ONLY to compile just it
// (standalone unit test).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace mm::ui {
namespace calc {

enum class Op : uint8_t { None, Add, Sub, Mul, Div, Pow };
enum class Fn : uint8_t { Sqrt, Inv, Neg, Sq, Cube, Sin, Cos, Tan, Log, Ln, Pow10, Exp, Fact };

constexpr int kMaxDigits = 16;
constexpr double kPi = 3.14159265358979323846;
constexpr double kE = 2.71828182845904523536;

inline std::string format_number(double v) {
    if (v == 0) return "0";   // also -0
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.15g", v);
    return buf;
}

inline const char* op_symbol(Op o) {
    switch (o) {
        case Op::Add: return "+";
        case Op::Sub: return "-";
        case Op::Mul: return "*";
        case Op::Div: return "/";
        case Op::Pow: return "^";
        default: return "";
    }
}

class Calc {
public:
    // ── entry ──
    void digit(int d) {
        if (!err_.empty()) clear();
        begin_entry();
        int n = 0;
        for (char ch : entry_) n += (ch >= '0' && ch <= '9');
        if (entry_ == "0") entry_.clear();
        else if (entry_ == "-0") entry_ = "-";
        else if (n >= kMaxDigits) return;
        entry_ += static_cast<char>('0' + d);
    }
    void point() {
        if (!err_.empty()) clear();
        begin_entry();
        if (entry_.find('.') == std::string::npos) entry_ += '.';
    }
    void backspace() {
        if (!err_.empty()) { clear(); return; }
        if (!typing_) return;
        entry_.pop_back();
        if (entry_.empty() || entry_ == "-") entry_ = "0";
    }

    // ── operators ──
    void op(Op o) {
        if (!err_.empty()) return;
        if (pending_ != Op::None && fresh_) {
            double r;
            if (!apply(acc_, pending_, value(), r)) return;
            acc_ = r;
        } else if (pending_ == Op::None) {
            acc_ = value();
        }
        pending_ = o;
        val_ = acc_;
        typing_ = false;
        fresh_ = false;
    }
    void equals() {
        if (!err_.empty()) return;
        double r;
        if (pending_ != Op::None) {
            const double operand = fresh_ ? value() : acc_;
            if (!apply(acc_, pending_, operand, r)) return;
            last_op_ = pending_;
            last_operand_ = operand;
            pending_ = Op::None;
        } else if (last_op_ != Op::None) {
            if (!apply(value(), last_op_, last_operand_, r)) return;
        } else {
            r = value();
        }
        acc_ = val_ = r;
        typing_ = false;
        fresh_ = false;
    }
    void percent() {
        if (!err_.empty()) return;
        const double v = value();
        set_operand(pending_ != Op::None ? acc_ * v / 100.0 : v / 100.0);
    }
    void unary(Fn f) {
        if (!err_.empty()) return;
        if (f == Fn::Neg && typing_) {   // sign of the number being typed
            if (entry_ == "0") return;
            if (entry_[0] == '-') entry_.erase(0, 1);
            else entry_.insert(0, "-");
            return;
        }
        const double v = value();
        const double rad = degrees ? v * kPi / 180.0 : v;
        double r = 0;
        switch (f) {
            case Fn::Neg: r = -v; break;
            case Fn::Sqrt:
                if (v < 0) return fail("Invalid input");
                r = std::sqrt(v);
                break;
            case Fn::Inv:
                if (v == 0) return fail("Cannot divide by zero");
                r = 1.0 / v;
                break;
            case Fn::Sq: r = v * v; break;
            case Fn::Cube: r = v * v * v; break;
            case Fn::Sin: r = snap(std::sin(rad)); break;
            case Fn::Cos: r = snap(std::cos(rad)); break;
            case Fn::Tan:
                if (degrees ? std::fabs(std::fmod(std::fabs(v), 180.0) - 90.0) < 1e-12
                            : std::fabs(std::cos(rad)) < 1e-15)
                    return fail("Invalid input");
                r = snap(std::tan(rad));
                break;
            case Fn::Log:
                if (v <= 0) return fail("Invalid input");
                r = std::log10(v);
                break;
            case Fn::Ln:
                if (v <= 0) return fail("Invalid input");
                r = std::log(v);
                break;
            case Fn::Pow10: r = std::pow(10.0, v); break;
            case Fn::Exp: r = std::exp(v); break;
            case Fn::Fact: {
                if (v < 0 || v != std::floor(v)) return fail("Invalid input");
                if (v > 170) return fail("Overflow");
                r = 1;
                for (int i = 2; i <= static_cast<int>(v); ++i) r *= i;
                break;
            }
        }
        if (!std::isfinite(r)) return fail("Overflow");
        set_operand(r);
    }
    void constant(double v) {
        if (!err_.empty()) clear();
        set_operand(v);
    }

    // ── clearing ──
    void clear() {
        entry_ = "0";
        val_ = acc_ = last_operand_ = 0;
        pending_ = last_op_ = Op::None;
        typing_ = fresh_ = false;
        err_.clear();
    }
    void clear_entry() {
        if (!err_.empty()) { clear(); return; }
        typing_ = false;
        val_ = 0;
        fresh_ = true;
    }

    // ── memory ──
    void mem_clear() { mem_ = 0; }
    void mem_recall() { if (err_.empty()) set_operand(mem_); }
    void mem_store() { if (err_.empty()) { mem_ = value(); end_entry(); } }
    void mem_add() { if (err_.empty()) { mem_ += value(); end_entry(); } }

    // ── readout ──
    std::string display() const { return !err_.empty() ? err_ : typing_ ? entry_ : format_number(val_); }
    // The pending left operand and operator, e.g. "12 +"; empty if none.
    std::string expression() const {
        if (!err_.empty() || pending_ == Op::None) return {};
        return format_number(acc_) + " " + op_symbol(pending_);
    }
    bool error() const { return !err_.empty(); }
    bool has_memory() const { return mem_ != 0; }
    double memory() const { return mem_; }
    double value() const { return typing_ ? std::strtod(entry_.c_str(), nullptr) : val_; }

    bool degrees = true;      // trig unit (Scientific)

private:
    void begin_entry() {
        if (typing_) return;
        entry_ = "0";
        typing_ = true;
        fresh_ = true;
    }
    // The display holds a computed operand (function result, MR, constant).
    void set_operand(double v) {
        val_ = v;
        typing_ = false;
        fresh_ = true;
    }
    // MS / M+ finish the number being typed: the next digit starts a new one.
    void end_entry() {
        if (typing_) set_operand(value());
    }
    void fail(const char* msg) {
        err_ = msg;
        typing_ = false;
        pending_ = last_op_ = Op::None;
    }
    static double snap(double r) { return std::fabs(r) < 1e-15 ? 0.0 : r; }
    bool apply(double a, Op o, double b, double& r) {
        switch (o) {
            case Op::Add: r = a + b; break;
            case Op::Sub: r = a - b; break;
            case Op::Mul: r = a * b; break;
            case Op::Div:
                if (b == 0) { fail(a == 0 ? "Result is undefined" : "Cannot divide by zero"); return false; }
                r = a / b;
                break;
            case Op::Pow: r = std::pow(a, b); break;
            default: r = b; break;
        }
        if (std::isnan(r)) { fail("Invalid input"); return false; }
        if (!std::isfinite(r)) { fail("Overflow"); return false; }
        return true;
    }

    std::string entry_ = "0";   // the number being typed (typing_)
    double val_ = 0;            // the number shown when not typing
    double acc_ = 0;            // left operand of pending_
    Op pending_ = Op::None;
    Op last_op_ = Op::None;     // for repeated =
    double last_operand_ = 0;
    bool typing_ = false;       // digits append to entry_
    bool fresh_ = false;        // the display is a new right operand for pending_
    std::string err_;
    double mem_ = 0;
};

} // namespace calc
} // namespace mm::ui

#ifndef MM_CALC_LOGIC_ONLY

#include "screens.h"

#include "widgets.h"
#include "win.h"

#include <vector>

namespace mm::ui {
namespace {

using calc::Calc;
using calc::Fn;
using calc::Op;

enum class Act : uint8_t { Digit, Point, Oper, Eq, Func, Pct, Clear, ClearEntry, Back,
                           MemClear, MemRecall, MemStore, MemAdd, Const, DegRad };

struct Btn {
    std::string label;
    Act act;
    int arg = 0;
    int x = 0, y = 0, w = 0, h = 0;
    bool red = false;     // operator / memory / clear colour (s.me), else s.them
};

constexpr int kReadH = 28;
constexpr int kRowY0 = 31;
constexpr int kRowPitch = 18;
constexpr int kBtnH = 16;
constexpr uint32_t kFlashMs = 140;

class CalcScreen final : public Screen {
public:
    explicit CalcScreen(Ctx& ctx) : Screen(ctx) { layout(); }

    const char* title() const override { return "Calculator"; }
    bool windowed() const override { return true; }
    bool typing() const override { return true; }   // c / x / f / z are calculator keys here

    std::string hints() const override {
        if (nav_) return "Arrows:move  Enter:press  Tab:" + std::string(sci_ ? "Standard" : "Scientific") + "  Esc:back";
        return std::string("0-9 + - * / =  c:C e:CE  Arrows:pick  Tab:") + (sci_ ? "Std" : "Sci") + "  Esc:back";
    }

    void build(lv_obj_t* body) override {
        body_ = body;
        draw();
    }

    void tick(uint32_t now_ms) override {
        if (pressed_ >= 0 && !hold_press_ && now_ms - pressed_at_ >= kFlashMs) {
            pressed_ = -1;
            draw();
        }
    }

    bool on_key(const KeyEvent& e) override {
        const uint32_t raw = e.raw;
        const bool arrow = raw == LV_KEY_UP || raw == LV_KEY_DOWN || raw == LV_KEY_LEFT || raw == LV_KEY_RIGHT;
        // The Router auto-repeats '.' and '/' (they double as arrows); swallow those.
        if (e.repeat && !arrow) return true;
        if (raw == LV_KEY_ESC || raw == '`') return false;   // leave the screen
        hold_press_ = false;

        if (arrow) {
            nav_ = true;
            move(raw);
            draw();
            return true;
        }
        if (e.key == Key::Tab) {
            toggle_sci();
            return true;
        }
        if (raw == LV_KEY_ENTER || raw == '\r') {
            if (nav_) press(focus_);
            else act(Act::Eq, 0);
            return true;
        }
        if (raw == ' ') {
            nav_ = true;
            press(focus_);
            return true;
        }
        if (raw == LV_KEY_BACKSPACE) { nav_ = false; act(Act::Back, 0); return true; }
        if (raw == LV_KEY_DEL || raw == 127) { nav_ = false; act(Act::ClearEntry, 0); return true; }

        Act a;
        int arg = 0;
        if (!shortcut(raw, a, arg)) return false;
        nav_ = false;
        act(a, arg);
        return true;
    }

    int demo_state_count() const override { return 5; }
    const char* demo_state_name(int i) const override {
        static const char* const kNames[] = {"idle", "midcalc", "memory", "error", "scientific"};
        return (i >= 0 && i < 5) ? kNames[i] : "idle";
    }
    void set_demo_state(int i) override {
        calc_ = Calc();
        sci_ = i == 4;
        nav_ = false;
        pressed_ = -1;
        hold_press_ = false;
        layout();
        focus_ = find(Act::Eq, 0);
        auto type = [this](const char* keys) {
            for (const char* k = keys; *k; ++k) {
                Act a;
                int arg = 0;
                if (shortcut(static_cast<uint8_t>(*k), a, arg)) act(a, arg, false);
            }
        };
        switch (i) {
            case 1:   // 1234.5 + 678, the 8 key caught mid-flash
                type("1234.5+678");
                pressed_ = find(Act::Digit, 8);
                hold_press_ = true;
                break;
            case 2:   // sqrt 144 stored in memory, 7 * MR, highlight on MR
                type("144qmc7*r");
                nav_ = true;
                focus_ = find(Act::MemRecall, 0);
                break;
            case 3:   // 9 / 0 =
                type("9/0=");
                break;
            case 4:   // Scientific: 3 * sin 30 (degrees)
                type("3*30s");
                pressed_ = find(Act::Func, static_cast<int>(Fn::Sin));
                hold_press_ = true;
                break;
            default: break;
        }
    }

private:
    // Keyboard shortcuts (raw key, case kept). Windows calc letters where they
    // do not clash: @ sqrt, % percent, r is MR here (i is 1/x).
    static bool shortcut(uint32_t raw, Act& a, int& arg) {
        arg = 0;
        if (raw >= '0' && raw <= '9') { a = Act::Digit; arg = static_cast<int>(raw - '0'); return true; }
        switch (raw) {
            case '.': case ',': a = Act::Point; return true;
            case '+': a = Act::Oper; arg = static_cast<int>(Op::Add); return true;
            case '-': a = Act::Oper; arg = static_cast<int>(Op::Sub); return true;
            case '*': case 'x': case 'X': a = Act::Oper; arg = static_cast<int>(Op::Mul); return true;
            case '/': a = Act::Oper; arg = static_cast<int>(Op::Div); return true;
            case '^': a = Act::Oper; arg = static_cast<int>(Op::Pow); return true;
            case '=': a = Act::Eq; return true;
            case '%': a = Act::Pct; return true;
            case 'c': case 'C': a = Act::Clear; return true;
            case 'e': case 'E': a = Act::ClearEntry; return true;
            case 'n': case 'N': case '_': case '~': a = Act::Func; arg = static_cast<int>(Fn::Neg); return true;
            case 'q': case 'Q': case '@': a = Act::Func; arg = static_cast<int>(Fn::Sqrt); return true;
            case 'i': case 'I': a = Act::Func; arg = static_cast<int>(Fn::Inv); return true;
            case 'l': case 'L': a = Act::MemClear; return true;
            case 'r': case 'R': a = Act::MemRecall; return true;
            case 'm': case 'M': a = Act::MemStore; return true;
            case 'p': case 'P': a = Act::MemAdd; return true;
            case 's': case 'S': a = Act::Func; arg = static_cast<int>(Fn::Sin); return true;
            case 'o': case 'O': a = Act::Func; arg = static_cast<int>(Fn::Cos); return true;
            case 't': case 'T': a = Act::Func; arg = static_cast<int>(Fn::Tan); return true;
            case 'g': case 'G': a = Act::Func; arg = static_cast<int>(Fn::Log); return true;
            case '!': a = Act::Func; arg = static_cast<int>(Fn::Fact); return true;
            case 'd': case 'D': a = Act::DegRad; return true;
            default: return false;
        }
    }

    // ── layout ──
    void layout() {
        btns_.clear();
        auto add = [this](const char* label, Act a, int arg, int x, int row, int w, bool red) {
            btns_.push_back({label, a, arg, x, kRowY0 + row * kRowPitch, w, kBtnH, red});
        };
        const auto fn = [](Fn f) { return static_cast<int>(f); };
        const auto op = [](Op o) { return static_cast<int>(o); };

        int mem_x, mem_w, main_x, pitch, w;
        if (sci_) {
            // 3 scientific columns | memory column | 5 main columns.
            const int sx[3] = {0, 38, 76};
            const int sw = 35;
            add(calc_.degrees ? "Deg" : "Rad", Act::DegRad, 0, 0, 0, 73, false);
            add("pi", Act::Const, 0, sx[2], 0, sw, false);
            const char* const names[4][3] = {{"sin", "cos", "tan"}, {"x^2", "x^3", "x^y"},
                                             {"log", "ln", "n!"}, {"10^x", "e^x", "e"}};
            const int fns[4][3] = {{fn(Fn::Sin), fn(Fn::Cos), fn(Fn::Tan)},
                                   {fn(Fn::Sq), fn(Fn::Cube), -1},
                                   {fn(Fn::Log), fn(Fn::Ln), fn(Fn::Fact)},
                                   {fn(Fn::Pow10), fn(Fn::Exp), -2}};
            for (int r = 0; r < 4; ++r) {
                for (int col = 0; col < 3; ++col) {
                    const int f = fns[r][col];
                    if (f == -1) add(names[r][col], Act::Oper, op(Op::Pow), sx[col], r + 1, sw, true);
                    else if (f == -2) add(names[r][col], Act::Const, 1, sx[col], r + 1, sw, false);
                    else add(names[r][col], Act::Func, f, sx[col], r + 1, sw, false);
                }
            }
            mem_x = 117; mem_w = 36; main_x = 159; pitch = 31; w = 29;
            add("Back", Act::Back, 0, 159, 0, 49, true);
            add("CE", Act::ClearEntry, 0, 211, 0, 49, true);
            add("C", Act::Clear, 0, 263, 0, 49, true);
        } else {
            mem_x = 0; mem_w = 44; main_x = 50; pitch = 53; w = 50;
            add("Backspace", Act::Back, 0, 50, 0, 84, true);
            add("CE", Act::ClearEntry, 0, 139, 0, 84, true);
            add("C", Act::Clear, 0, 228, 0, 84, true);
        }
        mem_x_ = mem_x;
        mem_w_ = mem_w;
        const auto col = [&](int i) { return main_x + i * pitch; };

        add("MC", Act::MemClear, 0, mem_x, 1, mem_w, true);
        add("MR", Act::MemRecall, 0, mem_x, 2, mem_w, true);
        add("MS", Act::MemStore, 0, mem_x, 3, mem_w, true);
        add("M+", Act::MemAdd, 0, mem_x, 4, mem_w, true);

        const int digits[3][3] = {{7, 8, 9}, {4, 5, 6}, {1, 2, 3}};
        for (int r = 0; r < 3; ++r)
            for (int c3 = 0; c3 < 3; ++c3) {
                const int d = digits[r][c3];
                static const char* const kDigit[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
                add(kDigit[d], Act::Digit, d, col(c3), r + 1, w, false);
            }
        add("0", Act::Digit, 0, col(0), 4, w, false);
        add("+/-", Act::Func, fn(Fn::Neg), col(1), 4, w, false);
        add(".", Act::Point, 0, col(2), 4, w, false);

        add("/", Act::Oper, op(Op::Div), col(3), 1, w, true);
        add("*", Act::Oper, op(Op::Mul), col(3), 2, w, true);
        add("-", Act::Oper, op(Op::Sub), col(3), 3, w, true);
        add("+", Act::Oper, op(Op::Add), col(3), 4, w, true);
        add("sqrt", Act::Func, fn(Fn::Sqrt), col(4), 1, w, false);
        add("%", Act::Pct, 0, col(4), 2, w, false);
        add("1/x", Act::Func, fn(Fn::Inv), col(4), 3, w, false);
        add("=", Act::Eq, 0, col(4), 4, w, true);

        if (focus_ < 0 || focus_ >= static_cast<int>(btns_.size())) focus_ = find(Act::Eq, 0);
    }

    int find(Act a, int arg) const {
        for (int i = 0; i < static_cast<int>(btns_.size()); ++i)
            if (btns_[i].act == a && btns_[i].arg == arg) return i;
        return -1;
    }

    void toggle_sci() {
        const Act fa = focus_ >= 0 ? btns_[focus_].act : Act::Eq;
        const int farg = focus_ >= 0 ? btns_[focus_].arg : 0;
        sci_ = !sci_;
        pressed_ = -1;
        layout();
        focus_ = find(fa, farg);
        if (focus_ < 0) focus_ = find(Act::Eq, 0);
        draw();
    }

    // Arrow navigation by geometry: the nearest button whose centre lies in
    // that direction, preferring the closest row/column.
    void move(uint32_t raw) {
        if (focus_ < 0) { focus_ = find(Act::Eq, 0); return; }
        const Btn& f = btns_[focus_];
        const int fx = f.x + f.w / 2, fy = f.y + f.h / 2;
        int best = -1;
        long best_score = 0;
        for (int i = 0; i < static_cast<int>(btns_.size()); ++i) {
            if (i == focus_) continue;
            const Btn& b = btns_[i];
            const int bx = b.x + b.w / 2, by = b.y + b.h / 2;
            int along = 0, across = 0;
            switch (raw) {
                case LV_KEY_UP:    along = fy - by; across = bx - fx; break;
                case LV_KEY_DOWN:  along = by - fy; across = bx - fx; break;
                case LV_KEY_LEFT:  along = fx - bx; across = by - fy; break;
                default:           along = bx - fx; across = by - fy; break;
            }
            if (along <= 0) continue;
            // Left/Right stay on the row (overlapping vertically); Up/Down pick
            // the next row first, then the nearest column.
            const bool horiz = raw == LV_KEY_LEFT || raw == LV_KEY_RIGHT;
            if (horiz && std::abs(across) > kBtnH / 2) continue;
            const long score = horiz ? along : static_cast<long>(along) * 1000 + std::abs(across);
            if (best < 0 || score < best_score) { best = i; best_score = score; }
        }
        if (best >= 0) focus_ = best;
    }

    // ── actions ──
    void press(int index) {
        if (index < 0 || index >= static_cast<int>(btns_.size())) return;
        act(btns_[index].act, btns_[index].arg);
    }

    void act(Act a, int arg, bool redraw = true) {
        switch (a) {
            case Act::Digit: calc_.digit(arg); break;
            case Act::Point: calc_.point(); break;
            case Act::Oper: calc_.op(static_cast<Op>(arg)); break;
            case Act::Eq: calc_.equals(); break;
            case Act::Func: calc_.unary(static_cast<Fn>(arg)); break;
            case Act::Pct: calc_.percent(); break;
            case Act::Clear: calc_.clear(); break;
            case Act::ClearEntry: calc_.clear_entry(); break;
            case Act::Back: calc_.backspace(); break;
            case Act::MemClear: calc_.mem_clear(); break;
            case Act::MemRecall: calc_.mem_recall(); break;
            case Act::MemStore: calc_.mem_store(); break;
            case Act::MemAdd: calc_.mem_add(); break;
            case Act::Const: calc_.constant(arg == 0 ? calc::kPi : calc::kE); break;
            case Act::DegRad: {
                calc_.degrees = !calc_.degrees;
                const int i = find(Act::DegRad, 0);
                if (i >= 0) btns_[i].label = calc_.degrees ? "Deg" : "Rad";
                break;
            }
        }
        pressed_ = find(a, arg);   // flash the button (if it is on screen)
        pressed_at_ = lv_tick_get();
        if (redraw) draw();
    }

    // ── drawing ──
    void draw() {
        if (!body_) return;
        lv_obj_clean(body_);
        const Skin& s = skin();

        // Readout: a sunken "LCD" with the number right-aligned.
        lv_obj_t* lcd = win::field(body_, 0, 0, kClientW, kReadH);
        const std::string shown = calc_.display();
        const lv_font_t* big = shown.size() > 20 ? &lv_font_montserrat_16 : &lv_font_montserrat_20;
        lv_obj_t* num = lv_label_create(lcd);
        lv_obj_set_style_text_font(num, big, 0);
        lv_obj_set_style_text_color(num, c(s.field_text), 0);
        lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(num, kClientW - 10);
        lv_label_set_long_mode(num, LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_text(num, shown.c_str());
        lv_obj_align(num, LV_ALIGN_RIGHT_MID, -5, 1);

        // Pending "12 +" and the trig unit, small, top-left (only when the
        // number leaves room for them).
        std::string note = calc_.expression();
        if (sci_) note = std::string(calc_.degrees ? "DEG" : "RAD") + (note.empty() ? "" : "   " + note);
        if (!note.empty() && shown.size() <= 15) {
            win::text(lcd, note, 4, 2, s.dim, 110);
        }

        // Memory indicator above MC, like calc.exe.
        lv_obj_t* mem = win::frame(body_, mem_x_, kRowY0, mem_w_, kBtnH, win::Edge::Sunken, s.face);
        if (calc_.has_memory()) {
            lv_obj_t* m = win::text(mem, "M", 0, 0, s.text, 0, true);
            lv_obj_center(m);
        }

        for (int i = 0; i < static_cast<int>(btns_.size()); ++i) draw_button(btns_[i], i == focus_ && nav_, i == pressed_);
    }

    void draw_button(const Btn& b, bool focused, bool pressed) {
        const Skin& s = skin();
        uint32_t fg = b.red ? s.me : s.them;
        lv_obj_t* o;
        if (pressed && s.bevel) {
            o = win::frame(body_, b.x, b.y, b.w, b.h, win::Edge::Sunken, s.face);
        } else if (pressed) {
            o = win::frame(body_, b.x, b.y, b.w, b.h, win::Edge::Raised, s.sel, s.radius);
            fg = s.sel_text;
        } else {
            o = win::frame(body_, b.x, b.y, b.w, b.h, win::Edge::Raised, s.face, s.radius);
        }
        if (focused) {
            lv_obj_set_style_outline_width(o, 1, 0);
            lv_obj_set_style_outline_color(o, c(s.text), 0);
            lv_obj_set_style_outline_pad(o, 0, 0);
        }
        lv_obj_t* l = win::text(o, b.label, 0, 0, fg, 0, focused);
        lv_obj_align(l, LV_ALIGN_CENTER, pressed && s.bevel ? 1 : 0, pressed && s.bevel ? 1 : 0);
    }

    Calc calc_;
    std::vector<Btn> btns_;
    lv_obj_t* body_ = nullptr;
    bool sci_ = false;
    bool nav_ = false;          // the arrows were used: the highlight shows, Enter presses it
    int focus_ = -1;
    int pressed_ = -1;          // flashing button
    uint32_t pressed_at_ = 0;
    bool hold_press_ = false;   // demo shots: keep the flash
    int mem_x_ = 0, mem_w_ = 44;
};

} // namespace

std::unique_ptr<Screen> make_calc_screen(Ctx& ctx) {
    return std::make_unique<CalcScreen>(ctx);
}

} // namespace mm::ui

#endif // MM_CALC_LOGIC_ONLY
