#pragma once

// The Mtn animation library's pose pass (main.dol, PAL GUPP8P), owned natively (docs/agents/opt-mtn2.md): the bone-tree walk, the
// node callbacks that compose rotations, scales and translations over the bone index list, and the matrix -> quaternion -> slerp ->
// matrix chain. Names are this project's own, from the disassembly. Round 1 (docs/agents/opt-mtn.md) owns the track sampler below.
//
//   0x804196F0 NodeWalk(r3 = motion, r4 = node): recursive. Node +8 = list of callbacks {fn, next, args...} called with r3 = entry + 8,
//       +0x1C = a post callback (r3 = a 3-word struct built in the frame), +0x10 = matrix to receive a copy of the stack top, +0x24 =
//       child, +0x28 = sibling (a non-zero sibling pushes the matrix stack [r13 + 0x5F50] first and pops it after the subtree).
//       Per channel index (motion +0x24 table, +0x20 count) with a non-zero record (node +0x18, 0x20 bytes each) the channel weight
//       table 0x805E25B0 is advanced (w = f2 * rec[2] + rec[3], f2 = node +0x14 table entry +0x10), the old value saved in the frame
//       (sp + 0x14 + 4 * index) and restored after the child. Motion +0x14 bit 1 stops after the node (no child, no sibling).
//   Node callbacks (r3 = args: +0 list {count, indices*, ..., parents* at +0xC}, +4 matrix, +8 pairs {source*, flags*} per index):
//       0x8041EC24 rotations (Euler, 16-bit turn quantised) blended with slerp over the parent order; pushes / pops the matrix stack
//       0x8041E5D0 scale (vec3 per index), 0x8041E8EC the same with the first base the column lengths of the matrix,
//       0x8041F174 translation, 0x80420B9C one bone's Euler angles applied to the stack top.
//   0x8042167C MatrixBlend(r3 = out, r4 = a, r5 = b, f1 = t): out = slerp(a, b, t) rotation (translation 0), normalised by 0x804121BC.
//   0x80421774 QuatSlerp(r3 = out, r4 = a, r5 = b, f1 = t), 0x8042193C MatrixToQuat(r3 = out, r4 = matrix), 0x80421BB0 QuatToMatrix.
//   0x804121BC MatrixNormalize(r3 = dst or 0 (stack top), r4 = src or 0 (stack top)): copy, then unit columns when longer than 2^-23.
//
// Exactness: every arithmetic step is the original's in the original's operand roles (ppc::fps_*), the game's sqrt through
// gekko::detail::guest_sqrt and acosf / sinf through shadow::math::msl in the caller's FPSCR[NI] mode; compares keep the NaN exception
// bits. Anything the original reads from a stack frame it did not write (a stale local) is read from the same guest address, with
// the guest r1 lowered by the original frame size while guest functions are called. Not kept: the dead frames (saved registers,
// link slots, FPR spills, local copies nothing reads), f0..f13 and r0, r3..r12, CR0, FPSCR[FPCC] / sticky bits of fctiwz.
// Calls out: the rotations 0x8040C85C / 0x8040C898 / 0x8040C8D4 by guest_call; 0x8040C5EC / 0x8040C7E0 through their native owners;
// 0x8040C748, 0x8040CB14 (retired lift site) and the matrix stack leaves 0x8040D198 / D254 / D294 / 0x80412750 / 0x800091D0 inline.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::mtn_pose {

constexpr u32 kNodeWalk = 0x804196F0u;
constexpr u32 kRotationPass = 0x8041EC24u;
constexpr u32 kScalePass = 0x8041E5D0u;
constexpr u32 kScaleLengthsPass = 0x8041E8ECu;
constexpr u32 kTranslationPass = 0x8041F174u;
constexpr u32 kEulerBone = 0x80420B9Cu;
constexpr u32 kMatrixBlend = 0x8042167Cu;
constexpr u32 kQuatSlerp = 0x80421774u;
constexpr u32 kMatrixToQuat = 0x8042193Cu;
constexpr u32 kQuatToMatrix = 0x80421BB0u;
constexpr u32 kMatrixNormalize = 0x804121BCu;

// The guest entries served here: equal to this section of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kNodeWalk, "Mtn NodeWalk"},
                                                 {kRotationPass, "Mtn RotationPass"},
                                                 {kScalePass, "Mtn ScalePass"},
                                                 {kScaleLengthsPass, "Mtn ScaleLengthsPass"},
                                                 {kTranslationPass, "Mtn TranslationPass"},
                                                 {kEulerBone, "Mtn EulerBone"},
                                                 {kMatrixBlend, "Mtn MatrixBlend"},
                                                 {kQuatSlerp, "Mtn QuatSlerp"},
                                                 {kMatrixToQuat, "Mtn MatrixToQuat"},
                                                 {kQuatToMatrix, "Mtn QuatToMatrix"},
                                                 {kMatrixNormalize, "Mtn MatrixNormalize"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

void serve_node_walk(Cpu& cpu);
void serve_rotation_pass(Cpu& cpu);
void serve_scale_pass(Cpu& cpu);
void serve_scale_lengths_pass(Cpu& cpu);
void serve_translation_pass(Cpu& cpu);
void serve_euler_bone(Cpu& cpu);
void serve_matrix_blend(Cpu& cpu);
void serve_quat_slerp(Cpu& cpu);
void serve_matrix_to_quat(Cpu& cpu);
void serve_quat_to_matrix(Cpu& cpu);
void serve_matrix_normalize(Cpu& cpu);


// ---- round 4 (docs/agents/s1-mtn.md): the NodeWalk callbacks folded into the owned walk ----------------------------------------------
// Five callbacks the walk used to enter as guest code are executed inside the native walk when the fold is on:
//   0x80420B28 scale of the stack top by the first bone's vector (skipped when it is exactly 1, 1, 1), 0x80420D10 translation of the
//   stack top by it, 0x8041FA14 / 0x804202E0 / 0x8041FDDC the "bone bank" passes (scale lengths, translation, rotation) over the
//   persistent per-bone matrices at 0x805E23A0 + 0x30 * position. They are NOT installed as owners (the guest bodies stay, for every other
//   caller); a case the fold cannot prove (a bone index above 3, a list whose first entry is not a processed root, a null base matrix where
//   the body reads it) is handed back to the guest body unchanged. Off by default: GEKKO_MTN_FOLD=1 enables (=0 disables), the build
//   option SHADOW_MTN_FOLD defines GEKKO_MTN_FOLD_DEFAULT=1. GEKKO_MTN_FOLD_STATS=1 prints the deterministic hit counts at exit.
constexpr u32 kFoldScaleCallback = 0x80420B28u;        // liveness-roots: value-only
constexpr u32 kFoldTranslateCallback = 0x80420D10u;    // liveness-roots: value-only
constexpr u32 kFoldScaleBank = 0x8041FA14u;            // liveness-roots: value-only
constexpr u32 kFoldTranslationBank = 0x804202E0u;      // liveness-roots: value-only
constexpr u32 kFoldRotationBank = 0x8041FDDCu;         // liveness-roots: value-only
constexpr unsigned kFoldKinds = 5;
void set_fold_enabled(bool enabled) noexcept;
bool fold_enabled() noexcept;
// Folded executions since start, per kind (order: scale callback, translate callback, scale bank, translation bank, rotation bank);
// handed-back calls are counted apart. Deterministic for a given input: the same replay gives the same numbers.
unsigned long long fold_hits(unsigned kind) noexcept;
unsigned long long fold_handed_back(unsigned kind) noexcept;
// The folded bodies as entry points for the contract test (r3 = the callback's args block). Not installed as owners.
void serve_fold_scale_callback(Cpu& cpu);
void serve_fold_translate_callback(Cpu& cpu);
void serve_fold_scale_bank(Cpu& cpu);
void serve_fold_translation_bank(Cpu& cpu);
void serve_fold_rotation_bank(Cpu& cpu);

// ---- round 5 (docs/agents/s1-mtn.md): the MotionUpdate tree --------------------------------------------------------------------------
//   0x80416EF4 MotionUpdate(r3 = motion): push the matrix stack, identity top, clear flag bits, per active channel 0x8041BB8C (guest),
//   then 0x8041AFAC / 0x8041A33C / 0x80419A9C (guest), 0x804198F0 sampler pick (native here: sampler triple by the sign of the channel rate,
//   the sampling context, the channel callback lists), 0x80419094 bone pass (native here: root chain, per-channel local matrices, the blended
//   bank, channel weights, then NodeWalk), 0x80418618 (guest), pop. An HLE hook (not an owner row: the guest body stays and runs whenever
//   the switch is off). Off by default: GEKKO_MTN_TREE=1 enables (=0 disables), GEKKO_MTN_TREE_STATS=1 (or an explicit =1) prints the
//   hit counts at exit, build option SHADOW_MTN_TREE defines GEKKO_MTN_TREE_DEFAULT=1.
constexpr u32 kMotionUpdate = 0x80416EF4u;
constexpr unsigned kTreeKinds = 4;  // motion updates, channels sampled, bone passes, declined (switch off)
void set_tree_enabled(bool enabled) noexcept;
bool tree_enabled() noexcept;
unsigned long long tree_hits(unsigned kind) noexcept;
void serve_motion_update(Cpu& cpu);  // contract-test entries: the native bodies (r3 = motion), not installed as owners
void serve_sampler_pick(Cpu& cpu);
void serve_bone_pass(Cpu& cpu);

// ---- round 6 (docs/agents/s1-mtn.md): candidate D, the pose rebuild phase 0x80418618 ---------------------------------------------------
//   0x80418618(r3 = motion): the last phase of MotionUpdate. Rebuilds the two-part pose deltas, inverts the root chain, composes every active
//   channel matrix, the partner blend (kind 1) and the delta propagation (kind 0), resets the per-channel state. HLE hook with hand-back
//   (declines to the guest body when off, or before any write when a bone index is above 3, a partner index is above 3, or the first active
//   channel has a partner: the original then reads an unwritten frame local). Off by default: GEKKO_MTN_PHASE_D=1 (=0 disables),
//   GEKKO_MTN_PHASE_D_STATS=1 prints the counts, build option SHADOW_MTN_PHASE_D -> GEKKO_MTN_PHASE_D_DEFAULT=1. The native MotionUpdate
//   calls it directly when it is on.
constexpr u32 kPoseRebuild = 0x80418618u;  // liveness-roots: value-only
void set_phase_d_enabled(bool enabled) noexcept;
bool phase_d_enabled() noexcept;
unsigned long long phase_d_hits(unsigned kind) noexcept;  // 0 folded, 1 handed back, 2 channels visited
void serve_phase_d(Cpu& cpu);                             // contract-test entry: the native body without the eligibility check
bool serve_phase_d_hand_back(Cpu& cpu);                   // contract-test entry: true when the hook declined (nothing written)

// ---- round 6, candidate C: the time advance phase 0x80419A9C ------------------------------------------------------------------------------
//   0x80419A9C(r3 = motion): the third phase of MotionUpdate. Per active channel: clamp / advance the parameter block (kind 0 timed with finish and
//   table compaction, kind 1 follows a partner, others clamp), advance the playback time with the loop / wrap / stop rules and the matrix wrap
//   accumulation (guest helper 0x8041ABF8), and call 0x8041BF20 for a channel that moved. HLE hook with hand-back (declines to the guest body
//   when off, or before any write when an active channel has no parameter block). Off by default: GEKKO_MTN_PHASE_C=1 (=0 disables),
//   GEKKO_MTN_PHASE_C_STATS=1 prints the counts, build option SHADOW_MTN_PHASE_C -> GEKKO_MTN_PHASE_C_DEFAULT=1.
constexpr u32 kTimeAdvance = 0x80419A9Cu;  // liveness-roots: value-only
void set_phase_c_enabled(bool enabled) noexcept;
// Contract-test hook, present only when GEKKO_MTN_TEST_HOOKS is defined (the mtn_pose_contract_tests target compiles native_mtn_pose.cpp itself with it; the
// game build has no such symbol): stop phase C (leaving memory as it is) when its wrap loops have arrived at their loop test more than `cap` times since
// this call (0 = unlimited); lets the test compare the loops that never terminate with the original frozen at the same arrival.
#ifdef GEKKO_MTN_TEST_HOOKS
void set_wrap_loop_cap(unsigned long long cap) noexcept;
#endif
bool phase_c_enabled() noexcept;
unsigned long long phase_c_hits(unsigned kind) noexcept;  // 0 folded, 1 handed back, 2 channels visited
void serve_phase_c(Cpu& cpu);                             // contract-test entry: the native body without the eligibility check
bool serve_phase_c_hand_back(Cpu& cpu);                   // contract-test entry: true when the hook declined (nothing written)

// ---- round 7, candidate B: the clip start phase 0x8041A33C and the bind tree below it ------------------------------------------------------
//   0x8041A33C(r3 = motion): the second phase of MotionUpdate. Per channel with a start request: 0x8041ADAC (release / acquire the clip, reset the
//   channel state, 0x8041C1C0 bind: 0x8041C74C reset the track state arrays, 0x8041C30C bind the key records of the clip's node tree through the
//   guest record cursor 0x8041352C and the track state init 0x80414E88), the range flags 0x8041BDE4, the guest callbacks 0x8041DEC4 / 0x8041ABF8 /
//   0x8041A934, then 0x8041A508 or 0x8041A6BC. The clip database / allocator side (0x80414368, 0x80414290, 0x80412E3C, 0x80413644, 0x8041342C,
//   0x8041174C, 0x8041352C) stays guest code. HLE hook with hand-back (declines to the guest body when off, or before any write when an active channel
//   has no parameter block). Off by default: GEKKO_MTN_PHASE_B=1 (=0 disables), GEKKO_MTN_PHASE_B_STATS=1 prints the counts, build option
//   SHADOW_MTN_PHASE_B -> GEKKO_MTN_PHASE_B_DEFAULT=1.
constexpr u32 kClipStartPhase = 0x8041A33Cu;  // liveness-roots: value-only
void set_phase_b_enabled(bool enabled) noexcept;
bool phase_b_enabled() noexcept;
unsigned long long phase_b_hits(unsigned kind) noexcept;  // 0 runs, 1 handed back, 2 clips started, 3 nodes bound, 4 node searches
void serve_phase_b(Cpu& cpu);                             // contract-test entries: the native bodies without the eligibility check
bool serve_phase_b_hand_back(Cpu& cpu);
void serve_bind_nodes(Cpu& cpu);    // r3 obj, r4 node, r5 idx, r6 cursor argument, f1 weight
void serve_reset_tracks(Cpu& cpu);  // r3 node, r4 idx
void serve_find_node(Cpu& cpu);     // r3 node, r4 key -> r3
void serve_range_flags(Cpu& cpu);   // r3 a, r4 b, f1, f2 -> r3
void serve_clip_bind(Cpu& cpu);     // r3 obj, r4 idx, r5 clip -> r3

void install(Cpu& cpu);

}  // namespace gekko::mtn_pose
