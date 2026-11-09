#include "motion/skeleton_pose.h"

#include <cmath>
#include <cstring>
#include <span>

#include "motion/curves.h"

namespace shadow::motion {
namespace {

std::uint32_t be32(const std::uint8_t* b, std::size_t at) {
    return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) | (std::uint32_t{b[at + 2]} << 8) |
           std::uint32_t{b[at + 3]};
}
std::uint16_t be16(const std::uint8_t* b, std::size_t at) {
    return static_cast<std::uint16_t>((b[at] << 8) | b[at + 1]);
}
float bef(const std::uint8_t* b, std::size_t at) {
    const std::uint32_t u = be32(b, at);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
std::string text(const std::uint8_t* b, std::size_t size, std::size_t at, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n && at + i < size && b[at + i] != 0; ++i)
        s.push_back(static_cast<char>(b[at + i]));
    return s;
}

constexpr std::size_t kRecordBones = 0x30;
constexpr std::size_t kBoneSize = 0x70;
constexpr std::size_t kClipFirstChunk = 0x34;

// world(bone) = world(parent) * S_pre * T * Rz * Ry * Rx * S over the tree, in pre-order.
template <class ValuesOf>
void pose_tree(const Skeleton& sk, ValuesOf values_of, std::vector<Mtx34>* world) {
    world->assign(sk.bones.size(), mtx_identity());
    const auto scale = [](Mtx34& m, const Vec3& s) {
        if (!(1.0f == s[0] && 1.0f == s[1] && 1.0f == s[2]))
            mtx_scale(m, s);
    };
    struct Item {
        std::int32_t bone;
        std::int32_t parent;
    };
    std::vector<Item> stack{{0, -1}};
    while (!stack.empty()) {
        const Item item = stack.back();
        stack.pop_back();
        const auto u = static_cast<std::size_t>(item.bone);
        const SkeletonBone& b = sk.bones[u];
        Vec3 pre, t, r, s;
        values_of(u, pre, t, r, s);
        Mtx34 m = item.parent >= 0 ? (*world)[static_cast<std::size_t>(item.parent)] : mtx_identity();
        if (b.flags & 0x400u)
            scale(m, pre);                                             // 80420B28 on the pre-scale
        mtx_translate(m, t);                                           // 80420D10 / 80420F20
        const bool rotates = (b.flags & 0x38u) != 0u || b.subkind != 6u;
        if ((b.flags & 0x40u) == 0u && rotates) {                      // 80420B9C / 80420DC0
            const QuantisedAngle qz = quantise_angle(r[2]), qy = quantise_angle(r[1]), qx = quantise_angle(r[0]);
            if (qz.apply)
                mtx_rotate_z(m, qz.radians);
            if (qy.apply)
                mtx_rotate_y(m, qy.radians);
            if (qx.apply)
                mtx_rotate_x(m, qx.radians);
        }
        scale(m, s);                                                   // 80420B28 / 80420D5C
        (*world)[u] = m;
        if (b.sibling >= 0)
            stack.push_back({b.sibling, item.parent});
        if (b.child >= 0)
            stack.push_back({b.child, item.bone});
    }
}

}  // namespace

std::vector<std::int32_t> Skeleton::preorder() const {
    std::vector<std::int32_t> order;
    std::vector<std::int32_t> stack;
    if (!bones.empty())
        stack.push_back(0);
    while (!stack.empty() && order.size() <= bones.size()) {
        const std::int32_t i = stack.back();
        stack.pop_back();
        order.push_back(i);
        const SkeletonBone& b = bones[static_cast<std::size_t>(i)];
        if (b.sibling >= 0)
            stack.push_back(b.sibling);
        if (b.child >= 0)
            stack.push_back(b.child);
    }
    return order;
}

bool parse_skeleton(const std::uint8_t* record, std::size_t size, std::uint32_t pointer_base, Skeleton* out) {
    *out = Skeleton{};
    if (size < kRecordBones || record[0] < 4u || record[1] != 1u)  // version >= 4, big-endian
        return false;
    const std::size_t count = be16(record, 0xA);
    if (count == 0 || size < kRecordBones + kBoneSize * count)
        return false;
    out->scale = bef(record, 0xC);
    out->model = text(record, size, 0x10, 32);
    // A child / sibling word is the pointer base plus the bone's offset from the record; 0 is none.
    const auto index_of = [&](std::uint32_t word) -> std::int32_t {
        if (word == 0u)
            return -1;
        if (word < pointer_base)
            return -2;
        const std::uint32_t at = word - pointer_base;
        if (at < kRecordBones || (at - kRecordBones) % kBoneSize != 0u || (at - kRecordBones) / kBoneSize >= count)
            return -2;
        return static_cast<std::int32_t>((at - kRecordBones) / kBoneSize);
    };
    out->bones.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t b = kRecordBones + kBoneSize * i;
        SkeletonBone& bone = out->bones[i];
        bone.id = be32(record, b);
        bone.kind = be16(record, b + 4);
        bone.flags = be16(record, b + 6);
        bone.subkind = be16(record, b + 0xC);
        for (std::size_t k = 0; k < 3; ++k) {
            bone.rest_t[k] = bef(record, b + 0x10 + 4u * k);
            bone.rest_r[k] = bef(record, b + 0x20 + 4u * k);
            bone.rest_s[k] = bef(record, b + 0x30 + 4u * k);
        }
        bone.child = index_of(be32(record, b + 0x48));
        bone.sibling = index_of(be32(record, b + 0x4C));
        if (bone.child == -2 || bone.sibling == -2)
            return false;
    }
    return out->preorder().size() == count;  // a tree reaching every bone once
}

bool parse_clip(const std::uint8_t* clip, std::size_t size, Clip* out) {
    *out = Clip{};
    if (size < kClipFirstChunk || clip[0] < 3u || clip[1] != 1u)  // version >= 3, big-endian
        return false;
    if ((clip[0xD] & 1u) == 0u)  // 32-bit keys: no clip uses them (MOTION_REPORT.md section 5)
        return false;
    out->start = static_cast<float>(be16(clip, 0x10));
    out->end = static_cast<float>(be16(clip, 0x12));
    out->model = text(clip, size, 0x14, 32);
    out->bytes.assign(clip, clip + size);
    // Chunks: {u32 size, s16 keys, s8 pad, s8 continuation}, the next at +size while the continuation is set.
    std::size_t at = kClipFirstChunk;
    for (;;) {
        if (at + 8u > size)
            return false;
        const std::uint32_t chunk = be32(clip, at);
        if (chunk < 8u || at + chunk > size)
            return false;
        out->chunks.push_back(static_cast<std::uint32_t>(at));
        if (clip[at + 7] == 0u)
            break;
        at += chunk;
    }
    return true;
}

void rest_pose(const Skeleton& skeleton, std::vector<Mtx34>* world) {
    if (skeleton.bones.empty()) {
        world->clear();
        return;
    }
    pose_tree(skeleton, [&](std::size_t u, Vec3& pre, Vec3& t, Vec3& r, Vec3& s) {
        const SkeletonBone& b = skeleton.bones[u];
        pre = {1.0f, 1.0f, 1.0f};
        t = b.rest_t;
        r = b.rest_r;
        s = b.rest_s;
    }, world);
}

struct SkeletonPose::State {
    struct Part {
        std::uint32_t bone = 0;
        int group = 0;                    // 0 pre-scale, 1 translation, 2 rotation, 3 scale
        std::size_t axis = 0;
        std::uint32_t chunk = 0;
        CurveCursor cursor;
    };
    struct BoneValues {
        Vec3 pre{1.0f, 1.0f, 1.0f}, t{}, r{}, s{};
    };
    const Skeleton* skeleton = nullptr;
    const Clip* clip = nullptr;
    std::vector<Part> parts;
    std::vector<BoneValues> values;
    float time = 0.0f;

    ChunkBytes chunk(std::uint32_t index) const {
        const std::uint32_t at = clip->chunks[index];
        return ChunkBytes{std::span<const std::uint8_t>(clip->bytes).subspan(at, be32(clip->bytes.data(), at))};
    }
};

SkeletonPose::SkeletonPose() : state_(std::make_unique<State>()) {}
SkeletonPose::~SkeletonPose() = default;
SkeletonPose::SkeletonPose(SkeletonPose&&) noexcept = default;
SkeletonPose& SkeletonPose::operator=(SkeletonPose&&) noexcept = default;

float SkeletonPose::time() const noexcept { return state_->time; }

bool SkeletonPose::bind(const Skeleton& skeleton, const Clip& clip) {
    State& st = *state_;
    st.skeleton = &skeleton;
    st.clip = &clip;
    st.parts.clear();
    st.values.assign(skeleton.bones.size(), State::BoneValues{});
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
        const SkeletonBone& b = skeleton.bones[i];
        st.values[i].t = b.rest_t;
        st.values[i].r = b.rest_r;
        st.values[i].s = b.rest_s;
    }
    const Timing timing{clip.start, clip.end, 1.0f, clip.start};
    std::uint32_t next = 0;
    bool ok = !skeleton.bones.empty();
    const auto consume = [&](std::uint32_t bone, int group, std::size_t axis) {
        if (next >= clip.chunks.size()) {
            ok = false;
            return;
        }
        State::Part p;
        p.bone = bone;
        p.group = group;
        p.axis = axis;
        p.chunk = next++;
        curve_init(p.cursor, p.chunk, st.chunk(p.chunk), timing);
        st.parts.push_back(p);
    };
    // The clip's model is the skeleton's own here: the posed bone is the clip bone.
    for (std::int32_t i : skeleton.preorder()) {
        if (!ok)
            break;
        const SkeletonBone& b = skeleton.bones[static_cast<std::size_t>(i)];
        const auto bone = static_cast<std::uint32_t>(i);
        if (b.flags & 0x400u)
            for (std::size_t k = 0; k < 3; ++k)
                consume(bone, 0, k);
        for (std::size_t k = 0; k < 3; ++k)
            if (b.flags & (1u << k))
                consume(bone, 1, k);
        if ((b.flags & 0x40u) == 0u)
            for (std::size_t k = 0; k < 3; ++k)
                if (b.flags & (8u << k))
                    consume(bone, 2, k);
        for (std::size_t k = 0; k < 3; ++k)
            if (b.flags & (0x80u << k))
                consume(bone, 3, k);
    }
    st.time = clip.start;
    return ok;
}

void SkeletonPose::advance(float factor) {
    State& st = *state_;
    const Clip& clip = *st.clip;
    float t = std::fmaf(1.0f, factor, st.time);
    bool wrapped = false;
    const float length = clip.end - clip.start;
    if (std::fabs(length) < 1e-4f) {
        t = clip.end;
    } else {
        while (t >= clip.end) {
            t = t - length;
            wrapped = true;
        }
    }
    st.time = t;
    if (!wrapped)
        return;
    const Timing timing{clip.start, clip.end, 1.0f, st.time};
    for (State::Part& p : st.parts)
        curve_rewind(p.cursor, st.chunk(p.chunk), timing);
}

void SkeletonPose::evaluate(std::vector<Mtx34>* world) {
    State& st = *state_;
    const Clip& clip = *st.clip;
    const Timing timing{clip.start, clip.end, 1.0f, st.time};
    for (State::Part& p : st.parts) {
        const float v = curve_value(p.cursor, st.chunk(p.chunk), timing);
        State::BoneValues& bv = st.values[p.bone];
        switch (p.group) {
            case 0: bv.pre[p.axis] = 1.0f / v; break;  // 80421358
            case 1: bv.t[p.axis] = v; break;           // 80421518 (bind scale -1: as is)
            case 2: bv.r[p.axis] = v; break;           // 8042148C
            default: bv.s[p.axis] = v; break;          // 804213FC
        }
    }
    pose_tree(*st.skeleton, [&](std::size_t u, Vec3& pre, Vec3& t, Vec3& r, Vec3& s) {
        const State::BoneValues& bv = st.values[u];
        pre = bv.pre;
        t = bv.t;
        r = bv.r;
        s = bv.s;
    }, world);
}

}  // namespace shadow::motion
