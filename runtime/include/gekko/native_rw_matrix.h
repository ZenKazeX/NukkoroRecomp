#pragma once

// RenderWare matrix routines served natively (docs/agents/native-boot.md Step 10b; docs/NATIVE_MATH_METHOD.md item 4).
//
//   RwMatrixScale 0x804803E8 / RwMatrixTranslate 0x804805EC / RwMatrixTransform 0x8048077C (r3 = matrix, r4 = vector or matrix, r5 =
//   replace 0 / pre-concatenate 1 / post-concatenate 2): the original's chains of lfs / fmuls / fmadds / fadds / stfs in its order;
//   Transform multiplies through RwMatrixMultiply (a RenderWare core function, called with guest_call) into a private frame and
//   copies the rows and flags back (the three pad words, which hold stale stack bytes in the original, are not copied). Any
//   other operation (the original raises a RenderWare error and faults on a null matrix) is declined.
//   RwMatrixInvert 0x80480100 (r3 = dst, r4 = src): an identity (flags & the enabled optimisations & internal-identity) is
//   copied word for word; an orthonormal matrix is transposed and its translation becomes -(pos . axes); anything else goes
//   through the cofactors and a reciprocal determinant (1.0 when the determinant's bits are zero). The operations are the
//   original's scalar single-precision chain (fmuls / fmadds / fmsubs / fnmsubs / fnmadds, one rounding each, the CPU model's own
//   helpers) in its load / store order, so overlapping operands (RenderWare forbids dst == src) read the same partly
//   overwritten words as the original. No register state is kept (volatile registers, f31's spill frame and the FPSCR sticky bits
//   are not read by any caller). Declined (the guest body runs) when the FPU is disabled or GQR0 is not the float format.

#include "gekko/cpu.h"

namespace gekko::rw_matrix {

constexpr u32 kInvert = 0x80480100u;
constexpr u32 kScale = 0x804803E8u;
constexpr u32 kTranslate = 0x804805ECu;
constexpr u32 kTransform = 0x8048077Cu;
constexpr u32 kUpdate = 0x8047FBF0u;
constexpr u32 kOptimize = 0x8047F9E0u;
constexpr u32 kRotate = 0x8047FD24u;  // RwMatrixRotateOneMinusCosineSine

bool serve_invert(Cpu& cpu);
bool serve_scale(Cpu& cpu);
bool serve_translate(Cpu& cpu);
bool serve_transform(Cpu& cpu);
bool serve_rotate(Cpu& cpu);
bool serve_update(Cpu& cpu);
bool serve_optimize(Cpu& cpu);

// Installs the owners.
void install(Cpu& cpu);

}  // namespace gekko::rw_matrix
