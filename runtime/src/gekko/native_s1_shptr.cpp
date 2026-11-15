// S1 core: the counted-handle family, native. Contract: include/gekko/native_s1_shptr.h; map: docs/re/core.md section 8.
#include "gekko/native_s1_shptr.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace gekko::s1_shptr {
namespace {

constexpr u32 kRelease = 0x8000945Cu, kScDtor = 0x80009408u, kScCopy = 0x800097E0u, kAssign = 0x80009E4Cu;
constexpr u32 kWeakDtor = 0x80017454u, kWeakCopyA = 0x80017930u, kWeakCopyB = 0x8001AEE8u;
constexpr u32 kAddRefLock = 0x80016914u, kScFromWeak = 0x80016838u, kLock = 0x80028DB0u;

// The hot per-type clones (exact instruction pattern, tools: .build/s1/scan_clones.py; docs/re/core.md section 8).
constexpr u32 kDtorClones[] = {0x80028D60u, 0x800521ECu, 0x80057850u, 0x80052DBCu, 0x8007254Cu, 0x8006C380u, 0x800BA09Cu, 0x800093B8u,
                               0x80009844u, 0x800166C8u, 0x80079B54u, 0x800A8DACu, 0x80099DCCu, 0x801C2DE4u, 0x8006DA78u, 0x8009EBFCu};
constexpr u32 kCopyClones[] = {0x80028D20u, 0x800521ACu, 0x8000D130u, 0x80052D7Cu, 0x80079BA4u, 0x800BA05Cu,
                               0x8006D83Cu, 0x80099D8Cu, 0x800A8DFCu, 0x8009F724u, 0x800F3658u, 0x80057810u};
constexpr u32 kAssignClones[] = {0x800A4068u, 0x800981A4u, 0x8008A310u};

Entry g_entries[8 + 1 + 1 + sizeof(kDtorClones) / 4 + sizeof(kCopyClones) / 4 + sizeof(kAssignClones) / 4];
unsigned g_count = 0;
signed char g_chain_state[sizeof(g_entries) / sizeof(g_entries[0])];  // 0 not asked, 1 no foreign hook on a chain member, 2 foreign
Stats g_stats;
#ifdef SHADOW_S1_SHPTR_DEFAULT
bool g_enabled = true;  // CMake option SHADOW_S1_SHPTR=ON; GEKKO_S1_SHPTR=0 disables
#else
bool g_enabled = false;  // off by default; GEKKO_S1_SHPTR=1 enables
#endif

struct Ctx {
    Cpu& c;
    u32 sp;  // r1 at entry
    u32 lr;
    u32 rd(u32 a) const { return c.mem_read32(a); }
    void wr(u32 a, u32 v) const { c.mem_write32(a, v); }
};

// The translated frame writes: stwu back chain, the LR save word, then the saved non-volatile registers.
inline void frame_small(const Ctx& x, u32 frame, u32 prev, u32 lr_save, u32 r30, u32 r31, bool two) {  // 0x10-byte frame
    x.wr(frame, prev);
    x.wr(frame + 0x14u, lr_save);
    if (two) x.wr(frame + 0x8u, r30);
    x.wr(frame + 0xCu, r31);
}

inline bool flag_deletes(u32 flag) { return static_cast<s16>(flag & 0xFFFFu) > 0; }

// ---- chain-member foreign hooks ---------------------------------------------------------------------------------------------------
bool foreign(Cpu& c, u32 address) {
    const std::string* name = c.hle_name(address);
    return name != nullptr && name->compare(0, 8, "S1 shptr") != 0;
}

bool chain_ok(Cpu& c, const Entry& e) {
    const unsigned index = static_cast<unsigned>(&e - g_entries);
    signed char& state = g_chain_state[index];
    if (state == 0) {
        static constexpr u32 kLockMembers[] = {0x80028EA0u, 0x800169ACu, 0x80028E90u, 0x80028E4Cu, kScFromWeak, kAddRefLock, 0x80028D20u,
                                               kScCopy, 0x80028D60u, kScDtor, kRelease};
        bool bad = false;
        const auto any = [&](std::initializer_list<u32> list) {
            for (u32 a : list) bad = bad || foreign(c, a);
        };
        switch (e.kind) {
            case Kind::ScDtor: any({kRelease}); break;
            case Kind::DtorWrap: any({kScDtor, kRelease}); break;
            case Kind::CopyWrap: any({kScCopy}); break;
            case Kind::AssignWrap: any({kAssign, kRelease}); break;
            case Kind::Assign: any({kRelease}); break;
            case Kind::ScFromWeak: any({kAddRefLock}); break;
            case Kind::Lock:
                for (u32 a : kLockMembers) bad = bad || foreign(c, a);
                break;
            default: break;
        }
        state = bad ? 2 : 1;
    }
    return state == 1;
}

// ---- the members -------------------------------------------------------------------------------------------------------------------
// release(pi): use--; the dispose / destroy virtuals are the guest's (handed back when the count would reach zero).
bool release_ok(const Ctx& x, u32 pi) { return pi != 0u && x.rd(pi + 4u) != 1u; }

// Writes of one release call whose frame sits at `frame` (the translated chain: back chain, LR save, r31).
void release_do(const Ctx& x, u32 pi, u32 frame, u32 prev, u32 lr_save, u32 r31) {
    x.wr(frame, prev);
    x.wr(frame + 0x14u, lr_save);
    x.wr(frame + 0xCu, r31);
    x.wr(pi + 4u, x.rd(pi + 4u) - 1u);
}

bool serve_release(Ctx& x) {
    const u32 pi = x.c.gpr[3];
    if (!release_ok(x, pi)) return false;
    release_do(x, pi, x.sp - 0x10u, x.sp, x.lr, x.c.gpr[31]);
    return true;
}

// shared_count dtor(this, flag) entered with frame owner `prev_sp`; `r30`/`r31` are the saved caller registers.
bool sc_dtor_ok(const Ctx& x, u32 self, u32 flag) {
    if (self == 0u) return true;
    const u32 pi = x.rd(self);
    if (pi != 0u && !release_ok(x, pi)) return false;
    return !flag_deletes(flag);
}

void sc_dtor_do(const Ctx& x, u32 self, u32 flag, u32 frame, u32 prev, u32 lr_save, u32 r30, u32 r31) {
    frame_small(x, frame, prev, lr_save, r30, r31, true);
    if (self == 0u) return;
    const u32 pi = x.rd(self);
    if (pi != 0u) release_do(x, pi, frame - 0x10u, frame, 0x80009434u, flag);
}

bool serve_sc_dtor(Ctx& x) {
    const u32 self = x.c.gpr[3], flag = x.c.gpr[4];
    if (!sc_dtor_ok(x, self, flag)) return false;
    sc_dtor_do(x, self, flag, x.sp - 0x10u, x.sp, x.lr, x.c.gpr[30], x.c.gpr[31]);
    x.c.gpr[3] = self;
    return true;
}

bool serve_dtor_wrap(Ctx& x, const Entry& e) {
    const u32 self = x.c.gpr[3], flag = x.c.gpr[4];
    if (flag_deletes(flag)) return false;
    if (self != 0u) {
        const u32 pi = x.rd(self + 4u);
        if (pi != 0u && !release_ok(x, pi)) return false;
    }
    const u32 f1 = x.sp - 0x10u;
    frame_small(x, f1, x.sp, x.lr, x.c.gpr[30], x.c.gpr[31], true);
    if (self != 0u) sc_dtor_do(x, self + 4u, 0xFFFFFFFFu, f1 - 0x10u, f1, e.address + 0x28u, self, flag);
    x.c.gpr[3] = self;
    return true;
}

// shared_count / weak_count copy: *dst = *src; ++count at +offset of the block.
bool serve_count_copy(Ctx& x, u32 offset) {
    const u32 dst = x.c.gpr[3], src = x.c.gpr[4];
    x.wr(dst, x.rd(src));
    const u32 pi = x.rd(dst);
    if (pi != 0u) x.wr(pi + offset, x.rd(pi + offset) + 1u);
    return true;
}

bool serve_copy_wrap(Ctx& x) {
    const u32 dst = x.c.gpr[3], src = x.c.gpr[4];
    const u32 f1 = x.sp - 0x10u;
    x.wr(f1, x.sp);
    x.wr(f1 + 0x14u, x.lr);
    x.wr(f1 + 0xCu, x.c.gpr[31]);
    x.wr(dst, x.rd(src));
    x.wr(dst + 4u, x.rd(src + 4u));
    const u32 pi = x.rd(dst + 4u);
    if (pi != 0u) x.wr(pi + 4u, x.rd(pi + 4u) + 1u);
    x.c.gpr[3] = dst;
    return true;
}

// shared_count assign(self, rhs): the new block counted first, the old released, then stored.
bool assign_ok(const Ctx& x, u32 self, u32 rhs) {
    const u32 pi = x.rd(rhs), old = x.rd(self);
    if (pi == old || old == 0u) return true;
    return release_ok(x, old);
}

void assign_do(const Ctx& x, u32 self, u32 rhs, u32 frame, u32 prev, u32 lr_save, u32 r30, u32 r31_entry) {
    frame_small(x, frame, prev, lr_save, r30, r31_entry, true);
    const u32 pi = x.rd(rhs), old = x.rd(self);
    if (pi != old) {
        if (pi != 0u) x.wr(pi + 4u, x.rd(pi + 4u) + 1u);
        if (old != 0u) release_do(x, old, frame - 0x10u, frame, 0x80009E94u, pi);
        x.wr(self, pi);
    }
}

bool serve_assign(Ctx& x) {
    const u32 self = x.c.gpr[3], rhs = x.c.gpr[4];
    if (!assign_ok(x, self, rhs)) return false;
    assign_do(x, self, rhs, x.sp - 0x10u, x.sp, x.lr, x.c.gpr[30], x.c.gpr[31]);
    x.c.gpr[3] = self;
    return true;
}

bool serve_assign_wrap(Ctx& x, const Entry& e) {
    const u32 dst = x.c.gpr[3], src = x.c.gpr[4];
    if (!assign_ok(x, dst + 4u, src + 4u)) return false;  // (the px store at dst cannot alias the counts)
    const u32 f1 = x.sp - 0x10u;
    x.wr(f1, x.sp);
    x.wr(f1 + 0x14u, x.lr);
    x.wr(f1 + 0xCu, x.c.gpr[31]);
    x.wr(dst, x.rd(src));
    assign_do(x, dst + 4u, src + 4u, f1 - 0x10u, f1, e.address + 0x28u, x.c.gpr[30], dst);
    x.c.gpr[3] = dst;
    return true;
}

bool serve_weak_dtor(Ctx& x) {
    const u32 self = x.c.gpr[3], flag = x.c.gpr[4];
    if (flag_deletes(flag)) return false;
    u32 pi = 0;
    if (self != 0u) {
        pi = x.rd(self);
        if (pi != 0u && x.rd(pi + 8u) == 1u) return false;  // the destroy virtual
    }
    frame_small(x, x.sp - 0x10u, x.sp, x.lr, x.c.gpr[30], x.c.gpr[31], true);
    if (pi != 0u) x.wr(pi + 8u, x.rd(pi + 8u) - 1u);
    x.c.gpr[3] = self;
    return true;
}

bool serve_add_ref_lock(Ctx& x) {
    const u32 pi = x.c.gpr[3];
    const u32 use = x.rd(pi + 4u);
    if (use == 0u) return false;  // bad_weak_ptr is thrown
    const u32 f = x.sp - 0x20u;
    x.wr(f, x.sp);
    x.wr(f + 0x24u, x.lr);
    x.wr(f + 0x1Cu, x.c.gpr[31]);
    x.wr(pi + 4u, use + 1u);
    x.c.gpr[3] = use;
    return true;
}

bool serve_sc_from_weak(Ctx& x) {
    const u32 dst = x.c.gpr[3], src = x.c.gpr[4];
    const u32 pi = x.rd(src);
    if (pi == 0u || x.rd(pi + 4u) == 0u) return false;  // throws bad_weak_ptr
    const u32 f = x.sp - 0x20u;
    x.wr(f, x.sp);
    x.wr(f + 0x24u, x.lr);
    x.wr(f + 0x1Cu, x.c.gpr[31]);
    x.wr(dst, pi);
    const u32 f2 = f - 0x20u;
    x.wr(f2, f);
    x.wr(f2 + 0x24u, 0x80016864u);
    x.wr(f2 + 0x1Cu, dst);
    x.wr(pi + 4u, x.rd(pi + 4u) + 1u);
    x.c.gpr[3] = dst;
    return true;
}

// weak_ptr::lock(out, weak) of the type at 0x80028DB0 (its helpers are at fixed addresses).
bool serve_lock(Ctx& x) {
    const u32 out = x.c.gpr[3], weak = x.c.gpr[4];
    const u32 pi = x.rd(weak + 4u);
    const u32 use = pi != 0u ? x.rd(pi + 4u) : 0u;
    const bool expired = use == 0u;
    if (!expired && use + 1u == 0u) return false;  // the temporaries' release would reach zero
    const u32 r28 = x.c.gpr[28], r29 = x.c.gpr[29], r30 = x.c.gpr[30], r31 = x.c.gpr[31];
    const u32 f1 = x.sp - 0x30u;
    x.wr(f1, x.sp);
    x.wr(f1 + 0x34u, x.lr);
    x.wr(f1 + 0x20u, r28);
    x.wr(f1 + 0x24u, r29);
    x.wr(f1 + 0x28u, r30);
    x.wr(f1 + 0x2Cu, r31);
    // 0x80028EA0 (expired?) called with LR 0x80028DD8
    x.wr(f1 - 0x10u, f1);
    x.wr(f1 - 0x10u + 0x14u, 0x80028DD8u);
    if (expired) {
        x.wr(f1 + 0x10u, 0);  // 0x80028E90: an empty shared_ptr
        x.wr(f1 + 0x14u, 0);
        // copy constructor (out, tmp)
        x.wr(f1 - 0x10u, f1);
        x.wr(f1 - 0x10u + 0x14u, 0x80028E10u);
        x.wr(f1 - 0x10u + 0xCu, weak);
        x.wr(out, x.rd(f1 + 0x10u));
        x.wr(out + 4u, x.rd(f1 + 0x14u));
        // dtor of tmp (flag -1): nothing to release
        x.wr(f1 - 0x10u, f1);
        x.wr(f1 - 0x10u + 0x14u, 0x80028E38u);
        x.wr(f1 - 0x10u + 0x8u, out);
        x.wr(f1 - 0x10u + 0xCu, weak);
        x.wr(f1 - 0x20u, f1 - 0x10u);
        x.wr(f1 - 0x20u + 0x14u, 0x80028D88u);
        x.wr(f1 - 0x20u + 0x8u, f1 + 0x10u);
        x.wr(f1 - 0x20u + 0xCu, 0xFFFFFFFFu);
        x.c.gpr[3] = f1 + 0x10u;
        return true;
    }
    // 0x80028E4C(tmp = f1 + 8, weak): shared_count from the weak count, then the px
    x.wr(f1 - 0x10u, f1);
    x.wr(f1 - 0x10u + 0x14u, 0x80028E00u);
    x.wr(f1 - 0x10u + 0x8u, out);
    x.wr(f1 - 0x10u + 0xCu, weak);
    x.wr(f1 - 0x30u, f1 - 0x10u);  // 0x80016838 (LR 0x80028E70)
    x.wr(f1 - 0x30u + 0x24u, 0x80028E70u);
    x.wr(f1 - 0x30u + 0x1Cu, weak);
    x.wr(f1 + 0xCu, pi);
    x.wr(f1 - 0x50u, f1 - 0x30u);  // 0x80016914 (LR 0x80016864)
    x.wr(f1 - 0x50u + 0x24u, 0x80016864u);
    x.wr(f1 - 0x50u + 0x1Cu, f1 + 0xCu);
    x.wr(pi + 4u, x.rd(pi + 4u) + 1u);
    x.wr(f1 + 8u, x.rd(weak));
    // copy constructor (out, tmp)
    x.wr(f1 - 0x10u, f1);
    x.wr(f1 - 0x10u + 0x14u, 0x80028E10u);
    x.wr(f1 - 0x10u + 0xCu, weak);
    x.wr(out, x.rd(f1 + 8u));
    x.wr(out + 4u, x.rd(f1 + 0xCu));
    x.wr(pi + 4u, x.rd(pi + 4u) + 1u);
    // dtor of tmp (flag -1): the release
    x.wr(f1 - 0x10u, f1);
    x.wr(f1 - 0x10u + 0x14u, 0x80028E24u);
    x.wr(f1 - 0x10u + 0x8u, out);
    x.wr(f1 - 0x10u + 0xCu, weak);
    x.wr(f1 - 0x20u, f1 - 0x10u);
    x.wr(f1 - 0x20u + 0x14u, 0x80028D88u);
    x.wr(f1 - 0x20u + 0x8u, f1 + 8u);
    x.wr(f1 - 0x20u + 0xCu, 0xFFFFFFFFu);
    x.wr(f1 - 0x30u, f1 - 0x20u);
    x.wr(f1 - 0x30u + 0x14u, 0x80009434u);
    x.wr(f1 - 0x30u + 0xCu, 0xFFFFFFFFu);
    x.wr(pi + 4u, x.rd(pi + 4u) - 1u);
    x.c.gpr[3] = f1 + 8u;
    return true;
}

}  // namespace

const Stats& stats() noexcept { return g_stats; }
void set_enabled(bool enabled) noexcept { g_enabled = enabled; }
void reset_chain_cache() noexcept {
    for (signed char& v : g_chain_state) v = 0;
}
const Entry* entries(unsigned& count) noexcept {
    count = g_count;
    return g_entries;
}

bool serve(Cpu& c, const Entry& e) {
    if (!g_enabled) return false;
    const unsigned k = static_cast<unsigned>(e.kind);
    Ctx x{c, c.gpr[1], c.lr};
    bool ok = false;
    if (chain_ok(c, e)) {
        switch (e.kind) {
            case Kind::Release: ok = serve_release(x); break;
            case Kind::ScDtor: ok = serve_sc_dtor(x); break;
            case Kind::ScCopy: ok = serve_count_copy(x, 4u); break;
            case Kind::WeakCopy: ok = serve_count_copy(x, 8u); break;
            case Kind::Assign: ok = serve_assign(x); break;
            case Kind::WeakDtor: ok = serve_weak_dtor(x); break;
            case Kind::ScFromWeak: ok = serve_sc_from_weak(x); break;
            case Kind::AddRefLock: ok = serve_add_ref_lock(x); break;
            case Kind::Lock: ok = serve_lock(x); break;
            case Kind::DtorWrap: ok = serve_dtor_wrap(x, e); break;
            case Kind::CopyWrap: ok = serve_copy_wrap(x); break;
            case Kind::AssignWrap: ok = serve_assign_wrap(x, e); break;
            case Kind::kCount: break;
        }
    }
    ++(ok ? g_stats.served[k] : g_stats.declined[k]);
    return ok;
}

namespace {
void print_stats() {
    static const char* const names[] = {"release", "sc_dtor", "sc_copy", "weak_copy", "assign", "weak_dtor", "sc_from_weak",
                                        "add_ref_lock", "lock", "dtor_wrap", "copy_wrap", "assign_wrap"};
    for (unsigned k = 0; k < static_cast<unsigned>(Kind::kCount); ++k)
        std::fprintf(stderr, "s1 shptr %-13s served %llu handed back %llu\n", names[k], static_cast<unsigned long long>(g_stats.served[k]),
                     static_cast<unsigned long long>(g_stats.declined[k]));
}
}  // namespace

void install(Cpu& cpu) {
    if (const char* v = std::getenv("GEKKO_S1_SHPTR_STATS"); v != nullptr && v[0] == '1') std::atexit(print_stats);
    if (const char* v = std::getenv("GEKKO_S1_SHPTR"); v != nullptr && (v[0] == '0' || v[0] == '1')) g_enabled = v[0] == '1';
    g_count = 0;
    const auto add = [&](u32 address, Kind kind) { g_entries[g_count++] = Entry{address, kind}; };
    add(kRelease, Kind::Release);
    add(kScDtor, Kind::ScDtor);
    add(kScCopy, Kind::ScCopy);
    add(kAssign, Kind::Assign);
    add(kWeakDtor, Kind::WeakDtor);
    add(kWeakCopyA, Kind::WeakCopy);
    add(kWeakCopyB, Kind::WeakCopy);
    add(kAddRefLock, Kind::AddRefLock);
    add(kScFromWeak, Kind::ScFromWeak);
    add(kLock, Kind::Lock);
    for (u32 a : kDtorClones) add(a, Kind::DtorWrap);
    for (u32 a : kCopyClones) add(a, Kind::CopyWrap);
    for (u32 a : kAssignClones) add(a, Kind::AssignWrap);
    // Hooks only where a hook replaces a chain: the leaves (shared_count / weak_count copy: 22.6M calls of 5-9 instructions on the tape),
    // the weak_count dtor (24.4M single-level calls) and the entries that always hand back (release, add_ref_lock) cost more in the
    // dispatch than their guest bodies. GEKKO_S1_SHPTR_ALL=1 hooks every entry (for timing the difference).
    const char* all = std::getenv("GEKKO_S1_SHPTR_ALL");
    const bool every = all != nullptr && all[0] == '1';
    for (unsigned i = 0; i < g_count; ++i) {
        const Entry* e = &g_entries[i];
        const bool leaf = e->kind == Kind::Release || e->kind == Kind::ScCopy || e->kind == Kind::WeakCopy || e->kind == Kind::WeakDtor ||
                          e->kind == Kind::AddRefLock;
        if (leaf && !every) continue;
        cpu.install_hle(e->address, [e](Cpu& c) { return serve(c, *e); }, "S1 shptr (native)");
    }
}

}  // namespace gekko::s1_shptr
