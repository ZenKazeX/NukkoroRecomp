#pragma once

// The GameCube driver's camera view-matrix cache (docs/agents/native-boot.md Step 11). Two rows on the device-state global
// 0x805E2A10 (the matrix stack: index [+0x244], pointers at +0x1C4 + 4 * index; the view matrix at +0x184; the cached matrices at
// +0x258 (scaled copy), +0x298 (its inverse) and +0x2D8 (product with the camera frame's LTM); the dirty flags at +0x318 and
// +0x32C; the camera at [+0x34C]):
//
//   0x804759B4  (f1, f2) -> r3 = 1: RwMatrixTranslate(current matrix, {f1, f2, 0}, pre-concatenate), then both cache flags
//               (+0x318, +0x32C) are cleared.
//   0x80475A1C  () -> r3 = &obj[0x2D8]: when +0x318 is clear: current * view matrix, scaled by {s, s, 1} (s = obj[+0x320], or 1
//               when the camera's projection type [camera + 0x14] is 2), by {-m, m, 1} (m = the smaller of obj[+0x248], obj[+0x24C]),
//               translated by {0.5 * obj[+0x248] * s, -(0.5 * obj[+0x24C] * s), 0}; the sixteen words go to +0x258, their inverse to
//               +0x298, and +0x318 is set. Always: +0x2D8 = (+0x258) * the camera frame's LTM (RwFrameGetLTM).
//
// The RenderWare core callees (RwMatrixMultiply, Scale, Translate, Invert, RwFrameGetLTM) are called with guest_call; everything
// else is exact native code on the device-state words with the CPU model's single-precision helpers. The original's frame temporaries
// (dead stack) and the three pad words of a matrix copied from them are not reproduced; f30 / f31 second slots keep the rounding
// the original's psq save pair gives them. Declined (the guest body runs) when the FPU is disabled or GQR0 is not float.

#include "gekko/cpu.h"

namespace gekko::rw_view_cache {

constexpr u32 kSetOffset = 0x804759B4u;
constexpr u32 kRefresh = 0x80475A1Cu;

bool serve_set_offset(Cpu& cpu);
bool serve_refresh(Cpu& cpu);

// Installs both owners.
void install(Cpu& cpu);

}  // namespace gekko::rw_view_cache
