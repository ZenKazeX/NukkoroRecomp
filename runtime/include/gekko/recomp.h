#pragma once

// The interface between statically recompiled guest code and the runtime.
//
// tools/ppcrecomp reads the original main.dol and emits one native C++ function
// per guest function. Those functions include this header and nothing else:
// everything they are allowed to touch is declared here.
//
// Rules the generated code obeys, so that the interpreter and the recompiled
// code are interchangeable at any function boundary:
//
//   * All guest state lives in the Cpu object. Nothing is cached across a call
//     or across an exit, so the guest is consistent at every boundary.
//   * Guest memory is reached only through Cpu::mem_read*/mem_write*, which
//     translate through the BATs and go to the same Bus the interpreter uses.
//     MMIO therefore behaves identically.
//   * Nothing counts instructions. Guest time is the game clock, which only a
//     wait point advances (docs/native-time-20261005/DESIGN.md); the time base
//     and the decrementer are views of it, identical for both engines.
//   * Any control transfer the generator could not resolve statically leaves
//     through leave_to_dispatcher. Native frames unwind without executing any
//     further guest instructions and the interpreter continues from the exact
//     guest address that was next.

#include "gekko/cpu.h"
#include "gekko/ppc_ops.h"

namespace gekko {
namespace recomp {

// Defined by the generated translation unit, or by recomp_table_empty.cpp when
// the build has no game image to recompile from.
extern const RecompiledCode::Entry kFunctions[];
extern const std::size_t kFunctionCount;
// Sorted, non-overlapping spans of translated instructions, each with an entry
// point that starts its function at any instruction inside the span.
extern const RecompiledCode::Range kRanges[];
extern const std::size_t kRangeCount;
// Guest functions whose original body the generator did not emit because a
// runtime handler (a native owner) serves every call; their generated entry is
// a thunk to Cpu::serve_native_owner. Startup refuses to run without them.
extern const u32 kNativeOwners[];
extern const std::size_t kNativeOwnerCount;
// Every listed GX write-gather pipe site (extract/gx-sites.txt), sorted by
// address: what the generator lowered each store to (gekko/gx_sites.h). The
// interpreter applies the same table; System installs it (Cpu::set_gx_sites).
extern const GxSiteRecord kGxSites[];
extern const std::size_t kGxSiteCount;
// Every listed platform site (extract/platform-sites.txt), sorted by address:
// what the generator lowered each lhz / sth to (gekko/platform_sites.h). The
// interpreter applies the same table; System installs it
// (Cpu::set_platform_site_table).
extern const PlatformSiteRecord kPlatformSites[];
extern const std::size_t kPlatformSiteCount;
// Every lowered call site (extract/lift-sites.txt), sorted by address, with its
// generated lowering (gekko/lift_sites.h). System installs it
// (Cpu::set_lift_sites); the dispatcher and the interpreter run the same
// function the translated code runs at the site.
extern const LiftSiteRecord kLiftSites[];
extern const std::size_t kLiftSiteCount;
#if !defined(GEKKO_SHIPPING)
// Validation-only original words, in the same order. Dispatch never needs them.
extern const u32 kLiftSiteWords[];
#endif

// Branch to a register-held target with a link (bctrl, and calls whose target
// the generator could not prove). If the target has been recompiled the call is
// made natively; otherwise the dispatcher resumes at the target with the link
// register already set, and every native frame unwinds. The call is recorded by
// the generated code before the branch, so this only performs the
// transfer. A linked call: the emitted site runs the callee's published
// transfers after it (Cpu::finish_transfers).
inline void call_indirect(Cpu& c, u32 target, u32 link) {
    c.lr = link;
    if (const RecompiledCode* table = c.recompiled()) {
        if (const RecompiledCode::Fn native = table->lookup(target)) {
            native(c);
            return;
        }
    }
    c.leave_to_dispatcher(target);
}

// Branch to a register-held target without a link (bctr, blr to a non-return
// target, and unresolved tail jumps). Every emitted site is
// `recomp::jump_indirect(c, tmp); return;`.
//
// R12 (2026-09-28): the target is PUBLISHED (Cpu::transfer_to) and the issuing
// body returns; the frame waiting for a return runs it (Cpu::finish_transfers).
// The 2026-09-22 trampoline failed because its publish went to a side channel
// that live `bl` callers did not consume: they unwound as though the callee had
// returned. Here the callers ARE the consumers - every generated linked-call
// site is `fn(c); if (c.exiting() && c.finish_transfers()) return;` and runs
// the chain before its own R2 check and its `c.pc != link` test, so a caller
// frame is never unwound by a transfer.
inline void jump_indirect(Cpu& c, u32 target) {
    if (const RecompiledCode* table = c.recompiled()) {
        if (const RecompiledCode::Fn native = table->lookup(target)) {
            ++c.native_transfer_calls;
            c.transfer_to(native);
            return;
        }
    }
    ++c.native_transfer_leaves;
    c.leave_to_dispatcher(target);
}

// A callee returned somewhere other than the link address. That is ordinary
// guest behaviour - a trampoline, a dispatcher, a longjmp, a context switch -
// and the caller simply has nothing left to do. Guest control is at c.pc, so it
// continues there natively whenever native code exists for it: published like
// jump_indirect (R12), after the caller's R2 check has already run.
inline void resume_elsewhere(Cpu& c, u32 site) {
    const u32 target = c.pc;
    if (const RecompiledCode* table = c.recompiled()) {
        if (const RecompiledCode::Fn native = table->lookup(target)) {
            ++c.native_transfer_calls;
            c.transfer_to(native);
            return;
        }
    }
    ++c.native_transfer_leaves;
    c.leave_to_dispatcher(target, FallbackReason::UnexpectedReturn, site);
}

// A trap that fired is a fault the generated code does not model. It exits so
// the interpreter raises the exception itself.
inline void exit_at(Cpu& c, u32 address) { c.leave_to_dispatcher(address); }

// ---- supervisor transfers ------------------------------------------------
//
// sc and rfi are native: the OS's exception entry is done by the CPU
// (os_exception_entry.cpp - no vector page is executed, nothing runs in real
// mode), and an rfi's destination is genuinely dynamic - an SRR0 the guest
// chose - so both go through the same translated-address lookup any indirect
// branch uses.

// sc: the system call exception. Unlike the fault-style exceptions this
// resumes at the instruction *after* sc.
inline void system_call(Cpu& c, u32 address) {
    c.system_call(address);
    c.leave_to_dispatcher(c.npc, FallbackReason::SystemCall, address);
}

// Floating-point unavailable, OS exception index 7.
//
// The GameCube OS switches FPU context LAZILY. On a thread switch it clears
// MSR[FP] and does NOT save or load the floating-point registers; it relies on
// the resumed thread's next floating-point instruction faulting into its
// handler, which saves the outgoing thread's FPRs, loads the incoming thread's,
// sets MSR[FP] and returns to re-execute the instruction. The game installs that
// handler in __OSExceptionTable[7] (the OS entry is native:
// os_exception_entry.cpp).
//
// Without this fault the recompiled code executes floating-point regardless of
// MSR[FP], the guest handler never runs, and a resumed thread keeps whatever
// FPRs the previously-running thread left behind. That is silent and delayed:
// it only becomes visible when the stale registers happen to matter. It cost
// this port a hard freeze - EffectParticle*::vf2 (0x80434824) resumed with
// f2 = f4 = 0.0 where it had loaded 2.0 and pi, so its rotation-wrap loop
// computed a step of 0.0 and could never terminate.
//
// This is a FAULT, not a trap-after: raise_exception copies c.pc into SRR0, so
// pointing pc at the faulting instruction makes the handler's rfi re-execute it
// once MSR[FP] is on. Contrast system_call above, which resumes at address + 4.
inline void fp_unavailable(Cpu& c, u32 address) {
    c.pc = address;
    c.raise_exception(Exception::FpUnavailable);
    c.leave_to_dispatcher(c.npc, FallbackReason::Trap, address);
}

// A Gekko extension executed while HID2 disables it (audit paired-single-10):
// dcbz_l with LCE = 0, psq_l/psq_lu/psq_st/psq_stu with LSQE = 0 or PSE = 0,
// any other paired-single instruction with PSE = 0 (PAGE 90). That is the
// illegal-instruction program exception, 0x700: a fault like fp_unavailable -
// the instruction does not execute and SRR0 addresses it - with SRR1 bit 12
// (kSrr1IllegalInstruction) set.
inline void illegal_instruction(Cpu& c, u32 address) {
    c.pc = address;
    c.raise_exception(Exception::Program, kSrr1IllegalInstruction);
    c.leave_to_dispatcher(c.npc, FallbackReason::Trap, address);
}

// rfi: restore the architected MSR bits from SRR1 and resume at SRR0.
inline void return_from_interrupt(Cpu& c, u32 address) {
    const u32 saved = c.spr[SPR_SRR1];
    c.set_msr(((c.msr() & ~0x87c0ff73u) | (saved & 0x87c0ff73u)) & ~kMsrPOW);
    c.restore_interrupt_fpr();
    c.leave_to_dispatcher(c.spr[SPR_SRR0] & ~3u, FallbackReason::ReturnFromInterrupt, address);
}

}  // namespace recomp
}  // namespace gekko
