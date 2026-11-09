#include "native_billboard.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace shadow::render {
namespace {
static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559,
              "native billboard requires IEEE binary32");
std::uint32_t bits(float value) noexcept {
    std::uint32_t word; std::memcpy(&word, &value, sizeof(word)); return word;
}
float value(std::uint32_t word) noexcept {
    float result; std::memcpy(&result, &word, sizeof(result)); return result;
}
bool nan(float v) noexcept { return (bits(v) & 0x7fffffffu) > 0x7f800000u; }
float quiet(float v) noexcept { return value(bits(v) | 0x00400000u); }
float flush(float v) noexcept {
    const auto word = bits(v);
    return (word & 0x7f800000u) == 0 ? value(word & 0x80000000u) : v;
}
float neg(float v) noexcept { return value(bits(v) ^ 0x80000000u); }
float nabs(float v) noexcept { return value(bits(v) | 0x80000000u); }
float finish(double exact) noexcept {
    if (exact != exact) return value(0x7fc00000u);
    if (std::fabs(exact) < 0x1p-126) return std::copysign(0.0f, static_cast<float>(exact));
    return static_cast<float>(exact);
}
float add(float a, float b) noexcept {
    if (nan(a)) return quiet(a); if (nan(b)) return quiet(b);
    return finish(double(flush(a)) + double(flush(b)));
}
float sub(float a, float b) noexcept {
    if (nan(a)) return quiet(a); if (nan(b)) return quiet(b);
    return finish(double(flush(a)) - double(flush(b)));
}
float mul(float a, float b) noexcept {
    if (nan(a)) return quiet(a); if (nan(b)) return quiet(b);
    return finish(double(flush(a)) * double(flush(b)));
}
float madd(float a, float multiplier, float addend) noexcept {
    // Payload priority follows the authored expression: multiplicand, addend,
    // multiplier. std::fma alone does not specify NaN payload selection.
    if (nan(a)) return quiet(a);
    if (nan(addend)) return quiet(addend);
    if (nan(multiplier)) return quiet(multiplier);
    a = flush(a); multiplier = flush(multiplier); addend = flush(addend);
    const double product = double(a) * double(multiplier); // exact for finite binary32
    const double sum = product + double(addend);
    if (sum != sum) return value(0x7fc00000u);
    if (std::fabs(sum) < 0x1p-126) return std::copysign(0.0f, static_cast<float>(sum));
    if (std::fabs(sum) == 0x1p-126) {
        // A rounded double at the minimum normal can hide a smaller exact sum.
        // Error-free two-sum recovers the residual without changing host state.
        const double tail = sum - product;
        const double residual = (product - (sum - tail)) + (double(addend) - tail);
        if (std::signbit(sum) ? residual > 0 : residual < 0)
            return std::copysign(0.0f, static_cast<float>(sum));
    }
    return flush(std::fma(a, multiplier, addend)); // single rounding, including midpoint cases
}
} // namespace

BillboardCorners expand_billboard(const Billboard& particle, const BillboardBasis& basis,
                                 const BillboardCurve& curve) noexcept {
    std::array<float, 2> trig{}, extent{};
    for (std::size_t lane = 0; lane < 2; ++lane) {
        float angle = madd(curve.angle_multiplier[lane], particle.rotation, curve.offset[lane]);
        angle = add(angle, angle >= 0 ? neg(curve.period[lane]) : curve.period[lane]);
        const float scaled = mul(angle, curve.scale);
        const float envelope = madd(curve.angle_multiplier[lane], curve.period[1], nabs(angle));
        trig[lane] = mul(envelope, scaled);
        extent[lane] = mul(particle.size[lane], curve.half_extent);
    }
    BillboardCorners result{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const float right_cos = mul(basis.right[axis], trig[1]);
        const float right_sin = mul(basis.right[axis], trig[0]);
        const float width = mul(madd(neg(basis.up[axis]), trig[0], right_cos), extent[0]);
        const float height = mul(madd(basis.up[axis], trig[1], right_sin), extent[1]);
        // This grouping matters. (P-W)-H is not equivalent to P-(W+H).
        const float sum = add(width, height), difference = sub(width, height);
        result[0][axis] = flush(sub(particle.center[axis], sum));
        result[1][axis] = flush(add(particle.center[axis], difference));
        result[2][axis] = flush(add(particle.center[axis], sum));
        result[3][axis] = flush(sub(particle.center[axis], difference));
    }
    return result;
}

bool emit_billboards(const Billboard* particles, const BillboardAttributes* attributes,
                     std::size_t count, const BillboardBasis& basis, const BillboardCurve& curve,
                     RenderVertex* output, std::size_t output_capacity) noexcept {
    if (count == 0) return true;
    constexpr auto max = std::numeric_limits<std::size_t>::max();
    if (!particles || !attributes || !output || count > max / 4 || output_capacity < count * 4 ||
        count > max / sizeof(Billboard) || count > max / sizeof(BillboardAttributes) ||
        count * 4 > max / sizeof(RenderVertex)) return false;
    struct Range { std::uintptr_t begin, end; };
    const auto range = [](const void* pointer, std::size_t bytes, Range& result) {
        result.begin = reinterpret_cast<std::uintptr_t>(pointer);
        if (bytes > std::numeric_limits<std::uintptr_t>::max() - result.begin) return false;
        result.end = result.begin + bytes;
        return true;
    };
    Range target{}, source{}, attrs{}, camera{}, polynomial{};
    if (!range(output, count * 4 * sizeof(RenderVertex), target) ||
        !range(particles, count * sizeof(Billboard), source) ||
        !range(attributes, count * sizeof(BillboardAttributes), attrs) ||
        !range(&basis, sizeof(basis), camera) || !range(&curve, sizeof(curve), polynomial)) return false;
    const auto overlaps = [&target](const Range& r) { return target.begin < r.end && r.begin < target.end; };
    if (overlaps(source) || overlaps(attrs) || overlaps(camera) || overlaps(polynomial)) return false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto corners = expand_billboard(particles[i], basis, curve);
        const auto& attr = attributes[i];
        for (std::size_t corner = 0; corner < 4; ++corner) {
            RenderVertex vertex{};
            std::memcpy(vertex.position, corners[corner].data(), sizeof(vertex.position));
            std::memset(vertex.color0, 255, sizeof(vertex.color0));
            if (attr.normal_present)
                for (std::size_t axis = 0; axis < 3; ++axis) vertex.normal[axis] = flush(attr.normal[axis]);
            if (attr.color_present) std::memcpy(vertex.color0, attr.colors[corner].data(), sizeof(vertex.color0));
            if (attr.uv_present)
                for (std::size_t axis = 0; axis < 2; ++axis) vertex.uv[0][axis] = flush(attr.uv[corner][axis]);
            output[i * 4 + corner] = vertex;
        }
    }
    return true;
}
} // namespace shadow::render
