#pragma once

// The seam for a native owner that must call the GAME's own functions (RenderWare core: RwStreamRead, RwRasterCreate, ...)
// instead of reimplementing them (docs/agents/gx-rw-plan.md, "rwR2 step 2: the guest-call seam").
//
//   u32 result;
//   if (!guest_call(c, 0x8048AEC4u, kLink, {width, height, depth, flags}, result)) return false;   // the run is stopped
//
// The call runs the guest function with the PowerPC calling convention: arguments in r3.., a new frame, `link` as the
// return address; the callee's result is r3. A native owner calls only RenderWare functions this way, never an SDK one
// (the removal planner's --max-sdk-calls is measured on the live graph, including what the callee reaches).
//
//  - Production (translated code): the target is called natively through the recompiled table (recomp::call_indirect, as
//    chara_colli_intrinsic's call_guest does). A target without translated code cannot be run: that is an ExecutionFault,
//    never a cut that resumes the owner halfway.
//  - Reference build (no translated code): the interpreter runs nested until pc == link. The step bound is a loud
//    ExecutionFault when reached; the caller gets false and no partial result (r3 is left alone).
//
// On true the caller's r1, lr, cr and the non-volatile registers r14..r31 are as they were before the call (the callee
// preserves them by the ABI; this restores them anyway, so a misbehaving callee is visible as a changed r3 only). r0 and
// r3..r12 are the callee's.
//
// LIMIT: a callee must not reach OSYieldThread / OSSleepThread / leave_to_dispatcher. A yield hands the CPU to the dispatcher, the
// callee does not return to `link`, and the call faults ("did not return to its link"; guest_call_tests runs a callee that
// yields). Found live: RwStreamRead on a file stream yields inside the RtGCNFS read. Call only functions that cannot block
// (raster / texture create and destroy, name setters, a read of a memory stream).
//
// `link` must be an address inside the owner's original function: a return address that is never fetched (production
// returns into the owner's C++, the reference build stops at it).

#include "gekko/cpu.h"
#include "gekko/types.h"

#include <initializer_list>

namespace gekko {

// The nested interpreter's bound, in dispatches (reference build only). The tests log it; the largest RenderWare function
// the texture reader calls (RwRasterCreate through the level allocator) takes well under a tenth of it.
constexpr u64 kGuestCallStepBound = 4'000'000ull;

// Temporaries that an owner passes BY ADDRESS to a guest_call (an RwError record, an out parameter, a small struct) must not sit
// at or below the owner's r1: guest_call builds the callee's frame just under r1, and the callee's own frames and register saves
// grow down from there, so anything written below r1 is overwritten by the callee. Found live (native-boot, a run its contract
// test had missed). GuestFrame reserves `size` bytes ABOVE the r1 that guest_call will use and puts r1 below them:
//
//   GuestFrame frame(c, 0x20);                        // r1 moves down; restored when `frame` leaves scope
//   c.mem_write32(frame.base(), 1u);                  // the temporary lives at frame.base() .. base() + size
//   if (!guest_call(c, target, link, {frame.base()}, result)) return false;
//
// base() is 16-byte aligned and lies below the caller's r1 and above the new r1, so no callee frame reaches it. The destructor
// restores r1 on every exit path (including a faulting guest_call). An owner that only passes values (registers) needs no frame.
class GuestFrame {
public:
    GuestFrame(Cpu& c, u32 size) : c_(c), sp0_(c.gpr[1]) {
        const u32 reserve = (size + 0xFu) & ~0xFu;
        sp_ = (sp0_ - reserve - 0x10u) & ~0xFu;      // the new r1: back chain word at sp_, the reserved bytes from sp_ + 0x10
        c_.mem_write32(sp_, sp0_);
        c_.gpr[1] = sp_;
    }
    ~GuestFrame() { c_.gpr[1] = sp0_; }
    GuestFrame(const GuestFrame&) = delete;
    GuestFrame& operator=(const GuestFrame&) = delete;
    u32 base() const { return sp_ + 0x10u; }

private:
    Cpu& c_;
    u32 sp0_, sp_ = 0;
};

// Defined in native_guest_call.cpp, compiled once per runtime build: the production library has no interpreter, the
// reference library has no translated code, and a header-inline body would be compiled with the wrong choice by a
// client that does not define SHADOW_CPU_REFERENCE_BUILD.
bool guest_call(Cpu& c, u32 target, u32 link, std::initializer_list<u32> args, u32& result,
                u64 step_bound = kGuestCallStepBound);

}  // namespace gekko
