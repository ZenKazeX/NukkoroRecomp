#include "render_vk/tests/draw_checks.h"

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

// A world point's pixel through the view's projection (the renderer's own arithmetic, render/frame_math.h).
struct Pixel {
    double x = 0, y = 0;
};
Pixel project(const render::Mat4& vp, const render::Float3& p, float width, float height) {
    double c[4];
    for (int r = 0; r < 4; ++r)
        c[r] = vp.m[r][0] * p.x + vp.m[r][1] * p.y + vp.m[r][2] * p.z + vp.m[r][3];
    return Pixel{(c[0] / c[3] + 1.0) * 0.5 * width, (1.0 - c[1] / c[3]) * 0.5 * height};
}

// A square of `size` world units centred at `centre` in the plane z = centre.z, coloured `colour`, its triangles wound
// counter-clockwise on screen when `ccw` (the faces the game keeps; render/renderer.cpp mesh_pipeline) or clockwise.
assets::MeshData quad_mesh(const render::Mat4& vp, const render::Float3& centre, float size, std::uint32_t colour,
                           bool ccw, float width, float height) {
    assets::MeshData mesh;
    mesh.vertex_count = 4;
    const float h = size * 0.5f;
    const float corners[4][2] = {{-h, -h}, {h, -h}, {h, h}, {-h, h}};
    for (const auto& c : corners) {
        mesh.positions.insert(mesh.positions.end(), {centre.x + c[0], centre.y + c[1], centre.z});
        for (int k = 0; k < 4; ++k)
            mesh.colours.push_back(static_cast<std::uint8_t>(colour >> (8 * k)));
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};
    // On screen (pixels, y down) a triangle is clockwise when the cross product of its edges is positive.
    const Pixel a = project(vp, {mesh.positions[0], mesh.positions[1], mesh.positions[2]}, width, height);
    const Pixel b = project(vp, {mesh.positions[3], mesh.positions[4], mesh.positions[5]}, width, height);
    const Pixel c = project(vp, {mesh.positions[6], mesh.positions[7], mesh.positions[8]}, width, height);
    const bool clockwise = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) > 0.0;
    if (clockwise == ccw)
        mesh.indices = {0, 2, 1, 0, 3, 2};
    mesh.submeshes.push_back(assets::MeshData::Submesh{0, 6, 0});
    return mesh;
}

// A canvas quad over pixels [x0, x1) x [y0, y1) (the view's own pixels: a canvas of zero size), as two triangles.
void canvas_quad(render::FrameBuilder& builder, float x0, float y0, float x1, float y1, std::uint32_t colour,
                 render::TextureHandle texture) {
    const render::ImmediateVertex v[4] = {
        {{x0, y0, 0.0f}, colour, {0, 0}}, {{x1, y0, 0.0f}, colour, {1, 0}},
        {{x1, y1, 0.0f}, colour, {1, 1}}, {{x0, y1, 0.0f}, colour, {0, 1}}};
    const std::uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    render::ImmediateDraw draw;
    draw.topology = render::Topology::Triangles;
    draw.space = render::Space::Canvas;
    draw.anchor = render::Anchor::Fill;
    draw.texture = texture;
    draw.sampler = render::Sampler{render::Filter::Point, render::Address::Clamp, render::Address::Clamp};
    render::Recipe recipe;
    recipe.cull = render::Cull::None;
    recipe.depth_write = false;
    builder.immediate(draw, v, 4, indices, 6, render::Float3x4{}, recipe);
}

}  // namespace

int run_draw_checks(Renderer& renderer, const std::function<void(const std::string&)>& log,
                    std::vector<std::uint8_t>* out) {
    const float w = static_cast<float>(kDrawCheckWidth), h = static_cast<float>(kDrawCheckHeight);
    std::string error;
    render::Camera camera;
    camera.frame = render::look_at({0, 0, -10}, {0, 0, 0}, {0, 1, 0});
    const render::Mat4 vp =
        render::multiply(render::projection_of(camera, w / h, render::canvas_aspect_of(render::Canvas{}, w, h)),
                         render::view_of(camera));

    // The resources: meshes and a 2 x 2 texture (red, green / blue, white).
    struct Quad {
        render::Float3 centre;
        float size;
        std::uint32_t colour;
        bool ccw;
    };
    const Quad quads[] = {
        {{-3.0f, 1.5f, 0.0f}, 1.5f, rgba8(0, 255, 0), true},     // shown: counter-clockwise on screen
        {{-3.0f, -1.5f, 0.0f}, 1.5f, rgba8(255, 0, 255), false},  // culled: clockwise
        {{0.0f, 0.0f, 2.0f}, 2.0f, rgba8(255, 0, 0), true},      // far, drawn first
        {{1.0f, 0.0f, -2.0f}, 2.0f, rgba8(0, 0, 255), true},     // near, drawn second: in front
        {{0.0f, 0.0f, 3.0f}, 2.0f, rgba8(255, 255, 255), true},  // farthest, drawn last: hidden
    };
    std::vector<render::MeshHandle> meshes;
    for (const Quad& q : quads) {
        meshes.push_back(renderer.create_mesh(render::Scope::Frame, quad_mesh(vp, q.centre, q.size, q.colour, q.ccw, w, h),
                                              &error));
        if (!meshes.back()) {
            log("create_mesh: " + error);
            return 1;
        }
    }
    assets::TextureImage image;
    image.width = 2;
    image.height = 2;
    image.mip_levels = 1;
    image.rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    image.level_offsets = {0};
    const render::TextureHandle texture = renderer.create_texture(render::Scope::Frame, image, &error);
    if (!texture) {
        log("create_texture: " + error);
        return 1;
    }

    // The packet.
    render::FrameBuilder builder;
    builder.begin_frame(1, render::Float4{0, 0, 0, 1});
    const render::ViewId view = builder.view(camera, {0, 0, 1, 1});
    builder.begin_pass(view, render::PassKind::Scene, render::Clear{});
    render::Material material;  // the vertex colours alone (prelit, unlit, untextured)
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        render::Recipe recipe;
        recipe.cull = render::Cull::Back;
        builder.draw(meshes[i], 0, material, render::Float3x4{}, recipe);
    }
    render::ParticleBatch batch;
    render::Particle particle;
    particle.position = {3.0f, 1.5f, 0.0f};
    particle.colour = rgba8(255, 255, 0);
    render::Recipe particle_recipe;
    particle_recipe.cull = render::Cull::None;
    builder.particles(batch, &particle, 1, particle_recipe);
    canvas_quad(builder, 2, 2, 14, 14, rgba8(255, 0, 0), {});                    // an exact colour
    canvas_quad(builder, 2, 100, 20, 118, rgba8(0, 0, 255, 128), {});            // half transparent over black
    canvas_quad(builder, 130, 90, 154, 114, rgba8(255, 255, 255), texture);       // the texture, point sampled
    builder.end_pass();
    render::FramePacket packet = builder.finish();

    std::vector<std::uint8_t> rgba;
    if (!renderer.render_to_image(packet, kDrawCheckWidth, kDrawCheckHeight, &rgba, &error)) {
        log("render_to_image: " + error);
        return 1;
    }
    std::vector<std::uint8_t> again;
    if (!renderer.render_to_image(packet, kDrawCheckWidth, kDrawCheckHeight, &again, &error)) {
        log("render_to_image (again): " + error);
        return 1;
    }
    int failures = 0;
    const auto at = [&](double x, double y) {
        const std::size_t p = (static_cast<std::size_t>(y) * kDrawCheckWidth + static_cast<std::size_t>(x)) * 4;
        return &rgba[p];
    };
    const auto expect = [&](const char* what, double x, double y, int r, int g, int b, int tolerance = 0) {
        const std::uint8_t* got = at(x, y);
        const bool ok = std::abs(got[0] - r) <= tolerance && std::abs(got[1] - g) <= tolerance &&
                        std::abs(got[2] - b) <= tolerance;
        if (!ok)
            ++failures;
        log(line("  %-44s (%3d, %3d): %3u %3u %3u, expected %3d %3d %3d%s", what, static_cast<int>(x),
                 static_cast<int>(y), got[0], got[1], got[2], r, g, b, ok ? "" : "  <-- WRONG"));
    };
    const auto centre = [&](const render::Float3& p) { return project(vp, p, w, h); };
    const Pixel shown = centre(quads[0].centre), culled = centre(quads[1].centre);
    const Pixel only_far = centre({-0.6f, 0.0f, 2.0f}), near_wins = centre({0.5f, 0.0f, -2.0f});
    const Pixel sprite = centre(particle.position);
    expect("counter-clockwise mesh shown", shown.x, shown.y, 0, 255, 0);
    expect("clockwise mesh culled (background)", culled.x, culled.y, 0, 0, 0);
    expect("far quad where nothing is nearer", only_far.x, only_far.y, 255, 0, 0);
    expect("near quad in front of far and farther", near_wins.x, near_wins.y, 0, 0, 255);
    expect("particle", sprite.x, sprite.y, 255, 255, 0);
    expect("canvas quad, exact colour", 8, 8, 255, 0, 0);
    expect("canvas quad, half transparent over black", 10, 109, 0, 0, 128, 1);
    expect("texture texel (0,0) red", 136, 96, 255, 0, 0);
    expect("texture texel (1,0) green", 148, 96, 0, 255, 0);
    expect("texture texel (0,1) blue", 136, 108, 0, 0, 255);
    expect("texture texel (1,1) white", 148, 108, 255, 255, 255);
    expect("background", 80, 110, 0, 0, 0);
    const bool same = again == rgba;
    if (!same)
        ++failures;
    const Renderer::Stats stats = renderer.stats();
    log(line("draw checks: re-render %s; draws %llu, skipped %llu, particles %llu, immediates %llu, textures %zu, "
             "meshes %zu, pipelines %zu",
             same ? "identical" : "DIFFERENT", static_cast<unsigned long long>(stats.draws),
             static_cast<unsigned long long>(stats.skipped_draws), static_cast<unsigned long long>(stats.particles),
             static_cast<unsigned long long>(stats.immediates), stats.textures, stats.meshes, stats.pipelines));
    if (stats.skipped_draws != 0) {
        log("  unexpected skipped draws");
        ++failures;
    }
    renderer.release_scope(render::Scope::Frame);
    if (out != nullptr)
        *out = std::move(rgba);
    return failures;
}

}  // namespace shadow::render_vk
