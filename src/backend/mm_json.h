// mm_json.h — tiny self-contained JSON DOM parser for the mmd IPC lines.
//
// Enough for what the daemon emits: objects, arrays, strings (with escapes and
// \uXXXX -> UTF-8), numbers, true/false/null. Lenient on purpose: the daemon
// builds JSON with snprintf and only escapes '"' and '\\', so raw control
// characters / high bytes inside strings are accepted verbatim.
//
// No exceptions, no external deps. Plain C++17.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mm {

class JsonValue {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;

    Type type() const { return type_; }
    bool is_null()   const { return type_ == Type::Null; }
    bool is_bool()   const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array()  const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    // Object member lookup; returns a shared Null value when absent / not an object.
    const JsonValue &operator[](const char *key) const;
    bool has(const char *key) const;
    // Array element; Null when out of range / not an array.
    const JsonValue &operator[](size_t idx) const;
    // int overload so `v[0]` isn't ambiguous with the const char* lookup.
    const JsonValue &operator[](int idx) const {
        return (*this)[idx < 0 ? (size_t)-1 : (size_t)idx];
    }
    size_t size() const;   // array/object element count, else 0

    const std::vector<JsonValue> &items() const { return arr_; }
    const std::vector<std::pair<std::string, JsonValue>> &members() const { return obj_; }

    // Lenient accessors: numbers-as-bools ("active":1) and bools-as-numbers work.
    double      as_double(double def = 0.0) const;
    int64_t     as_int(int64_t def = 0) const;
    uint32_t    as_u32(uint32_t def = 0) const;
    bool        as_bool(bool def = false) const;
    std::string as_string(const std::string &def = std::string()) const;
    const std::string &str() const { return str_; }   // raw string payload ("" if not a string)

    // Convenience: obj.get_int("level") etc.
    int64_t     get_int(const char *key, int64_t def = 0) const { return (*this)[key].as_int(def); }
    uint32_t    get_u32(const char *key, uint32_t def = 0) const { return (*this)[key].as_u32(def); }
    bool        get_bool(const char *key, bool def = false) const { return (*this)[key].as_bool(def); }
    std::string get_string(const char *key, const std::string &def = std::string()) const {
        return (*this)[key].as_string(def);
    }

private:
    friend class JsonParser;
    Type type_ = Type::Null;
    bool b_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<JsonValue> arr_;
    std::vector<std::pair<std::string, JsonValue>> obj_;
};

// Parse one JSON document. Returns false on a syntax error (out is then Null).
// `err` (optional) receives a short reason with the byte offset.
bool json_parse(const std::string &text, JsonValue &out, std::string *err = nullptr);
bool json_parse(const char *data, size_t len, JsonValue &out, std::string *err = nullptr);

// Escape a string for embedding in an outgoing JSON command (adds no quotes).
std::string json_escape(const std::string &s);

}  // namespace mm
