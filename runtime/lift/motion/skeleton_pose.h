// A skeleton (.BON record) posed by one motion clip (.MTP / .MTN), evaluated the way the game's single-channel
// evaluator does (docs/DRAW_DISTANCE.md "Enemies and characters"; the research: MOTION_REPORT.md of the animation
// reverse-engineering package, sections 1-4). Pure: bytes in, matrices out - no guest memory, no global state.
//
// Bone binding (8041C1C0 / 8041C30C): the clip's bones in pre-order (self, child, sibling) each consume chunks in a
// fixed order - 3 for pre-scale (0x400), one per translation bit (1 / 2 / 4), unless quaternion (0x40) one per Euler
// bit (8 / 0x10 / 0x20), one per scale bit (0x80 / 0x100 / 0x200); a chunk drives the component when the posed bone
// has it, and an unbound component keeps the bone's rest value.
// Per frame (804198F0, 804196F0): each bound component is its curve's value at the channel time (pre-scale its
// reciprocal); world(bone) = world(parent) * S_pre * T * Rz * Ry * Rx * S, Euler angles quantised to 16 bits.
// The header is C++17; the curves (motion/curves.h) stay inside the library.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "motion/pose_math.h"

namespace shadow::motion {

struct SkeletonBone {
    std::uint32_t id = 0;
    std::uint16_t kind = 0;
    std::uint16_t flags = 0;              // component flags (bone +6)
    std::uint16_t subkind = 0;            // bone +0xC: 6 = no rest rotation stage (80420DBC)
    Vec3 rest_t{}, rest_r{}, rest_s{};    // +0x10 / +0x20 / +0x30
    std::int32_t child = -1, sibling = -1; // bone indices
};

struct Skeleton {
    std::string model;                    // record +0x10
    float scale = 0.0f;                   // record +0xC
    std::vector<SkeletonBone> bones;      // record order (bone 0 = the root)
    std::vector<std::int32_t> preorder() const;   // bone indices: self, child, sibling, from the root
};

// `record`: a record's bytes (0x30 + 0x70 * count), big-endian; `pointer_base`: what its child / sibling words are
// relative to (the record's address for an in-memory record, 0 for a file). False when malformed.
bool parse_skeleton(const std::uint8_t* record, std::size_t size, std::uint32_t pointer_base, Skeleton* out);

struct Clip {
    std::string model;                    // clip +0x14
    float start = 0.0f, end = 0.0f;       // +0x10 / +0x12, in frames
    std::vector<std::uint8_t> bytes;      // the clip, big-endian (normalised)
    std::vector<std::uint32_t> chunks;    // chunk offsets in `bytes`
};

// `clip`: the clip's bytes (its serialized size, clip +8), big-endian. False when malformed or 32-bit keyed.
bool parse_clip(const std::uint8_t* clip, std::size_t size, Clip* out);

// The skeleton at rest (every component its rest value, no pre-scale), model space, indexed like Skeleton::bones.
void rest_pose(const Skeleton& skeleton, std::vector<Mtx34>* world);

// One skeleton posed by one clip of its own model, looping its interval. Matrices are model space (the root's parent
// is the identity), indexed like Skeleton::bones.
class SkeletonPose {
public:
    SkeletonPose();
    ~SkeletonPose();
    SkeletonPose(SkeletonPose&&) noexcept;
    SkeletonPose& operator=(SkeletonPose&&) noexcept;

    bool bind(const Skeleton& skeleton, const Clip& clip);
    // One frame on (80419A9C): time = fma(1, factor, time), looping by repeated subtraction of the interval; a wrap
    // rewinds every curve (80414B80).
    void advance(float factor = 1.0f);
    float time() const noexcept;
    // The pose at the current time.
    void evaluate(std::vector<Mtx34>* world);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace shadow::motion
