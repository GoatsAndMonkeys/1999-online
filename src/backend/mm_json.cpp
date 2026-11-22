// mm_json.cpp — see mm_json.h.
#include "mm_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {

namespace {
const JsonValue &null_value() {
    static const JsonValue v;
    return v;
}
constexpr int kMaxDepth = 32;
}  // namespace

// ── JsonValue accessors ──────────────────────────────────────────────────────

const JsonValue &JsonValue::operator[](const char *key) const {
    if (type_ != Type::Object || !key) return null_value();
    for (const auto &kv : obj_)
        if (kv.first == key) return kv.second;
    return null_value();
}

bool JsonValue::has(const char *key) const {
    if (type_ != Type::Object || !key) return false;
    for (const auto &kv : obj_)
        if (kv.first == key) return true;
    return false;
}

const JsonValue &JsonValue::operator[](size_t idx) const {
    if (type_ != Type::Array || idx >= arr_.size()) return null_value();
    return arr_[idx];
}

size_t JsonValue::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

double JsonValue::as_double(double def) const {
    switch (type_) {
        case Type::Number: return num_;
        case Type::Bool:   return b_ ? 1.0 : 0.0;
        case Type::String: {
            if (str_.empty()) return def;
            char *end = nullptr;
            double d = strtod(str_.c_str(), &end);
            return (end && *end == '\0') ? d : def;
        }
        default: return def;
    }
}

int64_t JsonValue::as_int(int64_t def) const {
    if (type_ != Type::Number && type_ != Type::Bool && type_ != Type::String) return def;
    double d = as_double((double)def);
    if (std::isnan(d)) return def;
    if (d > 9.2e18) return INT64_MAX;
    if (d < -9.2e18) return INT64_MIN;
    return (int64_t)d;
}

uint32_t JsonValue::as_u32(uint32_t def) const {
    int64_t v = as_int((int64_t)def);
    if (v < 0) return def;
    if (v > 0xFFFFFFFFll) return 0xFFFFFFFFu;
    return (uint32_t)v;
}

bool JsonValue::as_bool(bool def) const {
    switch (type_) {
        case Type::Bool:   return b_;
        case Type::Number: return num_ != 0.0;
        case Type::String: return str_ == "1" || str_ == "true";
        default: return def;
    }
}

std::string JsonValue::as_string(const std::string &def) const {
    switch (type_) {
        case Type::String: return str_;
        case Type::Number: {
            char buf[32];
            if (num_ == std::floor(num_) && std::fabs(num_) < 1e15)
                snprintf(buf, sizeof(buf), "%lld", (long long)num_);
            else
                snprintf(buf, sizeof(buf), "%g", num_);
            return buf;
        }
        case Type::Bool: return b_ ? "true" : "false";
        default: return def;
    }
}

// ── Parser ───────────────────────────────────────────────────────────────────

class JsonParser {
public:
    JsonParser(const char *p, size_t n) : s_(p), n_(n) {}

    bool parse(JsonValue &out, std::string *err) {
        skip_ws();
        if (!value(out, 0)) return fail(err);
        skip_ws();
        if (i_ != n_) { why_ = "trailing data"; return fail(err); }
        return true;
    }

private:
    const char *s_;
    size_t n_;
    size_t i_ = 0;
    const char *why_ = "syntax error";

    bool fail(std::string *err) {
        if (err) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s at offset %zu", why_, i_);
            *err = buf;
        }
        return false;
    }

    void skip_ws() {
        while (i_ < n_ && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }

    bool lit(const char *w) {
        size_t L = strlen(w);
        if (i_ + L > n_ || memcmp(s_ + i_, w, L) != 0) return false;
        i_ += L;
        return true;
    }

    bool value(JsonValue &v, int depth) {
        if (depth > kMaxDepth) { why_ = "nesting too deep"; return false; }
        if (i_ >= n_) { why_ = "unexpected end"; return false; }
        char c = s_[i_];
        switch (c) {
            case '{': return object(v, depth);
            case '[': return array(v, depth);
            case '"': v.type_ = JsonValue::Type::String; return string(v.str_);
            case 't': if (lit("true"))  { v.type_ = JsonValue::Type::Bool; v.b_ = true;  return true; } break;
            case 'f': if (lit("false")) { v.type_ = JsonValue::Type::Bool; v.b_ = false; return true; } break;
            case 'n': if (lit("null"))  { v.type_ = JsonValue::Type::Null; return true; } break;
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return number(v);
                break;
        }
        why_ = "unexpected character";
        return false;
    }

    bool number(JsonValue &v) {
        size_t start = i_;
        if (s_[i_] == '-') ++i_;
        while (i_ < n_ && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' ||
                           s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-'))
            ++i_;
        char buf[64];
        size_t len = i_ - start;
        if (len == 0 || len >= sizeof(buf)) { why_ = "bad number"; return false; }
        memcpy(buf, s_ + start, len);
        buf[len] = '\0';
        char *end = nullptr;
        double d = strtod(buf, &end);
        if (!end || *end != '\0') { why_ = "bad number"; i_ = start; return false; }
        v.type_ = JsonValue::Type::Number;
        v.num_ = d;
        return true;
    }

    static void put_utf8(std::string &out, uint32_t cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(uint32_t &cp) {
        if (i_ + 4 > n_) return false;
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            char h = s_[i_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= (uint32_t)(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= (uint32_t)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= (uint32_t)(h - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string &out) {
        ++i_;  // opening quote
        out.clear();
        while (i_ < n_) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }   // lenient: raw bytes / ctrl chars pass
            if (i_ >= n_) break;
            char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) { why_ = "bad \\u escape"; return false; }
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= n_ &&
                        s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        size_t save = i_;
                        i_ += 2;
                        uint32_t lo;
                        if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            i_ = save;
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: out += e; break;   // lenient: unknown escape -> literal
            }
        }
        why_ = "unterminated string";
        return false;
    }

    bool array(JsonValue &v, int depth) {
        ++i_;
        v.type_ = JsonValue::Type::Array;
        skip_ws();
        if (i_ < n_ && s_[i_] == ']') { ++i_; return true; }
        while (true) {
            JsonValue item;
            skip_ws();
            if (!value(item, depth + 1)) return false;
            v.arr_.push_back(std::move(item));
            skip_ws();
            if (i_ >= n_) { why_ = "unterminated array"; return false; }
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return true; }
            why_ = "expected , or ]";
            return false;
        }
    }

    bool object(JsonValue &v, int depth) {
        ++i_;
        v.type_ = JsonValue::Type::Object;
        skip_ws();
        if (i_ < n_ && s_[i_] == '}') { ++i_; return true; }
        while (true) {
            skip_ws();
            if (i_ >= n_ || s_[i_] != '"') { why_ = "expected key"; return false; }
            std::string key;
            if (!string(key)) return false;
            skip_ws();
            if (i_ >= n_ || s_[i_] != ':') { why_ = "expected :"; return false; }
            ++i_;
            skip_ws();
            JsonValue member;
            if (!value(member, depth + 1)) return false;
            v.obj_.emplace_back(std::move(key), std::move(member));
            skip_ws();
            if (i_ >= n_) { why_ = "unterminated object"; return false; }
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return true; }
            why_ = "expected , or }";
            return false;
        }
    }
};

bool json_parse(const char *data, size_t len, JsonValue &out, std::string *err) {
    out = JsonValue();
    if (!data) { if (err) *err = "null input"; return false; }
    JsonValue tmp;
    JsonParser p(data, len);
    if (!p.parse(tmp, err)) return false;
    out = std::move(tmp);
    return true;
}

bool json_parse(const std::string &text, JsonValue &out, std::string *err) {
    return json_parse(text.data(), text.size(), out, err);
}

std::string json_escape(const std::string &s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

}  // namespace mm
