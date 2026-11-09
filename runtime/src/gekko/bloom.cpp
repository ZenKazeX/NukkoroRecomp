#include "gekko/bloom.h"

#include <algorithm>
#include <cmath>

namespace gekko::bloom {
namespace {

// The original authors its kernel against a 128-tall level on a 480-tall
// raster, i.e. one level texel per 3.75 raster lines, and 96 columns per raster
// WIDTH of 640 - which is 128 columns only because 640/480 is 4:3. Keeping
// those two ratios is what keeps the halo the same angular size at every
// aspect; keeping the texel COUNT would not.
constexpr float kLevelColumnsPerHeight = 96.0f;
constexpr float kLevelRowsPerHeight = 128.0f;

// Even, and never below 2, so level B is exactly half of level A.
u32 even_at_least_two(long value) noexcept {
    if (value < 2) return 2u;
    return static_cast<u32>(value) & ~1u;
}

u8 to_byte(float value) noexcept {
    if (!(value > 0.0f)) return 0u;
    if (value >= 255.0f) return 255u;
    // Round to nearest, which is what a UNORM8 render target does on the way
    // out of the blend unit.
    return static_cast<u8>(value + 0.5f);
}

// Bilinear fetch in texel space with the half-texel convention, CLAMP
// addressing. `x`, `y` are texel-centre coordinates (texel k is at k + 0.5).
void sample_bilinear(const Image& src, float x, float y, float out[4]) noexcept {
    const float fx = x - 0.5f;
    const float fy = y - 0.5f;
    const long x0 = static_cast<long>(std::floor(fx));
    const long y0 = static_cast<long>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const auto clamp_x = [&](long v) {
        return static_cast<u32>(std::clamp<long>(v, 0, static_cast<long>(src.width) - 1));
    };
    const auto clamp_y = [&](long v) {
        return static_cast<u32>(std::clamp<long>(v, 0, static_cast<long>(src.height) - 1));
    };
    const u8* p00 = src.at(clamp_x(x0), clamp_y(y0));
    const u8* p10 = src.at(clamp_x(x0 + 1), clamp_y(y0));
    const u8* p01 = src.at(clamp_x(x0), clamp_y(y0 + 1));
    const u8* p11 = src.at(clamp_x(x0 + 1), clamp_y(y0 + 1));
    for (u32 c = 0; c < 4u; ++c) {
        const float top = p00[c] * (1.0f - tx) + p10[c] * tx;
        const float bottom = p01[c] * (1.0f - tx) + p11[c] * tx;
        out[c] = top * (1.0f - ty) + bottom * ty;
    }
}

}  // namespace

LevelSizes level_sizes(u32 width, u32 height, u32 quality) noexcept {
    LevelSizes sizes;
    if (width == 0u || height == 0u) return sizes;
    const u32 q = quality == 0u ? 1u : quality;
    const float qf = static_cast<float>(q);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    sizes.a_width = even_at_least_two(std::lround(kLevelColumnsPerHeight * qf * aspect));
    sizes.a_height = even_at_least_two(std::lround(kLevelRowsPerHeight * qf));
    sizes.b_width = sizes.a_width / 2u;
    sizes.b_height = sizes.a_height / 2u;
    return sizes;
}

float capture_gain(u8 intensity, bool apply_legacy_scale) noexcept {
    if (!apply_legacy_scale) {
        return static_cast<float>(intensity) / 255.0f;
    }
    // fmuls then fctiwz: multiply, then truncate toward zero to an integer.
    const float scaled = static_cast<float>(intensity) * 0.94999998807907104f;
    const float truncated = std::trunc(scaled);
    return truncated / 255.0f;
}

float level_gain(float capture, std::int32_t extra_h, std::int32_t extra_v) noexcept {
    return capture * static_cast<float>(taps_per_half(extra_h)) *
           static_cast<float>(taps_per_half(extra_v));
}

void tap_offsets(std::int32_t extra, float spread, std::vector<float>& out) {
    out.clear();
    out.push_back(0.0f);  // the unconditional centre tap
    if (extra <= 0) return;
    out.reserve(static_cast<std::size_t>(extra) + 1u);
    for (std::int32_t i = 0; i < extra; ++i) {
        const float step = static_cast<float>((i / 2) + 1);
        const float sign = (i % 2) == 0 ? 1.0f : -1.0f;
        out.push_back(step * sign * spread);
    }
}

Image downsample(const Image& src, u32 dst_width, u32 dst_height, float gain, Capture capture) {
    Image dst(dst_width, dst_height);
    if (src.width == 0u || src.height == 0u || dst_width == 0u || dst_height == 0u) return dst;
    const float sx = static_cast<float>(src.width) / static_cast<float>(dst_width);
    const float sy = static_cast<float>(src.height) / static_cast<float>(dst_height);

    if (capture == Capture::Probe) {
        for (u32 j = 0; j < dst_height; ++j) {
            for (u32 i = 0; i < dst_width; ++i) {
                float texel[4];
                sample_bilinear(src, sx * (static_cast<float>(i) + 0.5f),
                                sy * (static_cast<float>(j) + 0.5f), texel);
                u8* out = dst.at(i, j);
                for (u32 c = 0; c < 4u; ++c) out[c] = to_byte(texel[c] * gain);
            }
        }
        return dst;
    }

    // The exact continuous box. `sx` is 11.16 at 1080p, not an integer, so the
    // partial texels at each end carry fractional weight; truncating the
    // footprint to whole texels reintroduces a beat pattern.
    for (u32 j = 0; j < dst_height; ++j) {
        const float y_lo = sy * static_cast<float>(j);
        const float y_hi = sy * static_cast<float>(j + 1u);
        const u32 y_first = static_cast<u32>(std::floor(y_lo));
        const u32 y_last = std::min<u32>(src.height - 1u,
                                         static_cast<u32>(std::ceil(y_hi)) - 1u);
        for (u32 i = 0; i < dst_width; ++i) {
            const float x_lo = sx * static_cast<float>(i);
            const float x_hi = sx * static_cast<float>(i + 1u);
            const u32 x_first = static_cast<u32>(std::floor(x_lo));
            const u32 x_last = std::min<u32>(src.width - 1u,
                                             static_cast<u32>(std::ceil(x_hi)) - 1u);
            float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float weight_total = 0.0f;
            for (u32 y = y_first; y <= y_last; ++y) {
                const float wy = std::min(y_hi, static_cast<float>(y + 1u)) -
                               std::max(y_lo, static_cast<float>(y));
                if (!(wy > 0.0f)) continue;
                for (u32 x = x_first; x <= x_last; ++x) {
                    const float wx = std::min(x_hi, static_cast<float>(x + 1u)) -
                                   std::max(x_lo, static_cast<float>(x));
                    if (!(wx > 0.0f)) continue;
                    const float w = wx * wy;
                    const u8* p = src.at(x, y);
                    for (u32 c = 0; c < 4u; ++c) sum[c] += p[c] * w;
                    weight_total += w;
                }
            }
            u8* out = dst.at(i, j);
            if (weight_total > 0.0f) {
                for (u32 c = 0; c < 4u; ++c) out[c] = to_byte((sum[c] / weight_total) * gain);
            }
        }
    }
    return dst;
}

Image blur_axis(const Image& src, const std::vector<float>& taps, bool horizontal) {
    Image dst(src.width, src.height);
    if (src.width == 0u || src.height == 0u || taps.empty()) return dst;
    const long extent = static_cast<long>(horizontal ? src.width : src.height);
    for (u32 y = 0; y < src.height; ++y) {
        for (u32 x = 0; x < src.width; ++x) {
            const long k = static_cast<long>(horizontal ? x : y);
            float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (const float d : taps) {
                const float position = static_cast<float>(k) - d;
                // Zero border: a tap whose source index leaves the level
                // contributes nothing. The original shortens the quad rather
                // than clamping the UV, which is the same thing.
                if (position < 0.0f || position > static_cast<float>(extent - 1)) continue;
                const float fx = horizontal ? position + 0.5f : static_cast<float>(x) + 0.5f;
                const float fy = horizontal ? static_cast<float>(y) + 0.5f : position + 0.5f;
                float texel[4];
                sample_bilinear(src, fx, fy, texel);
                for (u32 c = 0; c < 4u; ++c) sum[c] += texel[c];
            }
            u8* out = dst.at(x, y);
            for (u32 c = 0; c < 4u; ++c) out[c] = to_byte(sum[c]);
        }
    }
    return dst;
}

Image merge_up2x(const Image& level_a, const Image& level_b) {
    Image dst(level_a.width, level_a.height);
    if (level_a.width == 0u || level_a.height == 0u) return dst;
    for (u32 y = 0; y < level_a.height; ++y) {
        for (u32 x = 0; x < level_a.width; ++x) {
            const u8* a = level_a.at(x, y);
            u8* out = dst.at(x, y);
            if (level_b.width == 0u || level_b.height == 0u) {
                for (u32 c = 0; c < 4u; ++c) out[c] = a[c];
                continue;
            }
            // uv = (k + 0.5) / A_extent, mapped onto B's texel grid.
            const float bx = (static_cast<float>(x) + 0.5f) *
                           static_cast<float>(level_b.width) / static_cast<float>(level_a.width);
            const float by = (static_cast<float>(y) + 0.5f) *
                           static_cast<float>(level_b.height) / static_cast<float>(level_a.height);
            float texel[4];
            sample_bilinear(level_b, bx, by, texel);
            for (u32 c = 0; c < 4u; ++c) out[c] = to_byte(static_cast<float>(a[c]) + texel[c]);
        }
    }
    return dst;
}

void composite(Image& scene, const Image& glare_source, const Image& level) {
    if (scene.width == 0u || scene.height == 0u) return;
    const bool has_source =
        glare_source.width == scene.width && glare_source.height == scene.height;
    const bool has_level = level.width != 0u && level.height != 0u;
    for (u32 y = 0; y < scene.height; ++y) {
        for (u32 x = 0; x < scene.width; ++x) {
            u8* out = scene.at(x, y);
            float add[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (has_source) {
                const u8* s = glare_source.at(x, y);
                for (u32 c = 0; c < 4u; ++c) add[c] += s[c];
            }
            if (has_level) {
                const float lx = (static_cast<float>(x) + 0.5f) * static_cast<float>(level.width) /
                               static_cast<float>(scene.width);
                const float ly = (static_cast<float>(y) + 0.5f) * static_cast<float>(level.height) /
                               static_cast<float>(scene.height);
                float texel[4];
                sample_bilinear(level, lx, ly, texel);
                for (u32 c = 0; c < 4u; ++c) add[c] += texel[c];
            }
            for (u32 c = 0; c < 4u; ++c) out[c] = to_byte(static_cast<float>(out[c]) + add[c]);
        }
    }
}

Image run(const Image& glare_source, const Parameters& parameters, u32 quality,
          Capture capture) {
    Image empty;
    if (!gate_open(parameters)) return empty;
    const LevelSizes sizes = level_sizes(glare_source.width, glare_source.height, quality);
    if (!sizes.valid()) return empty;

    std::vector<float> taps;
    Image level_a;
    if (parameters.enable_a) {
        level_a = downsample(glare_source, sizes.a_width, sizes.a_height,
                             capture_gain_a(parameters), capture);
        tap_offsets(parameters.taps_ah, parameters.spread_a, taps);
        level_a = blur_axis(level_a, taps, true);
        tap_offsets(parameters.taps_av, parameters.spread_a, taps);
        level_a = blur_axis(level_a, taps, false);
    }

    Image level_b;
    if (parameters.enable_b) {
        if (parameters.b_from_source || !parameters.enable_a) {
            level_b = downsample(glare_source, sizes.b_width, sizes.b_height,
                                 capture_gain(parameters.intensity_b, true), capture);
        } else {
            // Cascade: an exact 2x2 box of level A, which the even sizing rule
            // guarantees. The raw byte gain, with no 0.95, is the guest's.
            level_b = downsample(level_a, sizes.b_width, sizes.b_height,
                                 capture_gain(parameters.intensity_b, false), Capture::Box);
        }
        tap_offsets(parameters.taps_bh, parameters.spread_b, taps);
        level_b = blur_axis(level_b, taps, true);
        tap_offsets(parameters.taps_bv, parameters.spread_b, taps);
        level_b = blur_axis(level_b, taps, false);
    }

    if (!parameters.enable_a) return level_b;
    if (!parameters.enable_b) return level_a;
    if (parameters.separate) {
        // The caller composites both levels; this reference returns level A and
        // leaves B to a second magnification. Kept explicit rather than silently
        // merging, because the two arms are visibly different.
        return level_a;
    }
    return merge_up2x(level_a, level_b);
}

}  // namespace gekko::bloom
