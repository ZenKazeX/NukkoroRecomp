#pragma once

// The effect layers (docs/EFFECTS_INVENTORY.md "Layer switches"): every visual effect of the game as a layer the
// player can switch off, or solo (only that effect drawn, the other effects off; optionally without the world, so the
// effect is seen alone). A viewing tool, not a behaviour change: the game runs exactly as before, only what the
// renderer is handed is filtered, and with every layer on the packet is untouched.
//
// A draw belongs to a layer by its record (FrameBuilder stamps DrawRecord::layer with the layer the driver set when
// the draw was made) or by what the packet already says about it: glare is its passes, the environment and dual
// passes are a material's extra pass, fog is the recipe's, lighting is the draw's light set, the fade-in dissolve is
// the material's. The switches are shared between the threads (the window's keys and the launcher's flags set them,
// the game thread asks whether it must tag, the presenter filters), so they are atomics.

#include <atomic>
#include <cstdint>
#include <string>

namespace shadow::render {

struct FramePacket;

enum class EffectLayer : std::uint8_t {
    None = 0,      // the world, the characters, the HUD: not an effect
    Glare,         // the bloom: the glare passes and everything drawn only as a glare source
    LensFlare,     // the sun's glow, ghosts and flash (DrawGlobalLensFlare tasks)
    MotionBlur,    // DrawBlur: the trail of the previous frames (Chaos Control's slow motion)
    HeatHaze,      // the full-screen heat shimmer (FullScreenShimmer)
    Refraction,    // the refraction elements and the atomic shimmer (effect bucket 6)
    ScreenColour,  // DrawScreenColor: the screen multiply, overlay and inversion
    ScreenFade,    // the fade curtain and its logo (FadeDrawTask)
    Particles,     // the effect engine's particle systems (PTank batches)
    Particles3D,   // its 3D particles (camera-facing quads and model particles)
    EffectModels,  // its model elements (EffectObject: explosions' shells, lasers' beams ...)
    Rays,          // its light rays (EffectRay)
    Trails,        // its ribbons (EffectLocus) and the belt ribbons
    Shadows,       // the characters' ground shadows (Kage)
    // The landscape's render classes (the model-name tags of the game's landscape class table; docs/effects/census.md 4), the
    // world sectors of each class (the class's models - characters included - stay None):
    Sky,                // _N_ / _D_: unfogged opaque land (in Westopolis the tunnels as well as the sky)
    Landscape,          // _O_ _OS_ _OSL_ _OW_ _OL_ _P_: the fogged opaque and cut-out land
    LandscapeAlpha,     // _AF_ _A_ _AL_: alpha-blended land
    LandscapeAdditive,  // _K_: additive land (fire glow, light beams painted into the stage)
    LandscapeDarken,    // _M_: the inverse-colour class (src x (1 - dst))
    LandscapeGlow,      // _G_ / _GL_: land drawn only into the glare sources
    Lighting,      // the dynamic lights (a lit draw sees a neutral white ambient light instead)
    BakedLight,    // the light baked into the vertex colours (the stage's prelighting): off draws them white
    Fog,           // the stage fog
    EnvironmentMaps,  // MatFX environment reflections
    DualTextures,     // MatFX second texture layers
    AppearFade,       // the port's fade-in of models that start being drawn inside the view
    Count
};
inline constexpr std::uint32_t kEffectLayerCount = static_cast<std::uint32_t>(EffectLayer::Count);

// The layer's name on the command line and in the overlay ("glare", "lens-flare", ...), and its display title.
const char* effect_layer_name(EffectLayer layer) noexcept;
const char* effect_layer_title(EffectLayer layer) noexcept;
// A name back to its layer; false for an unknown name.
bool effect_layer_from_name(const std::string& name, EffectLayer* out) noexcept;

// What is drawn: the layers switched off, the soloed layer (None: no solo) and whether a solo hides the world.
struct EffectLayerState {
    std::uint32_t off = 0;  // bit (1 << layer)
    EffectLayer solo = EffectLayer::None;
    bool isolate = false;

    bool drawn(EffectLayer layer) const noexcept {
        if (layer == EffectLayer::None)
            return solo == EffectLayer::None || !isolate;
        if (solo != EffectLayer::None)
            return layer == solo;
        return (off & (1u << static_cast<std::uint32_t>(layer))) == 0u;
    }
    bool everything() const noexcept { return off == 0u && solo == EffectLayer::None; }
};

class EffectLayerSwitches {
public:
    EffectLayerState state() const noexcept;
    void set(const EffectLayerState& state) noexcept;
    void toggle(EffectLayer layer) noexcept;
    // Solo `layer`; soloing the soloed layer again ends the solo.
    void solo(EffectLayer layer) noexcept;
    void toggle_isolate() noexcept;
    void all_on() noexcept;
    // Whether the game thread must stamp draws with their layer this frame: only while a switch is not at its default
    // (the classification walks the guest's call chain, which costs nothing when nobody asked for it).
    bool tagging() const noexcept { return !state().everything(); }
    // "glare,fog" -> those layers off. False (and nothing changed) when a name is unknown; `error` names it.
    bool parse_off_list(const std::string& list, std::string* error);
    bool parse_solo(const std::string& name, std::string* error);
    std::string describe() const;

private:
    std::atomic<std::uint32_t> off_{0};
    std::atomic<std::uint32_t> solo_{0};
    std::atomic<bool> isolate_{false};
};

// The process's switches (one game, one picture).
EffectLayerSwitches& effect_layer_switches() noexcept;

// Filters the packet in place: draws of layers not drawn leave their passes, glare passes go when glare is off, and
// the modifier layers (lighting, fog, environment, dual, fade-in) are removed from the draws that carry them.
// Nothing happens when every layer is drawn.
void apply_effect_layers(FramePacket& packet, const EffectLayerState& state);

}  // namespace shadow::render
