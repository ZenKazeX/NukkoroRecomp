#include "gekko/audio_audit.h"
#include "gekko/cpu.h"
#include "gekko/cpu_diag.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2  // K32GetProcessMemoryInfo, exported from kernel32; no psapi.lib
#include <windows.h>
#include <psapi.h>
#endif

#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

#if !defined(GEKKO_SHIPPING)  // ES: the audit trace is not part of the user's exe
namespace gekko::audio_audit {
namespace {
u64 now_ns() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Record { u64 ns, cpu_100ns, cycles, tid; const char* stage; u64 v[8]; };
// Per-lane record capacity; GEKKO_AUDIO_AUDIT_RECORDS overrides it for long play sessions.
constexpr std::size_t default_capacity = 786432;
std::size_t capacity = default_capacity;
// Guest-sample interval; GEKKO_AUDIO_AUDIT_GUEST_MS overrides it (1 ms default).
u64 guest_interval_ns = 1000000;
struct Buffer { std::unique_ptr<Record[]> data; std::size_t size = 0; u64 lost = 0; };
struct Trace {
    std::string path;
    std::array<Buffer, 3> lanes;
    std::mutex marks_mutex;
    std::vector<std::pair<u64, u64>> marks;  // (ns, ordinal)
    Trace() {
        const char* setting = std::getenv("GEKKO_AUDIO_AUDIT");
        if (!setting || !*setting) return;
        path = setting;
        if (const char* records = std::getenv("GEKKO_AUDIO_AUDIT_RECORDS")) {
            const unsigned long long value = std::strtoull(records, nullptr, 10);
            if (value >= 1024ull && value <= 16000000ull) capacity = static_cast<std::size_t>(value);
        }
        if (const char* interval = std::getenv("GEKKO_AUDIO_AUDIT_GUEST_MS")) {
            const unsigned long long value = std::strtoull(interval, nullptr, 10);
            if (value >= 1ull && value <= 1000ull) guest_interval_ns = value * 1000000ull;
        }
        for (auto& lane : lanes) lane.data = std::make_unique<Record[]>(capacity);
    }
    ~Trace() {
        if (path.empty()) return;
        FILE* out = std::fopen(path.c_str(), "w");
        if (!out) { std::fprintf(stderr, "[audio-audit] cannot write %s\n", path.c_str()); return; }
        std::fprintf(out, "lane,ns,cpu_100ns,thread_cycles,tid,stage,a,b,c,d,e,f,g,h\n");
        for (unsigned index = 0; index < lanes.size(); ++index) {
            const auto& lane = lanes[index];
            for (std::size_t i = 0; i < lane.size; ++i) {
                const auto& r = lane.data[i];
                std::fprintf(out, "%u,%llu,%llu,%llu,%llu,%s", index, r.ns, r.cpu_100ns,
                             r.cycles, r.tid, r.stage);
                for (u64 v : r.v) std::fprintf(out, ",%llu", v);
                std::fputc('\n', out);
            }
            std::fprintf(stderr, "[audio-audit] lane=%u records=%zu lost=%llu\n", index, lane.size, lane.lost);
        }
        {
            std::lock_guard<std::mutex> lock(marks_mutex);
            for (const auto& mark : marks) {
                std::fprintf(out, "3,%llu,0,0,0,user-mark,%llu,0,0,0,0,0,0,0\n", mark.first, mark.second);
            }
            std::fprintf(stderr, "[audio-audit] user marks=%zu\n", marks.size());
        }
        std::fclose(out);
    }
};
Trace trace;  // allocated before either owner thread starts
}
bool enabled() noexcept { return !trace.path.empty(); }
void event(Lane lane, const char* stage, u64 a, u64 b, u64 c, u64 d,
           u64 e, u64 f, u64 g, u64 h) noexcept {
    if (!enabled()) return;
    auto& buffer = trace.lanes[static_cast<unsigned>(lane)];
    if (buffer.size == capacity) { ++buffer.lost; return; }
    Record& r = buffer.data[buffer.size++];
    r = {now_ns(), 0, 0, 0, stage, {a,b,c,d,e,f,g,h}};
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    r.cpu_100ns = (u64(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime +
                 (u64(user.dwHighDateTime) << 32) + user.dwLowDateTime;
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    r.cycles = cycles;
    r.tid = GetCurrentThreadId();
#endif
}
void guest(const Cpu& cpu) noexcept {
    if (!enabled()) return;
    static u64 previous = 0;
    const u64 now = now_ns();
    if (now - previous < guest_interval_ns) return;
    previous = now;
    // Guest progress: game time, native entries, pc, the time-base view,
    // dispatcher iterations and interpreted instructions.
    event(Lane::Guest, "guest", cpu.game_clock().now(), cpu.recompiled_calls, 0u, 0u, cpu.pc,
          cpu.time_base(), cpu_diag().dispatch_iterations, cpu.interpreted_instructions());
#ifdef _WIN32
    // Category discriminator for a guest-progress stall, sampled on the owner
    // thread beside the guest counters. User time rising is native computation;
    // kernel time rising with the page-fault count is a memory fault; neither
    // rising while wall time does is a block. The processor number catches a
    // migration to a slower core. GetThreadTimes has 15.625 ms granularity, so
    // these are only meaningful summed over a window, not per sample.
    FILETIME created{}, exited{}, kernel{}, user{};
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) memory = {};
    event(Lane::Guest, "guestos",
          (u64(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime,
          (u64(user.dwHighDateTime) << 32) + user.dwLowDateTime,
          memory.PageFaultCount, memory.WorkingSetSize, memory.PagefileUsage,
          GetCurrentProcessorNumber(), 0, 0);
#endif
}
void user_mark(u64 ordinal) noexcept {
    if (!enabled()) return;
    try {
        std::lock_guard<std::mutex> lock(trace.marks_mutex);
        trace.marks.emplace_back(now_ns(), ordinal);
    } catch (...) {
    }
}
Scope::Scope(const char*, const char* finish, Lane owner) noexcept : end(finish), lane(owner) {
    if (enabled()) start = now_ns();
}
Scope::~Scope() {
    if (start != 0 && now_ns() - start >= 100000) event(lane, end, start, now_ns() - start);
}
}
#endif  // !GEKKO_SHIPPING
