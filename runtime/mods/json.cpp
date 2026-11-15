// JSON reader/writer (shadow/mods/json.h).
#include "shadow/mods/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace shadow::mods::json {

const Value* Value::find(std::string_view key) const noexcept {
    if (kind_ != Kind::Object) return nullptr;
    for (auto it = members_.rbegin(); it != members_.rend(); ++it)
        if (it->first == key) return &it->second;
    return nullptr;
}

void Value::set(std::string key, Value v) {
    if (kind_ != Kind::Object) {
        *this = object();
    }
    for (auto& m : members_)
        if (m.first == key) {
            m.second = std::move(v);
            return;
        }
    members_.emplace_back(std::move(key), std::move(v));
}

namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view t) : text_(t) {}

    bool run(Value& out, std::string& error) {
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB && static_cast<unsigned char>(text_[2]) == 0xBF)
            pos_ = 3;
        skip_ws();
        if (!value(out, 0)) {
            error = where() + why_;
            return false;
        }
        skip_ws();
        if (pos_ != text_.size()) {
            error = where() + "unexpected content after the JSON value";
            return false;
        }
        return true;
    }

private:
    std::string where() const {
        int line = 1, column = 1;
        for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        return "line " + std::to_string(line) + " column " + std::to_string(column) + ": ";
    }

    bool fail(const char* why) {
        why_ = why;
        return false;
    }

    void skip_ws() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return fail("invalid literal");
        pos_ += word.size();
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > kMaxDepth) return fail("nesting deeper than 64");
        if (pos_ >= text_.size()) return fail("unexpected end of input");
        const char c = text_[pos_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            std::string s;
            if (!string(s)) return false;
            out = Value::string(std::move(s));
            return true;
        }
        if (c == 't') {
            if (!literal("true")) return false;
            out = Value::boolean(true);
            return true;
        }
        if (c == 'f') {
            if (!literal("false")) return false;
            out = Value::boolean(false);
            return true;
        }
        if (c == 'n') {
            if (!literal("null")) return false;
            out = Value{};
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return fail("unexpected character");
    }

    bool object(Value& out, int depth) {
        out = Value::object();
        ++pos_;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected a member name");
            std::string key;
            if (!string(key)) return false;
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') return fail("expected ':'");
            ++pos_;
            skip_ws();
            Value v;
            if (!value(v, depth + 1)) return false;
            out.set(std::move(key), std::move(v));  // a duplicate key replaces the earlier value
            skip_ws();
            if (pos_ >= text_.size()) return fail("unterminated object");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool array(Value& out, int depth) {
        out = Value::array();
        ++pos_;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_ws();
            Value v;
            if (!value(v, depth + 1)) return false;
            out.push(std::move(v));
            skip_ws();
            if (pos_ >= text_.size()) return fail("unterminated array");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    static void put_utf8(std::string& s, std::uint32_t cp) {
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(std::uint32_t& v) {
        if (pos_ + 4 > text_.size()) return fail("truncated \\u escape");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= static_cast<std::uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') v |= static_cast<std::uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= static_cast<std::uint32_t>(h - 'A' + 10);
            else return fail("invalid \\u escape");
        }
        return true;
    }

    bool string(std::string& s) {
        ++pos_;  // opening quote
        for (;;) {
            if (pos_ >= text_.size()) return fail("unterminated string");
            const auto c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return fail("control character in a string");
            if (c != '\\') {
                s.push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= text_.size()) return fail("unterminated escape");
            const char e = text_[pos_++];
            switch (e) {
                case '"': s.push_back('"'); break;
                case '\\': s.push_back('\\'); break;
                case '/': s.push_back('/'); break;
                case 'b': s.push_back('\b'); break;
                case 'f': s.push_back('\f'); break;
                case 'n': s.push_back('\n'); break;
                case 'r': s.push_back('\r'); break;
                case 't': s.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        std::uint32_t low = 0;
                        if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u')
                            return fail("unpaired surrogate");
                        pos_ += 2;
                        if (!hex4(low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) return fail("unpaired surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unpaired surrogate");
                    }
                    put_utf8(s, cp);
                    break;
                }
                default: return fail("invalid escape");
            }
        }
    }

    bool number(Value& out) {
        const std::size_t start = pos_;
        if (text_[pos_] == '-') ++pos_;
        if (pos_ >= text_.size()) return fail("truncated number");
        if (text_[pos_] == '0') {
            ++pos_;
        } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        } else {
            return fail("invalid number");
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') return fail("invalid fraction");
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') return fail("invalid exponent");
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        const std::string digits(text_.substr(start, pos_ - start));
        out = Value::number(std::strtod(digits.c_str(), nullptr));
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    const char* why_ = "";
};

void escape(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

void emit(std::string& out, const Value& v, int indent) {
    const std::string pad(static_cast<std::size_t>(indent) * 2u, ' ');
    const std::string inner(static_cast<std::size_t>(indent + 1) * 2u, ' ');
    switch (v.kind()) {
        case Value::Kind::Null: out += "null"; break;
        case Value::Kind::Bool: out += v.as_bool() ? "true" : "false"; break;
        case Value::Kind::Number: {
            const double d = v.as_number();
            char buf[40];
            if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9.0e15)
                std::snprintf(buf, sizeof(buf), "%.0f", d);
            else if (std::isfinite(d))
                std::snprintf(buf, sizeof(buf), "%.17g", d);
            else
                std::snprintf(buf, sizeof(buf), "null");
            out += buf;
            break;
        }
        case Value::Kind::String: escape(out, v.as_string()); break;
        case Value::Kind::Array:
            if (v.items().empty()) {
                out += "[]";
                break;
            }
            out += "[\n";
            for (std::size_t i = 0; i < v.items().size(); ++i) {
                out += inner;
                emit(out, v.items()[i], indent + 1);
                out += i + 1 < v.items().size() ? ",\n" : "\n";
            }
            out += pad + "]";
            break;
        case Value::Kind::Object:
            if (v.members().empty()) {
                out += "{}";
                break;
            }
            out += "{\n";
            for (std::size_t i = 0; i < v.members().size(); ++i) {
                out += inner;
                escape(out, v.members()[i].first);
                out += ": ";
                emit(out, v.members()[i].second, indent + 1);
                out += i + 1 < v.members().size() ? ",\n" : "\n";
            }
            out += pad + "}";
            break;
    }
}

}  // namespace

bool parse(std::string_view text, Value& out, std::string& error) {
    out = Value{};
    error.clear();
    Parser p(text);
    return p.run(out, error);
}

std::string write(const Value& value) {
    std::string out;
    emit(out, value, 0);
    out.push_back('\n');
    return out;
}

}  // namespace shadow::mods::json
