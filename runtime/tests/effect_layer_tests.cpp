// Tests of the effect layer switches (runtime/lift/render/effect_layers.h, docs/EFFECTS_INVENTORY.md "Layer switches"):
// what each switch, a solo and an isolated solo leave of a packet, the modifier layers, and the name parsing. Frame
// packets only: no device, no game.

#include <cstdint>
#include <cstdio>
#include <string>

#include "render/effect_layers.h"
#include "render/frame.h"

namespace {

using namespace shadow;
using render::EffectLayer;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// Indices of the scene pass's draws, in order.
enum SceneDraw : std::uint32_t { kWorld, kFogged, kEnvironment, kLit, kParticles, kTrail, kSceneDraws };

// One view: a scene pass with a plain world draw, a fogged one, one with an environment pass, a lit one, a particle
// batch and the trail; then the glare sources (one untagged world glare draw, one particle batch) and the glare.
render::FramePacket packet() {
    render::FrameBuilder b;
    b.begin_frame(1, render::Float4{0, 0, 0, 1});
    const render::ViewId view = b.view(render::Camera{}, render::ViewRect{});
    render::LightSet lights;
    lights.ambient = render::Float3{0.2f, 0.2f, 0.2f};
    lights.directional_count = 1;
    const render::LightSetId lit = b.lights(lights);
    b.begin_pass(view, render::PassKind::Scene, render::Clear{});
    const render::MeshHandle mesh{1};
    render::Material plain;
    plain.vertex_colours = false;
    render::Recipe recipe;
    b.draw(mesh, 0, plain, render::Float3x4{}, recipe);
    render::Recipe fogged = recipe;
    fogged.fog = true;
    b.draw(mesh, 0, plain, render::Float3x4{}, fogged);
    render::Material shiny = plain;
    shiny.extra.kind = render::ExtraPass::Kind::Environment;
    b.draw(mesh, 0, shiny, render::Float3x4{}, recipe);
    b.draw(mesh, 0, plain, render::Float3x4{}, fogged, lit);
    render::Particle particle;
    b.set_layer(EffectLayer::Particles);
    b.particles(render::ParticleBatch{}, &particle, 1, recipe);
    render::ImmediateVertex vertex;
    b.set_layer(EffectLayer::MotionBlur);
    b.immediate(render::ImmediateDraw{}, &vertex, 1, nullptr, 0, render::Float3x4{}, recipe);
    b.set_layer(EffectLayer::None);
    b.end_pass();
    b.begin_pass(view, render::PassKind::GlareSources, render::Clear{});
    b.draw(mesh, 0, plain, render::Float3x4{}, recipe);
    b.set_layer(EffectLayer::Particles);
    b.particles(render::ParticleBatch{}, &particle, 1, recipe);
    b.set_layer(EffectLayer::None);
    b.end_pass();
    b.glare(view, render::GlareParams{});
    return b.finish();
}

std::uint32_t scene_count(const render::FramePacket& p) { return p.passes[0].draw_count; }
std::uint32_t sources_count(const render::FramePacket& p) { return p.passes[1].draw_count; }
const render::DrawRecord& scene_draw(const render::FramePacket& p, std::uint32_t i) {
    return p.draws[p.passes[0].first_draw + i];
}

void test_stamping_and_default() {
    render::FramePacket p = packet();
    check(p.passes.size() == 3 && scene_count(p) == kSceneDraws && sources_count(p) == 2, "the test packet's shape");
    check(scene_draw(p, kParticles).layer == EffectLayer::Particles && scene_draw(p, kTrail).layer ==
                                                                           EffectLayer::MotionBlur &&
              scene_draw(p, kWorld).layer == EffectLayer::None,
          "the builder stamps the layer set before each draw");
    const std::size_t draws = p.draws.size();
    render::apply_effect_layers(p, render::EffectLayerState{});
    check(p.draws.size() == draws && scene_draw(p, kFogged).recipe.fog && p.passes[2].view == 0u,
          "every layer on: the packet is untouched");
}

void test_off() {
    render::FramePacket p = packet();
    render::EffectLayerState s;
    s.off = 1u << static_cast<std::uint32_t>(EffectLayer::Particles);
    render::apply_effect_layers(p, s);
    check(scene_count(p) == kSceneDraws - 1 && sources_count(p) == 1, "particles off: both batches go");
    check(p.passes[1].first_draw == p.passes[0].first_draw + scene_count(p), "the passes stay contiguous");
    check(scene_draw(p, kParticles).layer == EffectLayer::MotionBlur, "the draws after a removed one move up");

    p = packet();
    s.off = 1u << static_cast<std::uint32_t>(EffectLayer::Glare);
    render::apply_effect_layers(p, s);
    check(sources_count(p) == 0 && p.passes[1].view == 0xFFFFFFFFu && p.passes[2].view == 0xFFFFFFFFu,
          "glare off: the sources and the glare pass are not drawn");
    check(scene_count(p) == kSceneDraws, "glare off: the scene keeps its draws");

    p = packet();
    s.off = (1u << static_cast<std::uint32_t>(EffectLayer::Fog)) |
            (1u << static_cast<std::uint32_t>(EffectLayer::EnvironmentMaps));
    render::apply_effect_layers(p, s);
    check(scene_count(p) == kSceneDraws && !scene_draw(p, kFogged).recipe.fog &&
              scene_draw(p, kEnvironment).material.extra.kind == render::ExtraPass::Kind::None,
          "fog and environment off: the draws stay, without their fog and extra pass");

    p = packet();
    s.off = 1u << static_cast<std::uint32_t>(EffectLayer::Lighting);
    const std::size_t sets = p.light_sets.size();
    render::apply_effect_layers(p, s);
    const render::DrawRecord& lit = scene_draw(p, kLit);
    check(p.light_sets.size() == sets + 1 && lit.lights == sets && lit.material.ambient &&
              p.light_sets[sets].directional_count == 0 && p.light_sets[sets].ambient.x == 1.0f,
          "lighting off: a lit draw sees one white ambient light");
    check(scene_draw(p, kWorld).lights == render::kNoLights, "lighting off: an unlit draw is not given lights");

    p = packet();
    p.draws[p.passes[0].first_draw + kFogged].material.vertex_colours = true;  // a prelit land draw
    s.off = 1u << static_cast<std::uint32_t>(EffectLayer::BakedLight);
    render::apply_effect_layers(p, s);
    check(scene_count(p) == kSceneDraws && !scene_draw(p, kFogged).material.baked_light &&
              scene_draw(p, kWorld).material.baked_light,
          "baked light off: a prelit draw stays, without its baked light");
}

void test_solo() {
    render::FramePacket p = packet();
    render::EffectLayerState s;
    s.solo = EffectLayer::Particles;
    render::apply_effect_layers(p, s);
    check(scene_count(p) == 5 && sources_count(p) == 0, "solo particles: the world stays, the trail and glare go");
    check(!scene_draw(p, kFogged).recipe.fog && scene_draw(p, kEnvironment).material.extra.kind ==
                                                    render::ExtraPass::Kind::None,
          "solo: the modifier layers are off too");

    p = packet();
    s.isolate = true;
    render::apply_effect_layers(p, s);
    check(scene_count(p) == 1 && scene_draw(p, 0).layer == EffectLayer::Particles,
          "isolated solo: only the layer's draws");

    p = packet();
    s.solo = EffectLayer::Fog;
    render::apply_effect_layers(p, s);
    check(scene_count(p) == 2 && scene_draw(p, 0).recipe.fog && scene_draw(p, 1).recipe.fog,
          "isolated modifier solo: only the draws that carry it, with it");

    p = packet();
    s.solo = EffectLayer::Glare;
    render::apply_effect_layers(p, s);
    check(scene_count(p) == 0 && sources_count(p) == 1 && p.passes[2].view == 0u,
          "isolated glare: the world's glare sources and the glare alone");
}

void test_switches() {
    render::EffectLayerSwitches sw;
    std::string error;
    check(sw.parse_off_list("glare,fog", &error) && !sw.state().drawn(EffectLayer::Glare) &&
              !sw.state().drawn(EffectLayer::Fog) && sw.state().drawn(EffectLayer::Particles) && sw.tagging(),
          "--effects-off names");
    check(!sw.parse_off_list("glare,bogus", &error) && error.find("bogus") != std::string::npos &&
              !sw.state().drawn(EffectLayer::Fog),
          "an unknown name is refused and changes nothing");
    sw.toggle(EffectLayer::Glare);
    check(sw.state().drawn(EffectLayer::Glare), "toggle switches a layer back on");
    sw.solo(EffectLayer::Rays);
    check(sw.state().solo == EffectLayer::Rays && !sw.state().drawn(EffectLayer::Particles), "solo");
    sw.solo(EffectLayer::Rays);
    check(sw.state().solo == EffectLayer::None, "soloing the soloed layer ends the solo");
    sw.all_on();
    check(sw.state().everything() && !sw.tagging(), "all on");
    EffectLayer layer = EffectLayer::None;
    for (std::uint32_t i = 1; i < render::kEffectLayerCount; ++i) {
        const auto l = static_cast<EffectLayer>(i);
        check(render::effect_layer_from_name(render::effect_layer_name(l), &layer) && layer == l,
              "every layer's name maps back to it");
    }
}

}  // namespace

int main() {
    test_stamping_and_default();
    test_off();
    test_solo();
    test_switches();
    if (g_failures == 0)
        std::printf("effect_layer_tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
