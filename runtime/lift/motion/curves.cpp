// Native curve cursor + decoders - see curves.h. Each function names the original it transcribes; comparisons keep
// the original's sense (fcmpo + bge/ble/bgt/blt), so NaN behaves as in the original.
// ORIGINAL QUIRK, PRESERVED (game behaviour, not memory safety): the three BACKWARD decoders advance the decoded keys
// but do not store the key index / position when they stop inside the chunk (only on exhaustion) - 80415A30 @80415C04,
// 8041579C @80415994, 80415488 @804156B0 have no sth/stw on that path, unlike their forward twins.
#include "motion/curves.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace shadow::motion {

namespace {
constexpr float kEps = 9.99999975e-05f;   // r2+0x16E0 (0x38D1B717)
constexpr float kHalf = 0.5f;             // r2+0x16E4

[[noreturn]] void refuse(const char* w) { throw std::runtime_error(std::string("FAIL_CLOSED curve: ") + w); }

std::uint16_t be16(ChunkBytes c, std::uint32_t at) {
    if (at + 2 > c.bytes.size()) refuse("key record outside its chunk");
    return std::uint16_t((c.bytes[at] << 8) | c.bytes[at + 1]);
}
std::uint32_t be32(ChunkBytes c, std::uint32_t at) {
    if (at + 4 > c.bytes.size()) refuse("chunk header");
    return (std::uint32_t(c.bytes[at]) << 24) | (std::uint32_t(c.bytes[at + 1]) << 16) | (std::uint32_t(c.bytes[at + 2]) << 8) | c.bytes[at + 3];
}
float from_bits(std::uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
// 16-bit key field -> binary32: sign | ((h & 0x7FFF) << 13) + 0x38000000; zero word -> +0.0 (no denormal / inf case)
float half(std::uint16_t h) {
    if (h == 0) return 0.0f;
    const std::uint32_t mag = ((std::uint32_t(h) << 13) & 0x0FFFE000u) + 0x38000000u;
    return from_bits(mag | ((std::uint32_t(h) & 0x8000u) << 16));
}
// One key record at `at`: u16 time word (low 14 bits = frame; 0x8000 = out tangent equals in tangent; 0x4000 = the
// PREVIOUS record is the short form), in, [out], value. Returns the record size.
std::uint32_t decode(ChunkBytes c, std::uint32_t at, Key& k, std::uint16_t& word) {
    word = be16(c, at);
    k.time = float(word & 0x3FFF);
    k.in = half(be16(c, at + 2));
    std::uint32_t p = at + 4;
    if (word & 0x8000) k.out = k.in;
    else { k.out = half(be16(c, p)); p += 2; }
    k.value = half(be16(c, p)); p += 2;
    return p - at;
}
std::uint32_t previous_size(std::uint16_t word) { return 2u * (4u - ((word >> 14) & 1u)); }
float mid_slope(const Key& k) { return kHalf * (k.in + k.out); }

// 80414E88 / 80414B80 common body after the chunk / count are fixed.
void start(CurveCursor& c, ChunkBytes chunk, const Timing& t) {
    c.flags = 0;
    if (!(t.speed < 0.0f)) {                                         // bge: forward (also for NaN)
        c.index = 0;
        c.position = 8;
        std::uint16_t w;
        c.position += decode(chunk, c.position, c.a, w);
        c.b = c.a;
        c.index = std::int16_t(c.index + 1);
        curve_step_forward(c, chunk, t);
    } else {
        c.index = std::int16_t(c.count - 1);
        const std::int32_t trailer = std::int8_t(chunk.bytes.size() > 6 ? chunk.bytes[6] : 0);
        const std::uint32_t size = be32(chunk, 0);
        c.position = std::uint32_t(std::int64_t(size) - trailer - std::int64_t(previous_size(be16(chunk, 8))));
        std::uint16_t w;
        decode(chunk, c.position, c.b, w);
        c.position -= previous_size(w);
        c.a = c.b;
        c.index = std::int16_t(c.index - 1);
        curve_step_backward(c, chunk, t);
    }
}
}  // namespace

// ------------------------------------------------------------------------------------------------ Hermite forms
float hermite_value(const Key& a, const Key& b, float t) {           // 80416728
    const float f9 = t - a.time;
    const float f1 = b.time - a.time;
    const float f8 = f9 * f9;
    const float f12 = 1.0f / f1;
    const float f10a = f12 * f12;
    float f4 = f8 * f9;
    float f0 = 3.0f * f8;
    const float f11 = f8 * f12;
    f4 = f10a * f4;
    const float f10 = f0 * f10a;
    f0 = 2.0f * f4;
    const float f8b = f4 - f11;
    float f2 = f0 * f12;
    float f4b = f8b - f11;
    f0 = -f2;
    f2 = f2 - f10;
    f4b = f9 + f4b;
    f0 = f0 + f10;
    f2 = 1.0f + f2;
    f0 = b.value * f0;
    f0 = std::fmaf(a.value, f2, f0);
    f0 = std::fmaf(a.out, f4b, f0);
    return std::fmaf(b.in, f8b, f0);
}

float hermite_slope(const Key& a, const Key& b, float t) {           // 8041669C
    const float f9 = t - a.time;
    const float f0a = b.time - a.time;
    const float f10a = f9 * f9;
    const float f11 = 1.0f / f0a;
    const float f12 = f11 * f11;
    float f7 = 6.0f * f10a;
    const float f4a = 6.0f * f9;
    const float f2a = 2.0f * f9;
    f7 = f7 * f12;
    const float f0b = 3.0f * f10a;
    const float f9b = f4a * f12;
    f7 = f11 * f7;
    const float f10 = f2a * f11;
    const float f4b = f0b * f12;
    float f0 = -f7;
    const float f2 = f7 - f9b;
    const float f7b = f4b - f10;
    f0 = f0 + f9b;
    const float f4 = f7b - f10;
    f0 = b.value * f0;
    const float f1 = 1.0f + f4;
    f0 = std::fmaf(a.value, f2, f0);
    f0 = std::fmaf(a.out, f1, f0);
    return std::fmaf(b.in, f7b, f0);
}

void hermite_both(const Key& a, const Key& b, float t, float& value, float& slope) {   // 80416580
    const float f13 = t - a.time;
    const float f0a = b.time - a.time;
    const float f8a = f13 * f13;
    const float f30 = 1.0f / f0a;
    const float f29a = f30 * f30;
    float f6 = f8a * f13;
    float f1 = 6.0f * f8a;
    const float f4a = 3.0f * f8a;
    f6 = f29a * f6;
    f1 = f1 * f29a;
    float f3 = 6.0f * f13;
    float f2 = 2.0f * f6;
    const float f28 = f30 * f1;
    f1 = 2.0f * f13;
    const float f5a = f2 * f30;
    const float f31 = f8a * f30;
    float f9 = f4a * f29a;
    f2 = -f5a;
    const float f29 = f3 * f29a;
    f3 = f1 * f30;
    f1 = -f28;
    const float f8 = f6 - f31;
    float f4 = f5a - f9;
    f2 = f2 + f9;
    f9 = f9 - f3;
    f1 = f1 + f29;
    f6 = f8 - f31;
    f3 = f9 - f3;
    const float f5 = 1.0f + f4;
    f4 = b.value * f2;
    f2 = f28 - f29;
    f1 = b.value * f1;
    f6 = f13 + f6;
    f4 = std::fmaf(a.value, f5, f4);
    f3 = 1.0f + f3;
    f1 = std::fmaf(a.value, f2, f1);
    f2 = std::fmaf(a.out, f6, f4);
    f1 = std::fmaf(a.out, f3, f1);
    value = std::fmaf(b.in, f8, f2);
    slope = std::fmaf(b.in, f9, f1);
}

// ------------------------------------------------------------------------------------------------ cursor
void curve_init(CurveCursor& c, std::uint32_t chunk_index, ChunkBytes chunk, const Timing& t) {   // 80414E88
    c.chunk = chunk_index;
    c.count = std::int16_t(be16(chunk, 4));
    start(c, chunk, t);
}

void curve_rewind(CurveCursor& c, ChunkBytes chunk, const Timing& t) {   // 80414B80 (count kept)
    start(c, chunk, t);
}

void curve_step_forward(CurveCursor& c, ChunkBytes chunk, const Timing& t) {   // 80415300
    if (!(t.time >= kEps + c.b.time)) return;
    std::int16_t index = c.index;
    std::uint32_t pos = c.position;
    while (index < c.count) {
        c.a = c.b;
        std::uint16_t w;
        pos += decode(chunk, pos, c.b, w);
        index = std::int16_t(index + 1);
        if (t.time < kEps + c.b.time) break;
    }
    c.index = index;
    c.position = pos;
}

void curve_step_backward(CurveCursor& c, ChunkBytes chunk, const Timing& t) {   // 80415170
    if (!(t.time <= c.a.time - kEps)) return;
    std::int16_t index = c.index;
    std::uint32_t pos = c.position;
    while (index > -1) {
        c.b = c.a;
        std::uint16_t w;
        decode(chunk, pos, c.a, w);
        index = std::int16_t(index - 1);
        pos -= previous_size(w);
        if (t.time > c.a.time - kEps) break;
    }
    c.index = index;
    c.position = pos;
}

// ------------------------------------------------------------------------------------------------ value decoders
float curve_value_forward(CurveCursor& c, ChunkBytes chunk, const Timing& t) {   // 804162AC
    if (c.flags & curve_flags::ValueConst) return c.cached_value;
    const float T = t.time;
    if (!(T >= kEps + c.a.time)) return c.a.value;
    if (!(T >= c.b.time - kEps)) {
        if (c.flags & curve_flags::FlatSegment) return c.cached_value;
        return hermite_value(c.a, c.b, T);
    }
    if (!(T >= kEps + c.b.time)) return c.b.value;
    std::int16_t index = c.index;
    std::uint32_t pos = c.position;
    while (index < c.count) {
        c.a = c.b;
        std::uint16_t w;
        pos += decode(chunk, pos, c.b, w);
        index = std::int16_t(index + 1);
        if (!(T >= kEps + c.b.time)) {
            c.index = index;
            c.position = pos;
            c.flags &= ~curve_flags::ValueConst;
            if (!(T >= c.b.time - kEps)) {
                if (c.a.out == 0.0f && c.b.in == 0.0f && c.a.value == c.b.value) {
                    c.flags |= curve_flags::FlatSegment;
                    c.cached_value = c.b.value;
                    return c.b.value;
                }
                c.flags &= ~curve_flags::FlatSegment;
                return hermite_value(c.a, c.b, T);
            }
            return c.b.value;
        }
    }
    c.index = index;
    c.position = pos;
    if (0.0f == c.b.out) {
        c.flags |= curve_flags::ValueConst;
        c.cached_value = c.b.value;
        return c.b.value;
    }
    return std::fmaf(c.b.out, T - c.b.time, c.b.value);
}

float curve_value_backward(CurveCursor& c, ChunkBytes chunk, const Timing& t) {   // 80415A30
    if (c.flags & curve_flags::ValueConst) return c.cached_value;
    const float T = t.time;
    if (!(T <= c.b.time - kEps)) return c.b.value;
    if (!(T <= kEps + c.a.time)) {
        if (c.flags & curve_flags::FlatSegment) return c.cached_value;
        return hermite_value(c.a, c.b, T);
    }
    if (!(T <= c.a.time - kEps)) return c.a.value;
    std::int16_t index = c.index;
    std::uint32_t pos = c.position;
    while (index > -1) {
        c.b = c.a;
        std::uint16_t w;
        decode(chunk, pos, c.a, w);
        index = std::int16_t(index - 1);
        pos -= previous_size(w);
        if (!(T <= c.a.time - kEps)) {
            c.flags &= ~curve_flags::ValueConst;          // index / position NOT stored (original quirk)
            if (!(T <= kEps + c.a.time)) {
                if (c.a.out == 0.0f && c.b.in == 0.0f && c.a.value == c.b.value) {
                    c.flags |= curve_flags::FlatSegment;
                    c.cached_value = c.a.value;
                    return c.a.value;
                }
                c.flags &= ~curve_flags::FlatSegment;
                return hermite_value(c.a, c.b, T);
            }
            return c.a.value;
        }
    }
    c.index = index;
    c.position = pos;
    if (0.0f == c.a.in) {
        c.flags |= curve_flags::ValueConst;
        c.cached_value = c.a.value;
        return c.a.value;
    }
    return std::fmaf(c.a.in, T - c.a.time, c.a.value);
}

// ------------------------------------------------------------------------------------------------ slope decoders
float curve_slope_forward(CurveCursor& c, ChunkBytes chunk, const Timing& t) {   // 80416020
    if (c.flags & curve_flags::SlopeConst) return c.cached_slope;
    const float T = t.time;
    if (!(T >= kEps + c.a.time)) return !(T >= c.a.time - kEps) ? c.a.in : mid_slope(c.a);
    if (!(T >= c.b.time - kEps)) return hermite_slope(c.a, c.b, T);
    if (!(T >= kEps + c.b.time)) return mid_slope(c.b);
    std::int16_t index = c.index;
    std::uint32_t pos = c.position;
    while (index < c.count) {
        c.a = c.b;
        std::uint16_t w;
        pos += decode(chunk, pos, c.b, w);
        index = std::int16_t(index + 1);
        if (!(T >= kEps + c.b.time)) {
            c.index = index;
            c.position = pos;
            c.flags &= ~curve_flags::SlopeConst;
            return !(T >= c.b.time - kEps) ? hermite_slope(c.a, c.b, T) : mid_slope(c.b);
        }
    }
    c.index = index;
    c.position = pos;
    c.flags |= curve_flags::SlopeConst;
    c.cached_slope = c.b.out;
    return c.b.out;
}

float curve_slope_backward(CurveCursor& c, ChunkBytes chunk, const Timing& t) {   // 8041579C
    if (c.flags & curve_flags::SlopeConst) return c.cached_slope;
    const float T = t.time;
    if (!(T <= c.b.time - kEps)) return !(T <= kEps + c.b.time) ? c.b.out : mid_slope(c.b);
    if (!(T <= kEps + c.a.time)) return hermite_slope(c.a, c.b, T);
    if (!(T <= c.a.time - kEps)) return mid_slope(c.a);
    std::int16_t index = c.index;
    std::uint32_t pos = c.position;
    while (index > -1) {
        c.b = c.a;
        std::uint16_t w;
        decode(chunk, pos, c.a, w);
        index = std::int16_t(index - 1);
        pos -= previous_size(w);
        if (!(T <= c.a.time - kEps)) {
            c.flags &= ~curve_flags::SlopeConst;          // index / position NOT stored (original quirk)
            return !(T <= kEps + c.a.time) ? hermite_slope(c.a, c.b, T) : mid_slope(c.a);
        }
    }
    c.index = index;
    c.position = pos;
    c.flags |= curve_flags::SlopeConst;
    c.cached_slope = c.a.in;
    return c.a.in;
}

// ------------------------------------------------------------------------------------------------ value + slope decoders
void curve_both_forward(CurveCursor& c, ChunkBytes chunk, const Timing& t, float& value, float& slope) {   // 80415D0C
    if (c.flags & curve_flags::BothConst) { value = c.cached_value; slope = c.cached_slope; return; }
    const float T = t.time;
    if (!(T >= kEps + c.a.time)) { value = c.a.value; slope = !(T >= c.a.time - kEps) ? c.a.in : mid_slope(c.a); return; }
    if (!(T >= c.b.time - kEps)) { hermite_both(c.a, c.b, T, value, slope); return; }
    if (!(T >= kEps + c.b.time)) { value = c.b.value; slope = mid_slope(c.b); return; }
    std::int16_t index = c.index;
    std::uint32_t pos = c.position;
    while (index < c.count) {
        c.a = c.b;
        std::uint16_t w;
        pos += decode(chunk, pos, c.b, w);
        index = std::int16_t(index + 1);
        if (!(T >= kEps + c.b.time)) {
            c.index = index;
            c.position = pos;
            c.flags &= ~curve_flags::BothConst;
            if (!(T >= c.b.time - kEps)) hermite_both(c.a, c.b, T, value, slope);
            else { value = c.b.value; slope = mid_slope(c.b); }
            return;
        }
    }
    c.index = index;
    c.position = pos;
    if (0.0f == c.b.out) {
        c.flags |= curve_flags::BothConst;
        c.cached_value = c.b.value;
        c.cached_slope = c.b.out;
        value = c.b.value; slope = c.b.out;
        return;
    }
    value = std::fmaf(c.b.out, T - c.b.time, c.b.value);
    slope = c.b.out;
}

void curve_both_backward(CurveCursor& c, ChunkBytes chunk, const Timing& t, float& value, float& slope) {   // 80415488
    if (c.flags & curve_flags::BothConst) { value = c.cached_value; slope = c.cached_slope; return; }
    const float T = t.time;
    if (!(T <= c.b.time - kEps)) { value = c.b.value; slope = !(T <= kEps + c.b.time) ? c.b.out : mid_slope(c.b); return; }
    if (!(T <= kEps + c.a.time)) { hermite_both(c.a, c.b, T, value, slope); return; }
    if (!(T <= c.a.time - kEps)) { value = c.a.value; slope = mid_slope(c.a); return; }
    std::int16_t index = c.index;
    std::uint32_t pos = c.position;
    while (index > -1) {
        c.b = c.a;
        std::uint16_t w;
        decode(chunk, pos, c.a, w);
        index = std::int16_t(index - 1);
        pos -= previous_size(w);
        if (!(T <= c.a.time - kEps)) {
            c.flags &= ~curve_flags::BothConst;           // index / position NOT stored (original quirk)
            if (!(T <= kEps + c.a.time)) hermite_both(c.a, c.b, T, value, slope);
            else { value = c.a.value; slope = mid_slope(c.a); }
            return;
        }
    }
    c.index = index;
    c.position = pos;
    if (0.0f == c.a.in) {
        c.flags |= curve_flags::BothConst;
        c.cached_value = c.a.value;
        c.cached_slope = c.a.in;
        value = c.a.value; slope = c.a.in;
        return;
    }
    value = std::fmaf(c.a.in, T - c.a.time, c.a.value);
    slope = c.a.in;
}

}  // namespace shadow::motion
