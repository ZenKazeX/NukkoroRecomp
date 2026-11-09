#include "render/effect_layers.h"

#include <cstddef>

#include "render/frame.h"

namespace shadow::render {

namespace {

struct LayerNames {
    const char* name;
    const char* title;
};

constexpr LayerNames kNames[kEffectLayerCount] = {
    {"world", "World"},
    {"glare", "Glare (bloom)"},
    {"lens-flare", "Lens flare"},
    {"motion-blur", "Motion blur (trail)"},
    {"heat-haze", "Heat haze"},
    {"refraction", "Refraction"},
    {"screen-colour", "Screen colour"},
    {"screen-fade", "Screen fade"},
    {"particles", "Particles"},
    {"particles-3d", "3D particles"},
    {"effect-models", "Effect models"},
    {"rays", "Light rays"},
    {"trails", "Trails"},
    {"shadows", "Shadows"},
    {"land-unfogged", "Land, unfogged (_N_ _D_)"},
    {"land-fogged", "Land, fogged (_O_ _OW_ _P_ ...)"},
    {"land-alpha", "Land, alpha (_A_ _AF_ _AL_)"},
    {"land-additive", "Land, additive (_K_)"},
    {"land-darken", "Land, darken (_M_)"},
    {"land-glow", "Land, glow (_G_ _GL_)"},
    {"lighting", "Lighting (dynamic)"},
    {"baked-light", "Baked light (vertex colours)"},
    {"fog", "Fog"},
    {"environment-maps", "Environment maps"},
    {"dual-textures", "Dual textures"},
    {"appear-fade", "Fade-in dissolve"},
};

constexpr std::uint32_t bit(EffectLayer layer) noexcept { return 1u << static_cast<std::uint32_t>(layer); }

// A draw the driver did not tag still carries a modifier layer when its material or recipe has it.
bool carries(const DrawRecord& draw, EffectLayer layer) noexcept {
    switch (layer) {
        case EffectLayer::Lighting:
            return draw.lights != kNoLights;
        case EffectLayer::BakedLight:
            return draw.material.vertex_colours;
        case EffectLayer::Fog:
            return draw.recipe.fog;
        case EffectLayer::EnvironmentMaps:
            return draw.material.extra.kind == ExtraPass::Kind::Environment;
        case EffectLayer::DualTextures:
            return draw.material.extra.kind == ExtraPass::Kind::Dual;
        case EffectLayer::AppearFade:
            return draw.material.dissolve > 0.0f;
        default:
            return false;
    }
}

bool modifier(EffectLayer layer) noexcept {
    return layer == EffectLayer::Lighting || layer == EffectLayer::BakedLight || layer == EffectLayer::Fog || layer == EffectLayer::EnvironmentMaps ||
           layer == EffectLayer::DualTextures || layer == EffectLayer::AppearFade;
}

}  // namespace

const char* effect_layer_name(EffectLayer layer) noexcept {
    const auto i = static_cast<std::uint32_t>(layer);
    return i < kEffectLayerCount ? kNames[i].name : "?";
}

const char* effect_layer_title(EffectLayer layer) noexcept {
    const auto i = static_cast<std::uint32_t>(layer);
    return i < kEffectLayerCount ? kNames[i].title : "?";
}

bool effect_layer_from_name(const std::string& name, EffectLayer* out) noexcept {
    for (std::uint32_t i = 1; i < kEffectLayerCount; ++i) {
        if (name == kNames[i].name) {
            *out = static_cast<EffectLayer>(i);
            return true;
        }
    }
    return false;
}

EffectLayerState EffectLayerSwitches::state() const noexcept {
    EffectLayerState s;
    s.off = off_.load(std::memory_order_relaxed);
    s.solo = static_cast<EffectLayer>(solo_.load(std::memory_order_relaxed));
    s.isolate = isolate_.load(std::memory_order_relaxed);
    return s;
}

void EffectLayerSwitches::set(const EffectLayerState& state) noexcept {
    off_.store(state.off, std::memory_order_relaxed);
    solo_.store(static_cast<std::uint32_t>(state.solo), std::memory_order_relaxed);
    isolate_.store(state.isolate, std::memory_order_relaxed);
}

void EffectLayerSwitches::toggle(EffectLayer layer) noexcept {
    if (layer != EffectLayer::None && layer != EffectLayer::Count)
        off_.fetch_xor(bit(layer), std::memory_order_relaxed);
}

void EffectLayerSwitches::solo(EffectLayer layer) noexcept {
    const auto value = static_cast<std::uint32_t>(layer);
    solo_.store(solo_.load(std::memory_order_relaxed) == value ? 0u : value, std::memory_order_relaxed);
}

void EffectLayerSwitches::toggle_isolate() noexcept { isolate_.store(!isolate_.load(std::memory_order_relaxed)); }

void EffectLayerSwitches::all_on() noexcept { set(EffectLayerState{}); }

bool EffectLayerSwitches::parse_off_list(const std::string& list, std::string* error) {
    std::uint32_t off = 0;
    std::size_t begin = 0;
    while (begin <= list.size()) {
        const std::size_t end = list.find(',', begin);
        const std::string name = list.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!name.empty()) {
            EffectLayer layer;
            if (!effect_layer_from_name(name, &layer)) {
                if (error) *error = "unknown effect layer '" + name + "'";
                return false;
            }
            off |= bit(layer);
        }
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    off_.store(off, std::memory_order_relaxed);
    return true;
}

bool EffectLayerSwitches::parse_solo(const std::string& name, std::string* error) {
    EffectLayer layer;
    if (!effect_layer_from_name(name, &layer)) {
        if (error) *error = "unknown effect layer '" + name + "'";
        return false;
    }
    solo_.store(static_cast<std::uint32_t>(layer), std::memory_order_relaxed);
    return true;
}

std::string EffectLayerSwitches::describe() const {
    const EffectLayerState s = state();
    if (s.everything())
        return "all effect layers on";
    if (s.solo != EffectLayer::None)
        return std::string("solo ") + effect_layer_name(s.solo) + (s.isolate ? " (without the world)" : "");
    std::string out = "off:";
    for (std::uint32_t i = 1; i < kEffectLayerCount; ++i)
        if ((s.off & (1u << i)) != 0u)
            out += std::string(" ") + kNames[i].name;
    return out;
}

EffectLayerSwitches& effect_layer_switches() noexcept {
    static EffectLayerSwitches switches;
    return switches;
}

void apply_effect_layers(FramePacket& packet, const EffectLayerState& state) {
    if (state.everything())
        return;
    const bool glare = state.drawn(EffectLayer::Glare);
    const bool isolated_modifier = state.solo != EffectLayer::None && state.isolate && modifier(state.solo);
    LightSetId neutral = kNoLights;
    if (!state.drawn(EffectLayer::Lighting)) {
        LightSet white;
        white.ambient = Float3{1, 1, 1};
        neutral = static_cast<LightSetId>(packet.light_sets.size());
        packet.light_sets.push_back(white);
    }
    // Passes keep their order and only lose draws, so the draws are compacted in place pass by pass.
    std::uint32_t write = 0;
    for (PassRecord& pass : packet.passes) {
        const bool sources = pass.kind == PassKind::GlareSources;
        if (!glare && (sources || pass.kind == PassKind::Glare))
            pass.view = 0xFFFFFFFFu;  // no view draws it (its draws go below)
        const std::uint32_t first = pass.first_draw;
        const std::uint32_t count = pass.draw_count;
        pass.first_draw = write;
        std::uint32_t kept = 0;
        for (std::uint32_t d = first; d < first + count && d < packet.draws.size(); ++d) {
            DrawRecord draw = packet.draws[d];
            // An untagged glare source is the world's glare geometry (the _G_ models, drawn only into the glare).
            const EffectLayer layer = draw.layer == EffectLayer::None && sources ? EffectLayer::Glare : draw.layer;
            bool keep = state.drawn(layer) && (glare || !sources);
            if (isolated_modifier && layer == EffectLayer::None)
                keep = carries(draw, state.solo);
            if (!keep)
                continue;
            if (!state.drawn(EffectLayer::Lighting) && draw.lights != kNoLights) {
                // No directional or point light: the shading goes. A prelit draw keeps its vertex colours alone
                // (an ambient on top would brighten the two-stage form); an unlit-by-vertex draw sees white.
                draw.lights = neutral;
                draw.material.ambient = !draw.material.vertex_colours;
            }
            if (!state.drawn(EffectLayer::BakedLight) && draw.material.vertex_colours)
                draw.material.baked_light = false;
            if (!state.drawn(EffectLayer::Fog))
                draw.recipe.fog = false;
            if ((!state.drawn(EffectLayer::EnvironmentMaps) &&
                 draw.material.extra.kind == ExtraPass::Kind::Environment) ||
                (!state.drawn(EffectLayer::DualTextures) && draw.material.extra.kind == ExtraPass::Kind::Dual))
                draw.material.extra.kind = ExtraPass::Kind::None;
            if (!state.drawn(EffectLayer::AppearFade))
                draw.material.dissolve = 0.0f;
            packet.draws[write++] = draw;
            ++kept;
        }
        pass.draw_count = kept;
    }
    packet.draws.resize(write);
}

}  // namespace shadow::render
