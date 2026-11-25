// Tests of the independent frame's packet side (runtime/lift/render/frame_interp.h, frame_timeline.h;
// docs/re/independent-frame/): a transform between two transforms, a packet between two packets, and where between
// two game frames a picture belongs. Frame packets and synthetic clocks only: no device, no game, no wall clock.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "render/frame.h"
#include "render/frame_interp.h"
#include "render/frame_timeline.h"

namespace {

using namespace shadow;
using render::Float3x4;
using render::FramePacket;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

constexpr float kPi = 3.14159265358979323846f;
constexpr float kUnit = 1e-4f;  // the project's accuracy target for normalised quantities

// A rotation of `angle` about the (unit) axis, scaled per axis, at `position`.
Float3x4 transform(float ax, float ay, float az, float angle, float sx, float sy, float sz, float px, float py,
                   float pz) {
    const float c = std::cos(angle), s = std::sin(angle), k = 1.0f - c;
    const float r[3][3] = {
        {c + ax * ax * k, ax * ay * k - az * s, ax * az * k + ay * s},
        {ay * ax * k + az * s, c + ay * ay * k, ay * az * k - ax * s},
        {az * ax * k - ay * s, az * ay * k + ax * s, c + az * az * k},
    };
    const float scale[3] = {sx, sy, sz};
    Float3x4 out;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            out.m[row][col] = r[row][col] * scale[col];
    out.m[0][3] = px;
    out.m[1][3] = py;
    out.m[2][3] = pz;
    return out;
}
Float3x4 rotation_y(float angle) { return transform(0, 1, 0, angle, 1, 1, 1, 0, 0, 0); }

bool same(const Float3x4& a, const Float3x4& b) { return std::memcmp(a.m, b.m, sizeof(a.m)) == 0; }

float max_difference(const Float3x4& a, const Float3x4& b) {
    float worst = 0.0f;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            worst = std::fmax(worst, std::fabs(a.m[r][c] - b.m[r][c]));
    return worst;
}

// How far the 3x3 part is from `scale` times a rotation: the worst error of an axis length or of the squareness of
// two axes (0 for an exact scaled rotation).
float shape_error(const Float3x4& m, float sx, float sy, float sz) {
    const float want[3] = {sx, sy, sz};
    float axis[3][3], worst = 0.0f;
    for (int c = 0; c < 3; ++c) {
        float length = 0.0f;
        for (int r = 0; r < 3; ++r)
            length += m.m[r][c] * m.m[r][c];
        length = std::sqrt(length);
        worst = std::fmax(worst, std::fabs(length - std::fabs(want[c])) / std::fabs(want[c]));
        for (int r = 0; r < 3; ++r)
            axis[c][r] = m.m[r][c] / length;
    }
    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j)
            worst = std::fmax(worst, std::fabs(axis[i][0] * axis[j][0] + axis[i][1] * axis[j][1] +
                                               axis[i][2] * axis[j][2]));
    return worst;
}

float determinant(const Float3x4& m) {
    return m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) -
           m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0]) +
           m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
}

void transform_tests() {
    // Taken apart and put together again.
    const Float3x4 cases[] = {
        Float3x4{},
        rotation_y(0.3f),
        transform(0.6f, 0.0f, 0.8f, 2.9f, 1, 1, 1, 10, -20, 30),
        transform(0.0f, 0.6f, 0.8f, -1.1f, 2.0f, 0.5f, 3.0f, 1e5f, 2e5f, -3e5f),
        transform(1, 0, 0, kPi, 1, 1, 1, 0, 0, 0),
        transform(0, 0, 1, kPi * 0.999f, 1, 1, 1, 0, 0, 0),
        transform(0.6f, 0.8f, 0.0f, 1.0f, -1.0f, 1.0f, 1.0f, 3, 4, 5),  // mirrored
    };
    for (const Float3x4& m : cases) {
        render::Trs trs;
        check(render::decompose(m, &trs), "a scaled rotation is taken apart");
        const Float3x4 back = render::compose(trs);
        float scale = 0.0f;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                scale = std::fmax(scale, std::fabs(m.m[r][c]));
        float worst = 0.0f;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                worst = std::fmax(worst, std::fabs(back.m[r][c] - m.m[r][c]));
        check(worst <= kUnit * scale, "put together again it is the same matrix (1e-4 of its scale)");
        check(back.m[0][3] == m.m[0][3] && back.m[1][3] == m.m[1][3] && back.m[2][3] == m.m[2][3],
              "the position is kept exactly");
        const float q = trs.q[0] * trs.q[0] + trs.q[1] * trs.q[1] + trs.q[2] * trs.q[2] + trs.q[3] * trs.q[3];
        check(std::fabs(q - 1.0f) <= kUnit, "the rotation is a unit quaternion");
    }
    {
        Float3x4 sheared;
        sheared.m[0][1] = 0.5f;
        render::Trs trs;
        check(!render::decompose(sheared, &trs), "a sheared matrix is not a scaled rotation");
        Float3x4 flat;
        flat.m[1][1] = 0.0f;
        check(!render::decompose(flat, &trs), "an axis of no length is refused");
    }

    // Both ends are exact, value for value.
    for (const Float3x4& a : cases) {
        for (const Float3x4& b : cases) {
            check(same(render::interpolate_transform(a, b, 0.0f), a), "t = 0 is the first transform exactly");
            check(same(render::interpolate_transform(a, b, 1.0f), b), "t = 1 is the second transform exactly");
            check(same(render::interpolate_transform(a, b, -3.0f), a), "t below 0 is the first transform");
            check(same(render::interpolate_transform(a, b, 7.0f), b), "t above 1 is the second transform");
        }
    }
    check(same(render::interpolate_transform(cases[2], cases[2], 0.37f), cases[2]) ||
              max_difference(render::interpolate_transform(cases[2], cases[2], 0.37f), cases[2]) <= 30.0f * kUnit * 1e-2f,
          "between a transform and itself is that transform");

    // A quarter turn: half way is an eighth turn, and still a rotation.
    {
        const Float3x4 half = render::interpolate_transform(rotation_y(0.0f), rotation_y(kPi / 2.0f), 0.5f);
        check(max_difference(half, rotation_y(kPi / 4.0f)) <= kUnit, "half of a quarter turn is an eighth turn");
        check(shape_error(half, 1, 1, 1) <= kUnit, "... with unit, square axes (no scale, no shear)");
    }
    // A whole turn in 30 degree steps, each step halved: always the angle between, never the long way round.
    for (int step = 0; step < 12; ++step) {
        const float from = static_cast<float>(step) * kPi / 6.0f, to = static_cast<float>(step + 1) * kPi / 6.0f;
        const Float3x4 mid = render::interpolate_transform(transform(0.6f, 0.0f, 0.8f, from, 1, 1, 1, 0, 0, 0),
                                                           transform(0.6f, 0.0f, 0.8f, to, 1, 1, 1, 0, 0, 0), 0.5f);
        check(max_difference(mid, transform(0.6f, 0.0f, 0.8f, (from + to) / 2.0f, 1, 1, 1, 0, 0, 0)) <= kUnit,
              "a step of a whole turn about a tilted axis passes through its middle angle");
    }
    // The two quaternions of one rotation give one path.
    {
        render::Trs a, b;
        render::decompose(rotation_y(0.2f), &a);
        render::decompose(rotation_y(1.4f), &b);
        render::Trs negated = b;
        for (float& v : negated.q)
            v = -v;
        const Float3x4 one = render::compose(render::interpolate_trs(a, b, 0.3f));
        const Float3x4 other = render::compose(render::interpolate_trs(a, negated, 0.3f));
        check(max_difference(one, other) <= kUnit, "q and -q interpolate to the same transform");
        check(max_difference(one, rotation_y(0.2f + 0.3f * 1.2f)) <= kUnit, "... at a constant rate of turn");
    }
    // Nearly identical and nearly opposite rotations stay rotations.
    for (const float angle : {1e-5f, 1e-3f, kPi * 0.9999f, kPi}) {
        for (const float t : {0.1f, 0.5f, 0.9f}) {
            const Float3x4 m = render::interpolate_transform(rotation_y(0.0f), rotation_y(angle), t);
            bool finite = true;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    finite = finite && std::isfinite(m.m[r][c]);
            check(finite && shape_error(m, 1, 1, 1) <= kUnit, "a tiny or a half turn stays a finite rotation");
        }
    }
    // Scale moves in a straight line and the axes stay square; a mirror stays a mirror all the way.
    {
        const Float3x4 a = transform(0, 1, 0, 0.2f, 1, 1, 1, 0, 0, 0), b = transform(0, 1, 0, 1.0f, 3, 2, 1, 8, 0, -4);
        const Float3x4 mid = render::interpolate_transform(a, b, 0.5f);
        check(shape_error(mid, 2.0f, 1.5f, 1.0f) <= kUnit, "scales are half way and the axes square");
        check(std::fabs(mid.m[0][3] - 4.0f) <= kUnit && std::fabs(mid.m[2][3] + 2.0f) <= kUnit, "the position is half way");
        const Float3x4 ma = transform(0, 1, 0, 0.2f, -1, 1, 1, 0, 0, 0), mb = transform(0, 1, 0, 1.0f, -1, 1, 1, 0, 0, 0);
        for (const float t : {0.25f, 0.5f, 0.75f})
            check(determinant(render::interpolate_transform(ma, mb, t)) < -0.99f, "a mirrored object stays mirrored");
        // Mirrored against not mirrored: no rotation leads from one to the other; element by element, ends exact.
        check(same(render::interpolate_transform(a, ma, 1.0f), ma), "a mirror appearing still ends exactly");
    }
    // Far from the origin and tiny steps.
    {
        const Float3x4 a = transform(0, 1, 0, 0.0f, 1, 1, 1, 250000.0f, -4000.0f, 1.0e6f);
        Float3x4 b = a;
        b.m[0][3] += 3.0f;
        b.m[2][3] -= 1.0f;
        const Float3x4 mid = render::interpolate_transform(a, b, 0.5f);
        check(std::fabs(mid.m[0][3] - 250001.5f) <= 0.05f && std::fabs(mid.m[2][3] - 999999.5f) <= 0.1f,
              "a small step far from the origin is halved to the float's own precision");
        check(mid.m[0][3] >= a.m[0][3] && mid.m[0][3] <= b.m[0][3], "... and lies between the ends");
    }
    // The test's own measure must see a wrong method: mixing the matrices element by element over a quarter turn
    // shrinks the axes (1 - cos 45 = 29 %), which shape_error reports and the interpolator does not do.
    {
        const Float3x4 a = rotation_y(0.0f), b = rotation_y(kPi / 2.0f);
        Float3x4 naive;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                naive.m[r][c] = 0.5f * (a.m[r][c] + b.m[r][c]);
        check(shape_error(naive, 1, 1, 1) > 0.25f, "[control] the shape measure catches an element-wise blend");
    }
}

// ---- packets --------------------------------------------------------------------------------------------------

constexpr std::uint64_t kRigid = 0x80A00010u, kSkinned = 0x80A00020u, kNewcomer = 0x80A00030u,
                        kReused = 0x80A00040u, kPlaced = 0x80A00050u, kTwice = 0x80A00060u, kStill = 0x80A00070u;

struct Scene {
    float camera_x = 0.0f;       // the eye
    float camera_turn = 0.0f;    // about y
    float rigid_x = 0.0f;
    float rigid_turn = 0.0f;
    float bone_x = 0.0f;
    float bone_turn = 0.0f;
    float placed_x = 0.0f;
    float dissolve = 0.0f;
    float morph = 0.0f;
    std::uint32_t reused_mesh = 7;
    bool newcomer = false;
    std::uint32_t twice = 1;
    float land_x = 0.0f;         // a draw with no key
};

FramePacket build(const Scene& s, std::uint64_t tick) {
    render::FrameBuilder b;
    b.begin_frame(tick, render::Float4{0, 0, 0, 1});
    render::Camera camera;
    camera.frame = transform(0, 1, 0, s.camera_turn, 1, 1, 1, s.camera_x, 5, -20);
    const render::ViewId view = b.view(camera, render::ViewRect{});
    render::Clear clear;
    b.begin_pass(view, render::PassKind::Scene, clear, &camera);
    const render::Material material;
    const render::Recipe recipe;
    b.set_key(0);
    b.draw(render::MeshHandle{1}, 0, material, transform(0, 1, 0, 0, 1, 1, 1, s.land_x, 0, 0), recipe);  // draw 0
    b.set_key(kRigid);
    render::Material fading = material;
    fading.dissolve = s.dissolve;
    b.draw(render::MeshHandle{2}, 0, fading, transform(0, 1, 0, s.rigid_turn, 1, 1, 1, s.rigid_x, 0, 0), recipe);  // 1
    b.set_key(kSkinned);
    const Float3x4 palette[2] = {transform(0, 1, 0, 0, 1, 1, 1, s.bone_x, 0, 0),
                                 transform(0, 0, 1, s.bone_turn, 1, 1, 1, s.bone_x, 2, 0)};
    const render::BoneRange bones = b.bones(palette, 2);
    const float weights[2] = {s.morph, 0.25f};
    const render::MorphRange morph = b.morph(weights, 2);
    b.draw(render::MeshHandle{3}, 0, material, Float3x4{}, recipe, render::kNoLights, bones, morph);  // 2
    b.draw(render::MeshHandle{3}, 1, material, Float3x4{}, recipe, render::kNoLights, bones, morph);  // 3
    b.set_key(kReused);
    b.draw(render::MeshHandle{s.reused_mesh}, 0, material, transform(0, 1, 0, 0, 1, 1, 1, s.rigid_x, 9, 0), recipe);  // 4
    b.set_key(kPlaced);
    b.draw(render::MeshHandle{8}, 0, material, transform(0, 1, 0, 0, 1, 1, 1, s.placed_x, 0, 0), recipe);  // 5
    b.set_key(kStill);
    b.draw(render::MeshHandle{9}, 0, material, transform(0, 1, 0, 0.5f, 1, 1, 1, 4, 4, 4), recipe);  // 6
    b.set_key(kTwice);
    for (std::uint32_t i = 0; i < s.twice; ++i)
        b.draw(render::MeshHandle{10}, 0, material, transform(0, 1, 0, 0, 1, 1, 1, s.rigid_x + 10.0f * i, 0, 7), recipe);
    if (s.newcomer) {
        b.set_key(kNewcomer);
        b.draw(render::MeshHandle{11}, 0, material, transform(0, 1, 0, 0, 1, 1, 1, s.rigid_x, 0, 9), recipe);
    }
    // Immediate geometry and particles carry no followed identity.
    const render::ImmediateVertex vertices[3] = {};
    b.immediate(render::ImmediateDraw{}, vertices, 3, nullptr, 0, transform(0, 1, 0, 0, 1, 1, 1, s.land_x, 0, 0),
                recipe);
    const render::Particle particles[2] = {};
    b.particles(render::ParticleBatch{}, particles, 2, recipe);
    b.end_pass();
    return b.finish();
}

bool same_packet(const FramePacket& a, const FramePacket& b) {
    if (a.tick != b.tick || a.draws.size() != b.draws.size() || a.bones.size() != b.bones.size() ||
        a.views.size() != b.views.size() || a.passes.size() != b.passes.size() ||
        a.morph_weights != b.morph_weights || a.particles.size() != b.particles.size() ||
        a.immediate_vertices.size() != b.immediate_vertices.size())
        return false;
    for (std::size_t i = 0; i < a.draws.size(); ++i) {
        const render::DrawRecord& x = a.draws[i];
        const render::DrawRecord& y = b.draws[i];
        if (!same(x.world, y.world) || x.mesh.value != y.mesh.value || x.submesh != y.submesh || x.key != y.key ||
            x.material.dissolve != y.material.dissolve || x.bones.first != y.bones.first ||
            x.bones.count != y.bones.count)
            return false;
    }
    for (std::size_t i = 0; i < a.bones.size(); ++i)
        if (!same(a.bones[i], b.bones[i]))
            return false;
    for (std::size_t i = 0; i < a.views.size(); ++i)
        if (!same(a.views[i].camera.frame, b.views[i].camera.frame))
            return false;
    for (std::size_t i = 0; i < a.passes.size(); ++i)
        if (!same(a.passes[i].camera.frame, b.passes[i].camera.frame))
            return false;
    return true;
}

void packet_tests() {
    Scene sa, sb;
    sb.camera_x = 2.0f;
    sb.camera_turn = 0.2f;
    sb.rigid_x = 4.0f;
    sb.rigid_turn = 0.8f;
    sb.bone_x = 1.0f;
    sb.bone_turn = 0.6f;
    sb.dissolve = 0.5f;
    sa.dissolve = 1.0f;
    sb.morph = 0.8f;
    sb.reused_mesh = 12;   // the address names another model in B
    sb.newcomer = true;    // only in B
    sb.placed_x = 5000.0f; // moved further than anything moves in a frame
    sb.land_x = 3.0f;      // no key: B's
    const FramePacket a = build(sa, 100), b = build(sb, 101);
    const FramePacket a_before = a, b_before = b;

    render::PacketInterpolator interpolator;
    interpolator.pair(&a, &b);
    const render::InterpStats stats = interpolator.stats();
    check(!stats.cut, "a moving camera is not a cut");
    check(stats.cameras == 2u, "the view's and the pass's camera are followed");
    check(stats.bones == 2u, "the shared palette's two bones are followed once");
    // keyed: rigid, skinned x2, reused, placed, still, twice, newcomer = 8
    check(stats.keyed == 8u, "eight of B's draws carry a key");
    check(stats.followed == 4u, "rigid, the two skinned submeshes and the repeated one are followed");
    check(stats.still == 1u, "the object that did not move is counted as still");
    check(stats.snapped == 3u, "the newcomer, the reused address and the placed object show B");

    FramePacket out;
    interpolator.build(1.0f, &out);
    check(same_packet(out, b), "alpha 1 is B, value for value");
    interpolator.build(2.5f, &out);
    check(same_packet(out, b), "alpha above 1 is B");

    interpolator.build(0.0f, &out);
    check(out.draws.size() == b.draws.size(), "the draw list is always B's");
    check(same(out.draws[1].world, a.draws[1].world), "alpha 0: a followed object is where A had it");
    check(same(out.bones[0], a.bones[0]) && same(out.bones[1], a.bones[1]), "alpha 0: the palette is A's");
    check(same(out.views[0].camera.frame, a.views[0].camera.frame), "alpha 0: the view's camera is A's");
    check(same(out.passes[0].camera.frame, a.passes[0].camera.frame), "alpha 0: the pass's camera is A's");
    check(out.draws[1].material.dissolve == 1.0f, "alpha 0: the fade-in is A's");
    check(out.morph_weights[0] == 0.0f && out.morph_weights[1] == 0.25f, "alpha 0: the morph weights are A's");
    check(same(out.draws[0].world, b.draws[0].world), "a draw without a key is B's from the start");
    check(same(out.draws[4].world, b.draws[4].world), "an address reused by another mesh is B's");
    check(same(out.draws[5].world, b.draws[5].world), "an object placed far away is B's (no slide)");
    check(same(out.draws.back().world, b.draws.back().world) && out.particles.size() == b.particles.size(),
          "particles and immediate geometry are B's");

    interpolator.build(0.5f, &out);
    check(max_difference(out.draws[1].world, transform(0, 1, 0, 0.4f, 1, 1, 1, 2.0f, 0, 0)) <= kUnit,
          "alpha 0.5: half the turn and half the way");
    check(max_difference(out.bones[1], transform(0, 0, 1, 0.3f, 1, 1, 1, 0.5f, 2, 0)) <= kUnit,
          "alpha 0.5: a bone half way");
    check(max_difference(out.views[0].camera.frame, transform(0, 1, 0, 0.1f, 1, 1, 1, 1.0f, 5, -20)) <= kUnit,
          "alpha 0.5: the camera half way");
    check(std::fabs(out.draws[1].material.dissolve - 0.75f) <= kUnit, "alpha 0.5: the fade-in half way");
    check(std::fabs(out.morph_weights[0] - 0.4f) <= kUnit, "alpha 0.5: the morph weight half way");
    check(same(out.draws[2].world, b.draws[2].world) && same(out.draws[3].world, b.draws[3].world),
          "a skinned draw's own world matrix is untouched");
    check(same(out.draws[6].world, b.draws[6].world), "a still object is exactly B's (and A's)");

    // What the game made is never written.
    check(same_packet(a, a_before) && same_packet(b, b_before), "building a picture changes neither packet");
    check(a.draws[1].material.dissolve == 1.0f && b.morph_weights[0] == 0.8f, "... nor their scalars");

    // Monotonic along the way: the followed object's x never goes back.
    float last = -1.0f;
    bool monotonic = true;
    for (int i = 0; i <= 480; ++i) {
        interpolator.build(static_cast<float>(i) / 480.0f, &out);
        monotonic = monotonic && out.draws[1].world.m[0][3] >= last;
        last = out.draws[1].world.m[0][3];
    }
    check(monotonic && last == b.draws[1].world.m[0][3], "480 pictures of one pair move one way and end on B");

    // An object drawn another number of times: no pairing can be trusted.
    {
        Scene one = sa, two = sb;
        two.twice = 2;
        const FramePacket pa = build(one, 1), pb = build(two, 2);
        render::PacketInterpolator again;
        again.pair(&pa, &pb);
        again.build(0.0f, &out);
        check(same(out.draws[7].world, pb.draws[7].world) && same(out.draws[8].world, pb.draws[8].world),
              "an object drawn once in A and twice in B is B's both times");
    }
    // A cut: the camera jumps, or turns around. The whole pair is B.
    {
        Scene jumped = sb;
        jumped.camera_x = 4000.0f;
        const FramePacket pb = build(jumped, 2);
        render::PacketInterpolator cut;
        cut.pair(&a, &pb);
        check(cut.stats().cut, "a camera that jumps is a cut");
        cut.build(0.25f, &out);
        check(same_packet(out, pb), "every picture of a cut is B (nothing slides through the level)");
        Scene turned = sb;
        turned.camera_turn = 2.5f;
        const FramePacket pt = build(turned, 2);
        cut.pair(&a, &pt);
        check(cut.stats().cut, "a camera that turns around in one frame is a cut");
        cut.build(0.25f, &out);
        check(same_packet(out, pt), "... and its pictures are B");
    }
    // No partner packet.
    {
        render::PacketInterpolator alone;
        alone.pair(nullptr, &b);
        alone.build(0.2f, &out);
        check(same_packet(out, b), "without an earlier packet the picture is B");
        alone.clear();
        alone.build(0.2f, &out);
        check(out.draws.empty() && out.views.empty(), "without any packet there is nothing");
    }
    // The limits are honoured exactly at their edge.
    {
        render::InterpLimits limits;
        limits.object_step = 4.5f;  // the rigid object moves 4
        render::PacketInterpolator tight;
        tight.set_limits(limits);
        tight.pair(&a, &b);
        tight.build(0.0f, &out);
        check(same(out.draws[1].world, a.draws[1].world), "a step under the limit is followed");
        limits.object_step = 3.5f;
        tight.set_limits(limits);
        tight.pair(&a, &b);
        tight.build(0.0f, &out);
        check(same(out.draws[1].world, b.draws[1].world), "a step over the limit is not");
    }
    // [control] The comparison used above must see a single wrong value.
    {
        FramePacket wrong = b;
        wrong.bones[1].m[2][3] += 1e-3f;
        check(!same_packet(wrong, b), "[control] the packet comparison catches one changed bone element");
        wrong = b;
        wrong.passes[0].camera.frame.m[0][0] += 1e-6f;
        check(!same_packet(wrong, b), "[control] ... and one changed camera element");
    }
}

// ---- the timeline: synthetic clocks only -----------------------------------------------------------------------

constexpr std::uint64_t kFrameTicks = 675000;  // 1/60 s of game time
constexpr double kFrameNs = 1e9 / 60.0;

// A small deterministic generator for the hand-over jitter.
struct Jitter {
    std::uint32_t state = 12345u;
    double next(double range_ns) {
        state = state * 1664525u + 1013904223u;
        return (static_cast<double>(state >> 8) / 16777216.0 - 0.5) * 2.0 * range_ns;
    }
};

// Renders at `render_hz` (or, 0, at a period that varies between 2 and 12 ms) against a 60 Hz game for `frames` game
// frames. Returns false when a picture's position ever leaves [0, 1], goes back in game time, or strays from "the
// game one frame ago" by more than `tolerance_ns`.
bool run_rate(double render_hz, int frames, double jitter_ns, double tolerance_ns, std::uint64_t* pictures,
              std::uint64_t* between) {
    render::FrameTimeline timeline;
    Jitter jitter, vary;
    const std::int64_t base = 5'000'000'000ll;
    std::uint64_t frame = 0;
    double next_push = static_cast<double>(base);
    double now = static_cast<double>(base) + 0.37e6;
    double last_shown = -1e300;
    bool ok = true;
    std::uint64_t prev_ticks = 0, cur_ticks = 0;
    while (static_cast<int>(frame) < frames) {
        // Everything the game handed over up to `now`.
        while (next_push <= now && static_cast<int>(frame) < frames) {
            ++frame;
            prev_ticks = cur_ticks;
            cur_ticks = frame * kFrameTicks;
            timeline.arrive(frame, cur_ticks, static_cast<std::int64_t>(next_push + jitter.next(jitter_ns)));
            next_push = static_cast<double>(base) + static_cast<double>(frame) * kFrameNs;
        }
        if (frame >= 2) {
            const float alpha = timeline.alpha(static_cast<std::int64_t>(now));
            ok = ok && alpha >= 0.0f && alpha <= 1.0f;
            // The game time shown, in ns of game time.
            const double shown = (static_cast<double>(prev_ticks) +
                                  static_cast<double>(alpha) * static_cast<double>(cur_ticks - prev_ticks)) *
                                 render::FrameTimeline::kNsPerTick;
            ok = ok && shown >= last_shown - 1.0;  // never back (1 ns of float rounding)
            last_shown = shown;
            // Wanted: the game as it was one frame before now. Game time g is handed over at base + g - one frame
            // (frame k, game time k frames, is pushed at base + (k - 1) frames).
            const double wanted = now - static_cast<double>(base) + kFrameNs - kFrameNs;
            if (alpha > 0.0f && alpha < 1.0f) {
                ok = ok && std::fabs(shown - wanted) <= tolerance_ns;
                ++*between;
            }
            ++*pictures;
        }
        now += render_hz > 0.0 ? 1e9 / render_hz : 7.0e6 + vary.next(5.0e6);
    }
    return ok;
}

void timeline_tests() {
    // Every rate of the matrix, with the hand-over jitter measured on this machine (sd 0.05-0.24 ms; 0.3 ms here).
    const double rates[] = {30, 45, 60, 75, 90, 120, 144, 165, 180, 240, 360, 480, 1000, 5000, 0};
    for (const double rate : rates) {
        std::uint64_t pictures = 0, between = 0;
        const bool ok = run_rate(rate, 1200, 0.3e6, 0.35e6, &pictures, &between);
        char what[160];
        std::snprintf(what, sizeof(what),
                      "%g Hz: every picture lies between its two frames, never goes back, and shows the game one "
                      "frame ago within 0.35 ms",
                      rate);
        check(ok, what);
        if (rate >= 120.0)
            check(between * 3 >= pictures, "above the game's rate most pictures are intermediate states");
    }
    // Without jitter the position is exact: at 240 Hz the four pictures of a pair are a quarter apart.
    {
        render::FrameTimeline timeline;
        timeline.arrive(1, kFrameTicks, 1'000'000'000ll);
        timeline.arrive(2, 2 * kFrameTicks, 1'000'000'000ll + 16'666'667ll);
        check(timeline.continuous(), "two frames one period apart are one motion");
        check(std::fabs(timeline.period_ns() - kFrameNs) < 1.0, "the period is the game's 1/60 s");
        const std::int64_t at = 1'000'000'000ll + 16'666'667ll;
        for (int i = 0; i < 4; ++i) {
            const float alpha = timeline.alpha(at + static_cast<std::int64_t>(i * kFrameNs / 4.0));
            check(std::fabs(alpha - 0.25f * static_cast<float>(i)) <= 2e-3f, "240 Hz: alpha 0, 1/4, 1/2, 3/4");
        }
        check(timeline.alpha(at + 16'666'667ll) == 1.0f, "when the next frame is due the picture is B");
        check(timeline.alpha(at + 500'000'000ll) == 1.0f, "... and stays B however long nothing arrives");
        check(timeline.alpha(at - 50'000'000ll) == 0.0f, "before A's time the picture is A");
    }
    // A frame the renderer skipped: the pair spans two periods and a picture started when B arrives is half way.
    {
        render::FrameTimeline timeline;
        timeline.arrive(1, kFrameTicks, 0);
        timeline.arrive(3, 3 * kFrameTicks, 33'333'333ll);
        check(timeline.continuous(), "one skipped frame is still one motion");
        check(std::fabs(timeline.alpha(33'333'333ll) - 0.5f) <= 2e-3f, "... shown from its middle on, not replayed");
    }
    // 50 Hz screens: the period follows the game.
    {
        render::FrameTimeline timeline;
        timeline.arrive(1, 810000, 0);
        timeline.arrive(2, 2 * 810000, 20'000'000ll);
        check(timeline.continuous() && std::fabs(timeline.period_ns() - 20e6) < 1.0, "a 50 Hz frame is 20 ms");
    }
    // Discontinuities: a stall, a long gap in frames, time not moving, the first frame.
    {
        render::FrameTimeline timeline;
        timeline.arrive(1, kFrameTicks, 0);
        check(!timeline.continuous() && timeline.alpha(5'000'000ll) == 1.0f, "a first frame has nothing before it");
        timeline.arrive(2, 2 * kFrameTicks, 16'666'667ll);
        timeline.arrive(3, 3 * kFrameTicks, 33'333'333ll + 60'000'000ll);  // the host stalled 60 ms
        check(!timeline.continuous() && timeline.resyncs() == 1u, "a stalled hand-over starts the mapping again");
        check(timeline.alpha(33'333'333ll + 61'000'000ll) == 1.0f, "... and its pair is shown as B");
        timeline.arrive(4, 4 * kFrameTicks, 50'000'000ll + 60'000'000ll);
        check(timeline.continuous(), "the frame after the stall is one motion again");
        timeline.arrive(40, 40 * kFrameTicks, 110'000'000ll + 36 * 16'666'667ll);
        check(!timeline.continuous(), "36 frames apart is not one motion");
        timeline.arrive(41, 40 * kFrameTicks, 110'000'000ll + 37 * 16'666'667ll);
        check(!timeline.continuous(), "game time that did not move is not a step");
        timeline.reset();
        check(!timeline.continuous() && timeline.resyncs() == 0u, "reset forgets everything");
    }
    // Four hours of game time at 60 Hz (864,000 frames), jittered: the mapping neither drifts nor resyncs, and the
    // last pair is still placed to the same precision. Synthetic time only - not a run of the game or the GPU.
    {
        render::FrameTimeline timeline;
        Jitter jitter;
        const std::int64_t base = 777'000'000'000ll;
        const std::uint64_t frames = 864000;
        double worst = 0.0;
        for (std::uint64_t k = 1; k <= frames; ++k) {
            const double ideal = static_cast<double>(base) + static_cast<double>(k) * kFrameNs;
            timeline.arrive(k, k * kFrameTicks, static_cast<std::int64_t>(ideal + jitter.next(0.3e6)));
            worst = std::fmax(worst, std::fabs(timeline.wall_ns(k * kFrameTicks) - ideal));
        }
        check(timeline.resyncs() == 0u, "four hours of jittered frames never resync");
        check(worst <= 0.31e6, "the mapped time stays within the jitter of the ideal grid for four hours (no drift)");
        check(std::fabs(timeline.period_ns() - kFrameNs) < 1.0, "the period after four hours is still 1/60 s");
    }
    // [control] A timeline fed the wrong period must fail the rate check: the same run against a tolerance that a
    // 1 % error in alpha exceeds at 240 Hz (166 us) passes, so the check has the resolution to see such an error.
    {
        std::uint64_t pictures = 0, between = 0;
        check(run_rate(240, 600, 0.0, 0.02e6, &pictures, &between),
              "[control] without jitter every picture is within 20 us of the game one frame ago");
    }
}

// ---- particles ------------------------------------------------------------------------------------------------

// One emitter's batch per entry of `batches`: its key, texture and the particles with their tags.
struct Spark {
    std::uint32_t id;
    float age;
    float x, y, z;
    float size;
    float rotation;
    std::uint32_t colour;
    float u;  // the texture corner (a flip-book's cell)
};
struct Emitter {
    std::uint64_t key;
    std::uint32_t texture;
    std::vector<Spark> sparks;
    bool tagged = true;
    float right_x = 1.0f, right_z = 0.0f;  // the batch's right axis (in the ground plane)
    float constant = 1.0f;                 // the batch's constant colour (red)
};

FramePacket build_particles(const std::vector<Emitter>& emitters, std::uint64_t tick) {
    render::FrameBuilder b;
    b.begin_frame(tick, render::Float4{0, 0, 0, 1});
    render::Camera camera;
    const render::ViewId view = b.view(camera, render::ViewRect{});
    render::Clear clear;
    b.begin_pass(view, render::PassKind::Scene, clear, &camera);
    const render::Recipe recipe;
    for (const Emitter& e : emitters) {
        std::vector<render::Particle> records;
        std::vector<render::ParticleTag> tags;
        for (const Spark& s : e.sparks) {
            render::Particle p;
            p.position = render::Float3{s.x, s.y, s.z};
            p.size[0] = p.size[1] = s.size;
            p.rotation = s.rotation;
            p.colour = s.colour;
            p.uv0[0] = s.u;
            records.push_back(p);
            tags.push_back(render::ParticleTag{s.id, s.age});
        }
        render::ParticleBatch batch;
        batch.texture = render::TextureHandle{e.texture};
        batch.right = render::Float3{e.right_x, 0.0f, e.right_z};
        batch.colour = render::Float4{e.constant, 1, 1, 1};
        b.set_key(e.key);
        b.particles(batch, records.data(), static_cast<std::uint32_t>(records.size()), recipe,
                    e.tagged ? tags.data() : nullptr);
    }
    b.end_pass();
    return b.finish();
}

bool same_record(const render::Particle& a, const render::Particle& b) {
    return std::memcmp(&a, &b, sizeof(render::Particle)) == 0;
}

void particle_tests() {
    constexpr std::uint32_t kWhite = 0xFFFFFFFFu;
    // Emitter 1 (key 0x100): four sparks in A; in B one has died (id 12), the rest moved and are listed in another
    // order, one is new (id 15), one sits at a reused place (id 13: its age went back) and one was placed far away
    // (id 14). Emitter 2 (key 0x200) carries no tags. Emitter 3 (key 0x300) is only in B.
    const std::vector<Emitter> in_a = {
        {0x100, 5, {{11, 0.10f, 0, 0, 0, 2.0f, 3.0f, 0x80402010u, 0.00f},
                    {12, 0.20f, 5, 0, 0, 2.0f, 0.0f, kWhite, 0.00f},
                    {13, 0.90f, 9, 0, 0, 2.0f, 0.0f, kWhite, 0.00f},
                    {14, 0.30f, 1, 1, 1, 2.0f, 0.0f, kWhite, 0.00f},
                    {16, 0.40f, 7, 7, 7, 1.0f, 0.5f, kWhite, 0.25f}}},
        {0x200, 6, {{21, 0.1f, 0, 0, 0, 1.0f, 0.0f, kWhite, 0.0f}}, false},
    };
    const std::vector<Emitter> in_b = {
        {0x100, 5, {{15, 0.00f, 50, 0, 0, 1.0f, 0.0f, kWhite, 0.00f},                // born
                    {14, 0.32f, 1, 1, 900, 2.0f, 0.0f, kWhite, 0.00f},                // placed far away
                    {13, 0.02f, 9, 4, 0, 2.0f, 0.0f, kWhite, 0.00f},                  // a reused place
                    {11, 0.12f, 4, 0, 8, 4.0f, -3.0f, 0x00C06030u, 0.50f},            // followed
                    {16, 0.42f, 7, 7, 7, 1.0f, 0.5f, kWhite, 0.50f}},                 // still (only its cell stepped)
         true, 0.0f, 1.0f, 0.5f},
        {0x200, 6, {{21, 0.12f, 3, 0, 0, 1.0f, 0.0f, kWhite, 0.0f}}, false},
        {0x300, 7, {{31, 0.0f, 0, 0, 0, 1.0f, 0.0f, kWhite, 0.0f}}},
    };
    const FramePacket a = build_particles(in_a, 10), b = build_particles(in_b, 11);
    const FramePacket a_before = a, b_before = b;
    check(a.particle_tags.size() == a.particles.size() && b.particle_tags.size() == b.particles.size(),
          "a packet carries one tag per particle");
    check(a.particle_tags[5].id == 0u, "a batch without tags carries zero tags");

    render::PacketInterpolator interpolator;
    interpolator.pair(&a, &b);
    const render::InterpStats s = interpolator.stats();
    check(s.particles == 7u && s.particles_tagged == 6u, "B has seven particles, six of them tagged");
    check(s.particles_followed == 1u, "one particle is followed");
    check(s.particles_still == 1u, "one particle did not move");
    check(s.particles_born == 2u, "the new particle and the new emitter's are born");
    check(s.particles_reused == 1u, "an id whose age went back is another particle");
    check(s.particles_placed == 1u, "a particle moved past the limit is placed");
    check(s.particle_batches == 1u, "the batch whose axes and colour changed is followed");
    check(std::fabs(s.largest_particle_step - 899.0f) < 1e-3f, "the largest step seen is the placed particle's");

    FramePacket out;
    interpolator.build(1.0f, &out);
    bool equal = out.particles.size() == b.particles.size();
    for (std::size_t i = 0; equal && i < b.particles.size(); ++i)
        equal = same_record(out.particles[i], b.particles[i]);
    check(equal && out.particle_batches[0].right.z == 1.0f && out.particle_batches[0].colour.x == 0.5f,
          "alpha 1: every particle and batch is B's, value for value");

    interpolator.build(0.0f, &out);
    const render::Particle& at0 = out.particles[3];
    check(at0.position.x == 0.0f && at0.position.z == 0.0f && at0.size[0] == 2.0f && at0.rotation == 3.0f &&
              at0.colour == 0x80402010u,
          "alpha 0: a followed particle is A's");
    check(at0.uv0[0] == 0.5f, "... but its texture cell is B's (a flip-book steps)");
    check(out.particle_batches[0].right.x == 1.0f && out.particle_batches[0].right.z == 0.0f &&
              out.particle_batches[0].colour.x == 1.0f,
          "alpha 0: the batch's axes and constant colour are A's");
    check(same_record(out.particles[0], b.particles[0]), "a born particle is B's from the start");
    check(same_record(out.particles[1], b.particles[1]), "a placed particle is B's (no streak across the level)");
    check(same_record(out.particles[2], b.particles[2]), "a reused id is B's");
    check(same_record(out.particles[4], b.particles[4]), "a still particle is B's (and A's)");
    check(same_record(out.particles[5], b.particles[5]), "an untagged particle is B's");
    check(same_record(out.particles[6], b.particles[6]), "a new emitter's particle is B's");

    interpolator.build(0.5f, &out);
    const render::Particle& half = out.particles[3];
    check(std::fabs(half.position.x - 2.0f) <= kUnit && std::fabs(half.position.z - 4.0f) <= kUnit,
          "alpha 0.5: half the way");
    check(std::fabs(half.size[0] - 3.0f) <= kUnit, "alpha 0.5: half the growth");
    // 3.0 -> -3.0 through pi: the short way is +0.283, not -6.
    check(std::fabs(half.rotation - (3.0f + (2.0f * kPi - 6.0f) * 0.5f)) <= kUnit,
          "alpha 0.5: the rotation goes the short way through pi");
    check(half.colour == 0x40804020u, "alpha 0.5: each colour channel half way");
    check(half.uv0[0] == 0.5f, "alpha 0.5: the texture cell is B's, not blended");
    const render::Float3 right = out.particle_batches[0].right;
    check(std::fabs(right.x - right.z) <= kUnit &&
              std::fabs(std::sqrt(right.x * right.x + right.z * right.z) - 1.0f) <= kUnit,
          "alpha 0.5: the axis has turned half way and kept its length");
    check(std::fabs(out.particle_batches[0].colour.x - 0.75f) <= kUnit, "alpha 0.5: the constant colour half way");

    bool unchanged = a.particles.size() == a_before.particles.size();
    for (std::size_t i = 0; unchanged && i < a.particles.size(); ++i)
        unchanged = same_record(a.particles[i], a_before.particles[i]);
    for (std::size_t i = 0; unchanged && i < b.particles.size(); ++i)
        unchanged = same_record(b.particles[i], b_before.particles[i]);
    check(unchanged, "building a picture changes neither packet's particles");

    // Monotonic along the way.
    float last = -1.0f;
    bool monotonic = true;
    for (int i = 0; i <= 240; ++i) {
        interpolator.build(static_cast<float>(i) / 240.0f, &out);
        monotonic = monotonic && out.particles[3].position.z >= last;
        last = out.particles[3].position.z;
    }
    check(monotonic && last == 8.0f, "240 pictures of one pair move a particle one way and end on B");

    // The limit at its edge (the followed particle steps sqrt(16 + 64) = 8.94).
    {
        render::InterpLimits limits;
        limits.particle_step = 9.0f;
        render::PacketInterpolator tight;
        tight.set_limits(limits);
        tight.pair(&a, &b);
        check(tight.stats().particles_followed == 1u, "a particle's step under the limit is followed");
        limits.particle_step = 8.5f;
        tight.set_limits(limits);
        tight.pair(&a, &b);
        check(tight.stats().particles_followed == 0u && tight.stats().particles_placed == 2u,
              "a particle's step over the limit is not");
    }
    // A pair that spans a frame the renderer never took: twice the step is still motion.
    {
        const FramePacket later = build_particles(in_b, 12);
        render::InterpLimits limits;
        limits.particle_step = 5.0f;  // 8.94 over two frames
        render::PacketInterpolator skipped;
        skipped.set_limits(limits);
        skipped.pair(&a, &later);
        check(skipped.stats().particles_followed == 1u, "over a skipped frame the limit is two steps");
        skipped.pair(&a, &b);
        check(skipped.stats().particles_followed == 0u, "... and one step between neighbouring frames");
    }
    // An id that aged forward, but not like the others: a particle born into a place whose owner died young.
    {
        std::vector<Emitter> odd = in_b;
        odd[0].sparks[3].age = 0.10f + 0.31f;  // id 11: the others aged 0.02
        const FramePacket pb = build_particles(odd, 11);
        render::PacketInterpolator again;
        again.pair(&a, &pb);
        check(again.stats().particles_followed == 0u && again.stats().particles_reused == 2u,
              "an id that aged unlike its batch is another particle");
    }
    // Another texture under the same key: the address names another emitter.
    {
        std::vector<Emitter> other = in_b;
        other[0].texture = 9;
        const FramePacket pb = build_particles(other, 11);
        render::PacketInterpolator again;
        again.pair(&a, &pb);
        check(again.stats().particles_followed == 0u && again.stats().particles_born == 6u,
              "a batch whose texture changed has no partner: all its particles are born");
    }
    // The emitter drawn twice in B and once in A: no pairing can be trusted.
    {
        std::vector<Emitter> twice = in_b;
        twice.push_back(in_b[0]);
        const FramePacket pb = build_particles(twice, 11);
        render::PacketInterpolator again;
        again.pair(&a, &pb);
        check(again.stats().particles_followed == 0u, "a batch drawn another number of times is B's");
    }
    // An id twice in A's batch names nothing.
    {
        std::vector<Emitter> doubled = in_a;
        doubled[0].sparks.push_back(doubled[0].sparks[0]);
        const FramePacket pa = build_particles(doubled, 10);
        render::PacketInterpolator again;
        again.pair(&pa, &b);
        check(again.stats().particles_followed == 0u, "an id A holds twice is not followed");
    }
    // A camera cut shows B's particles too.
    {
        FramePacket cut_b = b;
        cut_b.views[0].camera.frame.m[0][3] = 5000.0f;
        cut_b.passes[0].camera.frame.m[0][3] = 5000.0f;
        render::PacketInterpolator cut;
        cut.pair(&a, &cut_b);
        cut.build(0.25f, &out);
        check(cut.stats().cut && same_record(out.particles[3], b.particles[3]), "on a cut the particles are B's");
    }
    // [control] The record comparison sees one changed byte of colour.
    {
        render::Particle wrong = b.particles[3];
        wrong.colour ^= 0x00010000u;
        check(!same_record(wrong, b.particles[3]), "[control] the particle comparison catches one colour step");
    }
}

// ---- world-space immediate geometry (trails) -------------------------------------------------------------------

struct Ribbon {
    std::uint64_t key;
    std::vector<render::ImmediateVertex> vertices;
    float world_x = 0.0f;
    bool canvas = false;
    std::uint32_t texture = 3;
};

render::ImmediateVertex vertex(float x, float y, float z, std::uint32_t colour, float u = 0.0f) {
    render::ImmediateVertex v;
    v.position = render::Float3{x, y, z};
    v.colour = colour;
    v.uv[0] = u;
    return v;
}

FramePacket build_ribbons(const std::vector<Ribbon>& ribbons, std::uint64_t tick) {
    render::FrameBuilder b;
    b.begin_frame(tick, render::Float4{0, 0, 0, 1});
    render::Camera camera;
    const render::ViewId view = b.view(camera, render::ViewRect{});
    render::Clear clear;
    b.begin_pass(view, render::PassKind::Scene, clear, &camera);
    const render::Recipe recipe;
    for (const Ribbon& r : ribbons) {
        render::ImmediateDraw draw;
        draw.topology = render::Topology::TriangleStrip;
        draw.space = r.canvas ? render::Space::Canvas : render::Space::World;
        draw.texture = render::TextureHandle{r.texture};
        b.set_key(r.key);
        b.immediate(draw, r.vertices.data(), static_cast<std::uint32_t>(r.vertices.size()), nullptr, 0,
                    transform(0, 1, 0, 0, 1, 1, 1, r.world_x, 0, 0), recipe);
        b.set_key(0);
    }
    b.end_pass();
    return b.finish();
}

bool same_vertex(const render::ImmediateVertex& a, const render::ImmediateVertex& b) {
    return std::memcmp(&a, &b, sizeof(render::ImmediateVertex)) == 0;
}

void immediate_tests() {
    constexpr std::uint32_t kWhite = 0xFFFFFFFFu;
    // A trail of four samples: each game frame every sample takes the place of the one before it and a new head is
    // written, so vertex for vertex the ribbon moves one sample along its path. Its tail fades.
    const std::vector<render::ImmediateVertex> trail_a = {vertex(30, 0, 0, kWhite, 0.0f), vertex(20, 0, 0, kWhite, 0.3f),
                                                          vertex(10, 0, 0, kWhite, 0.6f),
                                                          vertex(0, 0, 0, 0x80FFFFFFu, 1.0f)};
    const std::vector<render::ImmediateVertex> trail_b = {vertex(40, 4, 0, kWhite, 0.0f), vertex(30, 0, 0, kWhite, 0.3f),
                                                          vertex(20, 0, 0, kWhite, 0.6f),
                                                          vertex(10, 0, 0, 0x40FFFFFFu, 1.0f)};
    const std::vector<render::ImmediateVertex> still = {vertex(1, 2, 3, kWhite), vertex(4, 5, 6, kWhite),
                                                        vertex(7, 8, 9, kWhite)};
    std::vector<render::ImmediateVertex> grown = trail_b;
    grown.push_back(vertex(0, 0, 0, kWhite));
    std::vector<render::ImmediateVertex> thrown = trail_b;
    thrown[2].position.z = 5000.0f;
    // key 0x10: the trail; 0x20: a beam that did not change; 0x30: a trail that grew a sample; 0x40: one whose
    // vertex was placed far away; 0x50: a canvas draw given a key; 0x60: a draw whose world matrix moved.
    const std::vector<Ribbon> in_a = {{0x10, trail_a}, {0x20, still}, {0x30, trail_a}, {0x40, trail_a},
                                      {0x50, trail_a, 0.0f, true}, {0x60, still, 0.0f}};
    const std::vector<Ribbon> in_b = {{0x10, trail_b}, {0x20, still}, {0x30, grown}, {0x40, thrown},
                                      {0x50, trail_b, 0.0f, true}, {0x60, still, 8.0f}};
    const FramePacket a = build_ribbons(in_a, 20), b = build_ribbons(in_b, 21);
    const FramePacket b_before = b;
    check(b.draws[0].key == 0x10u && b.draws[4].key == 0u, "a world draw takes the key, a canvas draw never does");

    render::PacketInterpolator interpolator;
    interpolator.pair(&a, &b);
    const render::InterpStats s = interpolator.stats();
    check(s.immediates == 5u, "five world draws carry a key");
    check(s.immediates_followed == 2u && s.immediate_vertices == 7u, "the trail and the moved draw are followed");
    check(s.immediates_still == 1u, "the unchanged beam is still");
    check(s.immediates_snapped == 2u, "a grown trail and a thrown vertex show B");
    check(std::fabs(s.largest_immediate_step - 5000.0f) < 1.0f, "the largest step seen is the thrown vertex's");

    FramePacket out;
    interpolator.build(1.0f, &out);
    bool equal = out.immediate_vertices.size() == b.immediate_vertices.size();
    for (std::size_t i = 0; equal && i < b.immediate_vertices.size(); ++i)
        equal = same_vertex(out.immediate_vertices[i], b.immediate_vertices[i]);
    check(equal && same(out.draws[5].world, b.draws[5].world), "alpha 1: every vertex and world matrix is B's");

    interpolator.build(0.0f, &out);
    const std::uint32_t first = b.immediates[0].first_vertex;
    check(out.immediate_vertices[first].position.x == 30.0f && out.immediate_vertices[first].position.y == 0.0f &&
              out.immediate_vertices[first + 3].colour == 0x80FFFFFFu,
          "alpha 0: the trail's vertices and colours are A's");
    check(out.immediate_vertices[first + 1].uv[0] == 0.3f, "... with B's texture coordinates");
    check(same(out.draws[5].world, a.draws[5].world), "alpha 0: the moved draw's world matrix is A's");

    interpolator.build(0.5f, &out);
    check(std::fabs(out.immediate_vertices[first].position.x - 35.0f) <= kUnit &&
              std::fabs(out.immediate_vertices[first].position.y - 2.0f) <= kUnit,
          "alpha 0.5: the head is half way to its new place");
    check(std::fabs(out.immediate_vertices[first + 2].position.x - 15.0f) <= kUnit,
          "alpha 0.5: a sample is half way to the place of the one before it");
    check(out.immediate_vertices[first + 3].colour == 0x60FFFFFFu, "alpha 0.5: the tail's fade is half way");
    check(std::fabs(out.draws[5].world.m[0][3] - 4.0f) <= kUnit, "alpha 0.5: the moved draw is half way");
    const std::uint32_t grown_first = b.immediates[2].first_vertex, thrown_first = b.immediates[3].first_vertex,
                        canvas_first = b.immediates[4].first_vertex;
    check(same_vertex(out.immediate_vertices[grown_first], b.immediate_vertices[grown_first]),
          "a trail with another vertex count is B's");
    check(same_vertex(out.immediate_vertices[thrown_first], b.immediate_vertices[thrown_first]) &&
              same_vertex(out.immediate_vertices[thrown_first + 2], b.immediate_vertices[thrown_first + 2]),
          "a draw with one vertex over the limit is B's, all of it");
    check(same_vertex(out.immediate_vertices[canvas_first], b.immediate_vertices[canvas_first]),
          "canvas geometry is B's");

    bool unchanged = true;
    for (std::size_t i = 0; unchanged && i < b.immediate_vertices.size(); ++i)
        unchanged = same_vertex(b.immediate_vertices[i], b_before.immediate_vertices[i]);
    check(unchanged, "building a picture changes no vertex of the game's packet");

    // Two effects that draw from one buffer are paired in the order they draw; another count is not paired.
    {
        const FramePacket pa = build_ribbons({{0x70, trail_a}, {0x70, still}}, 1);
        const FramePacket pb = build_ribbons({{0x70, trail_b}, {0x70, still}}, 2);
        render::PacketInterpolator again;
        again.pair(&pa, &pb);
        check(again.stats().immediates_followed == 1u && again.stats().immediates_still == 1u,
              "draws under one key are paired in the order drawn");
        const FramePacket pc = build_ribbons({{0x70, trail_b}}, 2);
        again.pair(&pa, &pc);
        check(again.stats().immediates_followed == 0u && again.stats().immediates_snapped == 1u,
              "a key drawn another number of times is B's");
    }
    // Another texture under the key: another effect.
    {
        std::vector<Ribbon> other = in_b;
        other[0].texture = 9;
        const FramePacket pb = build_ribbons(other, 21);
        render::PacketInterpolator again;
        again.pair(&a, &pb);
        check(again.stats().immediates_followed == 1u, "a draw whose texture changed is not followed");
    }
    // The limit at its edge: the head moves sqrt(100 + 16) = 10.77.
    {
        render::InterpLimits limits;
        limits.object_step = 11.0f;
        render::PacketInterpolator tight;
        tight.set_limits(limits);
        tight.pair(&a, &b);
        check(tight.stats().immediates_followed == 2u, "a vertex step under the limit is followed");
        limits.object_step = 10.5f;
        tight.set_limits(limits);
        tight.pair(&a, &b);
        check(tight.stats().immediates_followed == 1u, "a vertex step over the limit is not");
    }
}

}  // namespace

int main() {
    transform_tests();
    packet_tests();
    particle_tests();
    immediate_tests();
    timeline_tests();
    std::printf("frame_interp_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
