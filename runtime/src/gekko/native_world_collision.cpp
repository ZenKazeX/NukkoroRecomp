// The game's world collision queries (line pick, sphere query), native. What they replace and why:
// include/gekko/native_world_collision.h; the originals word by word: docs/engine/WORLD_COLLISION.md.

#include "gekko/native_world_collision.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__AVX2__) || defined(__FMA__)
#include <immintrin.h>
#endif

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_world_line.h"
#include "gekko/recomp.h"

namespace gekko {

namespace {

constexpr u32 kDispatcher = 0x80443AF4u;    // RpCollisionWorldForAllIntersections
constexpr u32 kLineHit = 0x80020DC0u;       // the line pick's callback (closest hit)
constexpr u32 kLineFilterLink = 0x80020E7Cu;     // its filter call's return address (bctrl at 0x80020E78)
constexpr u32 kSphereHit = 0x80020C24u;     // the sphere query's callback
constexpr u32 kSphereFilterLink = 0x80020CE0u;   // its filter call's return address (bctrl at 0x80020CDC)
constexpr u32 kResultList = 0x80020D40u;    // the result list (created on first use), called from 0x80020D08
constexpr u32 kResultListLink = 0x80020D0Cu;
constexpr u32 kResultBuild = 0x800201D4u;   // builds one hit's result objects, called from 0x80020D1C
constexpr u32 kResultBuildLink = 0x80020D20u;
constexpr u32 kHitTriangle = 0x80571CCCu;   // the line pick's closest triangle record
constexpr u32 kSectorCtor = 0x80441DF8u;    // collision plugin: world-sector constructor (r3 sector)
constexpr u32 kSectorDtor = 0x80441DA8u;    //                   destructor (r3 sector)
constexpr u32 kSectorRead = 0x80441ED0u;    //                   stream reader (r5 sector)
constexpr u32 kLineType = 1u;               // rpINTERSECTLINE
constexpr u32 kSphereType = 3u;             // rpINTERSECTSPHERE
constexpr u32 kMaxDepth = 200u;             // deeper trees are not mirrored (the original runs)
// The original frames, for guest code called from inside a query: the dispatcher's, the query's, the callback's.
constexpr u32 kDispatcherFrame = 0x30u, kLineFrame = 0x640u, kSphereFrame = 0x3D0u, kCallbackFrame = 0x40u;
constexpr u32 kLineRecord = 8u, kSphereRecord = 0x48u;  // the RpCollisionTriangle in the query's frame

// The originals' constants (.sdata2): the line's tolerance scale, determinant epsilon and edge slack; the cube
// inscribed in the sphere (0.57 ~ 1/sqrt 3).
const float kTolScale = bits_to_float(0x358637BDu);
const float kDetEps = bits_to_float(0x322BCC77u);
const float kEdge = bits_to_float(0x3727C5ACu);
const float kInscribed = bits_to_float(0x3F11EB85u);

// a * b + c with one rounding, as the original's fmadds; a * b - c as its fmsubs.
inline float fmadd(float a, float b, float c) noexcept {
#if defined(__AVX2__) || defined(__FMA__)
    return _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)));
#else
    return std::fma(a, b, c);
#endif
}
inline float fmsub(float a, float b, float c) noexcept { return fmadd(a, b, -c); }

inline float magnitude(float x) noexcept { return x >= 0.0f ? x : -x; }

// y first, then x, then z: the order both queries accumulate dot products in.
inline float dot_yxz(const float a[3], const float b[3]) noexcept {
    return fmadd(a[2], b[2], fmadd(a[0], b[0], a[1] * b[1]));
}

// The filters the game passes (the record's RpCollisionTriangle-pointer field +8 is always 0 on both paths: both
// callbacks build the record from all-zero templates, 0x804AB880 / 0x804AB894).
bool native_filter(u32 fn) noexcept {
    switch (fn) {
    case 0u: case 0x80099454u: case 0x801A658Cu: case 0x801C80C0u: case 0x801C80A4u: case 0x80222138u:
    case 0x8005B6C8u: case 0x8006DDA8u: case 0x800F9394u: case 0x801C0910u: case 0x803350F0u:
        return true;
    default:
        return false;
    }
}

// A tree child: count 0xFF = interior node `start`; otherwise a leaf of `count` entries from `start`.
struct Desc {
    u8 count;
    u16 start;
};

}  // namespace

// One world sector's collision data, mirrored.
struct NativeWorldCollision::Sector {
    u32 coll = 0, triangles = 0, vertices = 0;  // the guest pointers it was built from (rebuilt when they change)
    bool valid = true;
    float sup[3]{}, inf[3]{};  // the tree's bounding box
    Desc root{0u, 0u};
    struct Node {
        u32 axis;
        float lo, hi;  // the left child's upper bound and the right child's lower bound along the axis
        Desc child[2];
        u16 first;  // the subtree's contiguous entry range (leftmost leaf start, length to the rightmost leaf's end)
        u16 count;
    };
    std::vector<Node> nodes;
    struct Entry {
        float p0[3], p1[3], p2[3];  // the vertices at tri + 0, tri + 2, tri + 4
        float e1[3], e2[3];         // p2 - p0, p1 - p0
        u32 index;                  // the triangle's index in the sector
        u32 v0, v1, v2;             // their guest addresses
    };
    std::vector<Entry> entries;  // by tree entry (through the remap table)
};

// One world's plane-sector BSP, mirrored. A child >= 0 is a node, < 0 is ~(index into sectors).
struct NativeWorldCollision::World {
    u32 root_guest = 0;
    bool valid = true;
    s32 root = 0;
    struct Node {
        u32 axis;
        float lo, hi;
        s32 child[2];
    };
    std::vector<Node> nodes;
    std::vector<u32> sectors;
    std::vector<const Sector*> resolved;  // the sectors' mirrors (rebuilt in place, so the pointers stay valid)
};

namespace {

// RenderWare's table reciprocal square root (0x804833E4).
inline float table_rsqrt(float x, const std::vector<u32>& table) noexcept {
    u32 word = float_to_bits(x);
    if (word != 0u) {
        word += 0x800u;
        word = (((~word) >> 1) & 0x3FC00000u) + table[(word >> 12) & 0xFFFu];
    }
    return bits_to_float(word);
}

// The triangle's normal as both queries compute it: (p1 - p0) x (p2 - p0) with fused differences of products,
// scaled by the table reciprocal square root of its squared length (nz^2 + (nx^2 + ny^2)). Returns the squared length.
inline float triangle_normal(const NativeWorldCollision::Sector::Entry& e, const std::vector<u32>& table,
                             float n[3]) noexcept {
    const float ax = e.e2[0], ay = e.e2[1], az = e.e2[2];
    const float bx = e.e1[0], by = e.e1[1], bz = e.e1[2];
    const float nz = fmsub(ax, by, ay * bx);
    const float nx = fmsub(ay, bz, az * by);
    const float ny = fmsub(az, bx, ax * bz);
    const float len2 = nz * nz + (nx * nx + ny * ny);
    const float scale = table_rsqrt(len2, table);
    n[0] = nx * scale, n[1] = ny * scale, n[2] = nz * scale;
    return len2;
}

// The RpCollisionTriangle record: +0 normal, +0xC point (the first vertex), +0x18 index, +0x1C / +0x20 / +0x24 the
// vertices at tri + 0, + 2, + 4.
void write_record(Cpu& c, u32 at, const NativeWorldCollision::Sector::Entry& e, const float n[3]) {
    c.mem_write32(at + 0x00u, float_to_bits(n[0]));
    c.mem_write32(at + 0x04u, float_to_bits(n[1]));
    c.mem_write32(at + 0x08u, float_to_bits(n[2]));
    c.mem_write32(at + 0x0Cu, float_to_bits(e.p0[0]));
    c.mem_write32(at + 0x10u, float_to_bits(e.p0[1]));
    c.mem_write32(at + 0x14u, float_to_bits(e.p0[2]));
    c.mem_write32(at + 0x18u, e.index & 0xFFFFu);
    c.mem_write32(at + 0x1Cu, e.v0);
    c.mem_write32(at + 0x20u, e.v1);
    c.mem_write32(at + 0x24u, e.v2);
}

// RenderWare's sphere-triangle test (fn_8047C770): a separating-axis reject on the vertices relative to the centre
// (by their sign bits), the plane (normal as above; a zero-area triangle misses; the centre's plane distance within
// the radius), then a hit if a vertex is inside, a miss if the nearest vertex's Voronoi region excludes the triangle,
// and the three edges' closest points against the radius and the opposite vertex. On a hit n is the unit normal and
// dist the centre's signed distance to the plane.
bool sphere_triangle(const float C[3], float R, const NativeWorldCollision::Sector::Entry& e,
                     const std::vector<u32>& table, float n[3], float& dist) noexcept {
    const float a0[3] = {e.p0[0] - C[0], e.p0[1] - C[1], e.p0[2] - C[2]};
    const float a1[3] = {e.p1[0] - C[0], e.p1[1] - C[1], e.p1[2] - C[2]};
    const float a2[3] = {e.p2[0] - C[0], e.p2[1] - C[1], e.p2[2] - C[2]};
    const auto sign = [](float f) { return static_cast<s32>(float_to_bits(f)) >> 31; };
    for (u32 k = 0; k < 3u; ++k) {
        const s32 sum = sign(a0[k]) + sign(a1[k]) + sign(a2[k]);
        if (sum == 0) {  // all on the positive side: a miss when all are beyond +R
            const u32 any = float_to_bits(a0[k] - R) | float_to_bits(a1[k] - R) | float_to_bits(a2[k] - R);
            if (static_cast<s32>(any) >= 0)
                return false;
        } else if (sum == -3) {  // all negative: a miss when all are beyond -R
            const u32 all = float_to_bits(a0[k] + R) & float_to_bits(a1[k] + R) & float_to_bits(a2[k] + R);
            if (static_cast<s32>(all) < 0)
                return false;
        }
    }
    const float ex = e.e2[0], ey = e.e2[1], ez = e.e2[2];  // p1 - p0
    const float fx = e.e1[0], fy = e.e1[1], fz = e.e1[2];  // p2 - p0
    float nx = fmsub(ey, fz, ez * fy), ny = fmsub(ez, fx, ex * fz), nz = fmsub(ex, fy, ey * fx);
    const float len2 = nz * nz + (nx * nx + ny * ny);
    if (len2 <= 0.0f)
        return false;
    const float scale = table_rsqrt(len2, table);
    nx *= scale, ny *= scale, nz *= scale;
    n[0] = nx, n[1] = ny, n[2] = nz;
    const float d = fmadd(a0[2], nz, fmadd(a0[0], nx, a0[1] * ny));
    if (d < -R || d > R)
        return false;
    dist = -d;
    const float R2 = R * R;
    const float m0 = a0[2] * a0[2] + (a0[0] * a0[0] + a0[1] * a0[1]);
    if (m0 <= R2)
        return true;
    const float m1 = a1[2] * a1[2] + (a1[0] * a1[0] + a1[1] * a1[1]);
    if (m1 <= R2)
        return true;
    const float m2 = a2[2] * a2[2] + (a2[0] * a2[0] + a2[1] * a2[1]);
    if (m2 <= R2)
        return true;
    // The nearest vertex: the triangle is outside the sphere when both edges leave it away from the centre.
    const float* near_v;
    const float *o1, *o2;
    float m;
    if (!(m0 >= m1)) {
        if (!(m2 >= m0))
            near_v = a2, o1 = a0, o2 = a1, m = m2;
        else
            near_v = a0, o1 = a1, o2 = a2, m = m0;
    } else {
        if (!(m2 >= m1))
            near_v = a2, o1 = a0, o2 = a1, m = m2;
        else
            near_v = a1, o1 = a0, o2 = a2, m = m1;
    }
    if (dot_yxz(near_v, o1) > m && dot_yxz(near_v, o2) > m)
        return false;
    // Each edge a -> b: the closest point of its line to the centre, p = a - d (d.a / d.d).
    const auto edge = [&](const float a[3], const float b[3], const float opp[3]) {
        const float dd[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float t = dot_yxz(dd, a) / dot_yxz(dd, dd);
        const float p[3] = {a[0] - dd[0] * t, a[1] - dd[1] * t, a[2] - dd[2] * t};
        const float pp = dot_yxz(p, p);
        return !(pp > R2) || pp >= dot_yxz(p, opp);
    };
    return edge(a0, a1, a2) && edge(a1, a2, a0) && edge(a2, a0, a1);
}

}  // namespace

NativeWorldCollision::NativeWorldCollision(Cpu& cpu, NativeWorldLine* world_line) noexcept
    : cpu_(cpu), world_line_(world_line) {
#if !defined(GEKKO_SHIPPING)
    if (const char* v = std::getenv("GEKKO_NATIVE_LINE_PICK"); v != nullptr && v[0] == '0')
        lines_ = false;
    if (const char* v = std::getenv("GEKKO_NATIVE_SPHERE_QUERY"); v != nullptr && v[0] == '0')
        spheres_ = false;
    if (const char* v = std::getenv("GEKKO_VERIFY_WORLD_COLLISION"); v != nullptr && v[0] != '\0')
        verify_every_ = std::strtoull(v, nullptr, 10);
    census_ = std::getenv("GEKKO_WORLD_COLLISION_STATS") != nullptr;
    if (const char* v = std::getenv("GEKKO_WORLD_COLLISION_TRACE"); v != nullptr)
        trace_left_ = static_cast<u32>(std::strtoul(v, nullptr, 10));
#endif
}

NativeWorldCollision::~NativeWorldCollision() {
    if (stats_.calls == 0u || (verify_every_ == 0u && std::getenv("GEKKO_WORLD_COLLISION_STATS") == nullptr))
        return;
    const auto n = [](u64 v) { return static_cast<unsigned long long>(v); };
    std::fprintf(stderr,
                 "[world-collision] calls %llu lines %llu spheres %llu declined %llu sectors %llu triangles %llu hits %llu"
                 " accepted %llu guest-filters %llu | mirrored %llu worlds %llu dropped %llu | verified %llu differed %llu"
                 " unverifiable %llu\n",
                 n(stats_.calls), n(stats_.lines), n(stats_.spheres), n(stats_.declined), n(stats_.sectors),
                 n(stats_.triangles), n(stats_.hits), n(stats_.accepted), n(stats_.guest_filters), n(stats_.mirrored),
                 n(stats_.worlds), n(stats_.dropped), n(stats_.verified), n(stats_.differed), n(stats_.unverifiable));
    for (const auto& [fn, hits] : filters_seen_)
        std::fprintf(stderr, "[world-collision] filter %08X hits %llu%s\n", fn, n(hits),
                     native_filter(fn) ? "" : " (guest)");
}

void NativeWorldCollision::install() {
    if (!lines_ && !spheres_)
        return;
    cpu_.install_hle(kDispatcher, [this](Cpu& c) { return serve(c); }, "world collision (line pick, sphere), native");
    // The plugin's lifecycle: drop the mirror, then the original runs.
    cpu_.install_hle(kSectorCtor, [this](Cpu& c) { drop_sector(c.gpr[3]); return false; }, "collision sector ctor (mirror drop)");
    cpu_.install_hle(kSectorDtor, [this](Cpu& c) { drop_sector(c.gpr[3]); return false; }, "collision sector dtor (mirror drop)");
    cpu_.install_hle(kSectorRead, [this](Cpu& c) { drop_sector(c.gpr[5]); return false; }, "collision sector read (mirror drop)");
}

void NativeWorldCollision::drop_sector(u32 sector) {
    bool any = sectors_.erase(sector) != 0u;
    last_world_ = nullptr;
    if (const auto it = sector_worlds_.find(sector); it != sector_worlds_.end()) {
        for (const u32 world : it->second)
            any = worlds_.erase(world) != 0u || any;
        sector_worlds_.erase(it);
    }
    if (any)
        ++stats_.dropped;
}

const NativeWorldCollision::Sector* NativeWorldCollision::sector_mirror(Cpu& c, u32 sector, u32 plugin) {
    const u32 coll = c.mem_read32(sector + plugin);
    const u32 triangles = c.mem_read32(sector + 4u), vertices = c.mem_read32(sector + 8u);
    auto& slot = sectors_[sector];
    if (slot != nullptr && slot->coll == coll && slot->triangles == triangles && slot->vertices == vertices)
        return slot.get();
    ++stats_.mirrored;
    if (slot == nullptr)
        slot = std::make_unique<Sector>();
    else
        *slot = Sector{};  // in place: world mirrors hold this pointer
    Sector& s = *slot;
    s.coll = coll, s.triangles = triangles, s.vertices = vertices;
    if (coll == 0u)
        return &s;
    // RpCollTree: bbox sup +4, inf +0x10; numEntries u16 +0x1C, numSplits u16 +0x1E, splits +0x20 (16 bytes: left,
    // right RpCollSector), map +0x24. The originals' root: an interior node 0 when there are splits, else a leaf of
    // numEntries' low byte.
    for (u32 k = 0; k < 3u; ++k) {
        s.sup[k] = bits_to_float(c.mem_read32(coll + 4u + 4u * k));
        s.inf[k] = bits_to_float(c.mem_read32(coll + 0x10u + 4u * k));
    }
    const u32 entries = c.mem_read16(coll + 0x1Cu), splits = c.mem_read16(coll + 0x1Eu);
    const u32 split_at = c.mem_read32(coll + 0x20u), map = c.mem_read32(coll + 0x24u);
    s.root = splits != 0u ? Desc{0xFFu, 0u} : Desc{static_cast<u8>(entries), 0u};
    s.nodes.resize(splits);
    for (u32 i = 0; i < splits; ++i) {
        const u32 at = split_at + 16u * i;
        Sector::Node& n = s.nodes[i];
        n.axis = (c.mem_read8(at) & 0x0Cu) >> 2;
        n.lo = bits_to_float(c.mem_read32(at + 4u));
        n.hi = bits_to_float(c.mem_read32(at + 0xCu));
        n.child[0] = {c.mem_read8(at + 1u), c.mem_read16(at + 2u)};
        n.child[1] = {c.mem_read8(at + 9u), c.mem_read16(at + 0xAu)};
        if (n.axis > 2u)
            s.valid = false;
    }
    // Every reachable leaf range must lie inside the entries and every interior index inside the splits.
    std::vector<std::pair<Desc, u32>> walk{{s.root, 0u}};
    std::vector<u8> seen(splits, 0u);
    while (!walk.empty() && s.valid) {
        const auto [d, depth] = walk.back();
        walk.pop_back();
        if (depth > kMaxDepth) {
            s.valid = false;
        } else if (d.count == 0xFFu) {
            if (d.start >= splits) {
                s.valid = false;
            } else if (seen[d.start] == 0u) {
                seen[d.start] = 1u;
                walk.push_back({s.nodes[d.start].child[0], depth + 1u});
                walk.push_back({s.nodes[d.start].child[1], depth + 1u});
            }
        } else if (static_cast<u32>(d.start) + d.count > entries) {
            s.valid = false;
        }
    }
    // Each node's whole-subtree range, as the sphere query takes it: the leftmost leaf's start, to the rightmost
    // leaf's end (16 bits).
    for (u32 i = 0; i < splits && s.valid; ++i) {
        Desc l = s.nodes[i].child[0], r = s.nodes[i].child[1];
        for (u32 steps = 0; l.count == 0xFFu && s.valid; ++steps)
            l = s.nodes[l.start].child[0], s.valid = steps < splits;
        for (u32 steps = 0; r.count == 0xFFu && s.valid; ++steps)
            r = s.nodes[r.start].child[1], s.valid = steps < splits;
        s.nodes[i].first = l.start;
        s.nodes[i].count = static_cast<u16>(r.start + r.count - l.start);
        if (static_cast<u32>(s.nodes[i].first) + s.nodes[i].count > entries)
            s.valid = false;
    }
    s.entries.resize(entries);
    for (u32 r = 0; r < entries && s.valid; ++r) {
        Sector::Entry& e = s.entries[r];
        e.index = map != 0u ? c.mem_read16(map + 2u * r) : r;
        const u32 tri = triangles + 8u * (e.index & 0xFFFFu);
        e.v0 = vertices + 0xCu * c.mem_read16(tri);
        e.v1 = vertices + 0xCu * c.mem_read16(tri + 2u);
        e.v2 = vertices + 0xCu * c.mem_read16(tri + 4u);
        for (u32 k = 0; k < 3u; ++k) {
            e.p0[k] = bits_to_float(c.mem_read32(e.v0 + 4u * k));
            e.p1[k] = bits_to_float(c.mem_read32(e.v1 + 4u * k));
            e.p2[k] = bits_to_float(c.mem_read32(e.v2 + 4u * k));
            e.e1[k] = e.p2[k] - e.p0[k];
            e.e2[k] = e.p1[k] - e.p0[k];
        }
    }
    return &s;
}

const NativeWorldCollision::World* NativeWorldCollision::world_mirror(Cpu& c, u32 world, u32 plugin) {
    const u32 root = c.mem_read32(world + 0x1Cu);
    if (last_world_ != nullptr && last_world_at_ == world && last_world_->root_guest == root)
        return last_world_;
    auto& slot = worlds_[world];
    if (slot != nullptr && slot->root_guest == root) {
        last_world_at_ = world, last_world_ = slot.get();
        return slot.get();
    }
    ++stats_.worlds;
    slot = std::make_unique<World>();
    World& w = *slot;
    w.root_guest = root;
    std::unordered_map<u32, s32> index;  // guest node or sector -> child code
    // RpPlaneSector: type +0 (0 / 4 / 8 = x / y / z; negative = a world sector), left +8, right +0xC, the left
    // child's upper bound +0x10, the right child's lower bound +0x14.
    struct Pending {
        u32 guest;
        s32 parent;
        u32 side, depth;
    };
    std::vector<Pending> walk{{root, -1, 0u, 0u}};
    while (!walk.empty() && w.valid) {
        const Pending p = walk.back();
        walk.pop_back();
        s32 code;
        if (const auto it = index.find(p.guest); it != index.end()) {
            code = it->second;
        } else if (p.depth > kMaxDepth || p.guest == 0u) {
            w.valid = false;
            break;
        } else {
            const s32 type = static_cast<s32>(c.mem_read32(p.guest));
            if (type < 0) {
                code = ~static_cast<s32>(w.sectors.size());
                w.sectors.push_back(p.guest);
                sector_worlds_[p.guest].push_back(world);
            } else {
                if ((type & 3) != 0 || type > 8) {
                    w.valid = false;
                    break;
                }
                code = static_cast<s32>(w.nodes.size());
                w.nodes.push_back({static_cast<u32>(type) >> 2, bits_to_float(c.mem_read32(p.guest + 0x10u)),
                                   bits_to_float(c.mem_read32(p.guest + 0x14u)), {0, 0}});
                walk.push_back({c.mem_read32(p.guest + 8u), code, 0u, p.depth + 1u});
                walk.push_back({c.mem_read32(p.guest + 0xCu), code, 1u, p.depth + 1u});
            }
            index.emplace(p.guest, code);
        }
        if (p.parent < 0)
            w.root = code;
        else
            w.nodes[static_cast<u32>(p.parent)].child[p.side] = code;
    }
    for (const u32 sector : w.sectors) {
        w.resolved.push_back(sector_mirror(c, sector, plugin));
        if (!w.resolved.back()->valid)
            w.valid = false;
    }
    last_world_at_ = world, last_world_ = &w;
    return &w;
}

void NativeWorldCollision::load_rsqrt(Cpu& c) {
    // RenderWare's reciprocal square root table: *(*(r13 + 0x615C) + *(r13 + 0x6140) + 4), 4096 words.
    const u32 table = c.mem_read32(c.mem_read32(c.gpr[13] + 0x615Cu) + c.mem_read32(c.gpr[13] + 0x6140u) + 4u);
    if (table == rsqrt_table_)
        return;
    rsqrt_.resize(4096u);
    for (u32 i = 0; i < 4096u; ++i)
        rsqrt_[i] = c.mem_read32(table + 4u * i);
    rsqrt_table_ = table;
}

bool NativeWorldCollision::filter(Cpu& c, u32 fn, u32 data, u32 attribute, u32 record, u32 query_frame, u32 link) {
    switch (fn) {
    case 0u:
        return true;
    case 0x80099454u:  // the player: reject data[1], need data[0]
        return (attribute & c.mem_read32(data + 4u)) == 0u && (attribute & c.mem_read32(data)) != 0u;
    case 0x801A658Cu:
        return (attribute & 0x1AB3u) != 0u;
    case 0x801C80C0u:
        return (attribute & 0x00100108u) == 0u;
    case 0x801C80A4u:
        return (attribute & 0x00050008u) == 0u;
    case 0x80222138u:
    case 0x8005B6C8u:
        return (attribute & 0x000C0018u) == 0u;
    case 0x8006DDA8u:
        return (attribute & 0x00040000u) == 0u && (attribute & 0x1A93u) != 0u;
    case 0x800F9394u:
        return data != 0u;
    case 0x801C0910u:
        return (attribute & c.mem_read32(data)) != 0u;
    case 0x803350F0u:  // (Doom's Base objects) need data[2]
        return (attribute & c.mem_read32(data + 8u)) != 0u;
    default:
        break;
    }
    // Guest filter: the original's frames, the triangle record already in the query's frame, the filter's own
    // record {&record vertices, 0, 0, attribute, data} in the callback's.
    ++stats_.guest_filters;
    const u32 caller_sp = c.gpr[1];
    const u32 sp_d = caller_sp - kDispatcherFrame, sp_q = sp_d - query_frame, sp_cb = sp_q - kCallbackFrame;
    c.mem_write32(sp_d, caller_sp);
    c.mem_write32(sp_q, sp_d);
    c.mem_write32(sp_cb, sp_q);
    const u32 rec = sp_cb + 8u;
    c.mem_write32(rec + 0x0u, record + 0x1Cu);
    c.mem_write32(rec + 0x4u, 0u);
    c.mem_write32(rec + 0x8u, 0u);
    c.mem_write32(rec + 0xCu, attribute);
    c.mem_write32(rec + 0x10u, data);
    c.gpr[1] = sp_cb;
    c.gpr[3] = rec;
    c.gpr[12] = fn;
    c.ctr = fn;
    recomp::call_indirect(c, fn, link);
    if (c.exiting())
        c.finish_transfers();
    const bool accepted = (c.gpr[3] & 0xFFu) != 0u;
    c.gpr[1] = caller_sp;
    return accepted;
}

bool NativeWorldCollision::serve(Cpu& c) {
    if (reference_)
        return false;
    ++stats_.calls;
    const u32 type = c.mem_read32(c.gpr[4] + 0x18u);
    // GEKKO_WORLD_COLLISION_TRACE=<n>: the first n queries' geometry (RpIntersection: line start +0 / end +0xC,
    // sphere centre +0 / radius +0xC) - a diagnostic, it changes nothing (docs/SHADOW_PLAYER_TEST_STAGE.md).
    if (trace_left_ != 0u) {
        --trace_left_;
        float v[6];
        for (u32 k = 0; k < 6u; ++k) v[k] = bits_to_float(c.mem_read32(c.gpr[4] + 4u * k));
        std::fprintf(stderr, "[world-collision-trace] call %llu type %u lr %08X %g %g %g | %g %g %g\n",
                     static_cast<unsigned long long>(stats_.calls), type, c.lr, v[0], v[1], v[2], v[3], v[4], v[5]);
    }
    const bool line = lines_ && type == kLineType && c.gpr[5] == kLineHit;
    const bool sphere = spheres_ && type == kSphereType && c.gpr[5] == kSphereHit;
    if (!line && !sphere) {
        ++stats_.declined;
        return false;
    }
    if (verify_every_ != 0u && depth_ == 0u && stats_.calls % verify_every_ == 0u)
        return verify(c, sphere);
    return sphere ? run_sphere(c) : run_line(c);
}

bool NativeWorldCollision::run_line(Cpu& c) {
    const u32 world_at = c.gpr[3], line = c.gpr[4], data = c.gpr[6];
    const u32 plugin = c.mem_read32(c.gpr[13] + 0x5FC8u);
    const World* world = world_mirror(c, world_at, plugin);
    if (!world->valid) {
        ++stats_.declined;
        return false;
    }
    load_rsqrt(c);
    ++depth_;
    ++stats_.lines;
    const u32 caller_sp = c.gpr[1], link = c.lr;
    c.mem_write32(caller_sp + 4u, link);  // the dispatcher's LR save word in its caller's frame

    // The line: start S, delta D, inverse delta (0 for a zero component), tolerance 1e-6 x max(|start|, |end|).
    float S[3], D[3], inv[3], tol[3];
    for (u32 i = 0; i < 3u; ++i) {
        S[i] = bits_to_float(c.mem_read32(line + 4u * i));
        const float e = bits_to_float(c.mem_read32(line + 0xCu + 4u * i));
        D[i] = e - S[i];
        inv[i] = D[i] == 0.0f ? 0.0f : 1.0f / D[i];
        const float m = magnitude(S[i]) >= magnitude(e) ? magnitude(S[i]) : magnitude(e);
        tol[i] = kTolScale * m;
    }

    const u32 filter_fn = c.mem_read32(c.gpr[13] + 0x2BF0u), filter_data = c.mem_read32(c.gpr[13] + 0x2BF4u);
    const bool guest_filter = !native_filter(filter_fn);
    const u32 userdata = c.mem_read32(c.gpr[13] + 0x6008u);
    const u32 record_at = caller_sp - kDispatcherFrame - kLineFrame + kLineRecord;
    float t_min = bits_to_float(c.mem_read32(data));
    const Sector* best = nullptr;
    u32 best_entry = 0u, best_attribute = 0u;
    bool effects = false;  // guest code ran: the original takes no call over from here, and every improvement is
                           // published as the original does (guest code could read it)
    auto publish = [&](const Sector& s, u32 entry, u32 attribute) {
        float n[3];
        triangle_normal(s.entries[entry], rsqrt_, n);
        c.mem_write32(data, float_to_bits(t_min));
        write_record(c, kHitTriangle, s.entries[entry], n);
        c.mem_write32(c.gpr[13] + 0x2BECu, attribute);
    };

    struct Span {
        s32 node;
        float t0, t1;
    };
    Span ws[kMaxDepth + 2u];
    u32 wn = 0;
    ws[wn++] = {world->root, 0.0f, 1.0f};
    struct TSpan {
        Desc d;
        float t0, t1;
    };
    TSpan ts[kMaxDepth + 2u];

    // One clip step, shared by both trees: the interval [t0, t1] against the planes (lo + tol), (hi - tol).
    // Returns 0 / 1 for one side only, 2 for both (near child pushed with t1 clipped, the top made far with t0 clipped).
    auto clip = [&](u32 axis, float lo, float hi, float t0, float t1, u32& near_side, float& new_t1, float& new_t0) {
        const float d = D[axis], s = S[axis], k = tol[axis];
        const float exit = fmadd(t1, d, s), entry = fmadd(t0, d, s);
        const float upper = hi - k, lower = lo + k;
        new_t1 = t1, new_t0 = t0;
        if (d > 0.0f) {
            if (upper > exit)
                return 0u;
            if (lower < entry)
                return 1u;
            near_side = 0u;
            if (lower < exit)
                new_t1 = fmadd(inv[axis], lower - exit, t1);
            if (upper > entry)
                new_t0 = fmadd(inv[axis], upper - entry, t0);
        } else {
            if (lower < exit)
                return 1u;
            if (upper > entry)
                return 0u;
            near_side = 1u;
            if (upper > exit)
                new_t1 = fmadd(inv[axis], upper - exit, t1);
            if (lower < entry)
                new_t0 = fmadd(inv[axis], lower - entry, t0);
        }
        return 2u;
    };
    auto decline = [&]() {
        --depth_, --stats_.lines, ++stats_.declined;
        return false;
    };

    bool stopped = false;
    for (;;) {
        for (;;) {
            Span& top = ws[wn - 1u];
            if (top.node < 0)
                break;
            const World::Node& n = world->nodes[static_cast<u32>(top.node)];
            u32 near_side = 0u;
            float t1, t0;
            const u32 side = clip(n.axis, n.lo, n.hi, top.t0, top.t1, near_side, t1, t0);
            if (side < 2u) {
                top.node = n.child[side];
                continue;
            }
            const Span pushed{n.child[near_side], top.t0, t1};
            top.node = n.child[near_side ^ 1u];
            top.t0 = t0;
            ws[wn++] = pushed;
        }
        const Span leaf = ws[wn - 1u];
        const u32 slot = static_cast<u32>(~leaf.node);
        const u32 sector_at = world->sectors[slot];
        const Sector* cached = world->resolved[slot];
        // Re-check the guest pointers the mirror was built from; a change rebuilds it in place.
        const Sector& s = cached->coll == c.mem_read32(sector_at + plugin) &&
                                  cached->triangles == c.mem_read32(sector_at + 4u) &&
                                  cached->vertices == c.mem_read32(sector_at + 8u)
                              ? *cached
                              : *sector_mirror(c, sector_at, plugin);
        if (!s.valid) {
            // A sector rebuilt since the world was mirrored and rejected: nothing is published yet unless guest
            // code ran, so the original takes the whole call.
            if (!effects)
                return decline();
            std::fprintf(stderr, "[world-collision] sector %08X not mirrorable after guest code ran: skipped\n",
                         sector_at);
        } else if (s.coll != 0u) {
            ++stats_.sectors;
            // The sector's surface attributes: its first user-data array, when any array has data.
            const u32 ud = sector_at + userdata;
            const s32 arrays = static_cast<s32>(c.mem_read32(ud));
            const u32 array_at = c.mem_read32(ud + 4u);
            bool any = false;
            for (s32 i = 0; i < arrays && !any; ++i)
                any = c.mem_read32(array_at + 16u * static_cast<u32>(i) + 0xCu) != 0u;
            const u32 attributes = any ? c.mem_read32(array_at + 0xCu) : 0u;

            u32 tn = 0u;
            ts[tn++] = {s.root, leaf.t0, leaf.t1};
            for (;;) {
                TSpan& top = ts[tn - 1u];
                if (top.d.count == 0xFFu) {
                    const Sector::Node& n = s.nodes[top.d.start];
                    u32 near_side = 0u;
                    float t1, t0;
                    const u32 side = clip(n.axis, n.lo, n.hi, top.t0, top.t1, near_side, t1, t0);
                    if (side < 2u) {
                        top.d = n.child[side];
                        continue;
                    }
                    const TSpan pushed{n.child[near_side], top.t0, t1};
                    top.d = n.child[near_side ^ 1u];
                    top.t0 = t0;
                    if (tn > kMaxDepth) {  // deeper than validated (the tree shares nodes): the original runs
                        if (!effects)
                            return decline();
                        break;
                    }
                    ts[tn++] = pushed;
                    continue;
                }
                const u32 end = static_cast<u32>(top.d.start) + top.d.count;
                for (u32 r = top.d.start; r < end && !stopped; ++r) {
                    ++stats_.triangles;
                    const Sector::Entry& e = s.entries[r];
                    // The original's triangle test, operation for operation.
                    const float e1x = e.e1[0], e1y = e.e1[1], e1z = e.e1[2];
                    const float e2x = e.e2[0], e2y = e.e2[1], e2z = e.e2[2];
                    const float f23 = fmsub(D[2], e1x, D[0] * e1z);
                    const float f24 = fmsub(D[1], e1z, D[2] * e1y);
                    const float f22 = fmsub(D[0], e1y, D[1] * e1x);
                    float det = e2y * f23;
                    det = fmadd(e2x, f24, det);
                    det = fmadd(e2z, f22, det);
                    if (!(det > kDetEps))
                        continue;
                    const float sy = S[1] - e.p0[1], sx = S[0] - e.p0[0], sz = S[2] - e.p0[2];
                    const float lower = kEdge * -det, upper = det - lower;
                    const float u = fmadd(sz, f22, fmadd(sx, f24, sy * f23));
                    if (!(u >= lower) || !(u <= upper))
                        continue;
                    const float q13 = fmsub(sz, e2x, sx * e2z);
                    const float q6 = fmsub(sy, e2z, sz * e2y);
                    const float q5 = fmsub(sx, e2y, sy * e2x);
                    const float v = fmadd(D[2], q5, fmadd(D[0], q6, D[1] * q13));
                    if (!(v >= lower) || !(u + v <= upper))
                        continue;
                    float t = fmadd(e1z, q5, fmadd(e1x, q6, e1y * q13));
                    if (!(t >= lower) || !(t <= upper))
                        continue;
                    t = t / det;
                    ++stats_.hits;
                    if (census_)
                        ++filters_seen_[filter_fn];
                    const u32 attribute = any ? c.mem_read32(attributes + 4u * (e.index & 0xFFFFu)) : 0u;
                    if (guest_filter) {
                        // the original: the filter first (it sees the record), then the comparison
                        float n[3];
                        triangle_normal(e, rsqrt_, n);
                        write_record(c, record_at, e, n);
                        effects = true;
                        if (!filter(c, filter_fn, filter_data, attribute, record_at, kLineFrame, kLineFilterLink))
                            continue;
                        if (c.exiting()) {
                            stopped = true;
                            break;
                        }
                        t_min = bits_to_float(c.mem_read32(data));
                    } else if (!(t < t_min) ||
                               !filter(c, filter_fn, filter_data, attribute, 0u, kLineFrame, kLineFilterLink)) {
                        continue;
                    }
                    if (!(t < t_min))
                        continue;
                    ++stats_.accepted;
                    t_min = t;
                    best = &s, best_entry = r, best_attribute = attribute;
                    if (effects)
                        publish(s, r, attribute);
                }
                if (stopped || tn <= 1u)
                    break;
                --tn;
            }
        }
        if (stopped || wn <= 1u)
            break;
        --wn;
    }

    if (best != nullptr && !effects)
        publish(*best, best_entry, best_attribute);
    c.gpr[1] = caller_sp;
    c.gpr[3] = world_at;
    c.lr = link;
    --depth_;
    return true;
}

bool NativeWorldCollision::run_sphere(Cpu& c) {
    // The dispatcher passes the intersection as the sphere (centre +0, radius +0xC) and as the callback's r3.
    const u32 world_at = c.gpr[3], sphere = c.gpr[4], data = c.gpr[6];
    const u32 plugin = c.mem_read32(c.gpr[13] + 0x5FC8u);
    const World* world = world_mirror(c, world_at, plugin);
    if (!world->valid) {
        ++stats_.declined;
        return false;
    }
    load_rsqrt(c);
    ++depth_;
    ++stats_.spheres;
    const u32 caller_sp = c.gpr[1], link = c.lr;
    c.mem_write32(caller_sp + 4u, link);  // the dispatcher's LR save word in its caller's frame

    // The sphere's box (C +- R) and the cube inscribed in the sphere (C +- 0.57 R).
    float C[3];
    for (u32 i = 0; i < 3u; ++i)
        C[i] = bits_to_float(c.mem_read32(sphere + 4u * i));
    const float R = bits_to_float(c.mem_read32(sphere + 0xCu));
    const float inv_r = 1.0f / R, R2 = R * R, kr = kInscribed * R;
    float imax[3], imin[3], omax[3], omin[3];
    for (u32 i = 0; i < 3u; ++i) {
        imax[i] = C[i] + R, imin[i] = C[i] - R;
        omax[i] = C[i] + kr, omin[i] = C[i] - kr;
    }

    const u32 filter_fn = c.mem_read32(c.gpr[13] + 0x2BF0u), filter_data = c.mem_read32(c.gpr[13] + 0x2BF4u);
    const u32 userdata = c.mem_read32(c.gpr[13] + 0x6008u);
    const u32 sp_d = caller_sp - kDispatcherFrame, sp_q = sp_d - kSphereFrame, sp_cb = sp_q - kCallbackFrame;
    const u32 record_at = sp_q + kSphereRecord;
    bool effects = false;  // guest code ran: the original takes no call over from here
    bool stopped = false;
    auto decline = [&]() {
        --depth_, --stats_.spheres, ++stats_.declined;
        return false;
    };
    // Guest code called from the callback: the original's frames.
    auto call = [&](u32 target, u32 return_to) {
        c.mem_write32(sp_d, caller_sp);
        c.mem_write32(sp_q, sp_d);
        c.mem_write32(sp_cb, sp_q);
        c.gpr[1] = sp_cb;
        c.gpr[12] = target;
        recomp::call_indirect(c, target, return_to);
        if (c.exiting()) {
            c.finish_transfers();
            stopped = true;
        }
    };

    // One triangle: the test, then the callback 0x80020C24 (attribute, filter, behind-the-plane drop, result build).
    auto triangle = [&](const Sector& s, u32 sector_at, u32 r, bool inside, bool any, u32 attributes) {
        ++stats_.triangles;
        const Sector::Entry& e = s.entries[r];
        float n[3], dist;
        if (inside) {  // the node is inside the inscribed cube: every triangle touches, with its plane distance
            triangle_normal(e, rsqrt_, n);
            dist = fmadd(C[2] - e.p0[2], n[2], fmadd(C[0] - e.p0[0], n[0], (C[1] - e.p0[1]) * n[1]));
        } else if (!sphere_triangle(C, R, e, rsqrt_, n, dist)) {
            return;
        }
        dist = dist * inv_r;
        ++stats_.hits;
        if (census_)
            ++filters_seen_[filter_fn];
        const u32 attribute = any ? c.mem_read32(attributes + 4u * (e.index & 0xFFFFu)) : 0u;
        bool written = false;
        if (!native_filter(filter_fn)) {
            write_record(c, record_at, e, n);
            written = effects = true;
            if (!filter(c, filter_fn, filter_data, attribute, record_at, kSphereFrame, kSphereFilterLink))
                return;
            if (c.exiting()) {
                stopped = true;
                return;
            }
        } else if (!filter(c, filter_fn, filter_data, attribute, 0u, kSphereFrame, kSphereFilterLink)) {
            return;
        }
        if (c.mem_read32(c.gpr[13] + 0x2BE8u) == 0u && !(dist >= 0.0f))
            return;
        if (!written)
            write_record(c, record_at, e, n);
        effects = true;
        ++stats_.accepted;
        call(kResultList, kResultListLink);
        if (stopped)
            return;
        c.gpr[4] = sphere;
        c.gpr[5] = sector_at;
        c.gpr[6] = record_at;
        c.fpr[1].bits[0] = double_to_bits(static_cast<double>(dist));
        call(kResultBuild, kResultBuildLink);
    };

    s32 ws[kMaxDepth + 2u];
    u32 wn = 0;
    ws[wn++] = world->root;
    struct TEntry {
        Desc d;
        u32 flags;  // the sides (bit 2a: low, 2a + 1: high) along which the node may still leave the inscribed cube
    };
    TEntry ts[kMaxDepth + 2u];

    while (wn != 0u && !stopped) {
        for (;;) {
            s32& top = ws[wn - 1u];
            if (top < 0)
                break;
            const World::Node& n = world->nodes[static_cast<u32>(top)];
            if (n.hi > imax[n.axis]) {
                top = n.child[0];
                continue;
            }
            top = n.child[1];
            if (n.lo >= imin[n.axis]) {
                if (wn > kMaxDepth) {
                    if (!effects)
                        return decline();
                    break;
                }
                ws[wn++] = n.child[0];
            }
        }
        const s32 leaf = ws[wn - 1u];
        --wn;
        if (leaf >= 0)
            continue;  // (only after a depth overflow with guest effects)
        const u32 slot = static_cast<u32>(~leaf);
        const u32 sector_at = world->sectors[slot];
        const Sector* cached = world->resolved[slot];
        const Sector& s = cached->coll == c.mem_read32(sector_at + plugin) &&
                                  cached->triangles == c.mem_read32(sector_at + 4u) &&
                                  cached->vertices == c.mem_read32(sector_at + 8u)
                              ? *cached
                              : *sector_mirror(c, sector_at, plugin);
        if (!s.valid) {
            if (!effects)
                return decline();
            std::fprintf(stderr, "[world-collision] sector %08X not mirrorable after guest code ran: skipped\n",
                         sector_at);
            continue;
        }
        if (s.coll == 0u)
            continue;
        // The sphere against the tree's box: the squared distance from the centre, then the six overlaps.
        float d2 = 0.0f;
        for (u32 k = 0; k < 3u; ++k) {
            const float d = !(C[k] >= s.inf[k]) ? s.inf[k] - C[k] : (C[k] > s.sup[k] ? C[k] - s.sup[k] : 0.0f);
            d2 = k == 0u ? d * d : fmadd(d, d, d2);
        }
        if (!(d2 <= R2))
            continue;
        if (!(s.inf[0] <= imax[0] && s.inf[1] <= imax[1] && s.inf[2] <= imax[2] && s.sup[0] >= imin[0] &&
              s.sup[1] >= imin[1] && s.sup[2] >= imin[2]))
            continue;
        ++stats_.sectors;
        const u32 ud = sector_at + userdata;
        const s32 arrays = static_cast<s32>(c.mem_read32(ud));
        const u32 array_at = c.mem_read32(ud + 4u);
        bool any = false;
        for (s32 i = 0; i < arrays && !any; ++i)
            any = c.mem_read32(array_at + 16u * static_cast<u32>(i) + 0xCu) != 0u;
        const u32 attributes = any ? c.mem_read32(array_at + 0xCu) : 0u;

        u32 flags = 0u;
        for (u32 k = 0; k < 3u; ++k) {
            if (!(s.inf[k] >= omin[k]))
                flags |= 1u << (2u * k);
            if (s.sup[k] > omax[k])
                flags |= 2u << (2u * k);
        }
        u32 tn = 0u;
        ts[tn++] = {s.root, flags};
        while (tn != 0u && !stopped) {
            TEntry& top = ts[tn - 1u];
            if (top.d.count != 0xFFu || top.flags == 0u) {
                u32 first, end;
                if (top.d.count != 0xFFu) {
                    first = top.d.start, end = static_cast<u32>(top.d.start) + top.d.count;
                } else {
                    const Sector::Node& n = s.nodes[top.d.start];
                    first = n.first, end = static_cast<u32>(n.first) + n.count;
                }
                const bool inside = top.flags == 0u;
                --tn;
                for (u32 r = first; r < end && !stopped; ++r)
                    triangle(s, sector_at, r, inside, any, attributes);
                continue;
            }
            const Sector::Node& n = s.nodes[top.d.start];
            if (n.hi > imax[n.axis]) {
                top.d = n.child[0];
                continue;
            }
            top.d = n.child[1];
            if (!(n.lo >= imin[n.axis]))
                continue;
            if (tn > kMaxDepth) {
                if (!effects)
                    return decline();
                break;
            }
            TEntry& left = ts[tn++] = {n.child[0], top.flags};
            if (n.lo <= omax[n.axis])
                left.flags &= ~(2u << (2u * n.axis));
            if (n.hi >= omin[n.axis])
                ts[tn - 2u].flags &= ~(1u << (2u * n.axis));
        }
    }

    c.gpr[1] = caller_sp;
    c.gpr[3] = world_at;
    c.lr = link;
    (void)data;
    --depth_;
    return true;
}

// Every n-th call: the native run, rolled back, then the original from the same state; contract registers and every
// byte either run wrote outside the current thread's dead stack compared; the original's result kept.
bool NativeWorldCollision::verify(Cpu& c, bool sphere) {
    const RecompiledCode* code = c.recompiled();
    const RecompiledCode::Range* body = code != nullptr ? code->find_range(kDispatcher) : nullptr;
    GuestMemory& memory = c.guest_memory();
    if (body == nullptr || memory.journaling())
        return sphere ? run_sphere(c) : run_line(c);
    struct Regs {
        u32 gpr[32], lr, cr, fpscr;
        u64 fpr[32];
    };
    auto take = [&](Regs& r) {
        std::memcpy(r.gpr, c.gpr, sizeof(r.gpr));
        r.lr = c.lr, r.cr = c.cr, r.fpscr = c.fpscr;
        for (u32 i = 0; i < 32u; ++i)
            r.fpr[i] = c.fpr[i].bits[0];
    };
    auto put = [&](const Regs& r) {
        std::memcpy(c.gpr, r.gpr, sizeof(r.gpr));
        c.lr = r.lr, c.cr = r.cr, c.fpscr = r.fpscr;
        for (u32 i = 0; i < 32u; ++i)
            c.fpr[i].bits[0] = r.fpr[i];
    };
    Regs before, native, guest;
    take(before);
    const u32 pc = c.pc;
    const u32 caller_sp = before.gpr[1];
    const u32 thread = c.mem_read32(0x800000E4u);
    const u32 dead_lo = thread != 0u ? c.mem_read32(thread + 0x308u) : caller_sp;
    const auto live = [&](u32 a) { return a < dead_lo || a >= caller_sp; };

    memory.journal_begin();
    const bool served = sphere ? run_sphere(c) : run_line(c);
    take(native);
    std::unordered_map<u32, u8> native_bytes;
    bool unverifiable = !served || memory.journal_overflowed() || memory.journal_touched_device();
    for (const GuestMemory::JournalEntry& e : memory.journal())
        for (u32 i = 0; i < e.size; ++i)
            if (live(e.address + i))
                native_bytes[e.address + i] = c.mem_read8(e.address + i);
    memory.journal_rollback();
    memory.journal_end();
    put(before);

    memory.journal_begin();
    reference_ = true;
    if (world_line_ != nullptr)
        world_line_->set_reference(true);
    body->at(c, kDispatcher);
    c.finish_transfers();
    reference_ = false;
    if (world_line_ != nullptr)
        world_line_->set_reference(false);
    take(guest);
    std::unordered_map<u32, u8> guest_bytes, previous;
    unverifiable = unverifiable || memory.journal_overflowed() || memory.journal_touched_device();
    for (const GuestMemory::JournalEntry& e : memory.journal())
        for (u32 i = 0; i < e.size; ++i)
            if (live(e.address + i)) {
                guest_bytes[e.address + i] = c.mem_read8(e.address + i);
                previous.emplace(e.address + i, static_cast<u8>(e.previous >> (8u * (e.size - 1u - i))));
            }
    memory.journal_end();
    c.pc = pc;
    if (unverifiable || c.lr != before.lr || dead_lo > caller_sp) {
        ++stats_.unverifiable;
        return true;
    }
    ++stats_.verified;
    u32 first = 0u;
    bool differs = false;
    for (u32 i = 1; i < 32u && !differs; ++i)
        if ((i <= 3u || i >= 13u) && native.gpr[i] != guest.gpr[i])
            differs = true, first = i;
    for (u32 i = 14; i < 32u && !differs; ++i)
        if (native.fpr[i] != guest.fpr[i])
            differs = true, first = 100u + i;
    if (native.lr != guest.lr || ((native.cr ^ guest.cr) & 0x00FFF000u) != 0u ||
        ((native.fpscr ^ guest.fpscr) & 0xFFu) != 0u)
        differs = true;
    u32 byte_at = 0u;
    u8 nv = 0u, gv = 0u;
    for (const auto& [address, value] : native_bytes) {
        const auto g = guest_bytes.find(address);
        const u8 guest_value = g != guest_bytes.end() ? g->second : c.mem_read8(address);
        if (guest_value != value) {
            differs = true, byte_at = address, nv = value, gv = guest_value;
            break;
        }
    }
    for (const auto& [address, value] : guest_bytes)
        if (byte_at == 0u && native_bytes.count(address) == 0u && previous[address] != value) {
            differs = true, byte_at = address, nv = previous[address], gv = value;
            break;
        }
    if (differs && ++stats_.differed <= 16u)
        std::fprintf(stderr,
                     "[world-collision] native %s differs from the original: register %u, byte %08X native %02X"
                     " original %02X (r1 %08X filter %08X)\n",
                     sphere ? "sphere" : "line", first, byte_at, nv, gv, caller_sp,
                     c.mem_read32(c.gpr[13] + 0x2BF0u));
    return true;
}

}  // namespace gekko
