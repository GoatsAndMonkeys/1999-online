#include "milk.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace mm::milk {
namespace {

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

enum : uint8_t {
    kConst, kLoad, kStore,
    kAdd, kSub, kMul, kDiv, kMod, kOr, kAnd, kPow, kNeg,
    kSin, kCos, kTan, kAsin, kAcos, kAtan, kAtan2, kAbs, kSqr, kSqrt, kPowF, kExp, kLog, kLog10,
    kMin, kMax, kSign, kInt, kRand, kSigmoid, kBnot, kBand, kBor, kAbove, kBelow, kEqual, kIf,
    kFloor, kCeil,
};

struct Func {
    const char* name;
    uint8_t op;
    int args;
};
constexpr Func kFuncs[] = {
    {"sin", kSin, 1},     {"cos", kCos, 1},     {"tan", kTan, 1},       {"asin", kAsin, 1},
    {"acos", kAcos, 1},   {"atan", kAtan, 1},   {"atan2", kAtan2, 2},   {"abs", kAbs, 1},
    {"sqr", kSqr, 1},     {"sqrt", kSqrt, 1},   {"pow", kPowF, 2},      {"exp", kExp, 1},
    {"log", kLog, 1},     {"log10", kLog10, 1}, {"min", kMin, 2},       {"max", kMax, 2},
    {"sign", kSign, 1},   {"int", kInt, 1},     {"rand", kRand, 1},     {"sigmoid", kSigmoid, 2},
    {"bnot", kBnot, 1},   {"band", kBand, 2},   {"bor", kBor, 2},       {"above", kAbove, 2},
    {"below", kBelow, 2}, {"equal", kEqual, 2}, {"if", kIf, 3},         {"floor", kFloor, 1},
    {"ceil", kCeil, 1},
};

// double -> integer for % | & and rand(): a NaN, an infinity or a huge value
// must not reach the cast (undefined behaviour), so they are pinned first.
long long to_ll(double v) {
    if (!(v == v)) return 0;
    if (v > 4.0e18) return 4000000000000000000LL;
    if (v < -4.0e18) return -4000000000000000000LL;
    return static_cast<long long>(v);
}

bool finite_or_zero(double& v) {
    if (std::isfinite(v)) return true;
    v = 0.0;
    return false;
}

} // namespace

// ── Vars ────────────────────────────────────────────────────────────────────

int Vars::index(const std::string& name) {
    const std::string key = lower(name);
    const auto it = names_.find(key);
    if (it != names_.end()) return it->second;
    const int i = static_cast<int>(values_.size());
    values_.push_back(0.0);
    names_.emplace(key, i);
    return i;
}

int Vars::find(const std::string& name) const {
    const auto it = names_.find(lower(name));
    return it == names_.end() ? -1 : it->second;
}

double Vars::get(const std::string& name, double fallback) const {
    const int i = find(name);
    return i < 0 ? fallback : values_[static_cast<size_t>(i)];
}

// ── Compiler: recursive descent to a small stack machine ────────────────────

class Compiler {
public:
    Compiler(const std::string& src, Vars& vars, Program& out) : s_(src), vars_(vars), out_(out) {}

    void run() {
        while (true) {
            skip();
            if (p_ >= s_.size()) break;
            if (s_[p_] == ';') { ++p_; continue; }
            const size_t mark = out_.code_.size();
            ok_ = true;
            statement();
            skip();
            if (ok_ && p_ < s_.size() && s_[p_] != ';') ok_ = false;
            if (!ok_) {
                out_.code_.resize(mark);
                ++out_.errors_;
                while (p_ < s_.size() && s_[p_] != ';') ++p_;      // resume at the next statement
            }
        }
    }

private:
    void skip() {
        while (p_ < s_.size()) {
            if (std::isspace(static_cast<unsigned char>(s_[p_]))) { ++p_; continue; }
            if (s_[p_] == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '/') {
                while (p_ < s_.size() && s_[p_] != '\n') ++p_;
                continue;
            }
            break;
        }
    }
    bool eat(char ch) {
        skip();
        if (p_ < s_.size() && s_[p_] == ch) { ++p_; return true; }
        return false;
    }
    void emit(uint8_t op, int32_t arg = 0, double v = 0.0) { out_.code_.push_back({op, arg, v}); }

    std::string ident() {
        skip();
        std::string id;
        while (p_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[p_])) || s_[p_] == '_')) id.push_back(s_[p_++]);
        return id;
    }

    void statement() {
        // "name = expr", "name += expr" ..., or a bare expression (no effect).
        const size_t save = p_;
        skip();
        if (p_ < s_.size() && (std::isalpha(static_cast<unsigned char>(s_[p_])) || s_[p_] == '_')) {
            const std::string id = ident();
            skip();
            char compound = 0;
            if (p_ + 1 < s_.size() && s_[p_ + 1] == '=' && (s_[p_] == '+' || s_[p_] == '-' || s_[p_] == '*' || s_[p_] == '/')) {
                compound = s_[p_];
                p_ += 2;
            } else if (p_ < s_.size() && s_[p_] == '=') {
                ++p_;
            } else {
                p_ = save;
                expr();
                emit(kStore, -1);
                return;
            }
            const int v = vars_.index(id);
            if (compound) emit(kLoad, v);
            expr();
            if (compound) emit(compound == '+' ? kAdd : compound == '-' ? kSub : compound == '*' ? kMul : kDiv);
            emit(kStore, v);
            return;
        }
        expr();
        emit(kStore, -1);
    }

    void expr() {                       // | &
        sum();
        while (ok_) {
            if (eat('|')) { sum(); emit(kOr); }
            else if (eat('&')) { sum(); emit(kAnd); }
            else break;
        }
    }
    void sum() {
        term();
        while (ok_) {
            if (eat('+')) { term(); emit(kAdd); }
            else if (eat('-')) { term(); emit(kSub); }
            else break;
        }
    }
    void term() {
        unary();
        while (ok_) {
            skip();
            if (p_ + 1 < s_.size() && s_[p_] == '/' && s_[p_ + 1] == '/') break;
            if (eat('*')) { unary(); emit(kMul); }
            else if (eat('/')) { unary(); emit(kDiv); }
            else if (eat('%')) { unary(); emit(kMod); }
            else break;
        }
    }
    void unary() {
        if (eat('-')) { unary(); emit(kNeg); return; }
        if (eat('+')) { unary(); return; }
        power();
    }
    void power() {
        primary();
        if (ok_ && eat('^')) { unary(); emit(kPow); }
    }
    void primary() {
        if (++depth_ > 200) { ok_ = false; --depth_; return; }
        skip();
        if (p_ >= s_.size()) {
            ok_ = false;
        } else if (s_[p_] == '(') {
            ++p_;
            expr();
            if (!eat(')')) ok_ = false;
        } else if (std::isdigit(static_cast<unsigned char>(s_[p_])) || s_[p_] == '.') {
            const char* begin = s_.c_str() + p_;
            char* end = nullptr;
            const double v = std::strtod(begin, &end);
            if (end == begin) ok_ = false;
            else { p_ += static_cast<size_t>(end - begin); emit(kConst, 0, v); }
        } else if (std::isalpha(static_cast<unsigned char>(s_[p_])) || s_[p_] == '_') {
            const std::string id = lower(ident());
            if (eat('(')) {
                const Func* f = nullptr;
                for (const Func& c : kFuncs) if (id == c.name) f = &c;
                int n = 0;
                if (!eat(')')) {
                    do { expr(); ++n; } while (ok_ && eat(','));
                    if (!eat(')')) ok_ = false;
                }
                if (!f || n != f->args) ok_ = false;
                else emit(f->op);
            } else {
                emit(kLoad, vars_.index(id));
            }
        } else {
            ok_ = false;
        }
        --depth_;
    }

    const std::string& s_;
    Vars& vars_;
    Program& out_;
    size_t p_ = 0;
    bool ok_ = true;
    int depth_ = 0;
};

void Program::compile(const std::string& source, Vars& vars) {
    code_.clear();
    errors_ = 0;
    Compiler(source, vars, *this).run();
}

void Program::run(Vars& vars, uint32_t& rng) const {
    double st[64];
    int sp = 0;
    const double eps = 0.00001;
    for (const Op& o : code_) {
        if (sp >= 60) sp = 0;                       // cannot happen with sane presets; stay in bounds
        switch (o.op) {
            case kConst: st[sp++] = o.value; break;
            case kLoad: st[sp++] = vars[o.arg]; break;
            case kStore:
                if (sp > 0) {
                    --sp;
                    finite_or_zero(st[sp]);
                    if (o.arg >= 0) vars[o.arg] = st[sp];
                }
                sp = 0;
                break;
#define BIN(expr_) { if (sp < 2) { sp = 0; st[sp++] = 0; break; } const double b = st[--sp]; const double a = st[sp - 1]; st[sp - 1] = (expr_); (void)a; (void)b; break; }
#define UN(expr_) { if (sp < 1) { st[sp++] = 0; break; } const double a = st[sp - 1]; st[sp - 1] = (expr_); break; }
            case kAdd: BIN(a + b)
            case kSub: BIN(a - b)
            case kMul: BIN(a * b)
            case kDiv: BIN(b == 0 ? 0 : a / b)
            case kMod: BIN((to_ll(b) == 0 || to_ll(b) == -1) ? 0 : static_cast<double>(to_ll(a) % to_ll(b)))
            case kOr: BIN(static_cast<double>(to_ll(a) | to_ll(b)))
            case kAnd: BIN(static_cast<double>(to_ll(a) & to_ll(b)))
            case kPow: case kPowF: BIN(std::pow(a, b))
            case kNeg: UN(-a)
            case kSin: UN(std::sin(a))
            case kCos: UN(std::cos(a))
            case kTan: UN(std::tan(a))
            case kAsin: UN(std::asin(a < -1 ? -1 : a > 1 ? 1 : a))
            case kAcos: UN(std::acos(a < -1 ? -1 : a > 1 ? 1 : a))
            case kAtan: UN(std::atan(a))
            case kAtan2: BIN(std::atan2(a, b))
            case kAbs: UN(std::fabs(a))
            case kSqr: UN(a * a)
            case kSqrt: UN(std::sqrt(std::fabs(a)))
            case kExp: UN(std::exp(a))
            case kLog: UN(a <= 0 ? 0 : std::log(a))
            case kLog10: UN(a <= 0 ? 0 : std::log10(a))
            case kMin: BIN(a < b ? a : b)
            case kMax: BIN(a > b ? a : b)
            case kSign: UN(a > 0 ? 1.0 : a < 0 ? -1.0 : 0.0)
            case kInt: UN(std::trunc(a))
            case kFloor: UN(std::floor(a))
            case kCeil: UN(std::ceil(a))
            case kRand: {
                if (sp < 1) { st[sp++] = 0; break; }
                rng = rng * 1664525u + 1013904223u;
                const long long n = to_ll(st[sp - 1]);
                st[sp - 1] = n >= 1 ? static_cast<double>((rng >> 8) % static_cast<uint32_t>(n > 0x7fffff ? 0x7fffff : n)) : 0.0;
                break;
            }
            case kSigmoid: BIN(1.0 / (1.0 + std::exp(-a * b)))
            case kBnot: UN(std::fabs(a) < eps ? 1.0 : 0.0)
            case kBand: BIN((std::fabs(a) > eps && std::fabs(b) > eps) ? 1.0 : 0.0)
            case kBor: BIN((std::fabs(a) > eps || std::fabs(b) > eps) ? 1.0 : 0.0)
            case kAbove: BIN(a > b ? 1.0 : 0.0)
            case kBelow: BIN(a < b ? 1.0 : 0.0)
            case kEqual: BIN(std::fabs(a - b) < eps ? 1.0 : 0.0)
#undef BIN
#undef UN
            case kIf: {
                if (sp < 3) { sp = 0; st[sp++] = 0; break; }
                const double no = st[--sp], yes = st[--sp], cond = st[sp - 1];
                st[sp - 1] = std::fabs(cond) > eps ? yes : no;
                break;
            }
            default: break;
        }
    }
}

// ── Preset files ────────────────────────────────────────────────────────────

double Preset::get(const std::string& key, double fallback) const {
    const auto it = params.find(key);
    return it == params.end() ? fallback : it->second;
}

namespace {

// "per_frame_12" -> ("per_frame_", 12). False if the key has no trailing number.
bool split_number(const std::string& key, std::string& stem, int& n) {
    size_t i = key.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(key[i - 1]))) --i;
    if (i == key.size() || i == 0) return false;
    stem = key.substr(0, i);
    n = std::atoi(key.c_str() + i);
    return true;
}

} // namespace

bool parse_preset(const std::string& text, Preset& out) {
    out = Preset{};
    std::istringstream in(text);
    std::string line;
    bool any = false;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = lower(line.substr(0, eq));
        while (!key.empty() && key.back() == ' ') key.pop_back();
        const std::string val = line.substr(eq + 1);
        any = true;

        if (key.compare(0, 5, "warp_") == 0 || key.compare(0, 5, "comp_") == 0) {
            if (!val.empty()) out.uses_shaders = true;
            continue;
        }
        std::string stem;
        int n = 0;
        if (key == "per_frame_init" || key == "per_frame" || key == "per_pixel") continue;
        if (split_number(key, stem, n)) {
            if (stem == "per_frame_init_") { out.init += val + "\n"; continue; }
            if (stem == "per_frame_") { out.per_frame += val + "\n"; continue; }
            if (stem == "per_pixel_") { out.per_pixel += val + "\n"; continue; }
        }
        // wavecode_2_samples=..., shapecode_0_enabled=..., wave_1_per_point7=..., shape_3_init1=...
        const bool wave = key.compare(0, 5, "wave_") == 0 || key.compare(0, 9, "wavecode_") == 0;
        const bool shape = key.compare(0, 6, "shape_") == 0 || key.compare(0, 10, "shapecode_") == 0;
        if (wave || shape) {
            const size_t us = key.find('_');
            if (us + 2 < key.size() && std::isdigit(static_cast<unsigned char>(key[us + 1])) && key[us + 2] == '_') {
                const int idx = key[us + 1] - '0';
                const std::string rest = key.substr(us + 3);
                if (idx >= 0 && idx < 4) {
                    CustomCode& c = wave ? out.waves[idx] : out.shapes[idx];
                    if (key.find("code_") != std::string::npos) {
                        const double v = std::strtod(val.c_str(), nullptr);
                        if (rest == "enabled") c.enabled = v != 0;
                        else c.params[rest] = v;
                    } else if (rest.compare(0, 9, "per_point") == 0) c.per_point += val + "\n";
                    else if (rest.compare(0, 9, "per_frame") == 0) c.per_frame += val + "\n";
                    else if (rest.compare(0, 4, "init") == 0) c.init += val + "\n";
                }
                continue;
            }
        }
        if (key == "psversion" || key == "psversion_warp" || key == "psversion_comp") {
            if (std::atoi(val.c_str()) >= 2) out.uses_shaders = true;
            continue;
        }
        char* end = nullptr;
        const double v = std::strtod(val.c_str(), &end);
        if (end != val.c_str()) out.params[key] = v;
    }
    return any;
}

bool load_preset(const std::string& path, Preset& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    if (text.size() > (1u << 20) || !parse_preset(text, out)) return false;
    size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.size() > 5 && lower(name.substr(name.size() - 5)) == ".milk") name.resize(name.size() - 5);
    out.name = name;
    return true;
}

} // namespace mm::milk
