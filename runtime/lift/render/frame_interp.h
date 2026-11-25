#pragma once

// Independent frame (docs/re/independent-frame/): a picture between two game frames.
//
// The game describes each of its frames (60 a second) as one immutable FramePacket. The renderer may draw more often
// than that. A PacketInterpolator is given the last two packets, A then B, and builds the packet for any moment
// between them: B's own draw list - what exists is always what the game last said exists - with every transform it can
// follow from A placed `alpha` of the way from A to B. alpha = 1 is B, value for value; nothing is ever written back
// to a packet the game made, and nothing here is seen by the game.
//
// What is followed:
//   * cameras - the views' and the passes', matched per view in their order of first use;
//   * mesh draws that carry a key (DrawRecord::key, the game object's identity): the world matrix, the skin palette,
//     the delta-morph weights and the fade-in's dissolve;
//   * particles that carry a tag (FramePacket::particle_tags, the game's own particle): position, size, rotation
//     (the short way) and colour, in a batch whose key is drawn as often in both packets; and that batch's axes and
//     constant colour. A particle's texture corners are B's (a flip-book steps, it does not slide).
//   * world-space immediate geometry that carries a key (trails, beams: the client's vertex array), drawn as often in
//     both packets with as many vertices: every vertex's position and colour, vertex for vertex, and the draw's world
//     matrix. A trail's samples shift one place along the ribbon each game frame, so vertex for vertex the ribbon
//     flows along its own path.
// What is not (it shows B's state from alpha = 0 on): draws without a key (the land, far stand-ins, canvas geometry
// - the HUD, screen passes), particles without a tag or born since A, texture-coordinate animation, lights.
//
// A transform is taken apart into a rotation, a scale per axis and a position; the rotation turns the short way at a
// constant rate (slerp), scale and position move in a straight line. A matrix that is not a scaled rotation (shear)
// moves element by element. Either way both ends are exact.
//
// Discontinuities are never blended: a camera that jumps or turns more than the limits allow is a cut and the whole
// pair shows B; an object whose key is drawn a different number of times in the two packets, whose mesh or bone count
// changed, or which moved further than the limit, shows B. A particle shows B when its id is not in A's batch (it was
// born since), when it aged differently from the batch's other survivors (another particle at a reused place: its age
// went back) or when it moved further than the limit.

#include <cstdint>
#include <vector>

#include "render/frame.h"

namespace shadow::render {

// A scaled rotation and a position. q = (x, y, z, w), unit length. A mirrored matrix carries the mirror as a negative
// first scale.
struct Trs {
    float q[4] = {0, 0, 0, 1};
    float s[3] = {1, 1, 1};
    float t[3] = {0, 0, 0};
};

// False when `m` is not a scaled rotation (an axis of no length, or axes more than a thousandth off square).
bool decompose(const Float3x4& m, Trs* out);
Float3x4 compose(const Trs& trs);
// The state `t` of the way from a to b (see above). t <= 0 is a and t >= 1 is b, value for value.
Float3x4 interpolate_transform(const Float3x4& a, const Float3x4& b, float t);
Trs interpolate_trs(const Trs& a, const Trs& b, float t);

struct InterpLimits {
    // World units between two game frames beyond which a followed object / a camera is taken to have been placed, not
    // moved. Measured on the Westopolis tape (docs/re/independent-frame/TEMPORAL_INTERPOLATION_SPECIFICATION.md):
    // continuous motion reaches 60.9 (an object) and 40.8 (the camera) units a frame, in runs that decay smoothly;
    // the one placement seen is 213. Twice the largest motion, well under the placement.
    float object_step = 120.0f;
    float camera_step = 120.0f;
    // The cosine of the largest turn of a camera's view direction between two game frames that is still a turn.
    float camera_turn_cos = 0.5f;  // 60 degrees
    // World units per game frame beyond which a particle is taken to have been placed (a pair that spans skipped
    // frames allows as many steps, up to three: further apart the timeline shows B anyway). Measured on the
    // Westopolis tape: between neighbouring frames a particle moves at most 7.6 units; four times that.
    float particle_step = 30.0f;
};

struct InterpStats {
    std::uint32_t keyed = 0;            // B's mesh draws that carry a key
    std::uint32_t followed = 0;         // ... of them, those placed between A and B (moving ones)
    std::uint32_t still = 0;            // ... those equal in both packets
    std::uint32_t snapped = 0;          // ... those shown as B: no partner, a changed mesh, or over the limit
    std::uint32_t bones = 0;            // skin matrices followed
    std::uint32_t cameras = 0;          // cameras followed (moving ones)
    bool cut = false;                   // the pair is a camera cut: every picture of it is B
    // What the pair held, whatever the limits made of it (for measuring the limits on real play):
    float largest_object_step = 0.0f;   // world units, the keyed objects with a partner
    float largest_camera_step = 0.0f;   // world units, the perspective cameras with a partner
    float smallest_camera_turn_cos = 1.0f;
    // Particles (B's):
    std::uint32_t particles = 0;           // all of them
    std::uint32_t particles_tagged = 0;    // ... those that carry a tag
    std::uint32_t particles_followed = 0;  // ... of the tagged, those placed between A and B (changing ones)
    std::uint32_t particles_still = 0;     // ... those equal in both packets
    std::uint32_t particles_born = 0;      // ... those shown as B: not in A's batch, or the batch has no partner
    std::uint32_t particles_reused = 0;    // ... those shown as B: the id is A's but the age went back
    std::uint32_t particles_placed = 0;    // ... those shown as B: over the limit
    std::uint32_t particle_batches = 0;    // batches whose axes or constant colour are followed
    float largest_particle_step = 0.0f;    // world units, the particles with a partner
    // World-space immediate draws that carry a key (B's):
    std::uint32_t immediates = 0;            // all of them
    std::uint32_t immediates_followed = 0;   // ... those whose vertices are placed between A and B
    std::uint32_t immediates_still = 0;      // ... those equal in both packets
    std::uint32_t immediates_snapped = 0;    // ... shown as B: no partner, another shape, or a vertex over the limit
    // ... the shown-as-B ones by cause (they add up to immediates_snapped):
    std::uint32_t immediates_new = 0;        // the key is not in A at all (an effect's first frame)
    std::uint32_t immediates_recount = 0;    // the key is in A, drawn another number of times
    std::uint32_t immediates_grown = 0;      // more vertices than its partner in A
    std::uint32_t immediates_shrunk = 0;     // fewer
    std::uint32_t immediates_restyled = 0;   // same vertex count; another index count, topology, source or texture
    std::uint32_t immediates_placed = 0;     // a vertex over the limit
    std::uint32_t immediate_vertices = 0;    // vertices of the followed draws
    float largest_immediate_step = 0.0f;     // world units, any vertex of a draw with a partner
};

class PacketInterpolator {
public:
    void set_limits(const InterpLimits& limits) noexcept { limits_ = limits; }
    const InterpLimits& limits() const noexcept { return limits_; }

    // The pair to build from: a, the earlier packet, and b. Both stay the caller's and must stay unchanged and alive
    // until the next pair() or clear(). Either may be null: then build() gives b (or nothing).
    void pair(const FramePacket* a, const FramePacket* b);
    void clear() noexcept { pair(nullptr, nullptr); }
    bool paired() const noexcept { return a_ != nullptr && b_ != nullptr; }

    // *out = the picture at `alpha` (0: A's transforms on B's draw list, 1: B). `out` is assigned B first - its
    // vectors keep their capacity from call to call.
    void build(float alpha, FramePacket* out) const;

    const InterpStats& stats() const noexcept { return stats_; }

private:
    struct Mover {
        std::uint32_t index = 0;  // B's draw (world) or B's bone
        Float3x4 from;
        Trs a, b;
        bool trs = false;
    };
    struct CameraMover {
        std::uint32_t index = 0;  // B's view, or B's pass
        bool pass = false;
        Camera from;
        Trs a, b;
        bool trs = false;
    };
    struct Scalar {
        std::uint32_t index = 0;  // B's draw (dissolve) or B's morph weight
        float from = 0.0f;
    };

    struct Pairing {
        std::uint32_t index = 0;  // B's particle / B's particle batch
        std::uint32_t from = 0;   // A's
    };

    void pair_cameras();
    void pair_draws();
    void pair_particles();
    void pair_immediates();

    InterpLimits limits_;
    const FramePacket* a_ = nullptr;
    const FramePacket* b_ = nullptr;
    InterpStats stats_;
    std::vector<Mover> worlds_, bones_;
    std::vector<CameraMover> cameras_;
    std::vector<Scalar> dissolves_, morphs_;
    std::vector<Pairing> particles_, batches_;
    struct Run {
        std::uint32_t first = 0;  // B's first vertex
        std::uint32_t from = 0;   // A's
        std::uint32_t count = 0;
    };
    std::vector<Run> runs_;
    // pair()'s scratch, kept for its capacity.
    struct Tagged {
        std::uint32_t id;
        std::uint32_t particle;
    };
    std::vector<Tagged> tagged_a_;
    std::vector<Pairing> candidates_;
    std::vector<float> ages_, sorted_ages_;
    struct Keyed {
        std::uint64_t key;
        std::uint32_t draw;
    };
    std::vector<Keyed> keyed_a_, keyed_b_;
    std::vector<std::uint8_t> bone_seen_;
};

}  // namespace shadow::render
