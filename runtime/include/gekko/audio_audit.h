#pragma once

// Optional, bounded, owner-thread trace. No guest state is modified, no I/O or
// allocation occurs while recording, and a full trace counts lost records.
#include "gekko/types.h"

namespace gekko {
class Cpu;
namespace audio_audit {
enum class Lane : unsigned { Guest, Device, Presenter };
#if defined(GEKKO_SHIPPING)
// GEKKO_SHIPPING (ES 2026-10-20): the audit trace does not exist in the user's exe; every call folds away.
constexpr bool enabled() noexcept { return false; }
inline void event(Lane, const char*, u64 = 0, u64 = 0, u64 = 0, u64 = 0, u64 = 0, u64 = 0, u64 = 0,
                  u64 = 0) noexcept {}
inline void guest(const Cpu&) noexcept {}
inline void user_mark(u64) noexcept {}
struct Scope {
    Scope(const char*, const char*, Lane = Lane::Guest) noexcept {}
};
#else
bool enabled() noexcept;
void event(Lane lane, const char* stage, u64 a = 0, u64 b = 0, u64 c = 0,
           u64 d = 0, u64 e = 0, u64 f = 0, u64 g = 0, u64 h = 0) noexcept;
void guest(const Cpu& cpu) noexcept;
// A user glitch marker (F8). Thread-safe; any thread. Written as lane-3 rows at exit.
void user_mark(u64 ordinal) noexcept;
struct Scope {
    const char* end;
    Lane lane;
    u64 start = 0;
    Scope(const char* begin, const char* finish, Lane owner = Lane::Guest) noexcept;
    ~Scope();
};
#endif
}  // namespace audio_audit
}  // namespace gekko
