// S1 core: the character sweep query 0x80021934, native. Contract: include/gekko/native_s1_chars.h; map: docs/re/core.md section 9.
#include "gekko/native_s1_chars.h"

#include <cstdio>
#include <cstdlib>

#include "gekko/recomp.h"

namespace gekko::s1_chars {
namespace {

constexpr u32 kSlotTable = 0x80572544u;                 // 0x80021C80's table (lbl_80572544): two halfwords per entry
constexpr u32 kLeafSlotTable = 0x80021C80u, kLeafUsed = 0x8003AA58u, kLeafCharacter = 0x8003A568u;
constexpr u32 kManager = 0x80045AC0u, kSweepOne = 0x80021480u, kSphere = 0x80021C04u, kFilterNew = 0x80021BC4u, kPool = 0x80021B08u,
              kQuery = 0x800C51C8u, kFilterFree = 0x80021B74u;
constexpr u32 kSlotCount = 0xA8u;

Stats g_stats;
#ifdef SHADOW_S1_CHARS_DEFAULT
bool g_enabled = true;  // CMake option SHADOW_S1_CHARS=ON; GEKKO_S1_CHARS=0 disables
#else
bool g_enabled = false;  // off by default; GEKKO_S1_CHARS=1 enables
#endif
signed char g_leaf_hooked = 0;  // 0 not asked, 1 a hook covers an inlined leaf, 2 none

void print_stats() {
    std::fprintf(stderr, "s1 chars: %llu calls, %llu served, %llu handed back, %llu slots tested, %llu swept\n",
                 static_cast<unsigned long long>(g_stats.calls), static_cast<unsigned long long>(g_stats.served),
                 static_cast<unsigned long long>(g_stats.declined), static_cast<unsigned long long>(g_stats.slots_tested),
                 static_cast<unsigned long long>(g_stats.slots_swept));
}

struct Frame {
    Cpu& c;
    u32 sp;                // r1 inside the function
    u32 g[32];             // the translated function's registers (written back to the Cpu before every call)
    void sync() {
        c.gpr[1] = sp;
        for (u32 r = 19; r <= 31; ++r) c.gpr[r] = g[r];
    }
};

// A callee as the translated code calls it: LR set, registers synced, the call, then the exit / pc checks. False: the caller returns at once
// (the callee left to the dispatcher or the machine stopped: the state is exact for the dispatcher to resume the translated function).
bool call(Frame& f, u32 target, u32 link) {
    f.sync();
    f.c.lr = link;
    if (!call_guest(f.c, target)) return false;
    if (f.c.exiting() && f.c.finish_transfers()) return false;
    if (f.c.pc != link) {
        recomp::resume_elsewhere(f.c, link - 4u);
        return false;
    }
    return true;
}

}  // namespace

const Stats& stats() noexcept { return g_stats; }
void set_enabled(bool enabled) noexcept { g_enabled = enabled; }

bool serve(Cpu& c) {
    if (!g_enabled) return false;
    ++g_stats.calls;
    if (g_leaf_hooked == 0) g_leaf_hooked = (c.has_hle(kLeafSlotTable) || c.has_hle(kLeafUsed) || c.has_hle(kLeafCharacter)) ? 1 : 2;
    const u32 mode = c.gpr[6];
    if (g_leaf_hooked == 1 || ((mode & 2u) != 0u && (c.msr() & 0x2000u) == 0u)) {
        ++g_stats.declined;
        return false;
    }
    const u32 osp = c.gpr[1], olr = c.lr;
    Frame f{c, osp - 0x70u, {}};
    for (u32 r = 0; r < 32; ++r) f.g[r] = c.gpr[r];
    const u32 sp = f.sp;
    // prologue: stwu, the LR save, stmw r19..r31
    c.mem_write32(sp, osp);
    c.mem_write32(sp + 0x74u, olr);
    for (u32 r = 19; r <= 31; ++r) c.mem_write32(sp + 0x3Cu + 4u * (r - 19u), f.g[r]);
    u32* g = f.g;
    g[25] = c.gpr[6];
    g[30] = c.mem_read8(sp + 0x7Bu);
    g[22] = c.gpr[3];
    g[23] = c.gpr[4];
    g[24] = c.gpr[5];
    g[26] = c.gpr[7];
    g[27] = c.gpr[8];
    g[28] = c.gpr[9];
    g[29] = c.gpr[10];
    g[31] = 0u;
    if ((g[25] & 1u) != 0u) {
        // 0x80021C80(r1 + 0x10, 0): the first table entry's two halfwords; then copied to r1 + 0x14
        const u16 h0 = c.mem_read16(kSlotTable), h1 = c.mem_read16(kSlotTable + 2u);
        c.mem_write16(sp + 0x10u, h0);
        c.mem_write16(sp + 0x12u, h1);
        c.mem_write16(sp + 0x14u, c.mem_read16(sp + 0x10u));
        c.mem_write16(sp + 0x16u, c.mem_read16(sp + 0x12u));
        c.gpr[3] = sp + 0x14u;
        if (!call(f, kManager, 0x80021998u)) return true;
        u32 table = c.gpr[3];
        if (table != 0u) table -= 0x28u;
        g[20] = table;
        if (table != 0u) {
            g[21] = g[30] == 0u ? 1u : 0u;
            for (g[19] = 0; g[19] < kSlotCount; ++g[19]) {
                ++g_stats.slots_tested;
                const u32 i = g[19];
                // 0x8003AA58(table, i, 0): either of the two banks' halfwords at +0x48 / +0x198 is non-zero
                if (c.mem_read16(table + 0x48u + 2u * i) == 0u && c.mem_read16(table + 0x198u + 2u * i) == 0u) continue;
                // 0x8003A568: the character pointer; its body at +0xB8
                const u32 character = c.mem_read32(table + 0x588u + 4u * i);
                if (character == 0u) continue;
                const u32 body = c.mem_read32(character + 0xB8u);
                if (body == 0u) continue;
                ++g_stats.slots_swept;
                c.mem_write32(sp + 0x8u, g[21]);
                c.gpr[3] = g[22];
                c.gpr[4] = body;
                c.gpr[5] = g[23];
                c.gpr[6] = g[24];
                c.gpr[7] = g[27];
                c.gpr[8] = g[28];
                c.gpr[9] = g[29];
                c.gpr[10] = g[31];
                if (!call(f, kSweepOne, 0x80021A18u)) return true;
                g[31] = c.gpr[3];
            }
        }
    }
    if ((g[25] & 2u) != 0u) {
        const u32 raw = c.mem_read32(g[23] + 0xCu);
        const u64 d = single_to_double_bits(raw);
        c.fpr[1].bits[0] = d;
        c.fpr[1].bits[1] = d;
        c.gpr[4] = g[23];
        c.gpr[3] = sp + 0x28u;
        if (!call(f, kSphere, 0x80021A40u)) return true;
        const bool with_exclusion = g[30] != 0u;
        const u32 filter = sp + (with_exclusion ? 0x20u : 0x18u);
        c.gpr[3] = filter;
        c.gpr[4] = c.gpr[13] + 0x3248u;
        if (!call(f, kFilterNew, with_exclusion ? 0x80021A54u : 0x80021AA8u)) return true;
        if (!call(f, kPool, with_exclusion ? 0x80021A58u : 0x80021AACu)) return true;
        // the pool singleton's pointer stays in r3: the query's first argument
        c.mem_write32(sp + 0x8u, filter);
        c.gpr[5] = g[24];
        c.gpr[6] = g[26];
        c.gpr[8] = g[31];
        c.gpr[9] = g[28];
        c.mem_write32(sp + 0xCu, with_exclusion ? g[27] : 0u);
        c.gpr[10] = g[29];
        c.gpr[4] = sp + 0x28u;
        c.gpr[7] = 1u;
        if (!call(f, kQuery, with_exclusion ? 0x80021A84u : 0x80021ADCu)) return true;
        g[31] = c.gpr[3];
        c.gpr[3] = filter;
        c.gpr[4] = 0xFFFFFFFFu;
        if (!call(f, kFilterFree, with_exclusion ? 0x80021A98u : 0x80021AF0u)) return true;
    }
    // epilogue: r3 = r31, lmw r19..r31 and the LR from the frame, r1 popped
    c.gpr[3] = g[31];
    for (u32 r = 19; r <= 31; ++r) c.gpr[r] = c.mem_read32(sp + 0x3Cu + 4u * (r - 19u));
    c.lr = c.mem_read32(sp + 0x74u);
    c.gpr[1] = osp;
    ++g_stats.served;
    return true;
}

void install(Cpu& cpu) {
    if (const char* v = std::getenv("GEKKO_S1_CHARS_STATS"); v != nullptr && v[0] == '1') std::atexit(print_stats);
    if (const char* v = std::getenv("GEKKO_S1_CHARS"); v != nullptr && (v[0] == '0' || v[0] == '1')) g_enabled = v[0] == '1';
    cpu.install_hle(kCharSweep, [](Cpu& c) { return serve(c); }, "S1 core character sweep (native)");
}

}  // namespace gekko::s1_chars
