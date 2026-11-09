#pragma once

// RenderWare's RwMatrixMultiply (0x8047FC08, 254 bytes of paired-single code; docs/CPU_FINAL_HAUNT.md), served
// natively. On Final Haunt it is the game thread's second-largest self-time function (3.8 % of samples).
//
//   RwMatrix* RwMatrixMultiply(RwMatrix* out r3, const RwMatrix* a r4, const RwMatrix* b r5)
//     rows right (0), up (0x10), at (0x20), pos (0x30); flags at 0x0C
//     out.row[k] = ((b.right[k] * a.row.x) + b.up[k] * a.row.y) + b.at[k] * a.row.z     (right, up, at)
//     out.pos[k] = ((b.right[k] * a.pos.x + b.pos[k]) + b.up[k] * a.pos.y) + b.at[k] * a.pos.z
//     out.flags = a.flags & b.flags; the padding words 0x1C, 0x2C, 0x3C are not written
//
// Each product is a ps_muls0/1 and each sum a ps_madds0/1 (one rounding to single, Gekko semantics).
//
// The common call is computed four lanes at a time (AVX2 fused multiply-add on doubles, then rounding to single:
// what each ppc_ops helper computes on its fast path), after proving per lane that no operation could leave that
// fast path - a tiny operand or result, a NaN, a fused double on a single-precision midpoint. The fast path changes
// no FPSCR bit, so the order of the 48 operations is then not observable. Any other call - such a lane, or an output
// overlapping an input or the stack frame - runs the ordered body below, as does every call on a host without AVX2.
//
// The ordered body is the guest body's instruction sequence with the register file held in host registers: every arithmetic
// operation is the same ppc_ops helper in the same order (so the FPSCR updates of an operation that leaves the fast
// path happen in the same order), and every load and store is made in the same order (so an output that overlaps an
// input or the stack frame reads and writes exactly what the guest's would). What a served call leaves, as the guest
// body does: the output matrix, the stack frame's words (the back chain at r1 - 0x30, f30 and f31 saved as doubles
// and as paired singles), r0 = a.flags, r4 = a.flags & b.flags, f0-f13 as the body leaves them, f30/f31 restored
// (ps1 through the paired-single save and restore), r1 unchanged.
//
// The hook declines (the guest body runs) when MSR[FP] is clear - the body raises FP-unavailable - and when GQR0
// would quantise (types 4-7): every psq_l/psq_st in the body names GQR0, which the game never sets to an integer
// type, and the native body moves single-precision words only.

#include "gekko/types.h"

namespace gekko {

class Cpu;

class MatrixMultiplyIntrinsic {
public:
    // The hook: true when the call was served, false to run the guest body.
    bool serve(Cpu& cpu);

    u64 calls() const noexcept { return calls_; }
    u64 declines() const noexcept { return declines_; }

    // --verify-matrix-multiply: every served call also runs the guest body from the same state (the native result
    // rolled back first) and compares every register and the bytes of the output, inputs and stack frame. The
    // guest body's state is the one kept.
    void set_verify(bool enabled) noexcept { verify_ = enabled; }
    u64 verified() const noexcept { return verified_; }
    u64 differed() const noexcept { return differed_; }

    u64 wide() const noexcept { return wide_; }  // calls computed four lanes at a time
    u64 overlapping() const noexcept { return overlapping_; }      // ordered: an output overlapping an input
    u64 off_fast_path() const noexcept { return off_fast_path_; }  // ordered: a lane off the fast path

private:
    bool execute(Cpu& cpu);
    bool run_wide(Cpu& cpu);  // false (nothing written) when the ordered body must run
    bool run(Cpu& cpu);       // the ordered body
    bool verify(Cpu& cpu);

    bool verify_ = false;
    u64 calls_ = 0, declines_ = 0, wide_ = 0, overlapping_ = 0, off_fast_path_ = 0, verified_ = 0, differed_ = 0;
};

}  // namespace gekko
