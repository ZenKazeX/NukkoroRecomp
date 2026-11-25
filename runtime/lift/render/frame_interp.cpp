// Independent frame: a picture between two game frames. Contract: render/frame_interp.h.

#include "render/frame_interp.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace shadow::render {

namespace {

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

bool same_matrix(const Float3x4& a, const Float3x4& b) { return std::memcmp(a.m, b.m, sizeof(a.m)) == 0; }

bool same_camera(const Camera& a, const Camera& b) {
    return same_matrix(a.frame, b.frame) && a.view_window_x == b.view_window_x && a.view_window_y == b.view_window_y &&
           a.near_plane == b.near_plane && a.far_plane == b.far_plane && a.parallel == b.parallel &&
           a.framing == b.framing;
}

float step_between(const Float3x4& a, const Float3x4& b) {
    const float dx = b.m[0][3] - a.m[0][3], dy = b.m[1][3] - a.m[1][3], dz = b.m[2][3] - a.m[2][3];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Float3x4 lerp_matrix(const Float3x4& a, const Float3x4& b, float t) {
    Float3x4 out;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            out.m[r][c] = lerp(a.m[r][c], b.m[r][c], t);
    return out;
}

// The unit quaternion of a rotation given by its three unit axes (the matrix's columns).
void quaternion_of(const float r[3][3], float q[4]) {
    const float trace = r[0][0] + r[1][1] + r[2][2];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (r[2][1] - r[1][2]) / s;
        q[1] = (r[0][2] - r[2][0]) / s;
        q[2] = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
        q[3] = (r[2][1] - r[1][2]) / s;
        q[0] = 0.25f * s;
        q[1] = (r[0][1] + r[1][0]) / s;
        q[2] = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        const float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
        q[3] = (r[0][2] - r[2][0]) / s;
        q[0] = (r[0][1] + r[1][0]) / s;
        q[1] = 0.25f * s;
        q[2] = (r[1][2] + r[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
        q[3] = (r[1][0] - r[0][1]) / s;
        q[0] = (r[0][2] + r[2][0]) / s;
        q[1] = (r[1][2] + r[2][1]) / s;
        q[2] = 0.25f * s;
    }
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0.0f)
        for (int i = 0; i < 4; ++i)
            q[i] /= n;
}

constexpr float kPi = 3.14159265358979323846f;

// RGBA8, each channel `t` of the way from a to b, rounded.
std::uint32_t mix_colour(std::uint32_t a, std::uint32_t b, float t) {
    if (a == b)
        return b;
    std::uint32_t colour = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float ca = static_cast<float>((a >> shift) & 0xFFu);
        const float cb = static_cast<float>((b >> shift) & 0xFFu);
        colour |= (static_cast<std::uint32_t>(lerp(ca, cb, t) + 0.5f) & 0xFFu) << shift;
    }
    return colour;
}

// What the independent frame moves of a particle: everything but the texture corners.
bool same_particle(const Particle& a, const Particle& b) {
    return a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z &&
           a.size[0] == b.size[0] && a.size[1] == b.size[1] && a.colour == b.colour && a.rotation == b.rotation;
}

// B's record with A's motion `t` of the way to B's (0 < t < 1). The rotation goes the short way round: the game keeps
// it in [-pi, pi], so a turn through pi arrives from the other end.
Particle mix_particle(const Particle& a, const Particle& b, float t) {
    Particle out = b;
    out.position = Float3{lerp(a.position.x, b.position.x, t), lerp(a.position.y, b.position.y, t),
                          lerp(a.position.z, b.position.z, t)};
    out.size[0] = lerp(a.size[0], b.size[0], t);
    out.size[1] = lerp(a.size[1], b.size[1], t);
    float turn = b.rotation - a.rotation;
    if (turn > kPi)
        turn -= 2.0f * kPi;
    else if (turn < -kPi)
        turn += 2.0f * kPi;
    out.rotation = a.rotation + turn * t;
    out.colour = mix_colour(a.colour, b.colour, t);
    return out;
}

// An axis `t` of the way from a to b: the direction blended, the length its own blend (the quad keeps its size while
// the camera turns).
Float3 mix_axis(const Float3& a, const Float3& b, float t) {
    const Float3 v{lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)};
    const float la = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    const float lb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
    const float lv = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(lv > 1e-12f))
        return b;
    const float scale = lerp(la, lb, t) / lv;
    return Float3{v.x * scale, v.y * scale, v.z * scale};
}

bool same_axis(const Float3& a, const Float3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

}  // namespace

bool decompose(const Float3x4& m, Trs* out) {
    float axis[3][3];  // axis[c] = column c
    float scale[3];
    for (int c = 0; c < 3; ++c) {
        const float x = m.m[0][c], y = m.m[1][c], z = m.m[2][c];
        const float length = std::sqrt(x * x + y * y + z * z);
        if (!(length > 1e-12f) || !std::isfinite(length))
            return false;
        scale[c] = length;
        axis[c][0] = x / length;
        axis[c][1] = y / length;
        axis[c][2] = z / length;
    }
    const auto dot = [&](int i, int j) {
        return axis[i][0] * axis[j][0] + axis[i][1] * axis[j][1] + axis[i][2] * axis[j][2];
    };
    constexpr float kSquare = 1e-3f;
    if (std::fabs(dot(0, 1)) > kSquare || std::fabs(dot(0, 2)) > kSquare || std::fabs(dot(1, 2)) > kSquare)
        return false;
    // The handedness: axis 0 . (axis 1 x axis 2). A mirror goes into the first scale.
    const float cx = axis[1][1] * axis[2][2] - axis[1][2] * axis[2][1];
    const float cy = axis[1][2] * axis[2][0] - axis[1][0] * axis[2][2];
    const float cz = axis[1][0] * axis[2][1] - axis[1][1] * axis[2][0];
    if (axis[0][0] * cx + axis[0][1] * cy + axis[0][2] * cz < 0.0f) {
        scale[0] = -scale[0];
        for (int k = 0; k < 3; ++k)
            axis[0][k] = -axis[0][k];
    }
    float r[3][3];
    for (int row = 0; row < 3; ++row)
        for (int c = 0; c < 3; ++c)
            r[row][c] = axis[c][row];
    quaternion_of(r, out->q);
    for (int k = 0; k < 3; ++k) {
        out->s[k] = scale[k];
        out->t[k] = m.m[k][3];
    }
    return true;
}

Float3x4 compose(const Trs& trs) {
    const float x = trs.q[0], y = trs.q[1], z = trs.q[2], w = trs.q[3];
    const float r[3][3] = {
        {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - z * w), 2.0f * (x * z + y * w)},
        {2.0f * (x * y + z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - x * w)},
        {2.0f * (x * z - y * w), 2.0f * (y * z + x * w), 1.0f - 2.0f * (x * x + y * y)},
    };
    Float3x4 out;
    for (int row = 0; row < 3; ++row) {
        for (int c = 0; c < 3; ++c)
            out.m[row][c] = r[row][c] * trs.s[c];
        out.m[row][3] = trs.t[row];
    }
    return out;
}

Trs interpolate_trs(const Trs& a, const Trs& b, float t) {
    Trs out;
    float bq[4] = {b.q[0], b.q[1], b.q[2], b.q[3]};
    float d = a.q[0] * bq[0] + a.q[1] * bq[1] + a.q[2] * bq[2] + a.q[3] * bq[3];
    if (d < 0.0f) {  // q and -q are one rotation: the short way round
        d = -d;
        for (float& v : bq)
            v = -v;
    }
    float wa = 1.0f - t, wb = t;
    if (d < 0.9995f) {
        const float theta = std::acos(std::min(d, 1.0f));
        const float sine = std::sin(theta);
        wa = std::sin((1.0f - t) * theta) / sine;
        wb = std::sin(t * theta) / sine;
    }
    float n = 0.0f;
    for (int i = 0; i < 4; ++i) {
        out.q[i] = wa * a.q[i] + wb * bq[i];
        n += out.q[i] * out.q[i];
    }
    n = std::sqrt(n);
    if (n > 0.0f)
        for (float& v : out.q)
            v /= n;
    for (int k = 0; k < 3; ++k) {
        out.s[k] = lerp(a.s[k], b.s[k], t);
        out.t[k] = lerp(a.t[k], b.t[k], t);
    }
    return out;
}

Float3x4 interpolate_transform(const Float3x4& a, const Float3x4& b, float t) {
    if (!(t > 0.0f))
        return a;
    if (t >= 1.0f)
        return b;
    Trs ta, tb;
    if (decompose(a, &ta) && decompose(b, &tb) && (ta.s[0] < 0.0f) == (tb.s[0] < 0.0f))
        return compose(interpolate_trs(ta, tb, t));
    return lerp_matrix(a, b, t);
}

void PacketInterpolator::pair(const FramePacket* a, const FramePacket* b) {
    a_ = a;
    b_ = b;
    stats_ = InterpStats{};
    worlds_.clear();
    bones_.clear();
    cameras_.clear();
    dissolves_.clear();
    morphs_.clear();
    particles_.clear();
    batches_.clear();
    runs_.clear();
    if (a_ == nullptr || b_ == nullptr)
        return;
    stats_.particles = static_cast<std::uint32_t>(b_->particles.size());
    if (b_->particle_tags.size() == b_->particles.size())
        for (const ParticleTag& tag : b_->particle_tags)
            stats_.particles_tagged += tag.id != 0u ? 1u : 0u;
    // A black frame (a video-mode switch, VISetBlack) has no views, and a frame boxed differently (an in-engine
    // cutscene's 4:3) is another shot: nothing leads from one to the other.
    if (a_->views.empty() || b_->views.empty() || a_->content_aspect != b_->content_aspect) {
        stats_.cut = true;
        return;
    }
    pair_cameras();
    if (stats_.cut) {
        cameras_.clear();
        return;
    }
    pair_draws();
    pair_particles();
    pair_immediates();
}

void PacketInterpolator::pair_immediates() {
    const FramePacket& a = *a_;
    const FramePacket& b = *b_;
    const auto collect = [](const FramePacket& p, std::vector<Keyed>& out) {
        out.clear();
        for (std::uint32_t i = 0; i < p.draws.size(); ++i) {
            const DrawRecord& d = p.draws[i];
            if (d.key == 0u || d.immediate >= p.immediates.size())
                continue;
            const ImmediateDraw& im = p.immediates[d.immediate];
            if (im.space == Space::World &&
                static_cast<std::uint64_t>(im.first_vertex) + im.vertex_count <= p.immediate_vertices.size())
                out.push_back(Keyed{d.key, i});
        }
        std::stable_sort(out.begin(), out.end(), [](const Keyed& x, const Keyed& y) { return x.key < y.key; });
    };
    collect(a, keyed_a_);
    collect(b, keyed_b_);
    stats_.immediates = static_cast<std::uint32_t>(keyed_b_.size());
    const std::uint64_t apart = b.tick > a.tick ? b.tick - a.tick : 1u;
    const float step_limit = limits_.object_step * static_cast<float>(apart < 3u ? apart : 3u);
    std::size_t ia = 0, ib = 0;
    while (ib < keyed_b_.size()) {
        const std::uint64_t key = keyed_b_[ib].key;
        std::size_t eb = ib;
        while (eb < keyed_b_.size() && keyed_b_[eb].key == key)
            ++eb;
        while (ia < keyed_a_.size() && keyed_a_[ia].key < key)
            ++ia;
        std::size_t ea = ia;
        while (ea < keyed_a_.size() && keyed_a_[ea].key == key)
            ++ea;
        const std::size_t group = eb - ib;
        if (ea - ia != group) {
            stats_.immediates_snapped += static_cast<std::uint32_t>(group);  // drawn another number of times
            (ea == ia ? stats_.immediates_new : stats_.immediates_recount) += static_cast<std::uint32_t>(group);
            ib = eb;
            ia = ea;
            continue;
        }
        for (std::size_t k = 0; k < group; ++k) {
            const std::uint32_t index_a = keyed_a_[ia + k].draw, index_b = keyed_b_[ib + k].draw;
            const ImmediateDraw& from = a.immediates[a.draws[index_a].immediate];
            const ImmediateDraw& to = b.immediates[b.draws[index_b].immediate];
            if (from.vertex_count != to.vertex_count || from.index_count != to.index_count ||
                from.topology != to.topology || from.source != to.source ||
                from.texture.value != to.texture.value) {
                ++stats_.immediates_snapped;  // another shape: a trail still growing, or another effect at the address
                ++(to.vertex_count > from.vertex_count   ? stats_.immediates_grown
                   : to.vertex_count < from.vertex_count ? stats_.immediates_shrunk
                                                         : stats_.immediates_restyled);
                continue;
            }
            const Float3x4& world_a = a.draws[index_a].world;
            const Float3x4& world_b = b.draws[index_b].world;
            const bool world_moved = !same_matrix(world_a, world_b);
            float largest = world_moved ? step_between(world_a, world_b) : 0.0f;
            bool changed = world_moved;
            for (std::uint32_t v = 0; v < to.vertex_count; ++v) {
                const ImmediateVertex& va = a.immediate_vertices[from.first_vertex + v];
                const ImmediateVertex& vb = b.immediate_vertices[to.first_vertex + v];
                const float dx = vb.position.x - va.position.x, dy = vb.position.y - va.position.y,
                            dz = vb.position.z - va.position.z;
                const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (step > largest)
                    largest = step;
                changed = changed || step != 0.0f || va.colour != vb.colour;
            }
            if (largest > stats_.largest_immediate_step)
                stats_.largest_immediate_step = largest;
            if (!changed) {
                ++stats_.immediates_still;
                continue;
            }
            if (!(largest <= step_limit)) {
                ++stats_.immediates_snapped;  // placed, or another effect's geometry under the same key
                ++stats_.immediates_placed;
                continue;
            }
            if (world_moved) {
                Mover mover;
                mover.index = index_b;
                mover.from = world_a;
                mover.trs = decompose(world_a, &mover.a) && decompose(world_b, &mover.b) &&
                            (mover.a.s[0] < 0.0f) == (mover.b.s[0] < 0.0f);
                worlds_.push_back(mover);
            }
            runs_.push_back(Run{to.first_vertex, from.first_vertex, to.vertex_count});
            ++stats_.immediates_followed;
            stats_.immediate_vertices += to.vertex_count;
        }
        ib = eb;
        ia = ea;
    }
}

void PacketInterpolator::pair_particles() {
    const FramePacket& a = *a_;
    const FramePacket& b = *b_;
    if (a.particle_tags.size() != a.particles.size() || b.particle_tags.size() != b.particles.size()) {
        stats_.particles_born = stats_.particles_tagged;
        return;
    }
    // The batches by key, in the order drawn: an emitter's n-th batch in one frame is its n-th in the other.
    const auto collect = [](const FramePacket& p, std::vector<Keyed>& out) {
        out.clear();
        for (std::uint32_t i = 0; i < p.draws.size(); ++i) {
            const DrawRecord& d = p.draws[i];
            if (d.key == 0u || d.particles >= p.particle_batches.size())
                continue;
            const ParticleBatch& batch = p.particle_batches[d.particles];
            if (static_cast<std::uint64_t>(batch.first) + batch.count <= p.particles.size())
                out.push_back(Keyed{d.key, d.particles});
        }
        std::stable_sort(out.begin(), out.end(), [](const Keyed& x, const Keyed& y) { return x.key < y.key; });
    };
    collect(a, keyed_a_);
    collect(b, keyed_b_);
    // The limit is a step per game frame: a pair that spans a frame the renderer never took may move twice as far.
    const std::uint64_t apart = b.tick > a.tick ? b.tick - a.tick : 1u;
    const float step_limit = limits_.particle_step * static_cast<float>(apart < 3u ? apart : 3u);
    std::uint32_t seen = 0;  // B's tagged particles in the batches looked at
    std::size_t ia = 0, ib = 0;
    while (ib < keyed_b_.size()) {
        const std::uint64_t key = keyed_b_[ib].key;
        std::size_t eb = ib;
        while (eb < keyed_b_.size() && keyed_b_[eb].key == key)
            ++eb;
        while (ia < keyed_a_.size() && keyed_a_[ia].key < key)
            ++ia;
        std::size_t ea = ia;
        while (ea < keyed_a_.size() && keyed_a_[ea].key == key)
            ++ea;
        const std::size_t group = eb - ib;
        for (std::size_t k = 0; k < group && ea - ia == group; ++k) {
            const std::uint32_t batch_a = keyed_a_[ia + k].draw, batch_b = keyed_b_[ib + k].draw;
            const ParticleBatch& from = a.particle_batches[batch_a];
            const ParticleBatch& to = b.particle_batches[batch_b];
            if (from.texture.value != to.texture.value || from.particle_colours != to.particle_colours)
                continue;  // the address names another emitter now
            if (!same_axis(from.right, to.right) || !same_axis(from.up, to.up) || from.colour.x != to.colour.x ||
                from.colour.y != to.colour.y || from.colour.z != to.colour.z || from.colour.w != to.colour.w) {
                batches_.push_back(Pairing{batch_b, batch_a});
                ++stats_.particle_batches;
            }
            tagged_a_.clear();
            for (std::uint32_t i = 0; i < from.count; ++i)
                if (a.particle_tags[from.first + i].id != 0u)
                    tagged_a_.push_back(Tagged{a.particle_tags[from.first + i].id, from.first + i});
            std::sort(tagged_a_.begin(), tagged_a_.end(),
                      [](const Tagged& x, const Tagged& y) { return x.id < y.id; });
            // First the ids both batches hold, with how much each aged from A to B.
            candidates_.clear();
            ages_.clear();
            for (std::uint32_t i = 0; i < to.count; ++i) {
                const std::uint32_t index = to.first + i;
                const ParticleTag& tag = b.particle_tags[index];
                if (tag.id == 0u)
                    continue;
                ++seen;
                const auto at = std::lower_bound(tagged_a_.begin(), tagged_a_.end(), tag.id,
                                                 [](const Tagged& x, std::uint32_t id) { return x.id < id; });
                // An id A's batch holds exactly once.
                if (at == tagged_a_.end() || at->id != tag.id ||
                    (at + 1 != tagged_a_.end() && (at + 1)->id == tag.id)) {
                    ++stats_.particles_born;
                    continue;
                }
                candidates_.push_back(Pairing{index, at->particle});
                ages_.push_back(tag.age - a.particle_tags[at->particle].age);
            }
            // The game ages every particle of a system by the same step, so the survivors all aged alike: the
            // middle one's ageing is the pair's. An id that aged differently - above all one whose age went back -
            // is another particle born at a place a dead one left.
            float aged = 0.0f;
            if (!ages_.empty()) {
                sorted_ages_ = ages_;
                const auto middle = sorted_ages_.begin() + static_cast<std::ptrdiff_t>(sorted_ages_.size() / 2);
                std::nth_element(sorted_ages_.begin(), middle, sorted_ages_.end());
                aged = *middle;
            }
            const float age_slack = std::fabs(aged) * 0.25f + 1e-6f;
            for (std::size_t c = 0; c < candidates_.size(); ++c) {
                const std::uint32_t index = candidates_[c].index;
                const std::uint32_t partner = candidates_[c].from;
                if (!(std::fabs(ages_[c] - aged) <= age_slack)) {
                    ++stats_.particles_reused;
                    continue;
                }
                const Particle& pa = a.particles[partner];
                const Particle& pb = b.particles[index];
                if (same_particle(pa, pb)) {
                    ++stats_.particles_still;
                    continue;
                }
                const float dx = pb.position.x - pa.position.x, dy = pb.position.y - pa.position.y,
                            dz = pb.position.z - pa.position.z;
                const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (step > stats_.largest_particle_step)
                    stats_.largest_particle_step = step;
                if (!(step <= step_limit)) {
                    ++stats_.particles_placed;
                    continue;
                }
                particles_.push_back(Pairing{index, partner});
                ++stats_.particles_followed;
            }
        }
        ib = eb;
        ia = ea;
    }
    stats_.particles_born += stats_.particles_tagged - seen;  // in a batch without a partner
}

void PacketInterpolator::pair_cameras() {
    const FramePacket& a = *a_;
    const FramePacket& b = *b_;
    const auto follow = [&](std::uint32_t index, bool pass, const Camera& from, const Camera& to) {
        if (same_camera(from, to))
            return;
        if (from.parallel != to.parallel || from.framing != to.framing)
            return;  // another kind of camera: B's
        CameraMover mover;
        mover.index = index;
        mover.pass = pass;
        mover.from = from;
        mover.trs = decompose(from.frame, &mover.a) && decompose(to.frame, &mover.b) &&
                    (mover.a.s[0] < 0.0f) == (mover.b.s[0] < 0.0f);
        if (!to.parallel) {
            // A cut: the eye jumps, or the view direction (the frame's third column) turns further than a turn does.
            float fa[3], fb[3], na = 0.0f, nb = 0.0f, d = 0.0f;
            for (int k = 0; k < 3; ++k) {
                fa[k] = from.frame.m[k][2];
                fb[k] = to.frame.m[k][2];
                na += fa[k] * fa[k];
                nb += fb[k] * fb[k];
                d += fa[k] * fb[k];
            }
            const float n = std::sqrt(na * nb);
            const float step = step_between(from.frame, to.frame);
            stats_.largest_camera_step = std::max(stats_.largest_camera_step, step);
            if (n > 0.0f)
                stats_.smallest_camera_turn_cos = std::min(stats_.smallest_camera_turn_cos, d / n);
            if (step > limits_.camera_step || (n > 0.0f && d / n < limits_.camera_turn_cos)) {
                stats_.cut = true;
                return;
            }
        }
        cameras_.push_back(mover);
        ++stats_.cameras;
    };
    // The views' own cameras, view for view, when the frame is laid out the same.
    const bool same_views = a.views.size() == b.views.size();
    if (same_views) {
        for (std::uint32_t v = 0; v < b.views.size() && !stats_.cut; ++v) {
            const ViewRecord& va = a.views[v];
            const ViewRecord& vb = b.views[v];
            if (va.rect.x != vb.rect.x || va.rect.y != vb.rect.y || va.rect.width != vb.rect.width ||
                va.rect.height != vb.rect.height)
                continue;
            follow(v, false, va.camera, vb.camera);
        }
    }
    if (!same_views || stats_.cut)
        return;
    // The passes' cameras: per view, the different cameras in their order of first use, perspective and parallel ones
    // counted apart (a frame opens the same few cameras in the same order; an effect's bracket may come and go).
    struct Distinct {
        ViewId view;
        const Camera* camera;
    };
    std::vector<Distinct> da, db;
    const auto collect = [](const FramePacket& p, std::vector<Distinct>& out) {
        for (const PassRecord& pass : p.passes) {
            if (!pass.has_camera)
                continue;
            bool known = false;
            for (const Distinct& d : out)
                known = known || (d.view == pass.view && same_camera(*d.camera, pass.camera));
            if (!known)
                out.push_back(Distinct{pass.view, &pass.camera});
        }
    };
    collect(a, da);
    collect(b, db);
    const auto count = [](const std::vector<Distinct>& list, ViewId view, bool parallel) {
        std::uint32_t n = 0;
        for (const Distinct& d : list)
            n += (d.view == view && d.camera->parallel == parallel) ? 1u : 0u;
        return n;
    };
    // B's k-th distinct camera of (view, kind) pairs with A's k-th, when both have as many.
    const auto partner = [&](const Distinct& of) -> const Camera* {
        const bool parallel = of.camera->parallel;
        if (count(da, of.view, parallel) != count(db, of.view, parallel))
            return nullptr;
        std::uint32_t k = 0;
        for (const Distinct& d : db) {
            if (&d == &of)
                break;
            k += (d.view == of.view && d.camera->parallel == parallel) ? 1u : 0u;
        }
        for (const Distinct& d : da) {
            if (d.view != of.view || d.camera->parallel != parallel)
                continue;
            if (k == 0u)
                return d.camera;
            --k;
        }
        return nullptr;
    };
    for (std::uint32_t i = 0; i < b.passes.size() && !stats_.cut; ++i) {
        const PassRecord& pass = b.passes[i];
        if (!pass.has_camera)
            continue;
        for (const Distinct& d : db) {
            if (d.view != pass.view || !same_camera(*d.camera, pass.camera))
                continue;
            if (const Camera* from = partner(d))
                follow(i, true, *from, pass.camera);
            break;
        }
    }
}

void PacketInterpolator::pair_draws() {
    const FramePacket& a = *a_;
    const FramePacket& b = *b_;
    const auto collect = [](const FramePacket& p, std::vector<Keyed>& out) {
        out.clear();
        for (std::uint32_t i = 0; i < p.draws.size(); ++i) {
            const DrawRecord& d = p.draws[i];
            if (d.key != 0u && d.particles == kNoParticles && d.immediate == kNoImmediate)
                out.push_back(Keyed{d.key, i});
        }
        // By key, and inside a key by submesh and then the order drawn: the n-th draw of an object's submesh in one
        // frame is the n-th in the other.
        std::stable_sort(out.begin(), out.end(), [&p](const Keyed& x, const Keyed& y) {
            if (x.key != y.key)
                return x.key < y.key;
            return p.draws[x.draw].submesh < p.draws[y.draw].submesh;
        });
    };
    collect(a, keyed_a_);
    collect(b, keyed_b_);
    stats_.keyed = static_cast<std::uint32_t>(keyed_b_.size());
    bone_seen_.assign(b.bones.size(), 0u);
    std::size_t ia = 0, ib = 0;
    while (ib < keyed_b_.size()) {
        const std::uint64_t key = keyed_b_[ib].key;
        std::size_t eb = ib;
        while (eb < keyed_b_.size() && keyed_b_[eb].key == key)
            ++eb;
        while (ia < keyed_a_.size() && keyed_a_[ia].key < key)
            ++ia;
        std::size_t ea = ia;
        while (ea < keyed_a_.size() && keyed_a_[ea].key == key)
            ++ea;
        const std::size_t group = eb - ib;
        if (ea - ia != group) {
            stats_.snapped += static_cast<std::uint32_t>(group);  // appeared, went, or drawn another number of times
            ib = eb;
            ia = ea;
            continue;
        }
        for (std::size_t k = 0; k < group; ++k) {
            const std::uint32_t index_a = keyed_a_[ia + k].draw, index_b = keyed_b_[ib + k].draw;
            const DrawRecord& from = a.draws[index_a];
            const DrawRecord& to = b.draws[index_b];
            if (from.mesh.value != to.mesh.value || from.submesh != to.submesh || from.bones.count != to.bones.count ||
                from.morph.count != to.morph.count) {
                ++stats_.snapped;  // the address names another object now
                continue;
            }
            bool moved = false, over = false;
            std::vector<Mover> palette;
            if (to.bones.count != 0u) {
                // The palette (world space), once however many submeshes share it.
                const std::uint64_t end_a = static_cast<std::uint64_t>(from.bones.first) + from.bones.count;
                const std::uint64_t end_b = static_cast<std::uint64_t>(to.bones.first) + to.bones.count;
                if (end_a > a.bones.size() || end_b > b.bones.size()) {
                    ++stats_.snapped;
                    continue;
                }
                if (bone_seen_[to.bones.first] == 0u) {
                    // The root bone decides whether the character was moved or placed.
                    const float step = step_between(a.bones[from.bones.first], b.bones[to.bones.first]);
                    stats_.largest_object_step = std::max(stats_.largest_object_step, step);
                    over = step > limits_.object_step;
                    for (std::uint32_t j = 0; j < to.bones.count && !over; ++j) {
                        const Float3x4& ma = a.bones[from.bones.first + j];
                        const Float3x4& mb = b.bones[to.bones.first + j];
                        if (same_matrix(ma, mb))
                            continue;
                        Mover mover;
                        mover.index = to.bones.first + j;
                        mover.from = ma;
                        mover.trs = decompose(ma, &mover.a) && decompose(mb, &mover.b) &&
                                    (mover.a.s[0] < 0.0f) == (mover.b.s[0] < 0.0f);
                        palette.push_back(mover);
                    }
                    bone_seen_[to.bones.first] = over ? 2u : 1u;
                    if (!over) {
                        moved = !palette.empty();
                        stats_.bones += static_cast<std::uint32_t>(palette.size());
                        bones_.insert(bones_.end(), palette.begin(), palette.end());
                    }
                } else {
                    over = bone_seen_[to.bones.first] == 2u;
                    moved = !over;  // counted with the palette's first draw
                }
            } else if (!same_matrix(from.world, to.world)) {
                const float step = step_between(from.world, to.world);
                stats_.largest_object_step = std::max(stats_.largest_object_step, step);
                over = step > limits_.object_step;
                if (!over) {
                    Mover mover;
                    mover.index = index_b;
                    mover.from = from.world;
                    mover.trs = decompose(from.world, &mover.a) && decompose(to.world, &mover.b) &&
                                (mover.a.s[0] < 0.0f) == (mover.b.s[0] < 0.0f);
                    worlds_.push_back(mover);
                    moved = true;
                }
            }
            if (over) {
                ++stats_.snapped;
                continue;
            }
            if (from.material.dissolve != to.material.dissolve) {
                dissolves_.push_back(Scalar{index_b, from.material.dissolve});
                moved = true;
            }
            const std::uint64_t morph_a = static_cast<std::uint64_t>(from.morph.first) + from.morph.count;
            const std::uint64_t morph_b = static_cast<std::uint64_t>(to.morph.first) + to.morph.count;
            if (to.morph.count != 0u && morph_a <= a.morph_weights.size() && morph_b <= b.morph_weights.size()) {
                for (std::uint32_t j = 0; j < to.morph.count; ++j) {
                    const float wa = a.morph_weights[from.morph.first + j];
                    if (wa != b.morph_weights[to.morph.first + j]) {
                        morphs_.push_back(Scalar{to.morph.first + j, wa});
                        moved = true;
                    }
                }
            }
            if (moved)
                ++stats_.followed;
            else
                ++stats_.still;
        }
        ib = eb;
        ia = ea;
    }
}

void PacketInterpolator::build(float alpha, FramePacket* out) const {
    if (b_ == nullptr) {
        *out = FramePacket{};
        return;
    }
    *out = *b_;
    if (a_ == nullptr || stats_.cut || alpha >= 1.0f)
        return;
    const float t = alpha > 0.0f ? alpha : 0.0f;
    const auto place = [t](const Mover& mover, const Float3x4& to) {
        if (t == 0.0f)
            return mover.from;
        return mover.trs ? compose(interpolate_trs(mover.a, mover.b, t)) : lerp_matrix(mover.from, to, t);
    };
    for (const Mover& mover : worlds_)
        out->draws[mover.index].world = place(mover, b_->draws[mover.index].world);
    for (const Mover& mover : bones_)
        out->bones[mover.index] = place(mover, b_->bones[mover.index]);
    for (const CameraMover& mover : cameras_) {
        const Camera& to = mover.pass ? b_->passes[mover.index].camera : b_->views[mover.index].camera;
        Camera& camera = mover.pass ? out->passes[mover.index].camera : out->views[mover.index].camera;
        if (t == 0.0f) {
            camera = mover.from;
            continue;
        }
        camera.frame = mover.trs ? compose(interpolate_trs(mover.a, mover.b, t))
                                 : lerp_matrix(mover.from.frame, to.frame, t);
        camera.view_window_x = lerp(mover.from.view_window_x, to.view_window_x, t);
        camera.view_window_y = lerp(mover.from.view_window_y, to.view_window_y, t);
        camera.near_plane = lerp(mover.from.near_plane, to.near_plane, t);
        camera.far_plane = lerp(mover.from.far_plane, to.far_plane, t);
    }
    for (const Scalar& scalar : dissolves_)
        out->draws[scalar.index].material.dissolve =
            lerp(scalar.from, b_->draws[scalar.index].material.dissolve, t);
    for (const Scalar& scalar : morphs_)
        out->morph_weights[scalar.index] = lerp(scalar.from, b_->morph_weights[scalar.index], t);
    for (const Pairing& pairing : particles_) {
        const Particle& from = a_->particles[pairing.from];
        Particle& particle = out->particles[pairing.index];
        if (t == 0.0f) {
            particle.position = from.position;
            particle.size[0] = from.size[0];
            particle.size[1] = from.size[1];
            particle.colour = from.colour;
            particle.rotation = from.rotation;
        } else {
            particle = mix_particle(from, b_->particles[pairing.index], t);
        }
    }
    for (const Run& run : runs_) {
        for (std::uint32_t v = 0; v < run.count; ++v) {
            const ImmediateVertex& from = a_->immediate_vertices[run.from + v];
            const ImmediateVertex& to = b_->immediate_vertices[run.first + v];
            ImmediateVertex& vertex = out->immediate_vertices[run.first + v];
            if (t == 0.0f) {
                vertex.position = from.position;
                vertex.colour = from.colour;
            } else {
                vertex.position = Float3{lerp(from.position.x, to.position.x, t), lerp(from.position.y, to.position.y, t),
                                         lerp(from.position.z, to.position.z, t)};
                vertex.colour = mix_colour(from.colour, to.colour, t);
            }
        }
    }
    for (const Pairing& pairing : batches_) {
        const ParticleBatch& from = a_->particle_batches[pairing.from];
        const ParticleBatch& to = b_->particle_batches[pairing.index];
        ParticleBatch& batch = out->particle_batches[pairing.index];
        if (t == 0.0f) {
            batch.right = from.right;
            batch.up = from.up;
            batch.colour = from.colour;
        } else {
            batch.right = mix_axis(from.right, to.right, t);
            batch.up = mix_axis(from.up, to.up, t);
            batch.colour = Float4{lerp(from.colour.x, to.colour.x, t), lerp(from.colour.y, to.colour.y, t),
                                  lerp(from.colour.z, to.colour.z, t), lerp(from.colour.w, to.colour.w, t)};
        }
    }
}

}  // namespace shadow::render
