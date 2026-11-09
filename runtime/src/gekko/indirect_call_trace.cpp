// Indirect-call trace (see gekko/indirect_call_trace.h): observation only.
#include "gekko/indirect_call_trace.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

namespace gekko::indirect_call_trace {
namespace {

struct Trace {
    std::string path;
    std::mutex lock;
    std::unordered_map<std::uint64_t, std::uint64_t> pairs;   // (site << 32 | target) -> calls

    Trace() {
        if (const char* p = std::getenv("SHADOW_INDIRECT_TRACE"); p != nullptr && *p != '\0') path = p;
    }
    ~Trace() { write(); }

    void write() {
        if (path.empty()) return;
        std::lock_guard<std::mutex> hold(lock);
        if (std::FILE* f = std::fopen(path.c_str(), "w")) {
            std::fprintf(f, "# site target calls (SHADOW_INDIRECT_TRACE, linked indirect calls)\n");
            for (const auto& [key, n] : pairs) {
                std::fprintf(f, "%08X %08X %llu\n", static_cast<unsigned>(key >> 32), static_cast<unsigned>(key),
                             static_cast<unsigned long long>(n));
            }
            std::fclose(f);
        }
    }
};

Trace& trace() {
    static Trace t;
    return t;
}

// written once at process exit, also when the process leaves through std::exit
struct Flush {
    Flush() { std::atexit([] { trace().write(); }); }
};

}  // namespace

namespace detail {
const bool g_enabled = [] {
    const bool on = !trace().path.empty();
    if (on) static Flush flush;   // the exit write is a no-op without a path
    return on;
}();
}  // namespace detail

void record(std::uint32_t site, std::uint32_t target) {
    Trace& t = trace();
    std::lock_guard<std::mutex> hold(t.lock);
    ++t.pairs[(static_cast<std::uint64_t>(site) << 32) | target];
}

}  // namespace gekko::indirect_call_trace
