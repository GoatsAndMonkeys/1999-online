#include "settings.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mm {

// ── store ───────────────────────────────────────────────────────────────────

namespace store {
namespace {

bool g_enabled = true;

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

// mkdir -p; true if the folder exists afterwards.
bool make_dirs(const std::string& dir) {
    if (dir.empty()) return true;
    struct stat st {};
    if (::stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    const size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && !make_dirs(dir.substr(0, slash))) return false;
    if (::mkdir(dir.c_str(), 0700) == 0 || errno == EEXIST) return true;
    return false;
}

} // namespace

void set_enabled(bool on) { g_enabled = on; }
bool enabled() { return g_enabled; }

std::string dir() {
    std::string d;
    if (const char* v = env("PC1999_CONFIG_DIR")) {
        d = v;
    } else if (const char* x = env("XDG_CONFIG_HOME")) {
        d = std::string(x) + "/1999online";
    } else if (const char* h = env("HOME")) {
        d = std::string(h) + "/.config/1999online";
    } else {
        d = "./.1999online";
    }
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    return d;
}

std::string path(const std::string& file) {
    return dir() + "/" + file;
}

bool read_text(const std::string& path, std::string& out) {
    out.clear();
    if (!g_enabled) return false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
}

bool write_atomic(const std::string& path, const std::string& text) {
    if (!g_enabled || path.empty()) return false;
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !make_dirs(path.substr(0, slash))) return false;

    const std::string tmp = path + ".tmp." + std::to_string(static_cast<long>(::getpid()));
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    bool ok = true;
    size_t off = 0;
    while (off < text.size()) {
        const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        off += static_cast<size_t>(n);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (ok && ::rename(tmp.c_str(), path.c_str()) != 0) ok = false;
    if (!ok) ::unlink(tmp.c_str());
    return ok;
}

bool remove_file(const std::string& path) {
    if (!g_enabled) return false;
    return ::unlink(path.c_str()) == 0;
}

Load load_json(const std::string& path, JsonValue& out) {
    out = JsonValue();
    std::string text;
    if (!read_text(path, text)) return Load::Missing;
    if (json_parse(text, out) && out.is_object()) return Load::Ok;
    out = JsonValue();
    ::rename(path.c_str(), (path + ".corrupt").c_str());
    return Load::Corrupt;
}

std::string quote(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}

} // namespace store

// ── Settings ────────────────────────────────────────────────────────────────

Settings& settings() {
    static Settings s;
    return s;
}

Settings::Settings(std::string path) : fixed_path_(std::move(path)) {
    reload();
}

Settings::~Settings() {
    flush();
}

const std::string& Settings::file() const {
    path_ = fixed_path_.empty() ? store::path("settings.json") : fixed_path_;
    return path_;
}

store::Load Settings::reload() {
    values_.clear();
    dirty_ = false;
    armed_ = false;
    JsonValue doc;
    const store::Load r = store::load_json(file(), doc);
    if (r != store::Load::Ok) return r;
    for (const auto& kv : doc.members()) {
        Value v;
        if (kv.second.is_string()) {
            v.kind = Value::Kind::String;
            v.s = kv.second.str();
        } else if (kv.second.is_number()) {
            v.kind = Value::Kind::Number;
            v.n = kv.second.as_double();
        } else if (kv.second.is_bool()) {
            v.kind = Value::Kind::Bool;
            v.b = kv.second.as_bool();
        } else {
            continue;   // null / array / object: not ours
        }
        values_[kv.first] = std::move(v);
    }
    return r;
}

bool Settings::has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

std::string Settings::get_string(const std::string& key, const std::string& def) const {
    const auto it = values_.find(key);
    return (it != values_.end() && it->second.kind == Value::Kind::String) ? it->second.s : def;
}

int64_t Settings::get_int(const std::string& key, int64_t def) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return def;
    if (it->second.kind == Value::Kind::Number) return static_cast<int64_t>(std::llround(it->second.n));
    if (it->second.kind == Value::Kind::Bool) return it->second.b ? 1 : 0;
    return def;
}

bool Settings::get_bool(const std::string& key, bool def) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return def;
    if (it->second.kind == Value::Kind::Bool) return it->second.b;
    if (it->second.kind == Value::Kind::Number) return it->second.n != 0.0;
    return def;
}

void Settings::put(const std::string& key, Value v) {
    if (key.empty()) return;
    const auto it = values_.find(key);
    if (it != values_.end() && it->second == v) return;
    values_[key] = std::move(v);
    dirty_ = true;
}

void Settings::set_string(const std::string& key, const std::string& value) {
    Value v;
    v.kind = Value::Kind::String;
    v.s = value;
    put(key, std::move(v));
}

void Settings::set_int(const std::string& key, int64_t value) {
    Value v;
    v.kind = Value::Kind::Number;
    v.n = static_cast<double>(value);
    put(key, std::move(v));
}

void Settings::set_bool(const std::string& key, bool value) {
    Value v;
    v.kind = Value::Kind::Bool;
    v.b = value;
    put(key, std::move(v));
}

void Settings::remove(const std::string& key) {
    if (values_.erase(key) > 0) dirty_ = true;
}

std::string Settings::to_json() const {
    std::string o = "{";
    bool first = true;
    for (const auto& kv : values_) {
        o += first ? "\n  " : ",\n  ";
        first = false;
        o += store::quote(kv.first) + ": ";
        const Value& v = kv.second;
        if (v.kind == Value::Kind::String) {
            o += store::quote(v.s);
        } else if (v.kind == Value::Kind::Bool) {
            o += v.b ? "true" : "false";
        } else {
            char buf[40];
            if (std::isfinite(v.n) && v.n == std::floor(v.n) && std::fabs(v.n) < 9.0e15) {
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.n));
            } else if (std::isfinite(v.n)) {
                std::snprintf(buf, sizeof(buf), "%.17g", v.n);
            } else {
                std::snprintf(buf, sizeof(buf), "0");
            }
            o += buf;
        }
    }
    o += first ? "}\n" : "\n}\n";
    return o;
}

void Settings::tick(uint32_t now_ms) {
    if (!dirty_) {
        armed_ = false;
        return;
    }
    if (!armed_) {
        armed_ = true;
        dirty_since_ms_ = now_ms;
        return;
    }
    if (now_ms - dirty_since_ms_ >= kFlushMs) flush();
}

bool Settings::flush() {
    if (!dirty_) return true;
    // Disabled (shots): the values stay in memory only; nothing to retry.
    armed_ = false;
    if (!store::enabled()) {
        dirty_ = false;
        return false;
    }
    // On failure stay dirty: tick() re-arms and tries again a second later.
    if (!store::write_atomic(file(), to_json())) return false;
    dirty_ = false;
    ++writes_;
    return true;
}

} // namespace mm
