#pragma once

// The Dolphin SDK's PSMTXConcat (mtx.c, paired singles; main.dol PAL GUPP8P 0x8037976C, 204 bytes), owned natively
// (docs/hal/HAL_DETACHMENT.md section 4 step 1). Its readable reconstruction and differential test are
// recovered/sdk/mtx/mtx.h and recovered/sdk/mtx/test/mtx_test.cpp. The one live translated caller is 0x804A7334 (a
// material's GX state, docs/NATIVE_WINDOWS_MIGRATION.md hand recovery 109).
//
//   0x8037976C PSMTXConcat(r3 = a, r4 = b, r5 = ab):  ab = a x b, 3x4 matrices (rows of x y z translation, single floats)
//       ab[i][j] = fma(b[2][j], a[i][2], fma(b[1][j], a[i][1], b[0][j] * a[i][0]))      every step rounded to single once
//       columns 2 and 3 take a fourth fused step with the pair unit01 = (0.0, 1.0) read from sdata2 0x805EEC18:
//       ab[i][2] = fma(unit01[0], a[i][3], ...)   (the quirk: a[i][3] infinite or NaN makes ab[i][2] NaN; -0 becomes +0)
//       ab[i][3] = fma(unit01[1], a[i][3], ...)   (= a[i][3] + sum, rounded once)
//   The original reads every word of a and b before its first store to ab, so ab may equal or overlap a or b.
//
// What a call leaves, as the original's: the twelve stored words; the dead frame it writes below SP (back chain at SP-0x40, f14 /
// f15 / f31 ps0 as doubles at SP-0x38 / -0x30 / -0x18); the three callee-saved registers it borrows restored by lfd, which
// restores ps0 only, so their ps1 keep what the original left there: f14.ps1 = ab[1][1], f15.ps1 = ab[1][3], f31.ps1 =
// unit01[1]. The quantised loads and stores go through GQR0 at the call. NOT kept: the volatile scratch the original uses
// (r6 = the constant's address, f0..f13). Nothing else is written (no FPSCR bit, CR, LR or CTR).

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::math_mtx {

constexpr u32 kConcat = 0x8037976Cu;
constexpr u32 kUnit01 = 0x805EEC18u;  // sdata2: the paired-single constant (0.0f, 1.0f)

void serve_concat(Cpu& cpu);
// The original sequence alone (no fast path), on the guest's registers in its instruction order.
void serve_concat_exact(Cpu& cpu);

// The fast path, exposed for the contract test (it counts the calls the fast path accepts). Returns true when it served the
// call (frame, results and the three ps1 lanes written), false when nothing was written and the exact path must run.
namespace detail {
bool concat_fast(Cpu& cpu, u32 a, u32 b, u32 ab);
}  // namespace detail

struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kConcat, "PSMTXConcat"}};
        count = 1;
        return kAddresses;
    }
};

void install(Cpu& cpu);

}  // namespace gekko::math_mtx
