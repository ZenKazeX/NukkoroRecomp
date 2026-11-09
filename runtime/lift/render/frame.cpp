#include "render/frame.h"

#include <cmath>
#include <utility>

namespace shadow::render {

void FrameBuilder::begin_frame(std::uint64_t tick, const Float4& background) {
    packet_ = FramePacket{};
    {
        // C3: the vectors of a rendered packet, emptied, with their capacity (every other field starts as new).
        std::lock_guard<std::mutex> lock(recycled_mutex_);
        if (has_recycled_) {
            has_recycled_ = false;
            const auto take = [](auto& into, auto& from) {
                from.clear();
                into.swap(from);
            };
            take(packet_.views, recycled_.views);
            take(packet_.light_sets, recycled_.light_sets);
            take(packet_.passes, recycled_.passes);
            take(packet_.draws, recycled_.draws);
            take(packet_.bones, recycled_.bones);
            take(packet_.morph_weights, recycled_.morph_weights);
            take(packet_.particles, recycled_.particles);
            take(packet_.particle_batches, recycled_.particle_batches);
            take(packet_.immediate_vertices, recycled_.immediate_vertices);
            take(packet_.immediate_indices, recycled_.immediate_indices);
            take(packet_.immediates, recycled_.immediates);
            take(packet_.canvas_regions, recycled_.canvas_regions);
        }
    }
    packet_.tick = tick;
    packet_.background = background;
    pass_open_ = false;
    snapshots_ = 0;
    layer_ = EffectLayer::None;
}

ViewId FrameBuilder::view(const Camera& camera, const ViewRect& rect, const Canvas& canvas, bool over_views) {
    packet_.views.push_back(ViewRecord{camera, rect, canvas, over_views});
    return static_cast<ViewId>(packet_.views.size() - 1);
}

std::uint32_t FrameBuilder::canvas_region(const CanvasRegion& region) {
    packet_.canvas_regions.push_back(region);
    return static_cast<std::uint32_t>(packet_.canvas_regions.size() - 1);
}

void FrameBuilder::set_immediate_anchor(std::size_t immediate, Anchor anchor) {
    if (immediate < packet_.immediates.size())
        packet_.immediates[immediate].anchor = anchor;
}

LightSetId FrameBuilder::lights(const LightSet& set) {
    packet_.light_sets.push_back(set);
    return static_cast<LightSetId>(packet_.light_sets.size() - 1);
}

void FrameBuilder::begin_pass(ViewId view, PassKind kind, const Clear& clear, const Camera* camera) {
    if (pass_open_)
        end_pass();
    PassRecord pass;
    pass.view = view;
    pass.kind = kind;
    pass.clear = clear;
    if (camera != nullptr) {
        pass.has_camera = true;
        pass.camera = *camera;
    }
    pass.first_draw = static_cast<std::uint32_t>(packet_.draws.size());
    packet_.passes.push_back(pass);
    pass_open_ = true;
}

BoneRange FrameBuilder::bones(const Float3x4* matrices, std::uint32_t count) {
    BoneRange range;
    range.first = static_cast<std::uint32_t>(packet_.bones.size());
    range.count = count;
    packet_.bones.insert(packet_.bones.end(), matrices, matrices + count);
    return range;
}

MorphRange FrameBuilder::morph(const float* weights, std::uint32_t count) {
    MorphRange range;
    range.first = static_cast<std::uint32_t>(packet_.morph_weights.size());
    range.count = count;
    packet_.morph_weights.insert(packet_.morph_weights.end(), weights, weights + count);
    return range;
}

void FrameBuilder::draw(MeshHandle mesh, std::uint32_t submesh, const Material& material, const Float3x4& world,
                        const Recipe& recipe, LightSetId lights, BoneRange bones, MorphRange morph) {
    if (!pass_open_)
        return;  // a draw outside a pass has nowhere to go
    packet_.draws.push_back(
        DrawRecord{mesh, submesh, material, world, recipe, lights, bones, kNoParticles, kNoImmediate, morph, layer_});
    ++packet_.passes.back().draw_count;
}

void FrameBuilder::immediate(const ImmediateDraw& draw, const ImmediateVertex* vertices, std::uint32_t vertex_count,
                             const std::uint32_t* indices, std::uint32_t index_count, const Float3x4& world,
                             const Recipe& recipe) {
    if (!pass_open_ || vertex_count == 0)
        return;
    ImmediateDraw d = draw;
    d.first_vertex = static_cast<std::uint32_t>(packet_.immediate_vertices.size());
    d.vertex_count = vertex_count;
    d.first_index = static_cast<std::uint32_t>(packet_.immediate_indices.size());
    d.index_count = indices != nullptr ? index_count : 0u;
    packet_.immediate_vertices.insert(packet_.immediate_vertices.end(), vertices, vertices + vertex_count);
    if (indices != nullptr)
        packet_.immediate_indices.insert(packet_.immediate_indices.end(), indices, indices + index_count);
    DrawRecord record;
    record.world = world;
    record.recipe = recipe;
    record.immediate = static_cast<std::uint32_t>(packet_.immediates.size());
    record.layer = layer_;
    packet_.immediates.push_back(d);
    packet_.draws.push_back(record);
    ++packet_.passes.back().draw_count;
}

void FrameBuilder::particles(const ParticleBatch& batch, const Particle* records, std::uint32_t count,
                             const Recipe& recipe) {
    if (!pass_open_ || count == 0)
        return;
    ParticleBatch b = batch;
    b.first = static_cast<std::uint32_t>(packet_.particles.size());
    b.count = count;
    packet_.particles.insert(packet_.particles.end(), records, records + count);
    DrawRecord draw;
    draw.recipe = recipe;
    draw.particles = static_cast<std::uint32_t>(packet_.particle_batches.size());
    draw.layer = layer_;
    packet_.particle_batches.push_back(b);
    packet_.draws.push_back(draw);
    ++packet_.passes.back().draw_count;
}

void FrameBuilder::glare(ViewId view, const GlareParams& params) {
    PassRecord pass;
    pass.view = view;
    pass.kind = PassKind::Glare;
    pass.first_draw = static_cast<std::uint32_t>(packet_.draws.size());
    pass.glare = params;
    packet_.passes.push_back(pass);
    pass_open_ = false;
}

// The packet's snapshots are numbered in pass order: the n-th Snapshot pass is snapshot n.
SnapshotId FrameBuilder::snapshot(ViewId view) {
    PassRecord pass;
    pass.view = view;
    pass.kind = PassKind::Snapshot;
    pass.first_draw = static_cast<std::uint32_t>(packet_.draws.size());
    packet_.passes.push_back(pass);
    pass_open_ = false;
    return snapshots_++;
}

void FrameBuilder::save_history(ViewId view) {
    PassRecord pass;
    pass.view = view;
    pass.kind = PassKind::SaveHistory;
    pass.first_draw = static_cast<std::uint32_t>(packet_.draws.size());
    packet_.passes.push_back(pass);
    pass_open_ = false;
}

void FrameBuilder::end_pass() {
    pass_open_ = false;
}

void FrameBuilder::recycle(FramePacket&& used) {
    std::lock_guard<std::mutex> lock(recycled_mutex_);
    recycled_ = std::move(used);
    has_recycled_ = true;
}

FramePacket FrameBuilder::finish() {
    pass_open_ = false;
    snapshots_ = 0;
    FramePacket out = std::move(packet_);
    packet_ = FramePacket{};
    return out;
}

Float3x4 look_at(const Float3& eye, const Float3& target, const Float3& up) {
    auto normalise = [](Float3 v) {
        const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return l > 0 ? Float3{v.x / l, v.y / l, v.z / l} : v;
    };
    auto cross = [](const Float3& a, const Float3& b) {
        return Float3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const Float3 forward = normalise(Float3{target.x - eye.x, target.y - eye.y, target.z - eye.z});
    // Right-handed world, y up: looking along +z, screen right is -x.
    const Float3 right = normalise(cross(forward, up));
    const Float3 true_up = cross(right, forward);
    Float3x4 frame;
    const Float3 columns[4] = {right, true_up, forward, eye};
    for (int c = 0; c < 4; ++c) {
        frame.m[0][c] = columns[c].x;
        frame.m[1][c] = columns[c].y;
        frame.m[2][c] = columns[c].z;
    }
    return frame;
}

}  // namespace shadow::render
