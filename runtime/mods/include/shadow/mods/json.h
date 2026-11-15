// A small strict JSON reader/writer for the mod loader's manifests (mod.json) and state file (mods.json).
//
// RFC 8259 grammar: objects, arrays, strings (UTF-8; \u escapes, surrogate pairs joined), numbers (kept as double and
// as their source text), true/false/null. Object member order is kept, so a file the manager rewrites keeps the
// author's order. Duplicate keys: the last one wins on lookup (find returns the last). Depth is limited (64) so a
// hostile file cannot exhaust the stack. No comments, no trailing commas: a manifest that other tools read must be
// plain JSON.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shadow::mods::json {

class Value {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Value() = default;
    static Value boolean(bool b) { Value v; v.kind_ = Kind::Bool; v.bool_ = b; return v; }
    static Value number(double d) { Value v; v.kind_ = Kind::Number; v.number_ = d; return v; }
    static Value string(std::string s) { Value v; v.kind_ = Kind::String; v.string_ = std::move(s); return v; }
    static Value array() { Value v; v.kind_ = Kind::Array; return v; }
    static Value object() { Value v; v.kind_ = Kind::Object; return v; }

    Kind kind() const noexcept { return kind_; }
    bool is_null() const noexcept { return kind_ == Kind::Null; }
    bool is_bool() const noexcept { return kind_ == Kind::Bool; }
    bool is_number() const noexcept { return kind_ == Kind::Number; }
    bool is_string() const noexcept { return kind_ == Kind::String; }
    bool is_array() const noexcept { return kind_ == Kind::Array; }
    bool is_object() const noexcept { return kind_ == Kind::Object; }

    bool as_bool(bool fallback = false) const noexcept { return kind_ == Kind::Bool ? bool_ : fallback; }
    double as_number(double fallback = 0.0) const noexcept { return kind_ == Kind::Number ? number_ : fallback; }
    const std::string& as_string() const noexcept { return string_; }  // empty unless a string

    // Arrays.
    const std::vector<Value>& items() const noexcept { return items_; }
    void push(Value v) { items_.push_back(std::move(v)); }

    // Objects.
    const std::vector<std::pair<std::string, Value>>& members() const noexcept { return members_; }
    const Value* find(std::string_view key) const noexcept;
    void set(std::string key, Value v);  // replaces an existing member in place, else appends

private:
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Value> items_;
    std::vector<std::pair<std::string, Value>> members_;
};

// Parses `text`; false with `error` ("line L column C: why") on any syntax error or trailing content.
// A leading UTF-8 byte order mark is accepted (Notepad writes one).
bool parse(std::string_view text, Value& out, std::string& error);

// Pretty-printed with 2-space indentation and "\n" line ends; strings escaped per RFC 8259 (control characters,
// quote and backslash; other UTF-8 passes through). Integral numbers print without a fraction.
std::string write(const Value& value);

}  // namespace shadow::mods::json
