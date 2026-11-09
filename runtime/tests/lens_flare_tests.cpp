// The lens flare's native layout (runtime/lift/effects/lens_flare.h, docs/boundary-20261007/FLARE.md) against the
// console's builder fn_80432360 reconstructed from its disassembly (below, `console_flare`: the raster as the screen,
// the glow's bounds compared with centre-relative x, y; the square root taken exactly here so the two differ only in
// their rules). At the raster's own aspect the native layout must give the console's positions, alphas, flash and ghost
// visibility bit for bit, and the glow everywhere except the documented edge bands; on wider / narrower views and on
// split-screen viewports the console's ghost / flash field is laid over the screen the output shows, each axis by its
// own shown extent (FLARE.md 6, 10: no blink at the start of Sky Troops). Pure arithmetic.
#include "effects/lens_flare.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using namespace shadow::effects;

int failures = 0;
int checks = 0;

void check(bool value, const char* what) {
    ++checks;
    if (!value) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::uint32_t bits(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

// The game's element table (0x8053DAC8, read from a Glyphic Canyon savestate): position, width (+6), height (+4),
// colour in the draw's byte order.
const FlareElement kElements[7] = {
    {1.0f, 128.0f, 128.0f, 255, 255, 255, 255},  {0.6f, 128.0f, 128.0f, 255, 255, 255, 128},
    {0.4f, 64.0f, 64.0f, 255, 255, 255, 128},    {0.2f, 64.0f, 64.0f, 255, 255, 255, 128},
    {-0.2f, 64.0f, 64.0f, 255, 255, 255, 128},   {-0.4f, 64.0f, 64.0f, 255, 255, 255, 128},
    {-0.6f, 128.0f, 128.0f, 255, 255, 255, 128},
};

struct ConsoleFlare {
    bool in_front = false, glow = false, ghosts = false;
    FlareVertex glow_quad[4];
    FlareVertex ghosts_quads[4 * 6];
    float glow_depth = 0.0f;
    std::uint8_t flash = 0;
};

// fn_80432360 as the console runs it (docs/boundary-20261007/FLARE.md; the exact square root in RwV2dLength's place).
ConsoleFlare console_flare(const float s[3], float W, float H, float near_plane, float far_plane, float zn, float zf,
                           std::uint8_t intensity, std::uint8_t flash) {
    ConsoleFlare c;
    if (s[2] < near_plane)
        return c;
    c.in_front = true;
    const float u = s[0] / s[2];
    const float v = s[1] / s[2];
    const float hx = 0.5f * W;
    const float hy = 0.5f * H;
    const float x = std::fma(W, u, -hx);
    const float y = std::fma(H, v, -hy);
    const float r = 0.5f * (W > H ? W : H);
    if (!(x > r) && !(x < -r) && !(y > r) && !(y < -r)) {
        c.ghosts = true;
        const float xx = x * x;
        const float yy = y * y;
        const float sq = xx + yy;
        const float len = std::sqrt(sq);
        const float room = r - len;
        float k = room / r;
        if (!(k >= 0.0f))
            k = 0.0f;
        const float strength = static_cast<float>(intensity) / 255.0f;
        k = k * strength;
        for (int i = 1; i < 7; ++i) {
            const FlareElement& e = kElements[i];
            const float px = std::fma(x, e.position, hx);
            const float py = std::fma(y, e.position, hy);
            FlareVertex* q = c.ghosts_quads + 4 * (i - 1);
            q[0].x = -std::fma(0.5f, e.width, -px);
            q[0].y = -std::fma(0.5f, e.height, -py);
            q[1].x = -std::fma(0.5f, e.width, -px);
            q[1].y = std::fma(0.5f, e.height, py);
            q[2].x = std::fma(0.5f, e.width, px);
            q[2].y = -std::fma(0.5f, e.height, -py);
            q[3].x = std::fma(0.5f, e.width, px);
            q[3].y = std::fma(0.5f, e.height, py);
            const float a = k * static_cast<float>(e.a);
            for (int n = 0; n < 4; ++n)
                q[n].a = static_cast<std::uint8_t>(static_cast<std::int32_t>(a) & 0xFF);
        }
        c.flash = static_cast<std::uint8_t>(static_cast<std::int32_t>(static_cast<float>(flash) * k) & 0xFF);
    }
    if (!(x > 1.5f * W) && !(x < 0.5f * (-W)) && !(y > 1.5f * H) && !(y < 0.5f * (-H))) {
        c.glow = true;
        const float span = far_plane - near_plane;
        const float ratio = far_plane / s[2];
        const float dspan = zf - zn;
        const float scale = dspan / span;
        const float ahead = s[2] - near_plane;
        const float scaled = scale * ahead;
        const float depth = ratio * scaled;
        c.glow_depth = zn + depth;
        const FlareElement& e = kElements[0];
        const float px = std::fma(e.position, x, hx);
        const float py = std::fma(e.position, y, hy);
        c.glow_quad[0].x = -std::fma(0.5f, e.width, -px);
        c.glow_quad[0].y = -std::fma(0.5f, e.height, -py);
        c.glow_quad[1].x = c.glow_quad[0].x;
        c.glow_quad[1].y = std::fma(0.5f, e.height, py);
        c.glow_quad[2].x = std::fma(0.5f, e.width, px);
        c.glow_quad[2].y = c.glow_quad[0].y;
        c.glow_quad[3].x = c.glow_quad[2].x;
        c.glow_quad[3].y = c.glow_quad[1].y;
    }
    return c;
}

// A deterministic stream.
struct Lcg {
    std::uint64_t s = 0x2545F4914F6CDD1Dull;
    double next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(s >> 11) / static_cast<double>(1ull << 53);
    }
};

// A sun at canvas offset (x, y) from the raster's centre, forward distance z, as RenderWare's view matrix gives it.
void sun_at(float x, float y, float z, float W, float H, float out[3]) {
    out[0] = (0.5f + x / W) * z;
    out[1] = (0.5f + y / H) * z;
    out[2] = z;
}

FlareView view_of(float W, float H, float shown_w, float shown_h) {
    FlareView v;
    v.raster_width = W;
    v.raster_height = H;
    v.shown_width = shown_w;
    v.shown_height = shown_h;
    v.near_plane = 1.0f;
    v.far_plane = 25000.142578125f;
    v.depth_near = 0.0f;
    v.depth_far = 1.0f;
    return v;
}

}  // namespace

int main() {
    // 1. At the raster's own aspect: the console's numbers, bit for bit, except the glow's documented edge bands.
    {
        Lcg rng;
        const FlareView v = view_of(640.0f, 480.0f, 640.0f, 480.0f);
        int glow_band = 0, glow_same = 0;
        for (int n = 0; n < 200000; ++n) {
            const float x = static_cast<float>((rng.next() * 2.0 - 1.0) * 1400.0);
            const float y = static_cast<float>((rng.next() * 2.0 - 1.0) * 1100.0);
            const float z = static_cast<float>(10.0 + rng.next() * 24000.0);
            float s[3];
            sun_at(x, y, z, 640.0f, 480.0f, s);
            FlareVertex ghosts[4 * 6];
            const FlareFrame f = layout_lens_flare(s, v, kElements, 7, 255, 8, ghosts);
            const ConsoleFlare c = console_flare(s, 640.0f, 480.0f, 1.0f, 25000.142578125f, 0.0f, 1.0f, 255, 8);
            check(f.in_front == c.in_front, "4:3: in front as the console");
            check(f.ghosts == c.ghosts, "4:3: the ghosts shown as the console shows them");
            if (f.ghosts && c.ghosts) {
                bool same = f.flash_level == c.flash;
                for (int i = 0; i < 24; ++i)
                    same = same && bits(ghosts[i].x) == bits(c.ghosts_quads[i].x) &&
                           bits(ghosts[i].y) == bits(c.ghosts_quads[i].y) && ghosts[i].a == c.ghosts_quads[i].a;
                check(same, "4:3: ghost corners, alphas and the flash bit-identical");
            }
            const float sx = s[0] / s[2] * 640.0f - 320.0f;
            const float sy = s[1] / s[2] * 480.0f - 240.0f;
            const bool in_band = (sx < -320.0f && sx >= -640.0f) || (sy < -240.0f && sy >= -480.0f) ||
                                 (sx > 640.0f && sx <= 960.0f) || (sy > 480.0f && sy <= 720.0f);
            if (f.glow != c.glow) {
                ++glow_band;
                check(in_band, "4:3: the glow differs only in the documented edge bands");
            } else {
                ++glow_same;
                if (f.glow) {
                    bool same = bits(f.glow_depth) == bits(c.glow_depth);
                    for (int i = 0; i < 4; ++i)
                        same = same && bits(f.glow_quad[i].x) == bits(c.glow_quad[i].x) &&
                               bits(f.glow_quad[i].y) == bits(c.glow_quad[i].y);
                    check(same, "4:3: glow corners and depth bit-identical");
                }
            }
        }
        std::printf("4:3: glow decided as the console in %d cases, differently (edge bands) in %d\n", glow_same,
                    glow_band);
    }
    // 2. The glow's band: where the console differed it only hid or kept a glow wholly or partly off the raster.
    {
        const FlareView v = view_of(640.0f, 480.0f, 640.0f, 480.0f);
        float s[3];
        FlareVertex ghosts[4 * 6];
        sun_at(-350.0f, 0.0f, 5000.0f, 640.0f, 480.0f, s);  // 30 units past the left edge: half the glow on screen
        const FlareFrame f = layout_lens_flare(s, v, kElements, 7, 255, 8, ghosts);
        const ConsoleFlare c = console_flare(s, 640.0f, 480.0f, 1.0f, 25000.142578125f, 0.0f, 1.0f, 255, 8);
        check(f.glow && !c.glow, "a sun just past the left edge keeps its (partly visible) glow; the console dropped it");
        check(f.glow_quad[2].x > 0.0f, "... and that glow does reach onto the raster");
        sun_at(700.0f, 0.0f, 5000.0f, 640.0f, 480.0f, s);  // a screen-width right of the centre: the glow wholly off
        const FlareFrame g = layout_lens_flare(s, v, kElements, 7, 255, 8, ghosts);
        const ConsoleFlare d = console_flare(s, 640.0f, 480.0f, 1.0f, 25000.142578125f, 0.0f, 1.0f, 255, 8);
        check(!g.glow && d.glow && d.glow_quad[0].x > 640.0f,
              "past half a screen beyond the right edge the console kept a glow wholly off the raster; not drawn");
    }
    // 3. 16:9: the sun beyond the 4:3 border but inside the picture keeps its flare; it fades out at the real edge.
    {
        const float shown = 480.0f * (16.0f / 9.0f);  // 853.33
        const FlareView v = view_of(640.0f, 480.0f, shown, 480.0f);
        float s[3];
        FlareVertex ghosts[4 * 6];
        sun_at(380.0f, -60.0f, 9000.0f, 640.0f, 480.0f, s);
        const FlareFrame f = layout_lens_flare(s, v, kElements, 7, 255, 8, ghosts);
        const ConsoleFlare c = console_flare(s, 640.0f, 480.0f, 1.0f, 25000.142578125f, 0.0f, 1.0f, 255, 8);
        check(f.ghosts && !c.ghosts, "16:9: a sun 60 units past the 4:3 border (inside the picture) keeps its ghosts");
        check(f.fade > 0.0f && f.fade < 0.15f, "16:9: ... faded by its distance from the centre of the real picture");
        check(f.glow, "16:9: ... and its glow");
        sun_at(0.5f * shown - 0.5f, 0.0f, 9000.0f, 640.0f, 480.0f, s);
        check(layout_lens_flare(s, v, kElements, 7, 255, 8, ghosts).ghosts, "16:9: shown up to the real edge");
        sun_at(0.5f * shown + 0.5f, 0.0f, 9000.0f, 640.0f, 480.0f, s);
        check(!layout_lens_flare(s, v, kElements, 7, 255, 8, ghosts).ghosts, "16:9: gone past the real edge");
        sun_at(0.5f * shown - 0.01f, 0.0f, 9000.0f, 640.0f, 480.0f, s);
        const FlareFrame edge = layout_lens_flare(s, v, kElements, 7, 255, 8, ghosts);
        check(edge.fade < 0.001f, "16:9: the fade reaches zero at the real edge (no pop)");
    }
    // 4. Ultrawide 32:9 and a portrait 9:16 output: the same rule over what each shows.
    {
        const float wide = 480.0f * (32.0f / 9.0f);
        const FlareView v = view_of(640.0f, 480.0f, wide, 480.0f);
        float s[3];
        FlareVertex ghosts[4 * 6];
        sun_at(800.0f, 0.0f, 9000.0f, 640.0f, 480.0f, s);
        check(layout_lens_flare(s, v, kElements, 7, 255, 8, ghosts).ghosts, "32:9: a sun 800 units out is on screen");
        const float tall = 640.0f / (9.0f / 16.0f);
        const FlareView p = view_of(640.0f, 480.0f, 640.0f, tall);
        sun_at(0.0f, -500.0f, 9000.0f, 640.0f, 480.0f, s);
        check(layout_lens_flare(s, p, kElements, 7, 255, 8, ghosts).ghosts,
              "9:16: a sun 500 units above the centre is on the taller screen");
        // The console's field stretched over the taller picture: vertically it reaches 4/3 of the shown half-height
        // (758.5, as 320 is 4/3 of 240 on the raster), sideways exactly to the picture's side edges (320).
        sun_at(0.0f, -750.0f, 9000.0f, 640.0f, 480.0f, s);
        check(layout_lens_flare(s, p, kElements, 7, 255, 8, ghosts).ghosts, "9:16: 750 units above: inside the field");
        sun_at(0.0f, -770.0f, 9000.0f, 640.0f, 480.0f, s);
        check(!layout_lens_flare(s, p, kElements, 7, 255, 8, ghosts).ghosts, "9:16: 770 units above: past it");
        sun_at(300.0f, 0.0f, 9000.0f, 640.0f, 480.0f, s);
        check(layout_lens_flare(s, p, kElements, 7, 255, 8, ghosts).ghosts, "9:16: 300 units to the side: on screen");
        sun_at(330.0f, 0.0f, 9000.0f, 640.0f, 480.0f, s);
        check(!layout_lens_flare(s, p, kElements, 7, 255, 8, ghosts).ghosts,
              "9:16: past the picture's side edge (the console's own reach sideways): none");
    }
    // 5. A split-screen viewport (the left half, raster 320 x 480). On a 4:3 output it shows its raster: the console's
    //    numbers (r = 240 on both axes, 80 units past each side). On a 16:9 output it shows 426.67 x 480 of the canvas:
    //    the same field stretched sideways by 426.67 / 320, never the window's.
    {
        const FlareView own = view_of(320.0f, 480.0f, 320.0f, 480.0f);
        Lcg rng;
        for (int n = 0; n < 20000; ++n) {
            const float x = static_cast<float>((rng.next() * 2.0 - 1.0) * 700.0);
            const float y = static_cast<float>((rng.next() * 2.0 - 1.0) * 1100.0);
            const float z = static_cast<float>(10.0 + rng.next() * 24000.0);
            float s[3];
            sun_at(x, y, z, 320.0f, 480.0f, s);
            FlareVertex ghosts[4 * 6];
            const FlareFrame f = layout_lens_flare(s, own, kElements, 7, 255, 8, ghosts);
            const ConsoleFlare c = console_flare(s, 320.0f, 480.0f, 1.0f, 25000.142578125f, 0.0f, 1.0f, 255, 8);
            bool same = f.ghosts == c.ghosts;
            if (same && f.ghosts) {
                same = f.flash_level == c.flash;
                for (int i = 0; i < 24; ++i)
                    same = same && bits(ghosts[i].x) == bits(c.ghosts_quads[i].x) && ghosts[i].a == c.ghosts_quads[i].a;
            }
            check(same, "split on 4:3: the console's ghosts, alphas and flash bit for bit");
        }
        const float view_aspect = (16.0f / 9.0f) * 0.5f;
        const float shown = 480.0f * view_aspect;  // 426.67 of the half's 320 units shown
        const FlareView v = view_of(320.0f, 480.0f, shown, 480.0f);
        float s[3];
        FlareVertex ghosts[4 * 6];
        sun_at(200.0f, 0.0f, 9000.0f, 320.0f, 480.0f, s);
        const FlareFrame f = layout_lens_flare(s, v, kElements, 7, 255, 8, ghosts);
        check(f.ghosts, "split on 16:9: a sun 200 units right of the half's centre keeps its ghosts");
        check(std::fabs(f.fade - (240.0f - 200.0f * 320.0f / shown) / 240.0f) < 1e-5f,
              "split on 16:9: the console's field (r = 240) stretched sideways by the shown width");
        sun_at(318.0f, 0.0f, 9000.0f, 320.0f, 480.0f, s);
        check(layout_lens_flare(s, v, kElements, 7, 255, 8, ghosts).ghosts, "split on 16:9: 318 units out: inside");
        sun_at(322.0f, 0.0f, 9000.0f, 320.0f, 480.0f, s);
        check(!layout_lens_flare(s, v, kElements, 7, 255, 8, ghosts).ghosts, "split on 16:9: 322 units out: none");
        sun_at(0.0f, -239.0f, 9000.0f, 320.0f, 480.0f, s);
        check(layout_lens_flare(s, v, kElements, 7, 255, 8, ghosts).ghosts,
              "split on 16:9: vertical reach as the console's");
        sun_at(0.0f, -241.0f, 9000.0f, 320.0f, 480.0f, s);
        check(!layout_lens_flare(s, v, kElements, 7, 255, 8, ghosts).ghosts, "split on 16:9: ... and no further");
    }
    // 6. Behind the near plane: nothing at all; a NaN depth passes the test as the console's compare let it.
    {
        const FlareView v = view_of(640.0f, 480.0f, 640.0f, 480.0f);
        FlareVertex ghosts[4 * 6];
        const float behind[3] = {0.0f, 0.0f, 0.5f};
        const FlareFrame f = layout_lens_flare(behind, v, kElements, 7, 255, 8, ghosts);
        check(!f.in_front && !f.glow && !f.ghosts, "behind the near plane: nothing");
        const float nan[3] = {0.0f, 0.0f, std::nanf("")};
        check(layout_lens_flare(nan, v, kElements, 7, 255, 8, ghosts).in_front, "a NaN depth passes, as on the console");
    }
    // 7. Wider outputs keep the console's vertical field: above and below the picture the ghosts and the flash reach as
    //    far (80 units past the top / bottom edge of the 640 x 480 raster) and fade as strongly as on the console; only
    //    the horizontal reach follows the shown width. For every aspect from 4:3 to 32:9, a sun on the vertical axis
    //    through the centre gets the console's decision, alphas and flash.
    {
        const float aspects[5] = {4.0f / 3.0f, 16.0f / 10.0f, 16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f};
        for (float aspect : aspects) {
            const FlareView v = view_of(640.0f, 480.0f, 480.0f * aspect, 480.0f);
            for (int k = -400; k <= 400; k += 3) {
                float s[3];
                FlareVertex ghosts[4 * 6];
                sun_at(0.0f, static_cast<float>(k), 12000.0f, 640.0f, 480.0f, s);
                const FlareFrame f = layout_lens_flare(s, v, kElements, 7, 255, 8, ghosts);
                const ConsoleFlare c = console_flare(s, 640.0f, 480.0f, 1.0f, 25000.142578125f, 0.0f, 1.0f, 255, 8);
                bool same = f.ghosts == c.ghosts;
                if (same && f.ghosts) {
                    same = f.flash_level == c.flash;
                    for (int i = 0; i < 24; ++i)
                        same = same && ghosts[i].a == c.ghosts_quads[i].a;
                }
                check(same, "wider outputs: on the vertical axis the console's ghosts, alphas and flash");
            }
        }
    }
    // 8. The reported blink (findings-20261019-LF/LF_REPORT.md): the first frames of Sky Troops (stg0402) on a wide
    //    output. The sun sits just above the picture while the first flare element ramps the flash byte from the effect
    //    library's default 0x20 down to the authored 0x08. A flash grey of 5 is the glare capture's first non-zero level
    //    (q8(5 x 30/255) = 1, BLOOM_SPEC 2.13), which the glare ladder spreads into a whole-screen veil of about +59.
    //    The sun points are the SHADOW_FLARE_TRACE rows of frames 291-297; the bytes are the upper ends of the flash
    //    bytes those rows imply. The first rule (half the shown larger side on both axes) gave 2, 4, 5, 5, 5, 5, 4 at
    //    16:9; the console at 4:3 gives 0, 0, 0, 1, 1, 1, 1.
    {
        struct Row {
            float x, y;
            std::uint8_t flash;
        };
        const Row rows[7] = {{151.77f, -359.08f, 32}, {144.27f, -307.12f, 24}, {140.65f, -281.89f, 22},
                             {138.46f, -266.46f, 20}, {136.96f, -255.72f, 18}, {135.83f, -247.56f, 17},
                             {134.94f, -241.0f, 14}};
        const float wide[3] = {16.0f / 10.0f, 16.0f / 9.0f, 21.0f / 9.0f};
        int first_rule_crossings = 0;
        for (int a = 0; a < 3; ++a) {
            const FlareView v = view_of(640.0f, 480.0f, 480.0f * wide[a], 480.0f);
            for (const Row& r : rows) {
                float s[3];
                FlareVertex ghosts[4 * 6];
                sun_at(r.x, r.y, 19000.0f, 640.0f, 480.0f, s);
                const FlareFrame f = layout_lens_flare(s, v, kElements, 7, 255, r.flash, ghosts);
                check(f.flash_level < 5, "Sky Troops start, wide output: the flash stays under the glare's first level");
                // The rule the blink came from, as a mutant this case must catch: half the SHOWN larger side as the
                // radius on both axes.
                const float radius = 0.5f * v.shown_width;
                const float length = std::sqrt(r.x * r.x + r.y * r.y);
                const float old_fade =
                    std::fabs(r.x) <= radius && std::fabs(r.y) <= radius && length < radius ? (radius - length) / radius
                                                                                              : 0.0f;
                if (a == 1 && static_cast<int>(static_cast<float>(r.flash) * old_fade) >= 5)
                    ++first_rule_crossings;
            }
        }
        check(first_rule_crossings == 4, "Sky Troops start, 16:9: the first rule crossed the glare's first level 4 times");
    }
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
