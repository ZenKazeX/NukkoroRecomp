#pragma once

// The policy the task-force fast bodies (recovered/.../fast/*_fast.h, interface `bool fast_XXXXXXXX(M&, Regs&)`) run on
// in the game: the machine's memory window and the translated code's own float helpers (the same ppc_ops.h calls the
// lifted sweep closure uses, gekko/native_lifted_sweep.cpp), plus calls out of a fast body through guest_call with the
// whole register file handed over.
//
// Register file: an owner copies the whole machine register file into a Regs (enter) before the body and back (leave)
// after it, so a fast body sees and leaves every register as the original instructions would.
//
// Calls out (external / indirect): the Regs go into the machine, the callee runs through guest_call (link = the return
// address the body set in s.lr), the machine's registers come back into the Regs. guest_call restores cr, lr, r1 and
// r14..r31 after the callee (native_guest_call.h); the original keeps the callee's volatile CR fields. Nothing read
// after a call depends on them (a compiled caller never reads a volatile CR field across a call). A callee that does
// not return (stop, yield) throws CallAborted: the owner catches it and leaves the machine stopped.

#include "gekko/cpu.h"
#include "gekko/native_guest_call.h"
#include "gekko/ppc_ops.h"
#include "gekko/types.h"

#include <cstdint>

namespace shadow::lifted {
#ifndef SHADOW_LIFTED_REGS
#define SHADOW_LIFTED_REGS
using u32 = std::uint32_t; using s32 = std::int32_t; using u64 = std::uint64_t;
struct Regs { u32 r[32]; u64 f[32]; u64 p[32]; u32 cr, lr, ctr, ca; };
inline u32 icmp(s32 a, s32 b) noexcept { return a < b ? 8u : a > b ? 4u : 2u; }
inline u32 ucmp(u32 a, u32 b) noexcept { return a < b ? 8u : a > b ? 4u : 2u; }
#endif
}  // namespace shadow::lifted

namespace gekko::fast_policy {

using shadow::lifted::Regs;

struct CallAborted {};

// Runs the guest function at `target` with the machine's registers as they are (lr = `link` already set) until it
// returns to `link`. False: the machine stopped or the callee did not return (a yield); the run is stopping.
// Defined in native_fast_owners.cpp (production: the translated code; reference build: the interpreter, nested).
bool call_out(Cpu& c, u32 target, u32 link);

GEKKO_ALWAYS_INLINE void enter(const Cpu& c, Regs& s) {
    for (u32 i = 0; i < 32u; ++i) {
        s.r[i] = c.gpr[i];
        s.f[i] = c.fpr[i].bits[0];
        s.p[i] = c.fpr[i].bits[1];
    }
    s.cr = c.cr;
    s.lr = c.lr;
    s.ctr = c.ctr;
    s.ca = (c.xer >> 29) & 1u;
}

GEKKO_ALWAYS_INLINE void leave(Cpu& c, const Regs& s) {
    for (u32 i = 0; i < 32u; ++i) {
        c.gpr[i] = s.r[i];
        c.fpr[i].bits[0] = s.f[i];
        c.fpr[i].bits[1] = s.p[i];
    }
    c.cr = s.cr;
    c.lr = s.lr;
    c.ctr = s.ctr;
    c.xer = (c.xer & ~0x20000000u) | (s.ca << 29);
}

struct Policy {
    Cpu& c;
    GEKKO_ALWAYS_INLINE u32 r8(u32 a) { return c.mem_read8(a); }
    GEKKO_ALWAYS_INLINE u32 r16(u32 a) { return c.mem_read16(a); }
    GEKKO_ALWAYS_INLINE u32 r32(u32 a) { return c.mem_read32(a); }
    GEKKO_ALWAYS_INLINE u64 r64(u32 a) { return (static_cast<u64>(c.mem_read32(a)) << 32) | c.mem_read32(a + 4u); }
    GEKKO_ALWAYS_INLINE void w8(u32 a, u32 v) { c.mem_write8(a, static_cast<u8>(v)); }
    GEKKO_ALWAYS_INLINE void w16(u32 a, u32 v) { c.mem_write16(a, static_cast<u16>(v)); }
    GEKKO_ALWAYS_INLINE void w32(u32 a, u32 v) { c.mem_write32(a, v); }
    GEKKO_ALWAYS_INLINE void w64(u32 a, u64 v) { c.mem_write32(a, static_cast<u32>(v >> 32)); c.mem_write32(a + 4u, static_cast<u32>(v)); }
    GEKKO_ALWAYS_INLINE u64 lfs(u32 a) { return single_to_double_bits(c.mem_read32(a)); }
    GEKKO_ALWAYS_INLINE void stfs(u32 a, u64 v) { c.mem_write32(a, double_to_single_bits(v)); }
    GEKKO_ALWAYS_INLINE u32 fcmp(u64 a, u64 b, bool ordered) {
        // field 0 of the machine's CR as scratch, as in native_lifted_sweep.cpp: leave() writes the whole CR back
        ppc::fp_compare_inl(c, 0u, a, b, ordered);
        return c.cr >> 28;
    }
    GEKKO_ALWAYS_INLINE u64 fadds(u64 a, u64 b) { return ppc::fps_add(c, a, b); }
    GEKKO_ALWAYS_INLINE u64 fsubs(u64 a, u64 b) { return ppc::fps_sub(c, a, b); }
    GEKKO_ALWAYS_INLINE u64 fmuls(u64 a, u64 x) { return ppc::fps_mul(c, a, x); }
    GEKKO_ALWAYS_INLINE u64 fdivs(u64 a, u64 b) { return ppc::fps_div(c, a, b); }
    GEKKO_ALWAYS_INLINE u64 fadd(u64 a, u64 b) { return ppc::fp_add(c, a, b); }
    GEKKO_ALWAYS_INLINE u64 fsub(u64 a, u64 b) { return ppc::fp_sub(c, a, b); }
    GEKKO_ALWAYS_INLINE u64 fmul(u64 a, u64 x) { return ppc::fp_mul(c, a, x); }
    GEKKO_ALWAYS_INLINE u64 fdiv(u64 a, u64 b) { return ppc::fp_div(c, a, b); }
    GEKKO_ALWAYS_INLINE u64 fmadds(u64 a, u64 x, u64 b) { return ppc::fps_madd(c, a, x, b); }
    GEKKO_ALWAYS_INLINE u64 fmsubs(u64 a, u64 x, u64 b) { return ppc::fps_msub(c, a, x, b); }
    GEKKO_ALWAYS_INLINE u64 fnmadds(u64 a, u64 x, u64 b) { return ppc::fps_nmadd(c, a, x, b); }
    GEKKO_ALWAYS_INLINE u64 fnmsubs(u64 a, u64 x, u64 b) { return ppc::fps_nmsub(c, a, x, b); }
    GEKKO_ALWAYS_INLINE u64 fmadd(u64 a, u64 x, u64 b) { return ppc::fp_madd(c, a, x, b); }
    GEKKO_ALWAYS_INLINE u64 fmsub(u64 a, u64 x, u64 b) { return ppc::fp_msub(c, a, x, b); }
    GEKKO_ALWAYS_INLINE u64 fnmadd(u64 a, u64 x, u64 b) { return ppc::fp_nmadd(c, a, x, b); }
    GEKKO_ALWAYS_INLINE u64 fnmsub(u64 a, u64 x, u64 b) { return ppc::fp_nmsub(c, a, x, b); }
    GEKKO_ALWAYS_INLINE u64 frsp(u64 b) { return ppc::fps_round(c, b); }
    GEKKO_ALWAYS_INLINE u64 fres(u64 b) { return ppc::fps_res(c, b); }
    GEKKO_ALWAYS_INLINE u64 frsqrte(u64 b) { return ppc::fp_rsqrte(c, b); }
    GEKKO_ALWAYS_INLINE u64 fctiw(u64 b) { return ppc::fctiw(c, b, false); }
    GEKKO_ALWAYS_INLINE u64 fctiwz(u64 b) { return ppc::fctiw(c, b, true); }
    void psq_l(u32 a, u32 w, u32 gqr, u64& f, u64& p) {
        const FpReg keep = c.fpr[0];
        ppc::psq_load_inl(c, 0u, a, w != 0u, gqr);
        f = c.fpr[0].bits[0];
        p = c.fpr[0].bits[1];
        c.fpr[0] = keep;
    }
    void psq_st(u32 a, u32 w, u32 gqr, u64 f, u64 p) {
        const FpReg keep = c.fpr[0];
        c.fpr[0].bits[0] = f;
        c.fpr[0].bits[1] = p;
        ppc::psq_store(c, 0u, a, w != 0u, gqr);
        c.fpr[0] = keep;
    }
    // A call out of the body: bl / bctrl to `target` with the registers in `s`, the return address in s.lr. Unlike
    // guest_call no frame of its own is pushed and no register is restored: the callee runs on r1 exactly as the
    // original's bl leaves it (its frames land on the original's stack words) and leaves its volatile registers.
    void external(u32 target, Regs& s) {
        leave(c, s);
        if (!call_out(c, target, s.lr)) throw CallAborted{};
        enter(c, s);
    }
    void indirect(u32 target, Regs& s) { external(target, s); }
};

}  // namespace gekko::fast_policy
