// S1 target (round 11): the target registry's query layer, native with a hand-back. Contract: include/gekko/native_s1_target2.h.
// The frames of straight-line functions come from tools/re/gen_frames.py (native_s1_target2_frames.inc); loops, branches and the
// guest allocator calls are written here by hand, each from the disassembly named in its comment.

#include "gekko/native_s1_target2.h"
#include "gekko/game_memory_end.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gekko/cpu.h"
#include "gekko/ppc_ops.h"
#include "gekko/native_guest_call.h"
#include "gekko/native_s1_chars.h"
#include "gekko/native_se_math.h"
#include "gekko/gekko_estimate.h"
#include <algorithm>

namespace gekko::s1_target2 {
namespace {

Stats g_stats;
#ifdef SHADOW_S1_TARGET2_DEFAULT
bool g_enabled = true;  // CMake option SHADOW_S1_TARGET2=ON; GEKKO_S1_TARGET2=0 disables
#else
bool g_enabled = false;  // off by default; GEKKO_S1_TARGET2=1 enables
#endif
std::FILE* g_alloc_trace = nullptr;  // GEKKO_S1_TARGET2_ALLOCTRACE
bool g_guard = true;                 // the Target vtable-word guard (off only for the contract test's negative control)

constexpr u32 kRamLo = 0x80000000u;  // liveness-roots: value-only
constexpr u32 kRamHi = 0x81800000u;  // liveness-roots: value-only
static_assert(kRamHi == gekko::kConsoleMemoryEnd, "the console's memory end");
// The end of the game's memory in this run: the console's, or higher when the run was given more (game_memory_end.h).
inline u32 ram_hi() noexcept { return gekko::game_memory_end(); }
constexpr u32 kWindow = 0x600u;      // stack bytes below the entry r1 the trees write (deepest frame is ~0x180)
constexpr u32 kMaxEntries = 4096u;
constexpr u32 kTargetVtable = 0x8053ECB0u;  // liveness-roots: value-only
constexpr u32 kTargetPosFn = 0x801E76FCu;   // liveness-roots: value-only
constexpr u32 kMaxQueryEntries = 256u;  // the floating-point entries' proof sorts the footprints of their entries

inline u32 rotl32(u32 v, unsigned n) noexcept { n &= 31u; return n == 0u ? v : (v << n) | (v >> (32u - n)); }
inline u64 dbl_bits_of_single(u32 bits) noexcept { float f; std::memcpy(&f, &bits, 4); const double d = f; u64 r; std::memcpy(&r, &d, 8); return r; }
inline u32 clz32(u32 v) noexcept { if (v == 0u) return 32u; u32 n = 0u; while ((v & 0x80000000u) == 0u) { v <<= 1; ++n; } return n; }

struct Regs { u32 r[32]; u64 f0[32]; u64 f1[32]; };  // r14..r31, and ps0 / ps1 of f14..f31, as the original holds them at a call

struct Ctx {
    Cpu& c;
    u32 r2, r13;
    bool dead = false;  // a guest call stopped the run
    bool requant30 = false, requant31 = false;  // psq_l of f30 / f31 ran: ps1 came back through the single format
    explicit Ctx(Cpu& cpu) : c(cpu), r2(cpu.gpr[2]), r13(cpu.gpr[13]) {}
    u32 rd32(u32 a) { return c.mem_read32(a); }
    u32 rd16(u32 a) { return c.mem_read16(a); }
    u32 rd8(u32 a) { return c.mem_read8(a); }
    void wr32(u32 a, u32 v) { c.mem_write32(a, v); }
    void wr16(u32 a, u32 v) { c.mem_write16(a, static_cast<u16>(v)); }
    void wr8(u32 a, u32 v) { c.mem_write8(a, static_cast<u8>(v)); }
    u64 rd64(u32 a) { return c.mem_read64(a); }
    void wr64(u32 a, u64 v) { c.mem_write64(a, v); }
    // psq_st f(k), a, 0, GQR0: the quantised store of ps0 / ps1 (the real instruction's code, with the register temporarily loaded).
    void psq_st(u32 a, u64 f0, u64 f1) {
        const FpReg saved = c.fpr[31];
        c.fpr[31].bits[0] = f0;
        c.fpr[31].bits[1] = f1;
        ppc::psq_store(c, 31u, a, false, 0u);
        c.fpr[31] = saved;
    }
};

#include "native_s1_target2_frames.inc"

#include "native_s1_target2_models.inc"

#include "native_s1_target2_fp.inc"

}  // namespace

const Stats& stats() noexcept { return g_stats; }
void set_enabled(bool enabled) noexcept { g_enabled = enabled; }
void set_guard_enabled(bool enabled) noexcept { g_guard = enabled; }
bool enabled() noexcept { return g_enabled; }

namespace {
inline bool ram_word(u32 a) noexcept { return (a & 3u) == 0u && a >= kRamLo && a < ram_hi() - 4u; }
}

bool serve(Cpu& c, Entry entry) noexcept {
    const unsigned e = static_cast<unsigned>(entry);
    const auto decline = [&](Reason why) noexcept {
        ++g_stats.declined[e][static_cast<unsigned>(why)];
        return false;
    };
    if (!g_enabled) return decline(Reason::Disabled);
    const u32 sp0 = c.gpr[1], lr0 = c.lr, pc0 = c.pc;
    if (sp0 < kRamLo + kWindow + 0x40u || sp0 >= ram_hi()) return decline(Reason::BadShape);
    const u32 stack_lo = sp0 - kWindow, stack_hi = sp0 + 8u;  // the owner writes below r1 and the caller's LR slot [r1 + 4]
    const auto in_stack = [&](u32 a) noexcept { return a + 4u > stack_lo && a < stack_hi; };
    u32 saved[32];
    for (u32 r = 0; r < 32u; ++r) saved[r] = c.gpr[r];
    u32 result = 0u;
    switch (entry) {
        case Entry::Factory: {
            // r3 = the registry. The list object at registry + 8: [+0] = N, the sentinel node embedded at +4 {prev, next}, nodes {prev, next, px, pn}.
            const u32 reg = c.gpr[3];
            if (!ram_word(reg + 0x10u) || in_stack(reg)) return decline(Reason::BadShape);
            const u32 n = c.mem_read32(reg + 8u);
            if (n > kMaxEntries) return decline(Reason::TooLarge);
            const u32 sentinel = reg + 0xCu;
            u32 node = c.mem_read32(sentinel + 4u);
            for (u32 i = 0; i < n; ++i) {
                if (!ram_word(node + 8u) || in_stack(node)) return decline(Reason::BadShape);
                const u32 px = c.mem_read32(node + 8u), pn = c.mem_read32(node + 0xCu);
                if (px == 0u || pn == 0u || !ram_word(px + 0x30u) || !ram_word(pn + 4u)) return decline(Reason::ExpiredOrNull);
                const u32 use = c.mem_read32(pn + 4u);
                if (use == 0u || use >= 0xFFFFFF00u) return decline(Reason::ExpiredOrNull);
                if (in_stack(px + 0x30u) || in_stack(pn + 4u)) return decline(Reason::Alias);
                node = c.mem_read32(node + 4u);
            }
            if (node != sentinel) return decline(Reason::BadShape);  // the list is a ring of exactly N nodes
            Ctx x(c);
            Regs R{};
            for (u32 r = 0; r < 32u; ++r) { R.r[r] = saved[r]; R.f0[r] = c.fpr[r].bits[0]; R.f1[r] = c.fpr[r].bits[1]; }
            result = H_801E7C40(x, sp0, lr0, reg, 0, 0, 0, 0, 0, R, 0u);
            if (x.dead) return false;
            break;
        }
        case Entry::CopyCtor: {
            // r3 = this (a TargetSearchList to build), r4 = src: the entries [[src + 0xC], [src + 0x10]) are copied into a fresh vector.
            const u32 self = c.gpr[3], src = c.gpr[4];
            if (!ram_word(self + 0x10u) || !ram_word(src + 0x10u) || in_stack(self) || in_stack(self + 0x10u)) return decline(Reason::BadShape);
            const u32 begin = c.mem_read32(src + 0xCu), end = c.mem_read32(src + 0x10u);
            if ((begin & 3u) != 0u || (end & 3u) != 0u || end < begin || (end - begin) / 4u > kMaxEntries) return decline(Reason::TooLarge);
            if (end != begin && (!ram_word(begin) || !ram_word(end - 4u))) return decline(Reason::BadShape);
            Ctx x(c);
            Regs R{};
            for (u32 r = 0; r < 32u; ++r) { R.r[r] = saved[r]; R.f0[r] = c.fpr[r].bits[0]; R.f1[r] = c.fpr[r].bits[1]; }
            result = F_801E8A68(x, sp0, lr0, self, src, 0, 0, 0, 0, R, 0u);
            if (x.dead) return false;
            break;
        }
        case Entry::Dtor: {
            // r3 = this (or null), r4 = flag: vector buffer delete, then delete of the object when the flag (s16) is > 0.
            const u32 self = c.gpr[3];
            if (self != 0u && (!ram_word(self + 0x10u) || in_stack(self) || in_stack(self + 0x10u))) return decline(Reason::BadShape);
            Ctx x(c);
            Regs R{};
            for (u32 r = 0; r < 32u; ++r) { R.r[r] = saved[r]; R.f0[r] = c.fpr[r].bits[0]; R.f1[r] = c.fpr[r].bits[1]; }
            result = H_801E8974(x, sp0, lr0, self, c.gpr[4], 0, 0, 0, 0, R, 0u);
            if (x.dead) return false;
            break;
        }
        case Entry::FlagA:
        case Entry::FlagB: {
            // r3 = query, r4 = mask: remove_if over [[q + 0xC], [q + 0x10]) with the flag predicate; every entry must be a live holder.
            const u32 q = c.gpr[3];
            if (!ram_word(q + 0x10u) || in_stack(q) || in_stack(q + 0x10u)) return decline(Reason::BadShape);
            const u32 begin = c.mem_read32(q + 0xCu), end = c.mem_read32(q + 0x10u);
            if ((begin & 3u) != 0u || (end & 3u) != 0u || end < begin || (end - begin) / 4u > kMaxEntries) return decline(Reason::TooLarge);
            if (end != begin && (!ram_word(begin) || !ram_word(end - 4u))) return decline(Reason::BadShape);
            if (end != begin && (in_stack(begin) || in_stack(end - 4u))) return decline(Reason::Alias);
            if (begin < stack_hi && end + 4u > stack_lo && end != begin) return decline(Reason::Alias);
            for (u32 a = begin; a < end; a += 4u) {
                const u32 holder = c.mem_read32(a);
                if (!ram_word(holder + 4u)) return decline(Reason::BadShape);
                const u32 px = c.mem_read32(holder), pn = c.mem_read32(holder + 4u);
                if (px == 0u || pn == 0u || !ram_word(px + 0x30u) || !ram_word(pn + 4u)) return decline(Reason::ExpiredOrNull);
                const u32 use = c.mem_read32(pn + 4u);
                if (use == 0u || use >= 0xFFFFFF00u) return decline(Reason::ExpiredOrNull);
                for (u32 w : {holder, holder + 4u, px + 0x30u, pn + 4u})
                    if (in_stack(w) || (w + 4u > begin && w < end) || (w + 4u > q && w < q + 0x14u)) return decline(Reason::Alias);
            }
            Ctx x(c);
            Regs R{};
            for (u32 r = 0; r < 32u; ++r) { R.r[r] = saved[r]; R.f0[r] = c.fpr[r].bits[0]; R.f1[r] = c.fpr[r].bits[1]; }
            result = (entry == Entry::FlagA ? F_801E83D0 : F_801E8448)(x, sp0, lr0, q, c.gpr[4], 0, 0, 0, 0, R, 0u);
            if (x.dead) return false;
            break;
        }
        case Entry::SameTarget: {
            // r3 = query, r4 = &handle {px, pn}: remove the entry whose target is the handle's.
            const u32 q = c.gpr[3], h = c.gpr[4];
            if (!ram_word(q + 0x10u) || in_stack(q) || in_stack(q + 0x10u) || !ram_word(h + 4u) || in_stack(h) || in_stack(h + 4u)) return decline(Reason::BadShape);
            const u32 begin = c.mem_read32(q + 0xCu), end = c.mem_read32(q + 0x10u);
            if ((begin & 3u) != 0u || (end & 3u) != 0u || end < begin || (end - begin) / 4u > kMaxEntries) return decline(Reason::TooLarge);
            if (end != begin && (!ram_word(begin) || !ram_word(end - 4u))) return decline(Reason::BadShape);
            if (begin < stack_hi && end + 4u > stack_lo && end != begin) return decline(Reason::Alias);
            const u32 hpn = c.mem_read32(h + 4u);
            if (hpn == 0u || !ram_word(hpn + 8u)) return decline(Reason::ExpiredOrNull);
            const u32 huse = c.mem_read32(hpn + 4u);
            if (huse == 0u || huse >= 0xFFFFFF00u || c.mem_read32(hpn + 8u) == 0u || c.mem_read32(hpn + 8u) >= 0xFFFFFF00u) return decline(Reason::ExpiredOrNull);
            for (u32 w : {hpn + 4u, hpn + 8u, h, h + 4u})
                if (in_stack(w) || (w + 4u > begin && w < end) || (w + 4u > q && w < q + 0x14u)) return decline(Reason::Alias);
            for (u32 a = begin; a < end; a += 4u) {
                const u32 holder = c.mem_read32(a);
                if (!ram_word(holder + 4u)) return decline(Reason::BadShape);
                const u32 px = c.mem_read32(holder), pn = c.mem_read32(holder + 4u);
                if (px == 0u || pn == 0u || !ram_word(pn + 4u)) return decline(Reason::ExpiredOrNull);
                const u32 use = c.mem_read32(pn + 4u);
                if (use == 0u || use >= 0xFFFFFF00u) return decline(Reason::ExpiredOrNull);
                for (u32 w : {holder, holder + 4u, pn + 4u})
                    if (in_stack(w) || (w + 4u > begin && w < end) || (w + 4u > q && w < q + 0x14u)) return decline(Reason::Alias);
            }
            Ctx x(c);
            Regs R{};
            for (u32 r = 0; r < 32u; ++r) { R.r[r] = saved[r]; R.f0[r] = c.fpr[r].bits[0]; R.f1[r] = c.fpr[r].bits[1]; }
            result = F_801E8158(x, sp0, lr0, q, h, 0, 0, 0, 0, R, 0u);
            if (x.dead) return false;
            break;
        }
        case Entry::Sphere:
        case Entry::ViewCone:
        case Entry::Range:
        case Entry::Nearest: {
            // Floating-point query entries. Sphere (q, vecB, vecA, f1), view cone (q, vec), range (q, vec, f1), nearest (out, q, point).
            if (c.spr[SPR_GQR0] != 0u) return decline(Reason::BadShape);  // psq_st / psq_l are modelled for the float format
            const bool nearest = entry == Entry::Nearest;
            const u32 q = nearest ? c.gpr[4] : c.gpr[3];
            const u32 out = c.gpr[3];
            u32 vecs[2] = {0u, 0u};
            unsigned nvec = 0;
            if (entry == Entry::Sphere) { vecs[nvec++] = c.gpr[4]; vecs[nvec++] = c.gpr[5]; }
            else if (entry == Entry::ViewCone || entry == Entry::Range) vecs[nvec++] = c.gpr[4];
            else vecs[nvec++] = c.gpr[5];
            if (!ram_word(q + 0x10u) || in_stack(q) || in_stack(q + 0x10u)) return decline(Reason::BadShape);
            if (nearest && (!ram_word(out + 4u) || in_stack(out) || in_stack(out + 4u))) return decline(Reason::BadShape);
            const u32 begin = c.mem_read32(q + 0xCu), end = c.mem_read32(q + 0x10u);
            if ((begin & 3u) != 0u || (end & 3u) != 0u || end < begin || (end - begin) / 4u > kMaxQueryEntries) return decline(Reason::TooLarge);
            if (end != begin && (!ram_word(begin) || !ram_word(end - 4u))) return decline(Reason::BadShape);
            const auto finite_word = [](u32 w) noexcept { const u32 e = w & 0x7F800000u; return e != 0x7F800000u && (e != 0u || (w & 0x007FFFFFu) == 0u); };
            u32 f1_word = 0u;
            if (entry == Entry::Sphere || entry == Entry::Range) {
                const u64 bits = c.fpr[1].bits[0];
                f1_word = double_to_single_bits_flush(bits);
                if (single_to_double_bits(f1_word) != bits || !finite_word(f1_word)) return decline(Reason::BadShape);
            }
            static thread_local std::vector<std::pair<u32, u32>> rd, wr;
            rd.clear();
            wr.clear();
            rd.emplace_back(q, q + 0x10u);
            wr.emplace_back(q + 0x10u, q + 0x14u);
            wr.emplace_back(stack_lo, stack_hi);
            if (end != begin) wr.emplace_back(begin, end);
            if (nearest) wr.emplace_back(out, out + 8u);
            for (unsigned k = 0; k < nvec; ++k) {
                const u32 v = vecs[k];
                if (!ram_word(v + 8u) || (v & 3u) != 0u) return decline(Reason::BadShape);
                if (!finite_word(c.mem_read32(v)) || !finite_word(c.mem_read32(v + 4u)) || !finite_word(c.mem_read32(v + 8u))) return decline(Reason::BadShape);
                rd.emplace_back(v, v + 12u);
            }
            for (u32 a = begin; a < end; a += 4u) {
                const u32 holder = c.mem_read32(a);
                if (!ram_word(holder + 4u)) return decline(Reason::BadShape);
                const u32 px = c.mem_read32(holder), pn = c.mem_read32(holder + 4u);
                if (px == 0u || pn == 0u || (px & 3u) != 0u || px < kRamLo || px + 0x38u > ram_hi() || !ram_word(pn + 8u)) return decline(Reason::ExpiredOrNull);
                const u32 use = c.mem_read32(pn + 4u), weak = c.mem_read32(pn + 8u);
                if (use == 0u || use >= 0xFFFFFF00u || weak == 0u || weak >= 0xFFFFFF00u) return decline(Reason::ExpiredOrNull);
                if (g_guard && (c.mem_read32(px) != kTargetVtable || c.mem_read32(kTargetVtable + 8u) != kTargetPosFn)) return decline(Reason::KindOfTarget);
                if (!finite_word(c.mem_read32(px + 8u)) || !finite_word(c.mem_read32(px + 0xCu)) || !finite_word(c.mem_read32(px + 0x10u))) return decline(Reason::BadShape);
                rd.emplace_back(holder, holder + 8u);
                rd.emplace_back(px, px + 4u);
                rd.emplace_back(px + 8u, px + 0x38u);
                wr.emplace_back(pn + 4u, pn + 0xCu);
                if (entry == Entry::Range) wr.emplace_back(px + 4u, px + 8u);
            }
            std::sort(rd.begin(), rd.end());
            std::sort(wr.begin(), wr.end());
            wr.erase(std::unique(wr.begin(), wr.end()), wr.end());
            for (std::size_t i = 1; i < wr.size(); ++i)
                if (wr[i].first < wr[i - 1].second) return decline(Reason::Alias);
            {
                static thread_local std::vector<u32> pmax;
                pmax.assign(rd.size(), 0u);
                u32 m = 0u;
                for (std::size_t i = 0; i < rd.size(); ++i) { m = std::max(m, rd[i].second); pmax[i] = m; }
                for (const auto& w : wr) {
                    // the last read interval that starts before w ends; the prefix maximum of the ends decides the overlap
                    const auto it = std::lower_bound(rd.begin(), rd.end(), std::make_pair(w.second, 0u));
                    if (it == rd.begin()) continue;
                    const std::size_t idx = static_cast<std::size_t>(it - rd.begin()) - 1u;
                    if (pmax[idx] > w.first) return decline(Reason::Alias);
                }
            }
            Ctx x(c);
            Regs R{};
            for (u32 r = 0; r < 32u; ++r) { R.r[r] = saved[r]; R.f0[r] = c.fpr[r].bits[0]; R.f1[r] = c.fpr[r].bits[1]; }
            switch (entry) {
                case Entry::Sphere: result = F_801E82C4(x, sp0, lr0, q, vecs[0], vecs[1], 0, 0, 0, R, f1_word); break;
                case Entry::ViewCone: result = F_801E84C0(x, sp0, lr0, q, vecs[0], 0, 0, 0, 0, R, 0u); break;
                case Entry::Range: result = H_801E8528(x, sp0, lr0, q, vecs[0], 0, 0, 0, 0, R, f1_word); break;
                default: result = H_801E879C(x, sp0, lr0, out, q, vecs[0], 0, 0, 0, R, 0u); break;
            }
            if (x.dead) return false;
            // psq_l f30 / f31 in the original's epilogues: ps1 comes back through the single format (ps0 is restored by lfd).
            const auto requant = [&](unsigned k) noexcept { c.fpr[k].bits[1] = single_to_double_bits(double_to_single_bits_flush(c.fpr[k].bits[1])); };
            if (x.requant30) requant(30u);
            if (x.requant31) requant(31u);
            break;
        }
        default: return decline(Reason::BadShape);
    }
    for (u32 r = 14u; r <= 31u; ++r) c.gpr[r] = saved[r];
    c.gpr[1] = sp0;
    c.lr = lr0;
    c.pc = pc0;
    c.gpr[3] = result;
    ++g_stats.served[e];
    return true;
}

void print_stats() {
    static const char* const names[] = {"factory 0x801E7C40", "copy ctor 0x801E8A68", "dtor 0x801E8974", "flag filter A 0x801E83D0", "flag filter B 0x801E8448", "same-target filter 0x801E8158", "sphere filter 0x801E82C4", "view-cone filter 0x801E84C0", "range filter 0x801E8528", "nearest 0x801E879C"};
    static const char* const why[] = {"none", "disabled", "shape", "expired/null", "too large", "alias", "kind of target"};
    for (unsigned e = 0; e < static_cast<unsigned>(Entry::kCount); ++e) {
        std::fprintf(stderr, "s1 target2 %s: served %llu", names[e], static_cast<unsigned long long>(g_stats.served[e]));
        for (unsigned r = 1; r < static_cast<unsigned>(Reason::kCount); ++r)
            if (g_stats.declined[e][r] != 0u) std::fprintf(stderr, ", %s %llu", why[r], static_cast<unsigned long long>(g_stats.declined[e][r]));
        std::fprintf(stderr, "\n");
    }
}

void install(Cpu& cpu) {
    if (const char* v = std::getenv("GEKKO_S1_TARGET2"); v != nullptr && (v[0] == '0' || v[0] == '1')) g_enabled = v[0] == '1';
    if (const char* path = std::getenv("GEKKO_S1_TARGET2_ALLOCTRACE"); path != nullptr && path[0] != '\0') {
        g_alloc_trace = std::fopen(path, "w");
        if (g_alloc_trace != nullptr) {  // declining hooks log the game's allocator calls; the owner logs its own (call_guest bypasses the hooks), in program order
            cpu.install_hle(0x803A1380u, [](Cpu& c) { std::fprintf(g_alloc_trace, "N %u %08X\n", c.gpr[3], c.lr); return false; }, "s1 target2 alloc trace new (declines)");
            cpu.install_hle(0x803A1334u, [](Cpu& c) { std::fprintf(g_alloc_trace, "D %08X %08X\n", c.gpr[3], c.lr); return false; }, "s1 target2 alloc trace delete (declines)");
        }
    }
    if (!g_enabled) return;  // hooks only when the switch is on
    if (const char* v = std::getenv("GEKKO_S1_TARGET2_STATS"); v != nullptr && v[0] == '1') std::atexit(print_stats);
    cpu.install_hle(0x801E7C40u, [](Cpu& c) { return serve(c, Entry::Factory); }, "S1 target2 factory (native, hand-back)");
    cpu.install_hle(0x801E8A68u, [](Cpu& c) { return serve(c, Entry::CopyCtor); }, "S1 target2 copy ctor (native, hand-back)");
    cpu.install_hle(0x801E8974u, [](Cpu& c) { return serve(c, Entry::Dtor); }, "S1 target2 dtor (native, hand-back)");
    cpu.install_hle(0x801E83D0u, [](Cpu& c) { return serve(c, Entry::FlagA); }, "S1 target2 flag filter A (native, hand-back)");
    cpu.install_hle(0x801E8448u, [](Cpu& c) { return serve(c, Entry::FlagB); }, "S1 target2 flag filter B (native, hand-back)");
    cpu.install_hle(0x801E8158u, [](Cpu& c) { return serve(c, Entry::SameTarget); }, "S1 target2 same-target filter (native, hand-back)");
    cpu.install_hle(0x801E82C4u, [](Cpu& c) { return serve(c, Entry::Sphere); }, "S1 target2 sphere filter (native, hand-back)");
    cpu.install_hle(0x801E84C0u, [](Cpu& c) { return serve(c, Entry::ViewCone); }, "S1 target2 view-cone filter (native, hand-back)");
    cpu.install_hle(0x801E8528u, [](Cpu& c) { return serve(c, Entry::Range); }, "S1 target2 range filter (native, hand-back)");
    cpu.install_hle(0x801E879Cu, [](Cpu& c) { return serve(c, Entry::Nearest); }, "S1 target2 nearest (native, hand-back)");
}

}  // namespace gekko::s1_target2
