#pragma once

// S1 core: the character sweep query 0x80021934 (main.dol, PAL GUPP8P), served natively as a whole function, with its callees called as
// the translated code calls them. Evidence and ranking: docs/re/core.md section 9.
//
//   0x80021934 CharSweep(r3 = query, r4 = sphere, r5, r6 = mode flags, r7..r10, stack: [r1 + 8] result slot, byte [r1 + 0xB] a flag): the
//   shared "sweep this sphere against the characters" entry (15 callers: bullets, throw objects, the camera, the vehicle, the player's contact
//   code 0x80098ABC). Mode bit 0: the handle manager 0x80045AC0 is asked for the character table, and every one of its 0xA8 slots that holds a
//   character with a body (+0xB8) is swept through 0x80021480 (the results chain through r10 -> r31). Mode bit 1: a sphere object is built
//   (0x80021C04) and the world / CharaColli query 0x800C51C8 runs with a filter object (0x80021BC4 / the pool singleton 0x80021B08 / 0x80021B74).
//   The slot loop is the hot part: per slot the translated code makes two guest calls (0x8003AA58 slot-used test, 0x8003A568 character
//   pointer) of 5-30 instructions each; they are frame-less leaves, so inlining them writes nothing to the dead stack.
//
// Native: the prologue / epilogue frame words, the leaf 0x80021C80 and the two leaves of the loop inlined (their writes: the two halfwords
// at [r1 + 0x10], [r1 + 0x14]); every other callee (0x80045AC0, 0x80021480, 0x80021C04, 0x80021BC4, 0x80021B08, 0x800C51C8, 0x80021B74) is
// called through the translated table with the registers, the stack argument words and LR the translated code sets (registers are written
// back to the Cpu before each call: a callee that leaves to the dispatcher resumes the translated function at the return address with the
// state exact). Hand-back (nothing written): MSR[FP] clear with mode bit 1 (the translated lfs raises the exception), and a hook of any kind on
// an inlined leaf (--watch, --context-hook): those stay observable.
//
// Switches: GEKKO_S1_CHARS=1/0 at run time, CMake option SHADOW_S1_CHARS (default OFF), GEKKO_S1_CHARS_STATS=1 (calls, served, hand-backs,
// slots tested, slots swept).

#include "gekko/cpu.h"

namespace gekko::s1_chars {

constexpr u32 kCharSweep = 0x80021934u;

struct Stats {
    u64 calls = 0;
    u64 served = 0;
    u64 declined = 0;
    u64 slots_tested = 0;
    u64 slots_swept = 0;
};

bool serve(Cpu& cpu);
const Stats& stats() noexcept;
void set_enabled(bool enabled) noexcept;
void install(Cpu& cpu);

bool call_guest(Cpu& c, u32 target);  // native_s1_call.cpp (both runtime variants)

}  // namespace gekko::s1_chars
