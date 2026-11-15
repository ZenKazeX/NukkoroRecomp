// S1 core: the layout manager's one-player per-frame scan, native with a hand-back. Contract: include/gekko/native_s1_core.h.

#include "gekko/native_s1_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gekko/cpu.h"

namespace gekko::s1_core {
namespace {

// Manager fields.
constexpr u32 kManagerCount = 0x08u;     // live slots counted this frame
constexpr u32 kManagerFactories = 0x14u; // non-null once the stage's layout is loaded
constexpr u32 kManagerSlots = 0x18u;     // the 0x800 slots
constexpr u32 kManagerMode = 0x428u;     // bit 0: restricted
constexpr u32 kSlotCount = 0x800u;
constexpr u32 kSlotSize = 0x20u;
constexpr u32 kSlotType = 0x10u;         // s32 factory index (negative: none)
// Record fields.
constexpr u32 kFlags = 0x18u;
constexpr u32 kSavedFlags = 0x1Cu;
constexpr u32 kRange = 0x23u;
constexpr u32 kLive = 0x2u, kDormant = 0x1u, kRunsRestricted = 0x8000u, kHeld = 0x20000u, kAlwaysInRange = 0x80800u;
constexpr u32 kMarkOut = 0x40u, kRestoreB = 0x800000u, kRespawnOut = 0x10000u;
// The return address of the original's 0x800CA8D8 call (the instruction after its bl): a value only, never fetched.
constexpr u32 kSpawnLink = 0x800CAD8Cu;  // liveness-roots: value-only

Stats g_stats;
#ifdef SHADOW_S1_CORE_SCAN_DEFAULT
bool g_enabled = true;  // CMake option SHADOW_S1_CORE_SCAN=ON; GEKKO_S1_CORE_SCAN=0 disables
#else
bool g_enabled = false;  // off by default (untimed, docs/agents/s1-core.md); GEKKO_S1_CORE_SCAN=1 enables
#endif

float word_to_float(u32 word) noexcept {
    float f;
    std::memcpy(&f, &word, sizeof f);
    return f;
}

}  // namespace

const Stats& stats() noexcept { return g_stats; }
void set_enabled(bool enabled) noexcept { g_enabled = enabled; }

bool serve_layout_scan(Cpu& c) noexcept {
    if (!g_enabled) return false;
    ++g_stats.calls;
    const u32 manager = c.gpr[3], point = c.gpr[4];
    const u32 sp = c.gpr[1];
    u32 count = 0;
    bool tested = false;
    u32 last_i = 0, last_record = 0, last_slot = 0;
    const bool table = c.mem_read32(manager + kManagerFactories) != 0u;
    if (table) {
        const u32 base = c.mem_read32(manager + kManagerSlots);
        const bool restricted = (c.mem_read32(manager + kManagerMode) & 1u) != 0u;
        float p[3] = {};
        bool have_point = false;
        // 0x800CA970: flag 0x80800 -> in range; else |pos - point|^2 <= (100 * range)^2 in single precision (NaN: out).
        const auto in_range = [&](u32 record, u32 flags) noexcept {
            if ((flags & kAlwaysInRange) != 0u) return true;
            if (!have_point) {
                p[0] = word_to_float(c.mem_read32(point));
                p[1] = word_to_float(c.mem_read32(point + 4u));
                p[2] = word_to_float(c.mem_read32(point + 8u));
                have_point = true;
            }
            // 0x8040DE54 then 0x8040DF7C: dz^2 + (dx^2 + dy^2), each step rounded to single (as NativeObjectRange).
            const float dx = word_to_float(c.mem_read32(record)) - p[0];
            const float dy = word_to_float(c.mem_read32(record + 4u)) - p[1];
            const float dz = word_to_float(c.mem_read32(record + 8u)) - p[2];
            const float d2 = dz * dz + (dx * dx + dy * dy);
            const float d = 100.0f * static_cast<float>(c.mem_read8(record + kRange));
            return d2 <= d * d;
        };
        u32 slot = base;
        for (u32 i = 0; i < kSlotCount; ++i, slot += kSlotSize) {
            const u32 record = c.mem_read32(slot);
            if (record == 0u) {  // the original would read the word at 0x18 (a data fault on the console): its code runs
                ++g_stats.declined_other;
                return false;
            }
            const u32 flags = c.mem_read32(record + kFlags);
            if (restricted && (flags & kRunsRestricted) == 0u) continue;
            if ((flags & kLive) != 0u) {
                ++count;
                continue;
            }
            if ((flags & kDormant) != 0u) {
                if (static_cast<s32>(c.mem_read32(slot + kSlotType)) < 0) continue;
                // 0x800CA8D8(manager, slot, point): live or held -> no; in range -> the factory creates the object.
                tested = true;
                last_i = i;
                last_record = record;
                last_slot = slot;
                if ((flags & (kLive | kHeld)) != 0u) continue;
                if (in_range(record, flags)) {
                    ++g_stats.declined_spawn;
                    return false;
                }
                continue;
            }
            if ((c.mem_read32(record + kSavedFlags) & 1u) == 0u) continue;
            if ((flags & (kMarkOut | kRestoreB)) != 0u) {  // restore the saved flags and relink the group: the original's
                ++g_stats.declined_spawn;
                return false;
            }
            if ((flags & kRespawnOut) != 0u && !in_range(record, flags)) {  // the slot gets the out-of-range mark
                ++g_stats.declined_spawn;
                return false;
            }
        }
        g_stats.slots_scanned += kSlotCount;
    }
    // Served. The original's writes, in its order.
    c.mem_write32(sp - 0x20u, sp);
    c.mem_write32(sp + 4u, c.lr);
    for (u32 k = 0; k < 5u; ++k) c.mem_write32(sp - 0x14u + 4u * k, c.gpr[27u + k]);
    if (table) {
        c.mem_write32(manager + kManagerCount, count);
        if (tested) {
            c.mem_write32(sp - 0x40u, sp - 0x20u);
            c.mem_write32(sp - 0x1Cu, kSpawnLink);
            c.mem_write32(sp - 0x2Cu, last_i);
            c.mem_write32(sp - 0x28u, last_record);
            c.mem_write32(sp - 0x24u, last_slot);
        }
    }
    ++g_stats.served;
    return true;
}

void print_stats() {
    std::fprintf(stderr, "s1 core layout scan: %llu calls, %llu served, %llu declined (spawn / restore %llu, null record %llu)\n",
                 static_cast<unsigned long long>(g_stats.calls), static_cast<unsigned long long>(g_stats.served),
                 static_cast<unsigned long long>(g_stats.calls - g_stats.served), static_cast<unsigned long long>(g_stats.declined_spawn),
                 static_cast<unsigned long long>(g_stats.declined_other));
}

void install(Cpu& cpu) {
    if (const char* v = std::getenv("GEKKO_S1_CORE_STATS"); v != nullptr && v[0] == '1') std::atexit(print_stats);
    if (const char* v = std::getenv("GEKKO_S1_CORE_SCAN"); v != nullptr && (v[0] == '0' || v[0] == '1')) g_enabled = v[0] == '1';
    cpu.install_hle(kLayoutScan, [](Cpu& c) { return serve_layout_scan(c); }, "S1 core layout scan (native, hand-back)");
}

}  // namespace gekko::s1_core
