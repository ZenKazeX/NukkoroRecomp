// Tests for the new native bloom system's mathematics.
//
// Every expectation here is traceable to docs/effects-2026-09-21/BLOOM_SPEC.md
// and, through it, to an instruction in the PAL DOL. The point of the suite is
// that the GPU implementation can be checked against this reference pass by
// pass, and that the sizing rule and the gain chain cannot drift silently.

#include "gekko/bloom.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

void check_near(double actual, double expected, double tolerance, const std::string& what) {
    if (!(std::fabs(actual - expected) <= tolerance)) {
        std::printf("FAIL: %s (got %.6f, expected %.6f +/- %.6f)\n", what.c_str(), actual,
                    expected, tolerance);
        ++g_failures;
    }
}

using namespace gekko;

// ---------------------------------------------------------------------------

void test_level_sizes() {
    // Rule R1 must reproduce the console's own levels exactly at 4:3.
    const bloom::LevelSizes at_4_3 = bloom::level_sizes(640, 480, 1);
    check(at_4_3.a_width == 128u && at_4_3.a_height == 128u,
          "4:3 level A is exactly 128x128 (the console's own size)");
    check(at_4_3.b_width == 64u && at_4_3.b_height == 64u, "4:3 level B is exactly 64x64");

    // The same aspect at any resolution gives the same levels: the bloom is a
    // function of the aspect and the quality, never of the internal scale.
    for (const auto& size : {std::pair<u32, u32>{640, 480}, {1280, 960}, {1920, 1440},
                             {3840, 2880}}) {
        const bloom::LevelSizes s = bloom::level_sizes(size.first, size.second, 1);
        check(s.a_width == 128u && s.a_height == 128u,
              "level A is scale-invariant at 4:3 (" + std::to_string(size.first) + "x" +
                  std::to_string(size.second) + ")");
    }

    // 16:9 keeps the vertical resolution and widens: the halo keeps its angular
    // size instead of being stretched with the frame.
    const bloom::LevelSizes at_16_9 = bloom::level_sizes(1920, 1080, 1);
    check(at_16_9.a_height == 128u, "16:9 level A keeps 128 rows");
    check(at_16_9.a_width == 170u, "16:9 level A widens to 170 columns (even(round(96*16/9)))");

    // Ultrawide.
    const bloom::LevelSizes at_21_9 = bloom::level_sizes(2560, 1080, 1);
    check(at_21_9.a_height == 128u, "21:9 level A keeps 128 rows");
    // 96 * 2560/1080 = 227.556 -> round 228, already even.
    check(at_21_9.a_width == 228u, "21:9 level A widens to 228 columns");

    // Every dimension even, so the cascade's 2x2 box is exact.
    for (const auto& size : {std::pair<u32, u32>{1920, 1080}, {2560, 1080}, {1366, 768},
                             {3440, 1440}, {800, 600}}) {
        const bloom::LevelSizes s = bloom::level_sizes(size.first, size.second, 1);
        check((s.a_width % 2u) == 0u && (s.a_height % 2u) == 0u,
              "level A dimensions are even at " + std::to_string(size.first) + "x" +
                  std::to_string(size.second));
        check(s.b_width * 2u == s.a_width && s.b_height * 2u == s.a_height,
              "level B is exactly half of level A at " + std::to_string(size.first) + "x" +
                  std::to_string(size.second));
    }

    // Quality scales the level, nothing else.
    const bloom::LevelSizes q2 = bloom::level_sizes(640, 480, 2);
    check(q2.a_width == 256u && q2.a_height == 256u, "quality 2 doubles level A");

    // Degenerate inputs must not produce an unusable set.
    check(!bloom::level_sizes(0, 0, 1).valid(), "a zero frame yields no levels");
}

void test_gain_chain() {
    bloom::Parameters p;  // the shipped values

    // Level A: trunc(32 * 0.94999998807907104) / 255 = trunc(30.4) / 255.
    check_near(bloom::capture_gain_a(p), 30.0 / 255.0, 1e-7, "level A capture gain is 30/255");

    // Level B cascades in the shipped configuration and takes the RAW byte -
    // the 0.95 is absent on that arm (guest lbz 0x8042F690 straight to stb).
    check_near(bloom::capture_gain_b(p), 48.0 / 255.0, 1e-7,
               "level B cascade capture gain is the raw 48/255");

    // ... but the direct-from-source arm does apply it.
    bloom::Parameters direct = p;
    direct.b_from_source = true;
    check_near(bloom::capture_gain_b(direct), std::trunc(48.0 * 0.94999998807907104) / 255.0,
               1e-7, "level B direct capture gain applies the 0.95");

    // Taps per half include the unconditional centre tap.
    check(bloom::taps_per_half(2) == 3, "extra 2 gives 3 taps per half");
    check(bloom::taps_per_half(4) == 5, "extra 4 gives 5 taps per half");
    check(bloom::taps_per_half(0) == 1, "extra 0 gives the centre tap alone");

    // The totals the specification states, which are what make level B clip.
    check_near(bloom::level_gain(bloom::capture_gain_a(p), p.taps_ah, p.taps_av), 1.0588, 1e-3,
               "level A total gain is ~1.0588 (unity)");
    check_near(bloom::level_gain(bloom::capture_gain_b(p), p.taps_bh, p.taps_bv), 4.7059, 1e-3,
               "level B total gain is ~4.7059");

    // Level B therefore saturates once its source exceeds ~51/255.
    const double clip_at = 255.0 / bloom::level_gain(bloom::capture_gain_b(p), p.taps_bh,
                                                     p.taps_bv);
    check_near(clip_at, 54.2, 1.0, "level B clips at roughly 54/255 of source");
}

void test_tap_sets() {
    std::vector<float> taps;

    bloom::tap_offsets(2, 1.0f, taps);
    check(taps.size() == 3u, "extra 2 yields 3 taps");
    check(taps[0] == 0.0f && taps[1] == 1.0f && taps[2] == -1.0f,
          "extra 2 yields { 0, +1, -1 }");

    bloom::tap_offsets(4, 1.0f, taps);
    check(taps.size() == 5u, "extra 4 yields 5 taps");
    check(taps[0] == 0.0f && taps[1] == 1.0f && taps[2] == -1.0f && taps[3] == 2.0f &&
              taps[4] == -2.0f,
          "extra 4 yields { 0, +1, -1, +2, -2 }");

    // The asymmetry at an odd count is deliberate, not a bug to smooth over.
    bloom::tap_offsets(3, 1.0f, taps);
    check(taps.size() == 4u && taps[3] == 2.0f,
          "an odd extra count is asymmetric: { 0, +1, -1, +2 }");

    // The spread scales the offsets and nothing else.
    bloom::tap_offsets(2, 2.5f, taps);
    check(taps[1] == 2.5f && taps[2] == -2.5f, "the spread scales the tap offsets");

    bloom::tap_offsets(0, 1.0f, taps);
    check(taps.size() == 1u && taps[0] == 0.0f, "extra 0 is the centre tap alone");
}

// A flat field must survive a box downsample exactly, scaled by the gain, at
// any ratio - including the non-integer ones. If the footprint were truncated
// to whole texels this would beat.
void test_box_downsample_is_flat_preserving() {
    for (const auto& src_size : {std::pair<u32, u32>{640, 480}, {1920, 1080}, {1366, 768}}) {
        bloom::Image src(src_size.first, src_size.second);
        for (auto& v : src.rgba) v = 200u;
        const bloom::LevelSizes sizes =
            bloom::level_sizes(src_size.first, src_size.second, 1);
        const bloom::Image dst =
            bloom::downsample(src, sizes.a_width, sizes.a_height, 1.0f, bloom::Capture::Box);
        u32 wrong = 0;
        for (const auto v : dst.rgba) {
            if (v != 200u) ++wrong;
        }
        check(wrong == 0u, "a flat field downsamples flat at " +
                               std::to_string(src_size.first) + "x" +
                               std::to_string(src_size.second) + " (" +
                               std::to_string(wrong) + " wrong texels)");
    }
}

// The box and the probe have the same FIRST MOMENT. Replacing the probe with
// the box changes variance, not brightness - which is the whole argument for
// making the change, so it has to be true.
void test_box_and_probe_agree_in_mean() {
    bloom::Image src(640, 480);
    // A smooth gradient plus a hard highlight, which is what a glare source is.
    for (u32 y = 0; y < src.height; ++y) {
        for (u32 x = 0; x < src.width; ++x) {
            u8* p = src.at(x, y);
            const u32 base = (x * 255u) / src.width;
            const bool spot = (x > 300u && x < 340u && y > 200u && y < 240u);
            const u32 value = spot ? 255u : base / 2u;
            p[0] = static_cast<u8>(value);
            p[1] = static_cast<u8>(value);
            p[2] = static_cast<u8>(value);
            p[3] = 255u;
        }
    }
    const bloom::Image box = bloom::downsample(src, 128, 128, 1.0f, bloom::Capture::Box);
    const bloom::Image probe = bloom::downsample(src, 128, 128, 1.0f, bloom::Capture::Probe);
    double box_sum = 0.0;
    double probe_sum = 0.0;
    for (std::size_t i = 0; i < box.rgba.size(); i += 4u) {
        box_sum += box.rgba[i];
        probe_sum += probe.rgba[i];
    }
    const double n = static_cast<double>(box.rgba.size() / 4u);
    check_near(box_sum / n, probe_sum / n, 2.0,
               "the box and the probe agree in mean (brightness is unchanged)");
}

// Zero border, not clamp: a bright edge column must FADE at the frame edge
// rather than smear along it.
void test_blur_border_is_zero() {
    bloom::Image src(16, 4);
    for (u32 y = 0; y < src.height; ++y) {
        u8* p = src.at(0, y);
        p[0] = p[1] = p[2] = p[3] = 100u;
    }
    std::vector<float> taps;
    bloom::tap_offsets(2, 1.0f, taps);  // { 0, +1, -1 }
    const bloom::Image out = bloom::blur_axis(src, taps, true);

    // Texel 0 receives its own 100 and the tap at -1 (source index 1, which is
    // zero); the tap at +1 reads source index -1, which is outside and
    // contributes nothing. So 100, not 200.
    check(out.at(0, 0)[0] == 100u, "the zero border does not wrap or clamp at texel 0");
    // Texel 1 receives source 0 through the +1 tap: 100.
    check(out.at(1, 0)[0] == 100u, "texel 1 receives the edge column once");
    // Far from the edge everything is zero.
    check(out.at(8, 0)[0] == 0u, "the blur does not smear the edge across the level");
}

void test_blur_saturates() {
    bloom::Image src(8, 1);
    for (auto& v : src.rgba) v = 200u;
    std::vector<float> taps;
    bloom::tap_offsets(4, 1.0f, taps);  // 5 taps
    const bloom::Image out = bloom::blur_axis(src, taps, true);
    // 5 x 200 = 1000, clamped to 255. The saturation IS the look.
    check(out.at(4, 0)[0] == 255u, "an unweighted tap sum saturates at 255");
}

// The whole chain must run, produce the level the compositor will magnify, and
// obey the gate.
void test_chain() {
    bloom::Image source(320, 240);
    for (u32 y = 100; y < 140; ++y) {
        for (u32 x = 140; x < 180; ++x) {
            u8* p = source.at(x, y);
            p[0] = p[1] = p[2] = p[3] = 255u;
        }
    }
    bloom::Parameters p;
    const bloom::Image level = bloom::run(source, p, 1, bloom::Capture::Box);
    const bloom::LevelSizes sizes = bloom::level_sizes(source.width, source.height, 1);
    check(level.width == sizes.a_width && level.height == sizes.a_height,
          "the chain returns a level-A-sized image");
    u32 lit = 0;
    for (std::size_t i = 0; i < level.rgba.size(); i += 4u) {
        if (level.rgba[i] != 0u) ++lit;
    }
    check(lit > 0u, "the chain produces a non-empty halo from a bright spot");

    // The halo must be WIDER than the spot: that is what a blur is.
    const bloom::Image unblurred =
        bloom::downsample(source, sizes.a_width, sizes.a_height, bloom::capture_gain_a(p),
                          bloom::Capture::Box);
    u32 unblurred_lit = 0;
    for (std::size_t i = 0; i < unblurred.rgba.size(); i += 4u) {
        if (unblurred.rgba[i] != 0u) ++unblurred_lit;
    }
    check(lit > unblurred_lit, "the blurred halo covers more texels than the capture");

    // The gate.
    bloom::Parameters off = p;
    off.enable = false;
    check(bloom::run(source, off, 1, bloom::Capture::Box).width == 0u,
          "a closed master gate produces nothing");
    bloom::Parameters no_levels = p;
    no_levels.enable_a = false;
    no_levels.enable_b = false;
    check(bloom::run(source, no_levels, 1, bloom::Capture::Box).width == 0u,
          "both levels disabled produces nothing");
}

// Compositing is additive and saturating, and an empty level leaves the scene
// plus the glare source alone.
void test_composite() {
    bloom::Image scene(8, 8);
    for (auto& v : scene.rgba) v = 100u;
    bloom::Image source(8, 8);
    for (auto& v : source.rgba) v = 50u;
    bloom::Image level;  // empty
    bloom::composite(scene, source, level);
    check(scene.at(0, 0)[0] == 150u, "the glare source is added back at unit weight");

    bloom::Image bright(8, 8);
    for (auto& v : bright.rgba) v = 250u;
    bloom::composite(scene, bright, level);
    check(scene.at(0, 0)[0] == 255u, "the composite saturates");
}

}  // namespace

int main() {
    test_level_sizes();
    test_gain_chain();
    test_tap_sets();
    test_box_downsample_is_flat_preserving();
    test_box_and_probe_agree_in_mean();
    test_blur_border_is_zero();
    test_blur_saturates();
    test_chain();
    test_composite();

    if (g_failures != 0) {
        std::printf("bloom_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("bloom_tests: all checks passed\n");
    return 0;
}
