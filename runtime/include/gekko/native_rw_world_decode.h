// The world-sector pipeline's vertex-format decoders (gx-rw-plan cluster rwR4), exact owners.
//
// Four pure functions of the world-sector instance code that read the packed descriptor words of a mesh object and answer
// a count: how many components an attribute has, per its format fields. None writes memory; no recording enters them (the
// pipeline nodes that call them are not run in the lifted build, docs/agents/axfx-effects.md Step K).
//   0x8046798C  (r3 = descriptor)               [r3+4] bit 0 -> 3 / 2, bits 1..3 select the multiplier: 4 -> x4, 2 and 3 -> x2,
//                                                0 and 1 -> x1, 5..7 -> 0
//   0x804679F0  (r3 = descriptor)               ([r3+4] >> 10) & 7: 1 -> 3, 3 -> 6, 4 -> 12, otherwise 0
//   0x80467A3C  (r3 = descriptor, r4 = index)   the 3-bit field at bit 14 + 4 * (index & 0xFF) + ... of [r3+4]
//                                                (shift = ((index << 2) & 0x3FC) + 14, srw semantics): 0 -> 2, 1 -> 3, 2 -> 4,
//                                                3 -> 2, 4 -> 3, 5 -> 4, 6 and 7 -> 0
//   0x80467AA4  (r3 = descriptor, r4 = index)   index byte 0..7 picks a (bit, 3-bit field) pair of the words [r3+4], [r3+8],
//                                                [r3+0xC]; the bit gives 1 -> 2, 0 -> 1; the field the multiplier as in 0x8046798C;
//                                                an index above 7 answers 0
#pragma once

#include "gekko/cpu.h"

namespace gekko::rw_world_decode {

constexpr u32 kComponentsA = 0x8046798Cu;
constexpr u32 kComponentsB = 0x804679F0u;
constexpr u32 kComponentsC = 0x80467A3Cu;
constexpr u32 kComponentsD = 0x80467AA4u;

void install(Cpu& cpu);

}  // namespace gekko::rw_world_decode
