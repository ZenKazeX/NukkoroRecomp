#include "render_vk/tests/effect_checks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "render/frame_math.h"

namespace shadow::render_vk {

namespace {

template <class... Args>
std::string line(const char* format, Args... args) {
    char text[512];
    std::snprintf(text, sizeof(text), format, args...);
    return text;
}

std::uint32_t rgba8(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    return r | (g << 8) | (b << 16) | (static_cast<std::uint32_t>(a) << 24);
}

// A canvas quad over pixels [x0, x1) x [y0, y1) of the view (a canvas of zero size), its texture coordinates `u0..v1`
// (canvas points when it samples a snapshot or the history).
void canvas_quad(render::FrameBuilder& builder, float x0, float y0, float x1, float y1, std::uint32_t colour,
                 render::ImageSource source = render::ImageSource::Texture, float u0 = 0, float v0 = 0, float u1 = 1,
                 float v1 = 1) {
    const render::ImmediateVertex v[4] = {{{x0, y0, 0.0f}, colour, {u0, v0}}, {{x1, y0, 0.0f}, colour, {u1, v0}},
                                          {{x1, y1, 0.0f}, colour, {u1, v1}}, {{x0, y1, 0.0f}, colour, {u0, v1}}};
    const std::uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    render::ImmediateDraw draw;
    draw.space = render::Space::Canvas;
    draw.anchor = render::Anchor::Fill;
    draw.source = source;
    draw.sampler = render::Sampler{render::Filter::Point, render::Address::Clamp, render::Address::Clamp};
    render::Recipe recipe;
    recipe.cull = render::Cull::None;
    recipe.depth_write = false;
    builder.immediate(draw, v, 4, indices, 6, render::Float3x4{}, recipe);
}

// A square in the plane z = 0 centred at (cx, cy), `size` world units, one colour, both windings drawn (cull none);
// skinned (every vertex fully on bone 0) and / or with one delta target moving every vertex by `delta`.
assets::MeshData square(float cx, float cy, float size, std::uint32_t colour, bool skinned, const float* delta) {
    assets::MeshData mesh;
    mesh.vertex_count = 4;
    const float h = size * 0.5f;
    const float corners[4][2] = {{-h, -h}, {h, -h}, {h, h}, {-h, h}};
    for (const auto& c : corners) {
        mesh.positions.insert(mesh.positions.end(), {cx + c[0], cy + c[1], 0.0f});
        for (int k = 0; k < 4; ++k)
            mesh.colours.push_back(static_cast<std::uint8_t>(colour >> (8 * k)));
        if (skinned) {
            mesh.skin_indices.insert(mesh.skin_indices.end(), {0, 0, 0, 0});
            mesh.skin_weights.insert(mesh.skin_weights.end(), {1.0f, 0.0f, 0.0f, 0.0f});
        }
    }
    if (delta != nullptr) {
        assets::MeshData::DeltaTarget target;
        for (int v = 0; v < 4; ++v)
            target.positions.insert(target.positions.end(), {delta[0], delta[1], delta[2]});
        mesh.delta_targets.push_back(target);
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.submeshes.push_back(assets::MeshData::Submesh{0, 6, 0});
    return mesh;
}

}  // namespace

int run_effect_checks(Renderer& renderer, const std::function<void(const std::string&)>& log,
                      std::vector<std::uint8_t>* out) {
    const std::uint32_t W = kEffectCheckWidth, H = kEffectCheckHeight;
    const float w = static_cast<float>(W), h = static_cast<float>(H);
    std::string error;
    int failures = 0;
    render::Camera camera;
    camera.frame = render::look_at({0, 0, -10}, {0, 0, 0}, {0, 1, 0});
    const render::Mat4 vp =
        render::multiply(render::projection_of(camera, w / h, render::canvas_aspect_of(render::Canvas{}, w, h)),
                         render::view_of(camera));
    const auto pixel_of = [&](float x, float y) {
        double c[4];
        for (int r = 0; r < 4; ++r)
            c[r] = vp.m[r][0] * x + vp.m[r][1] * y + vp.m[r][3];
        return std::array<double, 2>{(c[0] / c[3] + 1.0) * 0.5 * w, (1.0 - c[1] / c[3]) * 0.5 * h};
    };

    // The deformed meshes: a skinned square moved up 1.5 by its bone, a morphed one moved up 1.5 by its target.
    const float lift[3] = {0.0f, 1.5f, 0.0f};
    const render::MeshHandle skinned =
        renderer.create_mesh(render::Scope::Frame, square(0.5f, -0.5f, 1.0f, rgba8(255, 255, 0), true, nullptr), &error);
    const render::MeshHandle morphed =
        renderer.create_mesh(render::Scope::Frame, square(1.75f, -0.5f, 1.0f, rgba8(0, 255, 255), false, lift), &error);
    if (!skinned || !morphed) {
        log("create_mesh: " + error);
        return 1;
    }

    render::FrameBuilder builder;
    // Frame 1: a green quad, then the history saved.
    builder.begin_frame(1, render::Float4{0, 0, 0, 1});
    render::ViewId view = builder.view(camera, {0, 0, 1, 1});
    builder.begin_pass(view, render::PassKind::Scene, render::Clear{});
    canvas_quad(builder, 10, 50, 40, 80, rgba8(0, 255, 0));
    builder.end_pass();
    builder.save_history(view);
    const render::FramePacket frame1 = builder.finish();

    // Frame 2: everything else.
    builder.begin_frame(2, render::Float4{0, 0, 0, 1});
    view = builder.view(camera, {0, 0, 1, 1});
    builder.begin_pass(view, render::PassKind::Scene, render::Clear{});
    canvas_quad(builder, 10, 10, 40, 40, rgba8(255, 0, 0));
    render::Material material;  // the vertex colours alone
    render::Recipe recipe;
    recipe.cull = render::Cull::None;
    render::Float3x4 bone;  // bone 0: up 1.5
    bone.m[1][3] = 1.5f;
    const render::BoneRange bones = builder.bones(&bone, 1);
    builder.draw(skinned, 0, material, render::Float3x4{}, recipe, render::kNoLights, bones);
    const float weight = 1.0f;
    const render::MorphRange morph = builder.morph(&weight, 1);
    builder.draw(morphed, 0, material, render::Float3x4{}, recipe, render::kNoLights, {}, morph);
    builder.end_pass();
    builder.snapshot(view);
    builder.begin_pass(view, render::PassKind::Scene, render::Clear{});
    canvas_quad(builder, 100, 10, 130, 40, rgba8(255, 255, 255), render::ImageSource::Snapshot, 10, 10, 40, 40);
    canvas_quad(builder, 100, 50, 130, 80, rgba8(255, 255, 255), render::ImageSource::History, 10, 50, 40, 80);
    builder.end_pass();
    builder.begin_pass(view, render::PassKind::GlareSources, render::Clear{});
    canvas_quad(builder, 62, 96, 78, 112, rgba8(255, 255, 255));
    builder.end_pass();
    builder.glare(view, render::GlareParams{});
    builder.begin_pass(view, render::PassKind::Scene, render::Clear{});
    canvas_quad(builder, 140, 100, 152, 112, rgba8(0, 0, 255));
    builder.end_pass();
    render::FramePacket frame2 = builder.finish();

    std::vector<std::uint8_t> rgba, again, plain, scratch;
    if (!renderer.render_to_image(frame1, W, H, &scratch, &error) ||
        !renderer.render_to_image(frame2, W, H, &rgba, &error)) {
        log("render_to_image: " + error);
        return failures + 1;
    }
    const Renderer::Stats stats = renderer.stats();
    const auto at = [&](const std::vector<std::uint8_t>& image, double x, double y) {
        return &image[(static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x)) * 4];
    };
    const auto expect = [&](const char* what, double x, double y, int r, int g, int b, int tolerance = 0) {
        const std::uint8_t* got = at(rgba, x, y);
        const bool ok = std::abs(got[0] - r) <= tolerance && std::abs(got[1] - g) <= tolerance &&
                        std::abs(got[2] - b) <= tolerance;
        if (!ok)
            ++failures;
        log(line("  %-44s (%3d, %3d): %3u %3u %3u, expected %3d %3d %3d%s%s", what, static_cast<int>(x),
                 static_cast<int>(y), got[0], got[1], got[2], r, g, b, tolerance != 0 ? " (+-)" : "",
                 ok ? "" : "  <-- WRONG"));
    };
    const auto skin_at = pixel_of(0.5f, 1.0f), skin_was = pixel_of(0.5f, -0.5f);
    const auto morph_at = pixel_of(1.75f, 1.0f), morph_was = pixel_of(1.75f, -0.5f);
    expect("red quad", 25, 25, 255, 0, 0);
    expect("snapshot of the red quad, elsewhere", 115, 25, 255, 0, 0);
    expect("frame 1's green quad: not drawn now", 25, 65, 0, 0, 0);
    expect("history of frame 1, elsewhere", 115, 65, 0, 255, 0);
    expect("skinned quad where its bone moved it", skin_at[0], skin_at[1], 255, 255, 0);
    expect("skinned quad's bind pose: empty", skin_was[0], skin_was[1], 0, 0, 0);
    expect("morphed quad where its target moved it", morph_at[0], morph_at[1], 0, 255, 255);
    expect("morphed quad's base shape: empty", morph_was[0], morph_was[1], 0, 0, 0);
    expect("glare source: saturated", 70, 104, 255, 255, 255);
    expect("canvas quad drawn after the glare", 146, 106, 0, 0, 255);
    expect("far from the glare: untouched", 40, 104, 0, 0, 0);
    {
        // Beside the source: the halo - saturated next to it (the blur sums its taps unweighted: BLOOM_SPEC 6.4),
        // falling off a few pixels out; 6 px from the edge it is partial.
        const std::uint8_t* got = at(rgba, 84, 104);
        const bool ok = got[0] > 0 && got[0] < 255 && got[0] == got[1] && got[1] == got[2];
        if (!ok)
            ++failures;
        log(line("  %-44s (%3d, %3d): %3u %3u %3u, expected grey 1..254%s", "glare halo 6 px beside the source", 84, 104,
                 got[0], got[1], got[2], ok ? "" : "  <-- WRONG"));
    }
    // The fold applies at 4 samples and above (B3a: at 2x the plain composite is cheaper; at 1x there is no resolve).
    const std::uint64_t folds = renderer.scene_samples() >= VK_SAMPLE_COUNT_4_BIT ? 1u : 0u;
    const bool counts = stats.glares == 1 && stats.glares_folded == folds && stats.snapshots == 1 && stats.skin_jobs == 1 &&
                        stats.morph_jobs == 1 && stats.history_saves == 0 && stats.skipped_draws == 0;
    if (!counts)
        ++failures;
    log(line("frame 2: glares %llu (folded %llu, spans %llu), snapshots %llu, history saves %llu, skin jobs %llu, "
             "morph jobs %llu, skinned vertices %llu, draws %llu, skipped %llu%s",
             static_cast<unsigned long long>(stats.glares), static_cast<unsigned long long>(stats.glares_folded),
             static_cast<unsigned long long>(stats.folded_spans), static_cast<unsigned long long>(stats.snapshots),
             static_cast<unsigned long long>(stats.history_saves), static_cast<unsigned long long>(stats.skin_jobs),
             static_cast<unsigned long long>(stats.morph_jobs),
             static_cast<unsigned long long>(stats.skinned_vertices), static_cast<unsigned long long>(stats.draws),
             static_cast<unsigned long long>(stats.skipped_draws), counts ? "" : "  <-- UNEXPECTED COUNTS"));

    // The same frame again (identical), and with the plain composite (B3a's A/B: the same picture expected).
    renderer.render_to_image(frame2, W, H, &again, &error);
    renderer.set_fold_glare(false);
    renderer.render_to_image(frame2, W, H, &plain, &error);
    const std::uint64_t plain_folded = renderer.stats().glares_folded;
    renderer.set_fold_glare(true);
    const bool same = again == rgba;
    std::size_t differ = 0;
    int largest = 0;
    for (std::size_t i = 0; i < rgba.size() && plain.size() == rgba.size(); ++i)
        if (rgba[i] != plain[i]) {
            ++differ;
            largest = std::max(largest, std::abs(rgba[i] - plain[i]));
        }
    const bool fold_ok = plain_folded == 0 && plain.size() == rgba.size() && largest <= 1;
    if (!same || !fold_ok)
        ++failures;
    log(line("frame 2 re-render %s; folded vs plain composite: %zu channel values differ, largest by %d%s",
             same ? "identical" : "DIFFERENT", differ, largest, fold_ok ? "" : "  <-- WRONG"));

    // The history invalidated: its draw is left out (counted); a capture that does not commit makes no history.
    renderer.invalidate_histories();
    std::vector<std::uint8_t> invalid;
    renderer.render_to_image(frame2, W, H, &invalid, &error);
    const std::uint64_t misses = renderer.stats().skipped_by_reason[static_cast<std::size_t>(
        Renderer::SkipReason::ImmediateImage)];
    renderer.render_to_image(frame1, W, H, &scratch, &error, false);  // not committed
    std::vector<std::uint8_t> still_invalid;
    renderer.render_to_image(frame2, W, H, &still_invalid, &error);
    const std::uint8_t* a = at(invalid, 115, 65);
    const std::uint8_t* b = at(still_invalid, 115, 65);
    const bool history_ok = a[1] == 0 && b[1] == 0 &&
                            renderer.stats().skipped_by_reason[static_cast<std::size_t>(
                                Renderer::SkipReason::ImmediateImage)] == misses + 1;
    if (!history_ok)
        ++failures;
    log(line("history invalidated: copy %u %u %u; after an uncommitted capture: %u %u %u (expected black, its draw "
             "left out)%s",
             a[0], a[1], a[2], b[0], b[1], b[2], history_ok ? "" : "  <-- WRONG"));

    renderer.release_scope(render::Scope::Frame);
    if (out != nullptr)
        *out = std::move(rgba);
    return failures;
}

}  // namespace shadow::render_vk
