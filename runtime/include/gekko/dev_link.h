#pragma once

// The development link: a line-oriented JSON channel between a running game and the before / after comparison
// workstation (tools/comparator, docs/comparator/README.md).
//
// The workstation listens on a loopback TCP port and starts the game with `--dev-link 127.0.0.1:<port>`; the game
// connects as the client (it never listens), says who it is, and from then on answers requests: pause, resume, run a
// number of frames, capture a frame, save or load its own savestate, report its state. Every request is one JSON
// object on one line ({"id": 7, "cmd": "pause"}), every answer one line with the same id ({"id": 7, "ok": true,
// ...}); events carry "event" instead of "id".
//
// It is inert unless --dev-link names an endpoint. It is development tooling that talks about the running program;
// what each build reports is its own state in its own terms - the workstation, not the game, knows that two builds
// differ (docs/comparator/README.md "Architecture").

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace gekko::dev {

// One request: a flat JSON object (string, number, true / false / null values; nothing nested).
struct Request {
    std::uint64_t id = 0;
    std::string cmd;
    std::map<std::string, std::string> strings;
    std::map<std::string, double> numbers;  // numbers, and true / false as 1 / 0

    std::string text(const char* key, const std::string& fallback = std::string()) const;
    double number(const char* key, double fallback = 0.0) const;
    std::uint64_t unsigned_number(const char* key, std::uint64_t fallback = 0) const;
    bool flag(const char* key, bool fallback = false) const;
    bool has(const char* key) const;
};

// Parses one line. False (with `error`) for anything but a flat object with a "cmd" string.
bool parse_request(const std::string& line, Request* out, std::string* error);

// One JSON object as it is written (an answer, an event, a report).
class Json {
public:
    Json& field(const char* key, const std::string& value);
    Json& field(const char* key, const char* value);
    Json& field(const char* key, std::uint64_t value);
    Json& field(const char* key, std::int64_t value);
    Json& field(const char* key, std::uint32_t value) { return field(key, static_cast<std::uint64_t>(value)); }
    Json& field(const char* key, int value) { return field(key, static_cast<std::int64_t>(value)); }
    Json& field(const char* key, double value);
    Json& field(const char* key, bool value);
    Json& null_field(const char* key);
    // A value that is already JSON (an object or an array).
    Json& raw(const char* key, const std::string& json);
    std::string str() const;
    bool empty() const noexcept { return body_.empty(); }

private:
    void key(const char* name);
    std::string body_;
};

// The string as a JSON string literal (quotes and escapes; the text is UTF-8).
std::string quote(const std::string& text);

#if defined(GEKKO_SHIPPING)
// GEKKO_SHIPPING (ES 2026-10-20): the development link does not exist in the user's exe. This Link never
// connects, so every connected() / take() / wait() branch in the caller folds away; --dev-link is refused at parse.
class Link {
public:
    bool start(const std::string&, const Json&, std::string* error) {
        if (error != nullptr) *error = "the development link is not part of this build";
        return false;
    }
    void stop() {}
    constexpr bool connected() const noexcept { return false; }
    constexpr bool lost() const noexcept { return false; }
    bool take(Request*) { return false; }
    bool pending() const { return false; }
    bool wait(Request*, std::chrono::milliseconds) { return false; }
    void set_on_request(void (*)(void*), void*) {}
    void reply(std::uint64_t, bool, const Json&, const std::string& = std::string()) {}
    void event(const char*, const Json&) {}
};
#else
class Link {
public:
    Link() = default;
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    // Connects to `endpoint` ("127.0.0.1:<port>" or "localhost:<port>" - loopback only) and starts the reader
    // thread. `hello` is sent first, as the "hello" event's body.
    bool start(const std::string& endpoint, const Json& hello, std::string* error);
    void stop();
    bool connected() const noexcept { return connected_.load(std::memory_order_acquire); }
    // True once the workstation has gone (the socket closed after it was connected).
    bool lost() const noexcept { return lost_.load(std::memory_order_acquire); }

    // Requests received and not yet taken, oldest first (any thread).
    bool take(Request* out);
    bool pending() const;
    // Waits up to `timeout` for a request.
    bool wait(Request* out, std::chrono::milliseconds timeout);
    // Called (on the reader thread) whenever a request is queued - the game asks for a boundary there.
    void set_on_request(void (*callback)(void*), void* context) {
        on_request_ = callback;
        on_request_context_ = context;
    }

    // Thread-safe.
    void reply(std::uint64_t id, bool ok, const Json& body, const std::string& error = std::string());
    void event(const char* name, const Json& body);

private:
    void send_line(const std::string& line);
    void reader();

    std::uintptr_t socket_ = ~std::uintptr_t{0};
    std::thread thread_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> lost_{false};
    std::atomic<bool> stopping_{false};
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_ready_;
    std::deque<Request> queue_;
    std::mutex send_mutex_;
    void (*on_request_)(void*) = nullptr;
    void* on_request_context_ = nullptr;
    bool winsock_ = false;
};
#endif  // GEKKO_SHIPPING

}  // namespace gekko::dev
