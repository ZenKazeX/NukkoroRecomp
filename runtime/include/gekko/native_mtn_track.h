#pragma once

// The Mtn animation library's key-frame track sampler and its Hermite interpolators (main.dol, PAL GUPP8P), owned natively
// (docs/agents/opt-mtn.md). Names are from this project's own disassembly; "reference agrees" is not claimed.
//
// Track state (0x40 bytes, r3 of the sampler; one per animated scalar, three per vector track):
//   +0x00 s16 key index, +0x02 s16 key count, +0x04 u32 flags (bit0 finished/held, bit3 flat segment), +0x08 u32 pointer into the
//   packed key stream, +0x10 f32 cached value, +0x20 key A and +0x30 key B, each {f32 time, in-tangent, out-tangent, value}.
// Sampling context 0x805E2380: +0x0C f32 the time being sampled, +0x14 the value interpolator (set to 0x80416728 by the library's
//   context initialiser 0x804167D4), +0x18 the slope interpolator (0x8041669C), +0x1C the second-derivative one (0x80416580).
// Packed key (stream of u16): header = time (h & 0x3FFF, as an integer converted to f32), bit 0x8000 = out-tangent repeats the
//   in-tangent; then in-tangent (unless the header word is followed by a zero), out-tangent (unless repeated), value, each a
//   16-bit "half" decoded by bit placement: 0 -> +0.0, else sign | (((h & 0x7FFF) << 13) + 0x38000000) (no denormal / zero-sign
//   handling: 0x8000 decodes to -2^-15).
//
//   0x804162AC TrackSample(r3 = track state) -> f1: the value at the context time. Order of the original:
//       bit0: cached; t < A.time + eps: A.value; t < B.time - eps: bit3 ? cached : interpolate(A, B, t); t < B.time + eps: B.value;
//       otherwise advance (A = B, decode the next key into B) while index < count and t >= B.time + eps; then store the index /
//       stream pointer / flags, and return B.value, the flat value (sets bit3 + cached) or interpolate. Past the last key:
//       out-tangent == 0 holds (sets bit0, cached) else extrapolates B.value + B.out * (t - B.time). eps = 9.99999975e-05.
//   0x80415300 TrackSeekForward(r3): while t >= B.time + eps and index < count: A = B, decode the next key into B; stores index and
//       stream pointer (nothing at all when t < B.time + eps on entry).  0x80415170 TrackSeekBackward(r3): the mirror: B = A,
//       decode the previous key into A, stream -= (header bit 0x4000 ? 6 : 8), while index >= 0 and t <= A.time - eps.
//   0x80414B80 TrackInit(r3; record pointer at +0xC): flags = 0; context rate (+0x08) < 0 starts at the record's last key
//       (index count-1, stream = record end - s8(rec+6) - (first header bit 0x4000 ? 6 : 8)) then TrackSeekBackward, else at the
//       first key (record + 8, index 1) then TrackSeekForward.
//   0x80421358 / 0x804213FC / 0x8042148C / 0x80421518 SampleVec3...(r3 = object): samples up to three tracks (0x40 apart from
//       object + 0x10, or + 0x20 for the last two) through the sampler pointer at [r13 + 0x5F78] into three floats at the object's
//       start; a track whose stream pointer (+8) is zero is skipped; the first form stores 1 / sample (fdivs), the last multiplies all
//       three by the float at object + 0x10 when it is > 0.
//   0x80416728 HermiteValue(r3 = A, r4 = B, f1 = t) -> f1:  cubic Hermite value (the basis 2s^3-3s^2+1 ... as the original's
//       fused form), one fdivs 1 / (B.time - A.time).
//   0x8041669C HermiteSlope(r3 = A, r4 = B, f1 = t) -> f1:  its first derivative.
//
// Exactness: every arithmetic step is the original's, in the original's operand roles and order: the sampler's by ppc::fps_*
// (single rounding, FPSCR[NI] policy as the translated code), the two interpolators by plain host floats while no operand or
// intermediate is NaN or tiny (the fps_* operations are pure value functions) and by ppc::fps_* otherwise; compares follow fcmpo / fcmpu including the NaN exception bits of
// the original (the compare's CR0 / FPCC result is not kept: nothing reads it, the next compare overwrites it). An unknown
// interpolator pointer in the context is called as guest code (guest_call), never skipped.
// Preconditions as the architecture's: the f1 passed to an interpolator owner is a single value (lfs result), as at every caller.
// Not kept: f0..f12, r0..r12, CR0, the original's stack scratch (it writes below r1 and its own LR save slot only).

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::mtn_track {

constexpr u32 kTrackSample = 0x804162ACu;
constexpr u32 kHermiteValue = 0x80416728u;
constexpr u32 kHermiteSlope = 0x8041669Cu;
constexpr u32 kSeekForward = 0x80415300u;
constexpr u32 kSeekBackward = 0x80415170u;
constexpr u32 kTrackInit = 0x80414B80u;
constexpr u32 kSampleVec3Reciprocal = 0x80421358u;
constexpr u32 kSampleVec3 = 0x804213FCu;
constexpr u32 kSampleVec3At20 = 0x8042148Cu;
constexpr u32 kSampleVec3Scaled = 0x80421518u;
constexpr u32 kContext = 0x805E2380u;

// The guest entries served here: equal to this section of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kTrackSample, "Mtn TrackSample"},
                                                 {kHermiteValue, "Mtn HermiteValue"},
                                                 {kHermiteSlope, "Mtn HermiteSlope"},
                                                 {kSeekForward, "Mtn TrackSeekForward"},
                                                 {kSeekBackward, "Mtn TrackSeekBackward"},
                                                 {kTrackInit, "Mtn TrackInit"},
                                                 {kSampleVec3Reciprocal, "Mtn SampleVec3Reciprocal"},
                                                 {kSampleVec3, "Mtn SampleVec3"},
                                                 {kSampleVec3At20, "Mtn SampleVec3At20"},
                                                 {kSampleVec3Scaled, "Mtn SampleVec3Scaled"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

void serve_track_sample(Cpu& cpu);
void serve_hermite_value(Cpu& cpu) noexcept;
void serve_hermite_slope(Cpu& cpu) noexcept;
void serve_seek_forward(Cpu& cpu);
void serve_seek_backward(Cpu& cpu);
void serve_track_init(Cpu& cpu);
// 0x80414E88 (track state init): the record pointer and count stored, then the same reset and seek as TrackInit; used by the pose owners.
void state_init(Cpu& cpu, u32 state, u32 record);
void serve_sample_vec3_reciprocal(Cpu& cpu);
void serve_sample_vec3(Cpu& cpu);
void serve_sample_vec3_at_20(Cpu& cpu);
void serve_sample_vec3_scaled(Cpu& cpu);
// Statistics: calls of the four vector samplers by route. 0: two or three Hermite lanes evaluated side by side; 1: every lane
// decided by reading alone, the lanes one by one; 2: the lane-by-lane code (a lane advances, another sampler or interpolator).
unsigned long long vec3_route(unsigned kind) noexcept;

void install(Cpu& cpu);

}  // namespace gekko::mtn_track
