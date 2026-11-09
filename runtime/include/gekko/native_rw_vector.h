#pragma once

// RenderWare's vector transforms (RwV3dTransformPoint 0x804834A8, RwV3dTransformPoints 0x804834F4, RwV3dTransformVector
// 0x80483554) served natively (docs/agents/native-boot.md Step 10; docs/NATIVE_MATH_METHOD.md). The originals are leaf
// paired-single bodies: they load three floats of the vector (or point) and the matrix's right / up / at (/ pos) rows, form
// each output component as a chain of single-precision fused multiply-adds in the order right, up, at and store three floats.
// The owner computes the same chains on host floats (shadow::math::fused / multiply: the Gekko's NaN selection, the FPSCR[NI]
// subnormal policy read at the call) and writes the same three floats per output; no paired-single register, GQR or CTR
// state is kept. What the original leaves besides the output is the volatile register file (f0..f11, r5, CTR, CR), which no
// caller reads (a leaf C-ABI call), and the FPSCR sticky status bits, which nothing in the game reads (the only readers of
// FPSCR are the OS context save and the MSL fenv code).
//
// Aliasing: every input of RwV3dTransformPoint / Vector is read before the first store, as the original does; the array
// form reads element i + 1 before it stores element i (its software pipeline), so an output that overlaps the next input
// element sees that element's old value, and element count 0 (the original loops 2^32 times) stops the machine by name.
// The owner declines (the guest body runs) when the FPU is disabled or GQR0 is not the float format the psq loads assume.

#include "gekko/cpu.h"

namespace gekko::rw_vector {

constexpr u32 kTransformPoint = 0x804834A8u;
constexpr u32 kTransformPoints = 0x804834F4u;
constexpr u32 kTransformVector = 0x80483554u;
// The square-root family: _rwSqrt 0x80483390 / _rwInvSqrt 0x804833E4 (f1 -> f1, integer table lookups on the single's bits),
// RwV3dLength 0x80483350 (r3 = vector), RwV2dLength 0x80483440, RwV3dNormalize 0x8048327C (r3 = out, r4 = in; a length that is
// not above zero declines, the guest body runs the RenderWare error path).
constexpr u32 kRwSqrt = 0x80483390u;
constexpr u32 kRwInvSqrt = 0x804833E4u;
constexpr u32 kV3dLength = 0x80483350u;
constexpr u32 kV2dLength = 0x80483440u;
constexpr u32 kV3dNormalize = 0x8048327Cu;

// true: served; false: declined (the guest body runs). May stop the machine (count 0).
bool serve_transform_point(Cpu& cpu);
bool serve_transform_points(Cpu& cpu);
bool serve_transform_vector(Cpu& cpu);
bool serve_rw_sqrt(Cpu& cpu);
bool serve_rw_inv_sqrt(Cpu& cpu);
bool serve_v3d_length(Cpu& cpu);
bool serve_v2d_length(Cpu& cpu);
bool serve_v3d_normalize(Cpu& cpu);

// Installs the entries.
void install(Cpu& cpu);

}  // namespace gekko::rw_vector
