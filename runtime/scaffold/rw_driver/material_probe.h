// SCAFFOLDING (R2): material/texture flicker probe over the translated RenderWare draw stream - deleted by R6: the scene graph's lift owns materials and their animation clocks
#pragma once

// Diagnostics (GEKKO_MATERIAL_PROBE=<report.txt>): which draws flicker, and in which input.
//
// Every finished frame, each model draw is identified (mesh, submesh, its placement rounded to a world unit) and
// fingerprinted field by field (texture, sampler, UV transform, material colour, combine and switches, extra pass,
// blend, depth, cull, fog, alpha cutoff). A flicker is an A -> B -> A: a field whose value equals the one two frames
// ago but differs from the last frame's. The report ranks draws by such toggles and names the fields and values.
// Measurement only: it reads the packet, changes nothing.

#include <array>
#include <map>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

#include "gekko/types.h"
#include "render/frame.h"

namespace gekko {
class Cpu;
}

namespace gekko::scaffold {

class MaterialProbe {
public:
    // Enabled when GEKKO_MATERIAL_PROBE names a report file.
    MaterialProbe();
    ~MaterialProbe();
    bool enabled() const noexcept { return !path_.empty(); }
    void frame(const shadow::render::FramePacket& packet);
    void write_report() const;
    bool has_name(std::uint32_t texture) const { return names_.count(texture) != 0u; }
    void set_name(std::uint32_t texture, std::string name) { names_[texture] = std::move(name); }
    // The texture-pattern animations (0x8004F1C4, once per animation per frame): each definition once, and for
    // those named in GEKKO_TEXANIM_TRACE (comma-separated base names) the clock and chosen frame every frame.
    void install(Cpu& cpu);

    enum Field : int {
        kTexture, kSampler, kUv, kColour, kProgram, kExtraKind, kExtraTexture, kExtraUv, kExtraBlend, kBlend,
        kDepth, kCull, kFog, kCutoff, kPassKind, kFieldCount
    };

private:
    struct Values {
        std::array<std::uint64_t, kFieldCount> v{};
    };
    struct Track {
        std::uint64_t first_frame = 0, last_frame = 0, frames = 0;
        Values prev, last;  // the frame before last, and last
        bool have_prev = false, have_last = false;
        std::array<std::uint32_t, kFieldCount> toggles{}, changes{};
        std::array<std::uint64_t, kFieldCount> toggle_a{}, toggle_b{};  // one example A, B per field
        float position[3]{};
        std::uint32_t mesh = 0, submesh = 0;
    };
    std::string path_;
    std::uint64_t frame_ = 0;
    std::unordered_map<std::uint64_t, Track> tracks_;
    std::unordered_map<std::uint32_t, std::string> names_;  // texture handle -> its RwTexture's name
    std::unordered_map<std::uint32_t, bool> texanim_seen_;   // definition -> traced
    // Overlays near a depth-writing draw of the same rotation, keyed by (overlay texture, model texture): frames
    // exactly on it, frames a little apart (the largest offset, the last place) - write_report "shells".
    struct Shell {
        std::uint64_t exact = 0, off = 0, first_frame = 0, last_frame = 0;
        float max_offset = 0.0f;
        float position[3]{};
    };
    std::map<std::pair<std::uint32_t, std::uint32_t>, Shell> shells_;
    std::string texanim_trace_;
    std::FILE* texanim_log_ = nullptr;
    std::FILE* series_ = nullptr;  // <report>.particles.txt
    void texanim(Cpu& c);
};

}  // namespace gekko::scaffold
