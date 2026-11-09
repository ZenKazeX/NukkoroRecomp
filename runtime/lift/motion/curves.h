// Native curve cursor + the six per-frame key decoders (phase 12). Transcribed from the original words:
//   init 80414E88, rewind 80414B80, step forward 80415300, step backward 80415170,
//   value    forward 804162AC / backward 80415A30   (Hermite value 80416728)
//   slope    forward 80416020 / backward 8041579C   (Hermite slope 8041669C)
//   both     forward 80415D0C / backward 80415488   (Hermite value+slope 80416580)
// Representation: a cursor is (chunk index in the clip, key index, byte position of key record `index` inside the
// chunk) plus the two decoded bracketing keys - never a pointer into asset memory. The original's global timing record
// (805E2380) is an explicit argument. Float arithmetic follows the original operation order exactly (fmadds ->
// std::fmaf; everything else binary32), which is what makes the outputs bit-exact (V8).
#pragma once
#include <cstdint>
#include <span>

namespace shadow::motion {

struct Timing {                 // channel timing record: start, end, signed speed, current time (channel +0xA0)
    float start = 0, end = 0, speed = 0, time = 0;
};

struct Key { float time = 0, in = 0, out = 0, value = 0; };

namespace curve_flags {
constexpr std::uint32_t ValueConst = 1;   // value decoders: past the last key with zero slope (cached value)
constexpr std::uint32_t SlopeConst = 2;   // slope decoders: past the last key (cached slope)
constexpr std::uint32_t BothConst = 4;    // value+slope decoders: past the last key with zero slope
constexpr std::uint32_t FlatSegment = 8;  // value decoders: current segment is flat (cached value)
}

struct CurveCursor {
    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;
    std::uint32_t chunk = kNone;  // index of the chunk in the committed clip (bound iff != kNone)
    std::int16_t index = 0;       // key index of the next record to decode (signed16, as the original)
    std::int16_t count = 0;
    std::uint32_t flags = 0;
    std::uint32_t position = 0;   // byte position of key record `index` inside the chunk (derived from index)
    Key a, b;                     // bracketing keys (a earlier, b later)
    float cached_value = 0, cached_slope = 0;
    bool bound() const { return chunk != kNone; }
};

// A chunk as the decoders see it: its serialized bytes (header included, big-endian as normalised).
struct ChunkBytes {
    std::span<const std::uint8_t> bytes;
};

void curve_init(CurveCursor& c, std::uint32_t chunk_index, ChunkBytes chunk, const Timing& t);   // 80414E88
void curve_rewind(CurveCursor& c, ChunkBytes chunk, const Timing& t);                            // 80414B80
void curve_step_forward(CurveCursor& c, ChunkBytes chunk, const Timing& t);                      // 80415300
void curve_step_backward(CurveCursor& c, ChunkBytes chunk, const Timing& t);                     // 80415170

float curve_value_forward(CurveCursor& c, ChunkBytes chunk, const Timing& t);    // 804162AC
float curve_value_backward(CurveCursor& c, ChunkBytes chunk, const Timing& t);   // 80415A30
float curve_slope_forward(CurveCursor& c, ChunkBytes chunk, const Timing& t);    // 80416020
float curve_slope_backward(CurveCursor& c, ChunkBytes chunk, const Timing& t);   // 8041579C
void curve_both_forward(CurveCursor& c, ChunkBytes chunk, const Timing& t, float& value, float& slope);   // 80415D0C
void curve_both_backward(CurveCursor& c, ChunkBytes chunk, const Timing& t, float& value, float& slope);  // 80415488

// Direction selection of 804198F0: the backward trio when the channel speed is negative.
inline float curve_value(CurveCursor& c, ChunkBytes chunk, const Timing& t) {
    return t.speed < 0.0f ? curve_value_backward(c, chunk, t) : curve_value_forward(c, chunk, t);
}

float hermite_value(const Key& a, const Key& b, float t);                      // 80416728
float hermite_slope(const Key& a, const Key& b, float t);                      // 8041669C
void hermite_both(const Key& a, const Key& b, float t, float& value, float& slope);   // 80416580

}  // namespace shadow::motion
