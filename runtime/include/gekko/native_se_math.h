#pragma once

// The sound-effect manager's positional math, native (docs/SE_MANAGER_MAP.md section 3 "Math"; sound phase 2).
//
// Original behaviour (main.dol, PAL GUPP8P; r2 = 0x805FA780, r13 = 0x805EC500, K(x) = the f32 at r2 + x):
//   ListenerDistance 0x801D9D04(Vec3* p) -> f32. The listener is player 1's actor: the id *(r13 + 0x3130) is looked
//     up in the actor registry (singleton *(r13 + 0x2E88), 0x800742BC) by 0x800A8F5C, which walks the registry's
//     vector of 8-byte shared pointers (+4 count, +8 data) to nodes {obj, weak count block} and returns a weak pointer
//     to the first obj whose +0x298 equals the id. The weak pointer is copied (0x800520B0), locked (0x8005223C:
//     expired = use count +4 == 0, 0x8005232C) and copied again (0x800521AC); a live obj gives |obj - p| through
//     Sub 0x8040DDC4 and Length 0x8040DFA0, a missing or expired one K(-0x3884) = 0.0. With the two-player byte
//     0x8057E760 + 0xB2 set, both players are measured (player 2's id at *(r13 - 0x6AD8)), a missing one counts as
//     K(-0x3888) = 9999999.0, and the smaller wins (fcmpo).
//   Length 0x8040DFA0: fmuls x*x, y*y, z*z; fadds (x2 + y2); fadds z2 + that; sqrt 0x8000FEE4 -> 0x8000FF04, which
//     is frsqrte + three double Newton steps (g' = 0.5 g (3 - x g g), the last folded into x g) and frsp for x > 0;
//     x < 0 and NaN give MSL's __float_nan word 0x7FFFFFFF (0x805EED78); +-0 and +inf return x.
//   MaxDist 0x801D9F6C(row) -> f32: row + 0xC == 0 ? K(-0x3880) = 400.0 : 100.0f * (float)(u8).
//   PosVolume 0x801D979C(chan) -> s8: see docs/SE_MANAGER_MAP.md; 60 / max / mode 2 surround headroom of 30.
//   PosPan 0x801D95B4(chan, s8* surround) -> s32: mono (cfg + 0x78 == 1) and two-player return 0. Otherwise D =
//     chan + 0x1C - camera 0's eye (camera manager *(r13 + 0x2B34), 0x80009548; element 0 of its vector, transform at
//     +8: eye +0, target +0xC), D.y = 0, r = |D|. Surround (mode 2) stores 0 to a non-null *surround (after an
//     atan2 whose result is discarded) and returns 0. Stereo: yaw = atan2(-fwd.x, -fwd.z) of the normalised
//     target - eye (0x80109F68 -> 0x8000A610 -> 0x804107B0, which also computes the pitch atan2(fwd.y,
//     sqrt(x^2 + z^2))); ang = 16384.0f + (atan2f(D.z, D.x) + yaw) (two fadds - 16384 added to RADIANS, an original
//     quirk); p = 0.7f * sinf(ang); p = r < 50 ? 100 * (p * (r / 50)) : 100 * p; clamp [-127, 127]; fctiwz. The
//     mode-2 block after the clamp (0x801D9734: cos 0x8000F3B0, fabs 0x8000A980) is unreachable: control reaches it
//     only after mode 2 has already returned, and nothing in between writes the mode.
//   DopplerBend 0x801D9408(chan) -> s32: single listener only; d = |L - chan + 0x1C| (L = 0 when the listener is
//     missing or expired); prev = chan + 0x2C. prev == 0 && d != 0 (fcmpu): store d, return 0. prev > 800
//     (fcmpo f2, f0 - the PREVIOUS distance, not d as docs/SE_MANAGER_MAP.md says): store d,
//     return (s32)(float)(s16)chan + 0xA. Else b = 20 (prev - d), clamp [-6144, 6144], store d, return (s32)b.
//   The MSL double routines behind atan2f 0x80016260 and sinf 0x8000FFE8 are the native library math/msl.h
//     (docs/NATIVE_MSL_MATH.md), in the caller's FPSCR[NI] mode.
//
// Guest-memory side effects of the originals: only their own stack frames, chan + 0x2C (DopplerBend) and the
// surround byte (PosPan). The lookups also increment and then decrement shared/weak reference counts (registry
// element use +4, listener weak +8 and use +4, camera element use +4); with the registry's invariants (an element
// is held by the registry, so its use count is >= 1; a node holds its weak count block, so its weak count is >= 1)
// none of those counts reaches zero, so no dispose/destroy virtual runs and every count ends where it began. The
// native versions read the same pointers and do not touch the counts; se_math_contract_tests proves the guest
// memory equal byte for byte (every byte either side wrote) after every call.
//
// Native implementation: each function reads the guest inputs the original reads (the channel, the configuration,
// the r13 globals, the registry, the camera) and computes with the CPU's own floating-point model
// (gekko/ppc_ops.h): every single-precision instruction rounds to single, every fused instruction is fused, and
// frsqrte is the port's exact 1/sqrt, so the results are the translated code's by construction. FPSCR's control
// byte is the contract; its status bits are not (no game code reads them, docs/NATIVE_MSL_MATH.md). The DOL
// constants are compiled in (dol_constants() lists them; the contract test checks them against main.dol).
// Preconditions, all true in the shipped game from boot on (the originals would otherwise construct a singleton
// lazily, through the heap, or abort): the actor registry *(r13 + 0x2E88) and camera manager *(r13 + 0x2B34) exist,
// the camera manager holds at least one camera, registry nodes point at actors. A missing registry is read as an
// empty one (the value the original computes after constructing it) without constructing it.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::se_math {

// (The original functions' entry addresses live in runtime/tests/se_math_contract_tests.cpp only: a code literal
// here would make tools/ppcrecomp/liveness_roots.py keep the translated bodies alive.)

constexpr u32 kOutputConfig = 0x8057E760u;  // +0x78 s32 output mode (1 mono, 2 surround), +0xB2 u8 two players
constexpr u32 kR13 = 0x805EC500u;
constexpr u32 kR2 = 0x805FA780u;
constexpr u32 kRegistryPointer = kR13 + 0x2E88u;  // actor registry singleton
constexpr u32 kCameraPointer = kR13 + 0x2B34u;    // camera manager singleton
constexpr u32 kPlayer1Id = kR13 + 0x3130u;
constexpr u32 kPlayer2Id = kR13 - 0x6AD8u;
constexpr u32 kZeroVector = kR13 - 0xACA0u;        // 0x805E1860: Vec3 (BSS, zeroed by a static initialiser); Normalize's fallback

// Channel fields read or written (docs/SE_MANAGER_MAP.md 2b).
constexpr u32 kChanVolumeOffset = 0x07u, kChanVolumeCeiling = 0x08u, kChanBend = 0x0Au, kChanPosition = 0x1Cu,
              kChanMaxDistance = 0x28u, kChanPreviousDistance = 0x2Cu;

// A position as the guest stores it: three big-endian f32 words, as host u32 values.
struct Vec3 {
    u32 x = 0, y = 0, z = 0;
};

// The FPR results (f1) are returned as the register's double-format bits; each is a single-precision value.

// ListenerDistance(position): the guest Vec3 at `position`, or a position value.
u64 listener_distance(Cpu& cpu, u32 position) noexcept;
u64 listener_distance(Cpu& cpu, const Vec3& position) noexcept;

// MaxDist(row): `row` is a guest table row (reads row + 0xC), or the byte itself.
u64 max_distance(Cpu& cpu, u32 row) noexcept;
u64 max_distance_of(Cpu& cpu, u8 hundreds) noexcept;

// PosVolume(chan): reads chan + 0x1C, + 0x28, + 7, + 8 (or the values).
s8 pos_volume(Cpu& cpu, u32 chan) noexcept;
s8 pos_volume(Cpu& cpu, const Vec3& position, u32 max_distance_bits, s8 volume_offset, s8 volume_ceiling) noexcept;

// PosPan(chan, surround): reads chan + 0x1C (or the position). *out_surround is written exactly when the original
// stores to its out pointer (surround mode, not mono, not two-player, out non-null), and only then.
s32 pos_pan(Cpu& cpu, u32 chan, s8* out_surround) noexcept;
s32 pos_pan(Cpu& cpu, const Vec3& position, s8* out_surround) noexcept;

// DopplerBend(chan): reads chan + 0x1C, + 0xA, + 0x2C; writes chan + 0x2C (the f32 word), as the original. The value
// form reads and updates `previous_distance` (the f32 word) instead.
s32 doppler_bend(Cpu& cpu, u32 chan) noexcept;
s32 doppler_bend(Cpu& cpu, const Vec3& position, s16 bend, u32& previous_distance) noexcept;

// The ported routines on their own, for the contract test (double-format FPR bits in and out): MSL's atan2
// (f1 = y, f2 = x), sin and atan before their float wrappers' frsp, and the game's sqrt (single result).
namespace detail {
u64 msl_atan2(Cpu& cpu, u64 y, u64 x) noexcept;
u64 msl_sin(Cpu& cpu, u64 x) noexcept;
u64 msl_cos(Cpu& cpu, u64 x) noexcept;  // cos 0x803B2594
u64 msl_atan(Cpu& cpu, u64 x) noexcept;
u64 guest_sqrt(Cpu& cpu, u64 x) noexcept;
}  // namespace detail

// The DOL constants compiled into this module, for the contract test to compare with main.dol.
struct DolConstant {
    u32 address;
    u32 size;   // 4 (f32 / u32 word) or 8 (double)
    u64 value;
};
const DolConstant* dol_constants(std::size_t& count) noexcept;

}  // namespace gekko::se_math
