// Tests of the native immediate modes (runtime/lift/rwim, NE-RW2, HW_REPORT Y.3): primitive topologies and vertex
// orders, Im2D placement through a layout policy, the no-pass path, fog on canvas draws, Im3D lists, and the tiled
// strip's continuation. Frame packets only: no device, no game.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "render/frame.h"
#include "rwim/immediate.h"

namespace {

using namespace shadow;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// One full-screen 640 x 480 view; the pass opens on the first draw unless refused.
class TestHost final : public rwim::Host {
public:
    explicit TestHost(render::FrameBuilder& b) : builder_(b) {}
    bool ensure_pass() override {
        if (refuse)
            return false;
        if (!open_) {
            render::Clear clear;
            builder_.begin_pass(view_id, render::PassKind::Scene, clear);
            open_ = true;
        }
        return true;
    }
    bool pass_open() const override { return open_; }
    std::size_t current_view() const override { return 0; }
    bool view(std::size_t index, View* out) const override {
        if (index != 0)
            return false;
        *out = View{640.0f, 480.0f, 640.0f, 480.0f};
        return true;
    }
    float output_aspect() const override { return aspect; }
    std::uint64_t frame() const override { return 0; }
    void close() {
        if (open_)
            builder_.end_pass();
        open_ = false;
    }

    bool refuse = false;
    float aspect = 16.0f / 9.0f;
    render::ViewId view_id = 0;

private:
    render::FrameBuilder& builder_;
    bool open_ = false;
};

class TestLayout final : public rwim::LayoutPolicy {
public:
    rwim::Placement place(const rwim::Bounds& b, bool textured, bool shows) override {
        ++calls;
        last_bounds = b;
        last_textured = textured;
        last_shows = shows;
        return answer;
    }
    rwim::Placement answer;
    int calls = 0;
    rwim::Bounds last_bounds;
    bool last_textured = false, last_shows = false;
};

render::ImmediateVertex vtx(float x, float y, float u, float v, std::uint32_t colour = 0xFFFFFFFFu) {
    render::ImmediateVertex out;
    out.position = render::Float3{x, y, 0.0f};
    out.colour = colour;
    out.uv[0] = u;
    out.uv[1] = v;
    return out;
}

void test_topologies_and_orders() {
    render::Topology t;
    bool fan = false;
    check(rwim::topology_of(1, &t, &fan) && t == render::Topology::Lines && !fan, "1 = lines");
    check(rwim::topology_of(2, &t, &fan) && t == render::Topology::LineStrip, "2 = polyline");
    check(rwim::topology_of(3, &t, &fan) && t == render::Topology::Triangles && !fan, "3 = triangles");
    check(rwim::topology_of(4, &t, &fan) && t == render::Topology::TriangleStrip, "4 = strip");
    check(rwim::topology_of(5, &t, &fan) && t == render::Topology::Triangles && fan, "5 = fan as a list");
    check(!rwim::topology_of(0, &t, &fan) && !rwim::topology_of(6, &t, &fan), "none and points refused");

    std::vector<std::uint32_t> order;
    check(rwim::list_order(nullptr, 0, 4, false, &order) && order.empty(), "in order: no list");
    check(rwim::list_order(nullptr, 0, 5, true, &order) &&
              order == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3, 0, 3, 4},
          "fan of 5 as 3 triangles");
    const std::uint16_t idx[] = {2, 0, 1};
    check(rwim::list_order(idx, 3, 3, false, &order) && order == std::vector<std::uint32_t>{2, 0, 1}, "indexed");
    const std::uint16_t bad[] = {0, 1, 3};
    check(!rwim::list_order(bad, 3, 3, false, &order), "an index past the vertices fails");
    const std::uint16_t fan_idx[] = {3, 2, 1, 0};
    check(rwim::list_order(fan_idx, 4, 4, true, &order) && order == std::vector<std::uint32_t>{3, 2, 1, 3, 1, 0},
          "indexed fan");
}

void test_im2d_placement() {
    render::FrameBuilder builder;
    builder.begin_frame(1, render::Float4{0, 0, 0, 1});
    TestHost host(builder);
    host.view_id = builder.view(render::Camera{}, render::ViewRect{});
    TestLayout layout;
    rwim::Immediate imm(builder, host, layout);

    const render::ImmediateVertex quad[4] = {vtx(10, 20, 0, 0), vtx(10, 40, 0, 1), vtx(30, 20, 1, 0),
                                             vtx(30, 40, 1, 1)};
    rwim::Binding binding;
    binding.textured = true;
    binding.texture.value = 7;
    render::Recipe recipe;
    recipe.src = render::Blend::SrcAlpha;
    recipe.fog = true;
    layout.answer.anchor = render::Anchor::Right;
    layout.answer.region = 1;
    layout.answer.region_id = 42;
    const std::vector<std::uint32_t> none;
    auto r = imm.draw2d(quad, 4, render::Topology::TriangleStrip, none, false, binding, recipe, nullptr);
    check(r.drawn && layout.calls == 1, "a policy-placed draw is drawn");
    check(layout.last_bounds.x0 == 10 && layout.last_bounds.y0 == 20 && layout.last_bounds.x1 == 30 &&
              layout.last_bounds.y1 == 40 && layout.last_textured && layout.last_shows,
          "the policy sees bounds, texture and visibility");
    check(!r.recipe.fog && r.fog_armed && imm.stats().im2d_fog_armed == 1, "canvas draws are not fogged (counted)");
    check(imm.stats().im2d_in_regions == 1 && imm.stats().im2d_anchors[static_cast<int>(render::Anchor::Right)] == 1,
          "region and anchor counted");

    // Invisible under a source-alpha blend: the policy is told it does not show.
    const render::ImmediateVertex hidden[3] = {vtx(0, 0, 0, 0, 0x00FFFFFFu), vtx(5, 0, 0, 0, 0x00FFFFFFu),
                                               vtx(0, 5, 0, 0, 0x00FFFFFFu)};
    imm.draw2d(hidden, 3, render::Topology::Triangles, none, false, rwim::Binding{}, recipe, nullptr);
    check(!layout.last_shows && !layout.last_textured, "a fully transparent draw does not shape its anchor");

    // Off the canvas: drawn with Fill.
    layout.answer = rwim::Placement{};
    layout.answer.laid_out = false;
    r = imm.draw2d(hidden, 3, render::Topology::Triangles, none, false, rwim::Binding{}, render::Recipe{}, nullptr);
    check(r.anchor == render::Anchor::Fill && imm.stats().im2d_off_canvas == 1, "off canvas: fill, counted");

    // A caller's anchor: no policy call.
    const int calls = layout.calls;
    const render::Anchor projected = render::Anchor::Projected;
    r = imm.draw2d(quad, 4, render::Topology::TriangleStrip, none, false, binding, render::Recipe{}, &projected);
    check(layout.calls == calls && r.anchor == render::Anchor::Projected, "a caller's anchor bypasses the policy");

    // No pass: nothing drawn, counted.
    host.refuse = true;
    r = imm.draw2d(quad, 4, render::Topology::TriangleStrip, none, false, binding, render::Recipe{}, nullptr);
    check(!r.drawn && imm.stats().im2d_no_pass == 1, "no pass: not drawn, counted");
    host.refuse = false;

    host.close();
    const render::FramePacket packet = builder.finish();
    check(packet.immediates.size() == 4 && imm.stats().im2d_draws == 4, "four immediates recorded");
    check(packet.immediates[0].space == render::Space::Canvas && packet.immediates[0].region == 42 &&
              packet.immediates[0].texture.value == 7 && packet.immediates[0].anchor == render::Anchor::Right,
          "the first record carries canvas space, region id, texture and anchor");
    check(packet.immediates[1].texture.value == 0, "untextured: no texture");
}

void test_im3d() {
    render::FrameBuilder builder;
    builder.begin_frame(1, render::Float4{0, 0, 0, 1});
    TestHost host(builder);
    host.view_id = builder.view(render::Camera{}, render::ViewRect{});
    TestLayout layout;
    rwim::Immediate imm(builder, host, layout);
    check(host.ensure_pass(), "pass");
    const render::ImmediateVertex tri[4] = {vtx(0, 0, 0, 0), vtx(1, 0, 0, 0), vtx(1, 1, 0, 0), vtx(0, 1, 0, 0)};
    std::vector<std::uint32_t> order;
    check(rwim::list_order(nullptr, 0, 4, true, &order), "fan order");
    render::Float3x4 world;
    world.m[0][3] = 5.0f;
    imm.draw3d(tri, 4, render::Topology::Triangles, order, true, world, rwim::Binding{}, render::Recipe{});
    host.close();
    const render::FramePacket packet = builder.finish();
    check(packet.immediates.size() == 1 && packet.immediates[0].index_count == 6 &&
              packet.immediates[0].space == render::Space::World,
          "a fan of 4 as 6 world-space indices");
    check(packet.draws.size() == 1 && packet.draws[0].world.m[0][3] == 5.0f, "the world matrix rides on the draw");
}

void test_strip_continuation() {
    render::FrameBuilder builder;
    builder.begin_frame(1, render::Float4{0, 0, 0, 1});
    TestHost host(builder);
    host.view_id = builder.view(render::Camera{}, render::ViewRect{});
    TestLayout layout;  // Center, laid out
    rwim::Immediate imm(builder, host, layout);
    rwim::Binding binding;
    binding.textured = true;
    binding.texture.value = 3;
    const std::vector<std::uint32_t> none;
    // Three 256 x 64 tiles end to end across the 640-unit canvas, reaching both edges.
    for (int k = 0; k < 3; ++k) {
        const float x0 = -64.0f + 256.0f * static_cast<float>(k);
        const render::ImmediateVertex tile[4] = {vtx(x0, 200, 0, 0), vtx(x0, 264, 0, 1), vtx(x0 + 256, 200, 1, 0),
                                                 vtx(x0 + 256, 264, 1, 1)};
        imm.draw2d(tile, 4, render::Topology::TriangleStrip, none, false, binding, render::Recipe{}, nullptr);
    }
    imm.flush_strip();
    host.close();
    const render::FramePacket packet = builder.finish();
    check(imm.stats().strips_continued == 1 && imm.stats().strip_copies >= 2, "a 16:9 view continues the strip");
    check(packet.immediates.size() == 3 + imm.stats().strip_copies, "copies drawn after the strip");
    bool continued = true;
    for (const render::ImmediateDraw& d : packet.immediates)
        continued = continued && d.anchor == render::Anchor::Continued;
    check(continued, "the strip and its copies are placed as Continued");

    // A 4:3 output shows only the canvas: no copies.
    render::FrameBuilder b2;
    b2.begin_frame(1, render::Float4{0, 0, 0, 1});
    TestHost h2(b2);
    h2.aspect = 4.0f / 3.0f;
    h2.view_id = b2.view(render::Camera{}, render::ViewRect{});
    rwim::Immediate imm2(b2, h2, layout);
    for (int k = 0; k < 3; ++k) {
        const float x0 = -64.0f + 256.0f * static_cast<float>(k);
        const render::ImmediateVertex tile[4] = {vtx(x0, 200, 0, 0), vtx(x0, 264, 0, 1), vtx(x0 + 256, 200, 1, 0),
                                                 vtx(x0 + 256, 264, 1, 1)};
        imm2.draw2d(tile, 4, render::Topology::TriangleStrip, none, false, binding, render::Recipe{}, nullptr);
    }
    imm2.flush_strip();
    check(imm2.stats().strips_continued == 1 && imm2.stats().strip_copies == 0, "4:3: recognised, no copies");

    // An indexed draw is never a strip tile.
    render::FrameBuilder b3;
    b3.begin_frame(1, render::Float4{0, 0, 0, 1});
    TestHost h3(b3);
    h3.view_id = b3.view(render::Camera{}, render::ViewRect{});
    rwim::Immediate imm3(b3, h3, layout);
    const std::vector<std::uint32_t> order{0, 1, 2, 2, 1, 3};
    for (int k = 0; k < 3; ++k) {
        const float x0 = -64.0f + 256.0f * static_cast<float>(k);
        const render::ImmediateVertex tile[4] = {vtx(x0, 200, 0, 0), vtx(x0, 264, 0, 1), vtx(x0 + 256, 200, 1, 0),
                                                 vtx(x0 + 256, 264, 1, 1)};
        imm3.draw2d(tile, 4, render::Topology::Triangles, order, true, binding, render::Recipe{}, nullptr);
    }
    imm3.flush_strip();
    check(imm3.stats().strips_continued == 0, "indexed quads are not strip tiles");
}

}  // namespace

int main() {
    test_topologies_and_orders();
    test_im2d_placement();
    test_im3d();
    test_strip_continuation();
    if (g_failures != 0) {
        std::printf("rwim_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("rwim_tests: all passed\n");
    return 0;
}
