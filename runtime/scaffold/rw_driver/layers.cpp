// SCAFFOLDING (R2): which effect a lowered draw belongs to, read from the translated caller chain - deleted by R5 / R6: the effect library's lift names its draws' layers itself
#include <cstddef>

#include "gekko/cpu.h"
#include "rw_driver.h"

// The effect layer switches (render/effect_layers.h, docs/EFFECTS_INVENTORY.md "Layer switches"). The effect
// library still runs translated and reaches this layer through the same lowered sites as the world, so a draw is
// placed by the guest functions it was made from: the element draw functions of the effect engine (the third method
// of each Effect::PJS element class, rtti), the screen-effect tasks' execs and the other effect workers. A return
// address inside one of them on the guest call chain names the layer; the innermost one wins (a particle drawn by the
// glare bucket's walk is a particle). Only walked while a switch is set (RwDriver::open_frame).

namespace gekko::scaffold {

namespace render = shadow::render;

namespace {

struct LayerRange {
    u32 begin;
    u32 size;  // bytes (extract/functions.csv)
    render::EffectLayer layer;
};

constexpr LayerRange kLayerRanges[] = {
    // The lens flare: the two DrawGlobalLensFlare task execs and the draw they call (FLARE.md, flare_trail_shadow 1).
    {0x801D2928u, 176u, render::EffectLayer::LensFlare},
    {0x801D2A94u, 40u, render::EffectLayer::LensFlare},
    {0x80433270u, 44u, render::EffectLayer::LensFlare},
    // DrawBlur / DrawScreenColor / FullScreenShimmer task execs (EffectDraw*Task vf1).
    {0x801D27FCu, 36u, render::EffectLayer::MotionBlur},
    {0x801D2878u, 36u, render::EffectLayer::ScreenColour},
    {0x801D25F0u, 36u, render::EffectLayer::HeatHaze},
    {0x801D2564u, 52u, render::EffectLayer::HeatHaze},
    // The fade curtain (SonicteamUSA::System::Fade::FadeDrawTask exec).
    {0x80044E08u, 48u, render::EffectLayer::ScreenFade},
    // The ground shadows (EffectKage KageTask draw, the member pointer bound with RednerDrawArgs).
    {0x80357578u, 280u, render::EffectLayer::Shadows},
    // The effect engine's element draws (vf2 of EffectParticlePJS / Particle3D / Object / Ray / Locus / Shimmer).
    {0x80434824u, 840u, render::EffectLayer::Particles},
    {0x80433DA8u, 1064u, render::EffectLayer::Particles3D},
    {0x80433AA0u, 176u, render::EffectLayer::EffectModels},
    {0x8042D870u, 2148u, render::EffectLayer::Rays},
    {0x8043B618u, 724u, render::EffectLayer::Trails},
    {0x802DE4BCu, 944u, render::EffectLayer::Trails},  // Test::EffBelt (PlayerEffect Belt)
    {0x8043531Cu, 3656u, render::EffectLayer::Refraction},
    // The atomic shimmer passes (census 1: the LOGIC SET blend) and the shimmer bucket's walk (bucket 6).
    {0x80438168u, 400u, render::EffectLayer::Refraction},
    {0x804382F8u, 1092u, render::EffectLayer::Refraction},
    {0x8043873Cu, 1320u, render::EffectLayer::Refraction},
    {0x804369CCu, 1964u, render::EffectLayer::HeatHaze},  // the full-screen shimmer's draw (copy + grid)
    {0x8002761Cu, 36u, render::EffectLayer::Refraction},
    // A land class's world render (LandDispManager_<tag> -> 0x8004620C -> 0x800471BC: the level recipe 0x80046D80
    // with the class's flags, then RpWorldRender): which land layer is the flags' (land_layer).
    {0x800471BCu, 380u, render::EffectLayer::Landscape},
};

// The level recipe 0x80046D80 (census 4): its flags word in r31 from 0x80046D98 to its return.
constexpr u32 kLevelRecipe = 0x80046D80u, kLevelRecipeSize = 772u;

// The render-class flags of the table 0x8051E328 (census 4) -> the land layer: 0x100 the inverse-colour blend,
// 0x80 additive (fogged 0x04: _K_; unfogged: _G_ / _GL_ in GlareWorld), 0x40 alpha blend, else opaque or
// punch-through (unfogged: _N_ / _D_, the far scenery and the sky).
render::EffectLayer land_layer(u32 flags) noexcept {
    if ((flags & 0x100u) != 0u)
        return render::EffectLayer::LandscapeDarken;
    if ((flags & 0x80u) != 0u)
        return (flags & 0x04u) != 0u ? render::EffectLayer::LandscapeAdditive : render::EffectLayer::LandscapeGlow;
    if ((flags & 0x40u) != 0u)
        return render::EffectLayer::LandscapeAlpha;
    return (flags & 0x04u) != 0u ? render::EffectLayer::Landscape : render::EffectLayer::Sky;
}

render::EffectLayer layer_at(u32 address) noexcept {
    for (const LayerRange& r : kLayerRanges)
        if (address - r.begin < r.size)
            return r.layer;
    return render::EffectLayer::None;
}

}  // namespace

render::EffectLayer RwDriver::layer_of_caller(Cpu& c) {
    // PowerPC EABI (as asset_trace.cpp call_chain): at a lowered site r1 is the calling function's frame and LR its
    // return address; a frame's word 0 is the back chain, and a function saves its return address at 4(caller frame).
    render::EffectLayer layer = layer_at(c.lr & ~3u);
    u32 frame = c.gpr[1];
    for (int depth = 0; layer == render::EffectLayer::None && depth < 48; ++depth) {
        const u32 back = c.read_guest_u32(frame);
        if (back <= frame || (back & 7u) != 0u)
            break;
        const u32 saved = c.read_guest_u32(back + 4u);
        if (saved == 0u)
            break;
        layer = layer_at(saved & ~3u);
        frame = back;
    }
    return layer;
}

void RwDriver::note_land_class(Cpu& c) noexcept {
    // At a state dispatch r31 still holds the recipe's flags (0x80046D98 mr r31, r4; restored only at its return).
    if ((c.lr & ~3u) - kLevelRecipe < kLevelRecipeSize)
        land_flags_ = c.gpr[31];
}

void RwDriver::tag_draws(Cpu& c, render::EffectLayer known, bool land) {
    if (!tag_layers_)
        return;  // every draw stays None (open_frame's begin_frame reset the builder's layer)
    render::EffectLayer layer = known != render::EffectLayer::None ? known : layer_of_caller(c);
    // The class draw renders the class's world sectors and then its models (0x80047304: characters and props carry
    // the same render classes): only a sector is land.
    if (layer == render::EffectLayer::Landscape)
        layer = land ? land_layer(land_flags_) : render::EffectLayer::None;
    if (layer != builder_.layer())
        strip_flush();  // a pending 2D strip keeps the layer it was drawn in
    builder_.set_layer(layer);
}

}  // namespace gekko::scaffold
