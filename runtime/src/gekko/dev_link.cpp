#include "gekko/dev_link.h"
#include "gekko/thread_name.h"  // ES-T1

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

// The development link (gekko/dev_link.h; docs/comparator/README.md "The development link").

namespace gekko::dev {

// ---- requests ------------------------------------------------------------------------------------------------------

std::string Request::text(const char* key, const std::string& fallback) const {
    const auto it = strings.find(key);
    return it != strings.end() ? it->second : fallback;
}

double Request::number(const char* key, double fallback) const {
    const auto it = numbers.find(key);
    return it != numbers.end() ? it->second : fallback;
}

std::uint64_t Request::unsigned_number(const char* key, std::uint64_t fallback) const {
    const auto it = numbers.find(key);
    if (it == numbers.end() || !(it->second >= 0.0) || !std::isfinite(it->second)) return fallback;
    return static_cast<std::uint64_t>(it->second);
}

bool Request::flag(const char* key, bool fallback) const {
    const auto it = numbers.find(key);
    return it != numbers.end() ? it->second != 0.0 : fallback;
}

bool Request::has(const char* key) const {
    return strings.count(key) != 0u || numbers.count(key) != 0u;
}

namespace {

struct Cursor {
    const char* p;
    const char* end;
    void skip() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }
};

void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80u) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (code >> 6)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    } else if (code < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (code >> 12)));
        out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (code >> 18)));
        out.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    }
}

bool hex4(Cursor& c, std::uint32_t& value) {
    if (c.end - c.p < 4) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) {
        const char h = *c.p++;
        value <<= 4;
        if (h >= '0' && h <= '9') value |= static_cast<std::uint32_t>(h - '0');
        else if (h >= 'a' && h <= 'f') value |= static_cast<std::uint32_t>(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') value |= static_cast<std::uint32_t>(h - 'A' + 10);
        else return false;
    }
    return true;
}

bool parse_string(Cursor& c, std::string& out) {
    if (c.p >= c.end || *c.p != '"') return false;
    ++c.p;
    out.clear();
    while (c.p < c.end) {
        const char ch = *c.p++;
        if (ch == '"') return true;
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        if (c.p >= c.end) return false;
        const char e = *c.p++;
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                std::uint32_t code = 0;
                if (!hex4(c, code)) return false;
                if (code >= 0xD800u && code <= 0xDBFFu) {
                    std::uint32_t low = 0;
                    if (c.end - c.p < 6 || c.p[0] != '\\' || c.p[1] != 'u') return false;
                    c.p += 2;
                    if (!hex4(c, low) || low < 0xDC00u || low > 0xDFFFu) return false;
                    code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                }
                append_utf8(out, code);
                break;
            }
            default:
                return false;
        }
    }
    return false;
}

}  // namespace

bool parse_request(const std::string& line, Request* out, std::string* error) {
    const auto fail = [error](const char* why) {
        if (error != nullptr) *error = why;
        return false;
    };
    Request request;
    Cursor c{line.data(), line.data() + line.size()};
    c.skip();
    if (c.p >= c.end || *c.p != '{') return fail("not a JSON object");
    ++c.p;
    c.skip();
    if (c.p < c.end && *c.p == '}') return fail("empty object");
    for (;;) {
        c.skip();
        std::string key;
        if (!parse_string(c, key)) return fail("bad key");
        c.skip();
        if (c.p >= c.end || *c.p != ':') return fail("missing ':'");
        ++c.p;
        c.skip();
        if (c.p >= c.end) return fail("missing value");
        if (*c.p == '"') {
            std::string value;
            if (!parse_string(c, value)) return fail("bad string");
            request.strings[key] = std::move(value);
        } else if (*c.p == 't' && c.end - c.p >= 4 && std::memcmp(c.p, "true", 4) == 0) {
            c.p += 4;
            request.numbers[key] = 1.0;
        } else if (*c.p == 'f' && c.end - c.p >= 5 && std::memcmp(c.p, "false", 5) == 0) {
            c.p += 5;
            request.numbers[key] = 0.0;
        } else if (*c.p == 'n' && c.end - c.p >= 4 && std::memcmp(c.p, "null", 4) == 0) {
            c.p += 4;
        } else if (*c.p == '-' || (*c.p >= '0' && *c.p <= '9')) {
            const std::string number(c.p, static_cast<std::size_t>(c.end - c.p));
            char* stop = nullptr;
            const double value = std::strtod(number.c_str(), &stop);
            if (stop == number.c_str()) return fail("bad number");
            c.p += stop - number.c_str();
            request.numbers[key] = value;
        } else {
            return fail("unsupported value (nested values are not part of the protocol)");
        }
        c.skip();
        if (c.p < c.end && *c.p == ',') {
            ++c.p;
            continue;
        }
        if (c.p < c.end && *c.p == '}') {
            ++c.p;
            break;
        }
        return fail("missing ',' or '}'");
    }
    const auto cmd = request.strings.find("cmd");
    if (cmd == request.strings.end() || cmd->second.empty()) return fail("no \"cmd\"");
    request.cmd = cmd->second;
    request.id = request.unsigned_number("id", 0);
    *out = std::move(request);
    return true;
}

// ---- writing -------------------------------------------------------------------------------------------------------

std::string quote(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char ch : text) {
        const unsigned char u = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u < 0x20u) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", u);
                    out += escaped;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
    return out;
}

void Json::key(const char* name) {
    if (!body_.empty()) body_.push_back(',');
    body_ += quote(name);
    body_.push_back(':');
}

Json& Json::field(const char* name, const std::string& value) {
    key(name);
    body_ += quote(value);
    return *this;
}

Json& Json::field(const char* name, const char* value) {
    return field(name, std::string(value != nullptr ? value : ""));
}

Json& Json::field(const char* name, std::uint64_t value) {
    key(name);
    body_ += std::to_string(value);
    return *this;
}

Json& Json::field(const char* name, std::int64_t value) {
    key(name);
    body_ += std::to_string(value);
    return *this;
}

Json& Json::field(const char* name, double value) {
    key(name);
    if (!std::isfinite(value)) {
        body_ += "null";
    } else {
        char text[40];
        std::snprintf(text, sizeof(text), "%.9g", value);
        body_ += text;
    }
    return *this;
}

Json& Json::field(const char* name, bool value) {
    key(name);
    body_ += value ? "true" : "false";
    return *this;
}

Json& Json::null_field(const char* name) {
    key(name);
    body_ += "null";
    return *this;
}

Json& Json::raw(const char* name, const std::string& json) {
    key(name);
    body_ += json.empty() ? std::string("null") : json;
    return *this;
}

std::string Json::str() const {
    return "{" + body_ + "}";
}

#if !defined(GEKKO_SHIPPING)  // ES: the socket half is not part of the user's exe
// ---- the link ------------------------------------------------------------------------------------------------------

Link::~Link() {
    stop();
}

#ifdef _WIN32

bool Link::start(const std::string& endpoint, const Json& hello, std::string* error) {
    const auto fail = [error](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const std::size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos) return fail("--dev-link expects 127.0.0.1:<port>");
    const std::string host = endpoint.substr(0, colon);
    const unsigned long port = std::strtoul(endpoint.c_str() + colon + 1, nullptr, 10);
    if (host != "127.0.0.1" && host != "localhost") return fail("--dev-link connects to the loopback address only");
    if (port == 0ul || port > 65535ul) return fail("--dev-link: bad port");

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return fail("WSAStartup failed");
    winsock_ = true;
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return fail("socket() failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::closesocket(s);
        return fail("could not connect to the comparison workstation at " + endpoint);
    }
    // Small lines, answered at once: no Nagle delay.
    BOOL no_delay = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
    socket_ = static_cast<std::uintptr_t>(s);
    connected_.store(true, std::memory_order_release);
    event("hello", hello);
    thread_ = std::thread([this] { reader(); });
    return true;
}

void Link::stop() {
    stopping_.store(true, std::memory_order_release);
    if (socket_ != ~std::uintptr_t{0}) {
        ::shutdown(static_cast<SOCKET>(socket_), SD_BOTH);
        ::closesocket(static_cast<SOCKET>(socket_));
        socket_ = ~std::uintptr_t{0};
    }
    if (thread_.joinable()) thread_.join();
    connected_.store(false, std::memory_order_release);
    queue_ready_.notify_all();
    if (winsock_) {
        WSACleanup();
        winsock_ = false;
    }
}

void Link::send_line(const std::string& line) {
    if (!connected()) return;
    std::lock_guard<std::mutex> lock(send_mutex_);
    const std::string framed = line + "\n";
    const char* p = framed.data();
    std::size_t left = framed.size();
    while (left != 0u) {
        const int sent = ::send(static_cast<SOCKET>(socket_), p, static_cast<int>(left), 0);
        if (sent <= 0) {
            connected_.store(false, std::memory_order_release);
            lost_.store(true, std::memory_order_release);
            return;
        }
        p += sent;
        left -= static_cast<std::size_t>(sent);
    }
}

void Link::reader() {
    gekko::name_this_thread("dev-link-reader");  // ES-T1
    std::string pending;
    char buffer[4096];
    for (;;) {
        const int got = ::recv(static_cast<SOCKET>(socket_), buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        pending.append(buffer, static_cast<std::size_t>(got));
        for (std::size_t newline = pending.find('\n'); newline != std::string::npos;
             newline = pending.find('\n')) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            Request request;
            std::string why;
            if (!parse_request(line, &request, &why)) {
                // The id may still be readable; a bad line is answered rather than dropped.
                Json body;
                body.field("line", line.size() > 200 ? line.substr(0, 200) : line);
                reply(0, false, body, "unparsable request: " + why);
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                queue_.push_back(std::move(request));
            }
            queue_ready_.notify_all();
            if (on_request_ != nullptr) on_request_(on_request_context_);
        }
    }
    connected_.store(false, std::memory_order_release);
    if (!stopping_.load(std::memory_order_acquire)) lost_.store(true, std::memory_order_release);
    queue_ready_.notify_all();
    if (on_request_ != nullptr) on_request_(on_request_context_);
}

#else

bool Link::start(const std::string&, const Json&, std::string* error) {
    if (error != nullptr) *error = "the development link needs Windows sockets";
    return false;
}
void Link::stop() {}
void Link::send_line(const std::string&) {}
void Link::reader() {}

#endif

bool Link::take(Request* out) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.empty()) return false;
    *out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

bool Link::pending() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return !queue_.empty();
}

bool Link::wait(Request* out, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_ready_.wait_for(lock, timeout, [this] { return !queue_.empty() || !connected(); });
    if (queue_.empty()) return false;
    *out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void Link::reply(std::uint64_t id, bool ok, const Json& body, const std::string& error) {
    Json message;
    message.field("id", id).field("ok", ok);
    if (!error.empty()) message.field("error", error);
    std::string line = message.str();
    if (!body.empty()) {
        // Splice the body's fields into the answer object.
        line.pop_back();
        line += ",";
        const std::string fields = body.str();
        line += fields.substr(1, fields.size() - 2);
        line += "}";
    }
    send_line(line);
}

void Link::event(const char* name, const Json& body) {
    Json message;
    message.field("event", name);
    std::string line = message.str();
    if (!body.empty()) {
        line.pop_back();
        line += ",";
        const std::string fields = body.str();
        line += fields.substr(1, fields.size() - 2);
        line += "}";
    }
    send_line(line);
}

#endif  // !GEKKO_SHIPPING
}  // namespace gekko::dev
