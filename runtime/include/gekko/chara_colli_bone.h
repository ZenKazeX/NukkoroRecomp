#pragma once

// CharaColliGeneralBone's update (vf 0x88, 0x800C3D6C; docs/CPU_FINAL_HAUNT.md) as native code: the same function of
// guest memory, every floating-point operation the guest's ppc_ops helper on the guest's operands, every stored
// float the guest's single conversion. The mathematics, recovered from the guest's code:
//
//   matrices are the game's 3x4 (rows 0x00 / 0x10 / 0x20, each x y z translation), points transformed as m x [p 1]
//   mul34(a, b)[i][j] = a[i][2] b[2][j] + (a[i][0] b[0][j] + a[i][1] b[1][j])   (+ a[i][3] for j = 3)
//                                                         (0x8040C94C, 0x8040CB14: one fused multiply-add per term)
//   translation(v) = [I | v] (0x8040C084); euler(m, angles, order): for each non-zero angle, in the order's sequence
//     (0: z y x, 1: z x y, 2: y x z; other orders: none), m = m x R_axis(angle) with R from sin and cos (MSL's, in
//     double, rounded to single; 0x8001E070, 0x8040C110 / C188 / C200, 0x80031568)
//   inverse(m) = adj(m) / det(m) (0x8040CF48, a cofactor inverse; det == 0: a copy)
//   normalize(v) = v / |v|, or the game's zero vector when |v| < 1e-6 (0x8040DEC4; |v| through the game's sqrt)
//
//   vf 0x8C (dirty, 0x800C34D8): each child re-derived from the object's position (+0x40), rotation (+0x4C) and
//     rotation order (+0x58): capsule / cylinder (vf 0x50, 0x8002F774 / 0x80032A2C): +0x50 = euler(translation(pos),
//     rot) x translation(local), then euler(+0x50, own angles); sphere (0x80035500): centre = pos + local, or (flag
//     +0x18 bit 0 and a non-zero rotation) euler(translation(pos), rot) x [local 1]. this+0x29 = 1, +0x28 = 0.
//   vf 0x90 (0x800C3E00): each child with a bone (the array at this+0x6C, one entry per child): capsule / cylinder
//     (vf 0x54): +0x50 = euler(bone x translation(local), own angles); sphere: centre = bone x [local 1].
//     this+0x29 = 1.
//   capsule / cylinder after either: centre (+4) = +0x50's translation column, +0x80 = inverse(+0x50), the axis
//     a = +0x50's rotation x (k, k, half length +0x38): ends centre - a (+0xB0), centre + a (+0xBC), direction
//     normalize(a) (+0xC8) (0x8002F5BC / 0x8003286C)
//   vf 0x84 (0x800C3C3C): this+8 = max(|c0 - this+0x40| + r0, the enabled children's |c - this+0x40| + r)
//
// What a call leaves, as the guest's: every word above; FPSCR's FPCC from its last compare (and an invalid-compare
// bit for a NaN), the compares in the guest's order; the FPRs the guest's callees save with psq_st (f21-f31 when a
// capsule or cylinder is computed, f23 for a bone multiply, f30 for a rotation or the radius, none for a sphere
// re-derived without one) with ps1 rounded to single. Volatile registers and the dead stack
// are the ABI's. The arithmetic helpers change no FPSCR bit, so independent operations are free to run in any order;
// the compares are not.

#include "gekko/types.h"

namespace gekko {

class Cpu;

namespace chara_colli_bone {

// vf 0x8C (when the dirty byte this+0x28 is set), vf 0x90 and vf 0x84 for the object at `self` (0x800C409C already
// done). False, with nothing written, when the object is outside this contract: a child class other than the three
// Range shapes, an empty child list, more than 32 children, or a sine / cosine argument the port does not cover.
// `wide`: set when a capsule or cylinder was computed (f21-f31 rounded), else f30-f31.
bool update(Cpu& cpu, u32 self, bool* wide = nullptr);

// vf 0x8C of a dirty CharaColliGeneral / GeneralCharInfo (0x800C34D8): every child re-derived from the object's
// position, rotation and order (vf 0x50), then this+0x29 = 1 and this+0x28 = 0. The same contract and leftovers.
bool rederive_children(Cpu& cpu, u32 self);
// vf 0x8C of a dirty CharaColliBody / BodyCharInfo (0x800C1714): its shape (this+0x5C, vf 0x28) re-derived, the
// flags as above.
bool rederive_shape(Cpu& cpu, u32 self);

}  // namespace chara_colli_bone

}  // namespace gekko
