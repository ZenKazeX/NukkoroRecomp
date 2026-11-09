// Native tests of the RenderWare world reader (rpworld/src/world.cpp) for what the oracle (rpworld_world_tests) cannot
// cover:
// - converted-shape worlds read back exactly: every array, triangle (both orders), mesh index (narrowed to 16 bits),
//   material, plane and box against the bytes written; sector boxes against the planes; RpWorldSectorGetWorld and
//   RpWorldForAllWorldSectors (pre-order, stopping);
// - every deliberate difference (rwcore/README.md "Native contracts" -> Worlds): structs longer than the original's
//   stack buffers read into their size and the rest skipped (world 0x40, plane 0x18; the stream stays in step);
//   GameCube-native worlds refused; more than 8 texture coordinate sets refused; world totals below the tree's,
//   sector counts above 0xFFFF, sector struct chunks shorter than their header or their arrays, triangle vertex
//   indices outside the sector, plane types other than 0 / 4 / 8, a short prelit read, a negative material count, a
//   material index that is not an earlier entry, a failing material array, mesh counts above 0xFFFF, mesh materials
//   outside the list, mesh indices past the total, a sector without its mesh chunk, a sector whose world could not be
//   registered: each a clean failure (no leak, every plugin destructor matching a constructor); extra bytes in a
//   sector struct skipped; a second mesh chunk replacing (and freeing) the first; a 10,000-level BSP read, walked,
//   lit and destroyed (the original's walks have 64-entry stacks);
// - every truncation of a world and every allocation of its read failing in turn: no leak, no destructor on plugin
//   data never constructed (the original runs them), the next read succeeds;
// - world lights: ties against the mathematical definition (a positional light reaches an atomic sector when its
//   radius box overlaps the sector's plane-bounded region strictly) over 2,000 random lights; directional lights
//   untied; a world destroyed with lights in it (they leave it: the original leaves them pointing at the freed
//   world), a light added to a second world (moved; the original corrupts both lists), a light destroyed in a world
//   (it leaves the world's list), a tie allocation failing (E_RW_NOMEM; the original does not check).
// Memory is counted: every part ends with no live allocation.
//
// Usage: rpworld_world_native_tests
#include "rw/core/binary.h"
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/frame.h"
#include "rw/core/memory.h"
#include "rw/core/stream.h"
#include "rw/world/light.h"
#include "rw/world/material.h"
#include "rw/world/world.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
unsigned g_checks = 0;
void check(bool c, const std::string& what) {
    ++g_checks;
    require(c, what);
}

// ---- counted memory with failures on request ----------------------------------------------------------------------
std::map<void*, std::size_t> g_live;
long g_allocations = 0;   // RwMalloc / RwRealloc / RwCalloc calls so far
long g_failAt = -1;       // that call (by g_allocations) fails
u32 g_failHint = 0;       // or: the next call with this hint
bool shouldFail(RwUInt32 hint) {
    const bool byIndex = g_allocations++ == g_failAt;
    if (g_failHint != 0 && hint == g_failHint) {
        g_failHint = 0;
        return true;
    }
    return byIndex;
}
void* countMalloc(std::size_t size, RwUInt32 hint) {
    if (shouldFail(hint)) return nullptr;
    void* p = std::malloc(size ? size : 1);
    g_live[p] = size;
    return p;
}
void countFree(void* p) {
    if (p == nullptr) return;
    require(g_live.erase(p) == 1, "free of a pointer the allocator did not hand out");
    std::free(p);
}
void* countRealloc(void* p, std::size_t size, RwUInt32 hint) {
    if (p == nullptr) return countMalloc(size, hint);
    if (shouldFail(hint)) return nullptr;
    void* q = std::realloc(p, size ? size : 1);
    if (q != nullptr) g_live.erase(p), g_live[q] = size;
    return q;
}
void* countCalloc(std::size_t n, std::size_t size, RwUInt32 hint) {
    void* p = countMalloc(n * size, hint);
    if (p != nullptr) std::memset(p, 0, n * size);
    return p;
}
const RwMemoryFunctions kCounting{countMalloc, countFree, countRealloc, countCalloc};
RwError takeError() {
    RwError e{};
    RwErrorGet(&e);
    return e;
}

// ---- plugins that check their constructor ran before their destructor --------------------------------------------
constexpr u32 kConstructed = 0xC0FFEE00u;
RwInt32 g_worldOff = -1, g_sectorOff = -1;
int g_ctors = 0, g_dtors = 0;
void* ctor(void* o, RwInt32 offset, RwInt32) {
    auto* slot = reinterpret_cast<u32*>(static_cast<char*>(o) + offset);
    require(*slot == 0 || (*slot & 0xFFFFFF00u) != kConstructed, "a constructor on data already constructed");
    *slot = kConstructed;
    ++g_ctors;
    return o;
}
void* dtor(void* o, RwInt32 offset, RwInt32) {
    auto* slot = reinterpret_cast<u32*>(static_cast<char*>(o) + offset);
    require((*slot & 0xFFFFFF00u) == kConstructed, "a plugin destructor on data never constructed");
    *slot = 0;
    ++g_dtors;
    return o;
}
RwStream* pluginRead(RwStream* stream, RwInt32, void* object, RwInt32 offset, RwInt32) {
    auto* slot = reinterpret_cast<u32*>(static_cast<char*>(object) + offset);
    RwInt32 v = 0;
    if (RwStreamReadInt32(stream, &v, 4) == nullptr) return nullptr;
    *slot = kConstructed | (static_cast<u32>(v) & 0xFFu);
    return (static_cast<u32>(v) & 0xFF000000u) == 0xFA000000u ? nullptr : stream;
}

void startEngine() {
    RwEngineOpenParams params{nullptr};
    require(RwEngineInit(&kCounting, 0, 0) == 1 && RpWorldPluginAttach() == 1, "init");
    g_worldOff = RpWorldRegisterPlugin(4, 0x7303, ctor, dtor, nullptr);
    g_sectorOff = RpWorldSectorRegisterPlugin(4, 0x7302, ctor, dtor, nullptr);
    require(g_worldOff > 0 && g_sectorOff > 0 && RpWorldRegisterPluginStream(0x7303, pluginRead, nullptr, nullptr) >= 0 &&
                RpWorldSectorRegisterPluginStream(0x7302, pluginRead, nullptr, nullptr) >= 0,
            "plugins");
    require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "start");
}
void stopEngine(const std::string& what) {
    require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, what + ": shutdown");
    require(g_live.empty(), what + ": " + std::to_string(g_live.size()) + " allocations leaked");
    require(g_ctors == g_dtors, what + ": " + std::to_string(g_ctors) + " constructors, " + std::to_string(g_dtors) + " destructors");
}

// ---- streams --------------------------------------------------------------------------------------------------------
constexpr u32 libraryID(u32 v) { return (((v - 0x30000u) << 14) & 0xFFC00000u) | ((v & 0x3Fu) << 16) | 0x37u; }
constexpr u32 kMarker = 0x4D41524Bu;
struct Writer {
    std::vector<u8> b;
    std::vector<std::size_t> open;
    void put(u32 w) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(w >> (8 * i))); }
    void put16(u32 h) { b.push_back(static_cast<u8>(h)), b.push_back(static_cast<u8>(h >> 8)); }
    void putF(float f) { put(std::bit_cast<u32>(f)); }
    void begin(u32 type, u32 version = 0x36003u) { put(type), put(0), put(libraryID(version)), open.push_back(b.size()); }
    void end(int lengthDelta = 0) {
        const std::size_t start = open.back();
        open.pop_back();
        const u32 length = static_cast<u32>(static_cast<long>(b.size() - start) + lengthDelta);
        for (int i = 0; i < 4; ++i) b[start - 8 + i] = static_cast<u8>(length >> (8 * i));
    }
    void patch(std::size_t at, u32 w) { for (int i = 0; i < 4; ++i) b[at + i] = static_cast<u8>(w >> (8 * i)); }
};
struct Rng {
    std::uint64_t s = 0x5EC7012345678901ull;
    u32 next() {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        return static_cast<u32>(s >> 16);
    }
    float in(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(next() & 0xFFFFFF) / 16777216.0f; }
};

/// A world as written, to compare a read with.
struct SectorData {
    u32 nv = 0, nt = 0, window = 0;
    float tight[6] = {};   // inf, sup
    std::vector<u32> vertices, normals, prelit;
    std::vector<std::vector<u32>> uv;
    std::vector<u16> triangles;   // v0, v1, v2, matIndex
    u32 meshFlags = 0, meshTotal = 0;
    std::vector<std::vector<u32>> meshIndices;
    std::vector<u32> meshMaterial;
};
struct PlaneData {
    u32 type;
    float value, leftValue, rightValue;
};
struct Spec {
    u32 format = rpWORLDNORMALS | rpWORLDPRELIT | rpWORLDTEXTURED;
    u32 structLength = 0x40, structExtra = 0;
    u32 materials = 3;
    std::vector<RwInt32> indices;   // the material list's entries (default: every one read)
    RwInt32 listCount = std::numeric_limits<RwInt32>::min();   // the count written (default: the entries)
    u32 depth = 2;                  // a full tree of this depth (0: the root is an atomic sector)
    u32 chain = 0;                  // or a chain of this many planes, each with a leaf on its left
    u32 nv = 6, nt = 4, version = 0x36003u;
    bool zeroTotals = false;        // the world's vertex and triangle totals written as 0
    u32 planeExtra = 0, planeType = ~0u;         // the root plane's struct: bytes past 0x18, its type
    // the first sector only:
    u32 sectorExtra = 0;            // bytes in its struct chunk after the arrays
    int sectorLengthDelta = 0;      // its struct chunk's length written this much off
    u32 sectorLength = 0;           // or written as this
    u32 countField = 0;             // its numVertices field written as this (when non-zero)
    u32 windowField = 0;            // its matListWindowBase field written as this (when non-zero)
    bool meshNoRecords = false;     // its mesh chunk without mesh records (whatever count the header says)
    bool badTriangle = false;       // a vertex index = numVertices
    u32 meshChunks = 1;
    RwInt32 meshCount = -1, meshMaterial = -1;
    bool meshIndicesOver = false;   // a mesh's count past the total
    bool pluginChunks = true;
};
struct Built {
    std::vector<u8> bytes;
    std::vector<SectorData> sectors;   // pre-order
    std::vector<PlaneData> planes;     // pre-order
    std::vector<std::pair<std::size_t, std::size_t>> prelit;
    float box[6];                      // sup, inf
    float origin[3];
};
Built build(const Spec& s, Rng& rng) {
    Built out;
    Writer w;
    const u32 nTex = (s.format & 0x00FF0000u) ? (s.format >> 16) & 0xFFu : (s.format & 0x80u) ? 2u : (s.format >> 2) & 1u;
    for (u32 a = 0; a < 3; ++a) {
        out.box[3 + a] = rng.in(-1000, 0);
        out.box[a] = out.box[3 + a] + rng.in(100, 2000);
        out.origin[a] = rng.in(-50, 50);
    }
    w.begin(rwID_STRUCT);
    const std::size_t structAt = w.b.size();
    const u32 words[16] = {s.depth == 0 && s.chain == 0 ? 1u : 0u,
                           std::bit_cast<u32>(-out.origin[0]), std::bit_cast<u32>(-out.origin[1]), std::bit_cast<u32>(-out.origin[2]),
                           0, 0, 0, 0, 0, s.format,
                           std::bit_cast<u32>(out.box[0]), std::bit_cast<u32>(out.box[1]), std::bit_cast<u32>(out.box[2]),
                           std::bit_cast<u32>(out.box[3]), std::bit_cast<u32>(out.box[4]), std::bit_cast<u32>(out.box[5])};
    for (u32 i = 0; i < s.structLength; ++i) w.b.push_back(static_cast<u8>(words[i / 4] >> (8 * (i % 4))));
    for (u32 i = 0; i < s.structExtra; ++i) w.b.push_back(0xEE);
    w.end();
    // materials
    std::vector<RwInt32> indices = s.indices;
    if (indices.empty()) indices.assign(s.materials, -1);
    w.begin(rwID_MATLIST);
    w.begin(rwID_STRUCT);
    w.put(static_cast<u32>(s.listCount != std::numeric_limits<RwInt32>::min() ? s.listCount : static_cast<RwInt32>(indices.size())));
    for (RwInt32 i : indices) w.put(static_cast<u32>(i));
    w.end();
    for (RwInt32 i : indices) {
        if (i >= 0) continue;
        w.begin(rwID_MATERIAL);
        w.begin(rwID_STRUCT);
        w.put(0), w.put(0xFF8040C0u), w.put(0), w.put(0), w.putF(1.0f), w.putF(0.5f), w.putF(0.25f);
        w.end();
        w.begin(rwID_EXTENSION);
        w.end();
        w.end();
    }
    w.end();
    u32 planes = 0, sectors = 0, nvTotal = 0, ntTotal = 0;
    const auto atomic = [&](const float* inf, const float* sup) {
        const bool first = out.sectors.empty();
        SectorData d;
        d.nv = s.nv, d.nt = s.nv == 0 ? 0 : s.nt, d.window = rng.next() % 4u;
        for (u32 a = 0; a < 3; ++a) d.tight[a] = rng.in(inf[a], sup[a]), d.tight[3 + a] = rng.in(d.tight[a], sup[a]);
        w.begin(rwID_ATOMICSECT);
        w.begin(rwID_STRUCT, s.version);
        w.put(first && s.windowField ? s.windowField : d.window), w.put(d.nt), w.put(first && s.countField ? s.countField : d.nv);
        for (float f : d.tight) w.putF(f);
        w.put(0), w.put(0);
        for (u32 i = 0; i < 3 * d.nv; ++i) d.vertices.push_back(std::bit_cast<u32>(rng.in(inf[i % 3], sup[i % 3])));
        for (u32 v : d.vertices) w.put(v);
        if (d.nv && (s.format & rpWORLDNORMALS)) for (u32 i = 0; i < d.nv; ++i) d.normals.push_back(rng.next()), w.put(d.normals.back());
        if (d.nv && (s.format & rpWORLDPRELIT)) {
            const std::size_t at = w.b.size();
            for (u32 i = 0; i < d.nv; ++i) d.prelit.push_back(rng.next()), w.put(d.prelit.back());
            out.prelit.push_back({at, w.b.size()});
        }
        if (d.nv) {
            for (u32 t = 0; t < nTex; ++t) {
                d.uv.emplace_back();
                for (u32 i = 0; i < 2 * d.nv; ++i) d.uv.back().push_back(std::bit_cast<u32>(rng.in(-4, 4))), w.put(d.uv.back().back());
            }
        }
        for (u32 i = 0; i < d.nt; ++i) {
            u16 t[4] = {static_cast<u16>(rng.next() % d.nv), static_cast<u16>(rng.next() % d.nv), static_cast<u16>(rng.next() % d.nv),
                        static_cast<u16>(rng.next() % 4u)};
            if (first && s.badTriangle && i == d.nt - 1) t[1] = static_cast<u16>(d.nv);
            d.triangles.insert(d.triangles.end(), t, t + 4);
            if (s.version < 0x36002u) w.put16(t[3]), w.put16(t[0]), w.put16(t[1]), w.put16(t[2]);
            else w.put16(t[0]), w.put16(t[1]), w.put16(t[2]), w.put16(t[3]);
        }
        if (first) for (u32 i = 0; i < s.sectorExtra; ++i) w.b.push_back(0xDD);
        int delta = first ? s.sectorLengthDelta : 0;
        if (first && s.sectorLength != 0) delta = static_cast<int>(s.sectorLength) - static_cast<int>(w.b.size() - w.open.back());
        w.end(delta);
        w.begin(rwID_EXTENSION);
        if (s.pluginChunks) w.begin(0x7302), w.put(rng.next() & 0xFFu), w.end();
        for (u32 copy = 0; copy < (first ? s.meshChunks : 1u); ++copy) {
            w.begin(rwID_BINMESHPLUGIN);
            d.meshIndices.clear(), d.meshMaterial.clear();
            const u32 meshes = s.materials == 0 || (first && s.meshNoRecords) ? 0u : 1u + rng.next() % 3u;
            u32 total = 0;
            std::vector<std::vector<u32>> written(meshes);
            for (u32 k = 0; k < meshes; ++k) {
                written[k].resize(rng.next() % 12u);
                for (u32& x : written[k]) x = rng.next() % 3u == 0 ? rng.next() : rng.next() % std::max(d.nv, 1u);
                total += static_cast<u32>(written[k].size());
                d.meshMaterial.push_back(rng.next() % s.materials);
            }
            d.meshFlags = rng.next() % 2u, d.meshTotal = total;
            w.put(d.meshFlags);
            w.put(first && s.meshCount >= 0 ? static_cast<u32>(s.meshCount) : meshes);
            w.put(first && s.meshIndicesOver && total > 0 ? total - 1 : total);
            for (u32 k = 0; k < meshes; ++k) {
                w.put(static_cast<u32>(written[k].size())), w.put(first && s.meshMaterial >= 0 ? static_cast<u32>(s.meshMaterial) : d.meshMaterial[k]);
                for (u32 x : written[k]) w.put(x);
                std::vector<u32> narrowed;
                for (u32 x : written[k]) narrowed.push_back(x & 0xFFFFu);
                d.meshIndices.push_back(narrowed);
            }
            w.end();
        }
        w.end();
        w.end();
        out.sectors.push_back(d);
        ++sectors, nvTotal += d.nv, ntTotal += d.nt;
    };
    // one plane's chunk header and struct; its region split into left / right
    const auto plane = [&](const float* inf, const float* sup, bool leftLeaf, bool rightLeaf, float* linf, float* lsup, float* rinf,
                           float* rsup) {
        const bool root = out.planes.empty();
        PlaneData p;
        const u32 axis = rng.next() % 3u;
        p.type = root && s.planeType != ~0u ? s.planeType : axis * 4;
        p.value = rng.in(inf[axis], sup[axis]);
        const bool overlap = (s.format & rpWORLDSECTORSOVERLAP) != 0;
        const float lv = p.value + rng.in(0, 5), rv = p.value - rng.in(0, 5);
        p.leftValue = overlap ? lv : p.value, p.rightValue = overlap ? rv : p.value;
        out.planes.push_back(p);
        ++planes;
        w.begin(rwID_PLANESECT);
        w.begin(rwID_STRUCT);
        w.put(p.type), w.putF(p.value), w.put(leftLeaf ? 1u : 0u), w.put(rightLeaf ? 1u : 0u), w.putF(lv), w.putF(rv);
        if (root) for (u32 i = 0; i < s.planeExtra; ++i) w.b.push_back(0xCC);
        w.end();
        std::memcpy(linf, inf, 12), std::memcpy(lsup, sup, 12), std::memcpy(rinf, inf, 12), std::memcpy(rsup, sup, 12);
        lsup[axis] = p.leftValue, rinf[axis] = p.rightValue;
    };
    std::function<void(const float*, const float*, u32)> tree = [&](const float* inf, const float* sup, u32 level) {
        if (level >= s.depth) return atomic(inf, sup);
        float linf[3], lsup[3], rinf[3], rsup[3];
        plane(inf, sup, level + 1 >= s.depth, level + 1 >= s.depth, linf, lsup, rinf, rsup);
        tree(linf, lsup, level + 1);
        tree(rinf, rsup, level + 1);
        w.end();
    };
    float inf[3] = {out.box[3], out.box[4], out.box[5]}, sup[3] = {out.box[0], out.box[1], out.box[2]};
    if (s.chain != 0) {   // iteratively: a plane per level, its left a leaf, its right the next level
        for (u32 level = 0; level < s.chain; ++level) {
            float linf[3], lsup[3], rinf[3], rsup[3];
            plane(inf, sup, true, level + 1 == s.chain, linf, lsup, rinf, rsup);
            atomic(linf, lsup);
            std::memcpy(inf, rinf, 12), std::memcpy(sup, rsup, 12);
        }
        atomic(inf, sup);
        for (u32 level = 0; level < s.chain; ++level) w.end();
    } else {
        tree(inf, sup, 0);
    }
    w.begin(rwID_EXTENSION);
    if (s.pluginChunks) w.begin(0x7303), w.put(0x5Au), w.end();
    w.end();
    if (s.structLength >= 0x20) {
        w.patch(structAt + 0x10, s.zeroTotals ? 0u : ntTotal);
        w.patch(structAt + 0x14, s.zeroTotals ? 0u : nvTotal);
        w.patch(structAt + 0x18, planes), w.patch(structAt + 0x1C, sectors);
    }
    out.bytes = w.b;
    return out;
}

/// The world read from the bytes (with a marker after them: inStep says the reader stopped right before it).
struct ReadResult {
    RpWorld* world;
    bool inStep;
};
ReadResult readWorld(std::vector<u8> bytes, bool marker = true) {
    if (marker) for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<u8>(kMarker >> (8 * i)));
    RwMemory memory{bytes.data(), bytes.size()};
    RwStream* st = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory);
    if (st == nullptr) return {nullptr, false};   // (an allocation failing on purpose)
    RpWorld* w = RpWorldStreamRead(st);
    RwUInt32 m = 0;
    const bool inStep = marker && RwStreamRead(st, &m, 4) == 4 && m == kMarker;
    RwStreamClose(st, nullptr);
    return {w, inStep};
}

/// Every atomic sector of a tree in pre-order, with the region its planes bound (infinite where no plane bounds it).
struct Leaf {
    const RpWorldSector* s;
    float inf[3], sup[3];   // from the planes only
    RwBBox box;             // from the world's box and the planes
};
std::vector<Leaf> leaves(const RpWorld* w) {
    std::vector<Leaf> out;
    struct Item {
        const RpSector* s;
        Leaf region;
    };
    Leaf start{};
    for (int a = 0; a < 3; ++a) start.inf[a] = -std::numeric_limits<float>::infinity(), start.sup[a] = std::numeric_limits<float>::infinity();
    start.box = w->boundingBox;
    std::vector<Item> stack{{w->rootSector, start}};
    while (!stack.empty()) {
        Item it = stack.back();
        stack.pop_back();
        if (it.s->type < 0) {
            it.region.s = reinterpret_cast<const RpWorldSector*>(it.s);
            out.push_back(it.region);
            continue;
        }
        const auto* p = reinterpret_cast<const RpPlaneSector*>(it.s);
        const int a = p->type / 4;
        Item l = it, r = it;
        l.s = p->leftSubTree, r.s = p->rightSubTree;
        l.region.sup[a] = p->leftValue, (&l.region.box.sup.x)[a] = p->leftValue;
        r.region.inf[a] = p->rightValue, (&r.region.box.inf.x)[a] = p->rightValue;
        stack.push_back(r), stack.push_back(l);
    }
    return out;
}

bool sameWords(const void* p, const std::vector<u32>& words) { return words.empty() || std::memcmp(p, words.data(), words.size() * 4) == 0; }

/// The world read against what was written.
void checkWorld(const RpWorld* w, const Built& b, const Spec& s, const std::string& what) {
    check(w->object.type == rpWORLD && w->object.privateFlags == 1 && w->flags == s.format && w->renderOrder == 2 &&
              w->numClumpsInWorld == 0 && w->currentClumpLink == &w->clumpList.link && w->renderCallBack == nullptr && w->pipeline == nullptr,
          what + ": world fields");
    check(w->worldOrigin.x == b.origin[0] && w->worldOrigin.y == b.origin[1] && w->worldOrigin.z == b.origin[2] &&
              std::memcmp(&w->boundingBox, b.box, 24) == 0,
          what + ": origin and box");
    std::vector<RwInt32> indices = s.indices;
    if (indices.empty()) indices.assign(s.materials, -1);
    check(w->matList.numMaterials == static_cast<RwInt32>(indices.size()) && w->matList.space == w->matList.numMaterials, what + ": materials");
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const RpMaterial* m = w->matList.materials[i];
        check(indices[i] < 0 ? m != nullptr : m == w->matList.materials[indices[i]], what + ": a material entry");
        int refs = 0;
        for (RwInt32 k = 0; k < w->matList.numMaterials; ++k) refs += w->matList.materials[k] == m;
        check(m->refCount == refs && m->color.red == 0xC0 && m->color.alpha == 0xFF && m->surfaceProps.diffuse == 0.25f,
              what + ": a material's references (one per entry) and fields");
    }
    const std::vector<Leaf> ls = leaves(w);
    check(ls.size() == b.sectors.size(), what + ": sector count");
    const u32 nTex = static_cast<u32>(w->numTexCoordSets);
    for (std::size_t i = 0; i < ls.size(); ++i) {
        const RpWorldSector& sec = *ls[i].s;
        const SectorData& d = b.sectors[i];
        const std::string sw = what + ": sector " + std::to_string(i);
        check(sec.type == rwSECTORATOMIC && sec.numVertices == d.nv && sec.numTriangles == d.nt && sec.matListWindowBase == d.window &&
                  sec.repEntry == nullptr && sec.pipeline == nullptr,
              sw + " fields");
        check(std::memcmp(&sec.tightBoundingBox.inf, d.tight, 12) == 0 && std::memcmp(&sec.tightBoundingBox.sup, d.tight + 3, 12) == 0,
              sw + " tight box");
        check(std::memcmp(&sec.boundingBox, &ls[i].box, sizeof(RwBBox)) == 0, sw + " box from the planes");
        check((d.nv == 0) == (sec.vertices == nullptr) && sameWords(sec.vertices, d.vertices), sw + " vertices");
        check((d.normals.empty()) == (sec.normals == nullptr) && sameWords(sec.normals, d.normals), sw + " normals");
        check((d.prelit.empty()) == (sec.preLitLum == nullptr) && sameWords(sec.preLitLum, d.prelit), sw + " prelit");
        for (u32 t = 0; t < 8; ++t) {
            const bool present = d.nv != 0 && t < nTex;
            check(present == (sec.texCoords[t] != nullptr) && (!present || sameWords(sec.texCoords[t], d.uv[t])), sw + " texture coordinates");
        }
        check((d.nt == 0) == (sec.triangles == nullptr) &&
                  (d.nt == 0 || std::memcmp(sec.triangles, d.triangles.data(), d.triangles.size() * 2) == 0),
              sw + " triangles (in the 3.6.0.2 order)");
        for (u32 t = 0; t < d.nt; ++t) {
            check(reinterpret_cast<std::uintptr_t>(&sec.triangles[t]) % alignof(RpTriangle) == 0, sw + " aligned triangles");
        }
        check(reinterpret_cast<std::uintptr_t>(&sec) % 16 == 0 && (sec.vertices == nullptr || reinterpret_cast<std::uintptr_t>(sec.vertices) % 8 == 0),
              sw + " aligned in the block");
        const RpMeshHeader* h = sec.mesh;
        check(h != nullptr && h->flags == d.meshFlags && h->numMeshes == d.meshIndices.size() && h->totalIndicesInMesh == d.meshTotal &&
                  h->firstMeshOffset == 0,
              sw + " mesh header");
        const RpMesh* meshes = RpMeshHeaderGetMeshes(h);
        const RwUInt16* next = reinterpret_cast<const RwUInt16*>(meshes + h->numMeshes);
        for (std::size_t k = 0; k < d.meshIndices.size(); ++k) {
            check(meshes[k].indices == next && meshes[k].numIndices == d.meshIndices[k].size() &&
                      meshes[k].material == w->matList.materials[d.meshMaterial[k]],
                  sw + " mesh");
            for (std::size_t x = 0; x < d.meshIndices[k].size(); ++x) check(meshes[k].indices[x] == d.meshIndices[k][x], sw + " mesh index");
            next += d.meshIndices[k].size();
        }
        check(RpWorldSectorGetWorld(&sec) == w, sw + " RpWorldSectorGetWorld");
    }
    // planes in pre-order
    std::vector<const RpPlaneSector*> ps;
    std::vector<const RpSector*> stack{w->rootSector};
    while (!stack.empty()) {
        const RpSector* x = stack.back();
        stack.pop_back();
        if (x->type < 0) continue;
        const auto* p = reinterpret_cast<const RpPlaneSector*>(x);
        ps.push_back(p);
        stack.push_back(p->rightSubTree), stack.push_back(p->leftSubTree);
    }
    check(RpWorldSectorGetWorld(reinterpret_cast<const RpWorldSector*>(&b)) == nullptr, what + ": no world for an address in none");
    check(ps.size() == b.planes.size(), what + ": plane count");
    for (std::size_t i = 0; i < ps.size(); ++i) {
        check(ps[i]->type == static_cast<RwInt32>(b.planes[i].type) && ps[i]->value == b.planes[i].value &&
                  ps[i]->leftValue == b.planes[i].leftValue && ps[i]->rightValue == b.planes[i].rightValue,
              what + ": plane " + std::to_string(i));
    }
    // the walk: pre-order, stopping where the callback says
    static std::vector<const RpWorldSector*> visited;
    static std::size_t stopAt;
    for (std::size_t stop : {std::size_t{0}, std::size_t{1}, ls.size() / 2 + 1}) {
        visited.clear(), stopAt = stop;
        RpWorldForAllWorldSectors(const_cast<RpWorld*>(w), [](RpWorldSector* x, void*) -> RpWorldSector* {
            visited.push_back(x);
            return visited.size() == stopAt ? nullptr : x;
        }, nullptr);
        const std::size_t expect = stop == 0 ? ls.size() : std::min(stop, ls.size());
        bool order = visited.size() == expect;
        for (std::size_t i = 0; order && i < visited.size(); ++i) order = visited[i] == ls[i].s;
        check(order, what + ": RpWorldForAllWorldSectors visits in pre-order and stops");
    }
}

/// A read that must fail cleanly: NULL, nothing left allocated beyond what lived before, constructors = destructors.
void failsCleanly(const std::vector<u8>& bytes, const std::string& what, RwInt32 error = 0) {
    const std::size_t live = g_live.size();
    const int constructed = g_ctors - g_dtors;   // objects alive before
    takeError();
    const ReadResult r = readWorld(bytes, false);
    const RwError e = takeError();
    check(r.world == nullptr, what + ": the read fails");
    // (a free list may give back a block that emptied: fewer is fine)
    check(g_live.size() <= live, what + ": nothing leaks (" + std::to_string(g_live.size() - live) + " allocations)");
    check(g_ctors - g_dtors == constructed, what + ": every plugin destructor matches a constructor");
    if (error != 0) {
        char got[64];
        std::snprintf(got, sizeof got, " (%08X/%08X)", static_cast<u32>(e.pluginID), static_cast<u32>(e.errorCode));
        check(e.errorCode == error && e.pluginID == 0, what + ": the error" + got);   // a common code: held with pluginID 0
    }
}

RpLight* makeLight(RwInt32 type, RwFrame** frame) {
    RpLight* l = RpLightCreate(type);
    *frame = RwFrameCreate();
    RpLightSetFrame(l, *frame);
    return l;
}
void moveLight(RwFrame* f, float x, float y, float z) {
    const RwV3d v{x, y, z};
    RwFrameTranslate(f, &v, rwCOMBINEREPLACE);
    _rwFrameSyncDirty();
}
std::vector<const RpWorldSector*> tiesOf(RpLight* l) {
    std::vector<const RpWorldSector*> out;
    for (RwLLLink* k = l->WorldSectorsInLight.link.next; k != &l->WorldSectorsInLight.link; k = k->next) {
        out.push_back(reinterpret_cast<rpLightTie*>(reinterpret_cast<char*>(k) - offsetof(rpLightTie, WorldSectorInLight))->sect);
    }
    return out;
}
bool inList(const RwLinkList& list, const RpLight* l) {
    for (const RwLLLink* k = list.link.next; k != &list.link; k = k->next) {
        if (k == &l->inWorld) return true;
    }
    return false;
}

}  // namespace

int main() {
    try {
        Rng rng;
        startEngine();
        // ---- converted-shape worlds read back exactly -------------------------------------------------------------
        {
            const u32 formats[] = {rpWORLDNORMALS | rpWORLDPRELIT | rpWORLDTEXTURED, 0u, rpWORLDTEXTURED2 | rpWORLDSECTORSOVERLAP,
                                   rpWORLDPRELIT | (8u << 16), rpWORLDNORMALS | (3u << 16) | rpWORLDTRISTRIP | 0x20u};
            int n = 0;
            for (u32 format : formats) {
                for (u32 depth : {0u, 1u, 3u}) {
                    for (u32 version : {0x35000u, 0x36003u}) {
                        Spec s;
                        s.format = format, s.depth = depth, s.version = version, s.nv = 3 + rng.next() % 20u, s.nt = 1 + rng.next() % 20u;
                        s.indices = {-1, -1, 0, -1, 1};
                        s.materials = 3;
                        const Built b = build(s, rng);
                        const ReadResult r = readWorld(b.bytes);
                        const std::string what = "world " + std::to_string(n++);
                        check(r.world != nullptr && r.inStep, what + ": read, the stream in step");
                        checkWorld(r.world, b, s, what);
                        check(RpWorldDestroy(r.world) == 1, what + ": destroyed");
                    }
                }
            }
        }
        // ---- deliberate differences --------------------------------------------------------------------------------
        {
            Spec s;
            s.structExtra = 8;
            Built b = build(s, rng);
            ReadResult r = readWorld(b.bytes);
            check(r.world != nullptr && r.inStep, "a 0x48-byte world struct: read into 0x40, the rest skipped");
            checkWorld(r.world, b, s, "long world struct");
            RpWorldDestroy(r.world);
            Spec p;
            p.planeExtra = 12;
            b = build(p, rng);
            r = readWorld(b.bytes);
            check(r.world != nullptr && r.inStep, "a 0x24-byte plane struct: read into 0x18, the rest skipped");
            checkWorld(r.world, b, p, "long plane struct");
            RpWorldDestroy(r.world);
            Spec x;
            x.sectorExtra = 6;
            b = build(x, rng);
            r = readWorld(b.bytes);
            check(r.world != nullptr && r.inStep, "bytes after a sector's arrays in its struct chunk: skipped");
            checkWorld(r.world, b, x, "long sector struct");
            RpWorldDestroy(r.world);
            Spec two;
            two.meshChunks = 2;
            b = build(two, rng);
            r = readWorld(b.bytes);
            check(r.world != nullptr && r.inStep, "two mesh chunks in a sector");
            checkWorld(r.world, b, two, "two mesh chunks (the second kept)");
            RpWorldDestroy(r.world);
        }
        const auto failing = [&](Spec s, const std::string& what, RwInt32 error = 0) { failsCleanly(build(s, rng).bytes, what, error); };
        {
            Spec s;
            s.format |= rpWORLDNATIVE;
            failing(s, "a GameCube-native world", E_RW_BADPARAM);
            s = {};
            s.format = 9u << 16;
            failing(s, "9 texture coordinate sets");
            s = {};
            s.zeroTotals = true, s.nv = 20, s.nt = 20;
            failing(s, "world totals of no vertices and triangles below the tree's");
            s = {};
            s.countField = 0x10000u;
            failing(s, "a sector count above 0xFFFF");
            s = {};
            s.windowField = 0x10000u;   // read on, the original keeps its low 16 bits (0)
            failing(s, "a sector window base above 0xFFFF");
            s = {};
            s.sectorLength = 0x20;
            failing(s, "a sector struct chunk shorter than its header");
            s = {};
            s.sectorLengthDelta = -4;
            failing(s, "a sector struct chunk shorter than its arrays");
            s = {};
            s.badTriangle = true;
            failing(s, "a triangle vertex index outside the sector");
            s = {};
            s.planeType = 12;
            failing(s, "a plane type other than 0 / 4 / 8");
            s = {};
            s.listCount = -1;
            failing(s, "a negative material count");
            s = {};
            s.indices = {-1, 1, -1};
            failing(s, "a material index that is not an earlier entry");
            s = {};
            s.indices = {0, -1};
            failing(s, "a first material referring to itself");
            s = {};
            s.meshCount = 0x10000;
            failing(s, "a mesh count above 0xFFFF");
            s.meshNoRecords = true;   // the original reads it as 0 meshes (its 16-bit field) and succeeds
            failing(s, "a mesh count of 0x10000 with no meshes");
            s = {};
            s.meshMaterial = 3;
            failing(s, "a mesh material outside the list");
            s = {};
            s.meshMaterial = 0x7FFFFFFF;
            failing(s, "a mesh material index far outside the list");
            s = {};
            s.meshIndicesOver = true;
            Built b = build(s, rng);
            bool any = false;   // the first sector's meshes must hold an index for the total to be exceeded
            for (const auto& k : b.sectors[0].meshIndices) any = any || !k.empty();
            if (any) failsCleanly(b.bytes, "mesh indices past the total");
            s = {};
            s.meshChunks = 0;
            failing(s, "a sector without its mesh chunk", E_RW_BADPARAM);
            s = {};
            g_failHint = 0x01030008u;
            failing(s, "the material array cannot be allocated", E_RW_NOMEM);
            g_failHint = 0;
            // a short prelit read (the original accepts any non-empty one)
            s = {};
            s.depth = 0, s.format = rpWORLDPRELIT;
            b = build(s, rng);
            std::vector<u8> cut(b.bytes.begin(), b.bytes.begin() + static_cast<long>(b.prelit[0].first + 5));
            failsCleanly(cut, "a short prelit read");
        }
        {   // a sector whose world could not be registered: its mesh has no world
            std::vector<RpWorld*> alive;
            Spec s;
            s.depth = 1;
            for (int i = 0; i < 8; ++i) alive.push_back(readWorld(build(s, rng).bytes).world);
            for (RpWorld* w : alive) require(w != nullptr, "eight worlds");
            g_failHint = 0x00040507u;   // the registry's next free-list block (allocated with the entry's hint)
            failing(s, "a world that could not be registered");
            check(g_failHint == 0, "the registry did ask for a block");
            for (RpWorld* w : alive) RpWorldDestroy(w);
        }
        stopEngine("deliberate differences");

        // ---- every truncation and every allocation of a read failing ------------------------------------------------
        startEngine();
        {
            Spec s;
            s.depth = 2, s.nv = 3, s.nt = 2, s.format = rpWORLDNORMALS | rpWORLDTEXTURED;
            const Built b = build(s, rng);
            // the last 3 cuts end inside the world plugin's data word, which still reads (RwStreamReadInt32 fails only
            // when no byte arrives, as the original): a world
            for (std::size_t n = 0; n + 3 < b.bytes.size(); ++n) {
                failsCleanly(std::vector<u8>(b.bytes.begin(), b.bytes.begin() + static_cast<long>(n)), "cut at byte " + std::to_string(n));
            }
            for (std::size_t n = b.bytes.size() - 3; n < b.bytes.size(); ++n) {
                const ReadResult r = readWorld(std::vector<u8>(b.bytes.begin(), b.bytes.begin() + static_cast<long>(n)), false);
                check(r.world != nullptr, "cut inside the last word: read");
                RpWorldDestroy(r.world);
            }
            const long before = g_allocations;
            ReadResult r = readWorld(b.bytes);
            const long used = g_allocations - before;
            check(r.world != nullptr && used > 5, "the whole stream reads");
            RpWorldDestroy(r.world);
            unsigned survived = 0;
            for (long k = 0; k < used; ++k) {
                const std::size_t live = g_live.size();
                g_failAt = g_allocations + k;
                r = readWorld(b.bytes);
                g_failAt = -1;
                if (r.world != nullptr) {   // an allocation the read could do without (a free list's spare block)
                    ++survived;
                    checkWorld(r.world, b, s, "allocation " + std::to_string(k) + " failed but the read succeeded");
                    RpWorldDestroy(r.world);
                }
                check(g_live.size() <= live && g_ctors == g_dtors, "allocation " + std::to_string(k) + " failing: nothing leaks");
            }
            r = readWorld(b.bytes);
            check(r.world != nullptr, "a read after the failures succeeds");
            checkWorld(r.world, b, s, "after the failures");
            RpWorldDestroy(r.world);
            std::printf("  %zu truncations, %ld allocations failed in turn (%u of them survivable)\n", b.bytes.size(), used, survived);
        }
        stopEngine("truncations and allocation failures");

        // ---- deep trees -------------------------------------------------------------------------------------------
        startEngine();
        {
            Spec s;
            s.chain = 10000, s.nv = 3, s.nt = 1, s.materials = 1, s.format = 0;
            const Built b = build(s, rng);
            const ReadResult r = readWorld(b.bytes);
            check(r.world != nullptr && r.inStep, "a 10,000-level BSP reads");
            checkWorld(r.world, b, s, "10,000 levels");
            RwFrame* f = nullptr;
            RpLight* l = makeLight(rpLIGHTPOINT, &f);
            RpLightSetRadius(l, 1e9f);
            RpWorldAddLight(r.world, l);
            moveLight(f, 0, 0, 0);
            check(tiesOf(l).size() == b.sectors.size(), "a light reaching every one of 10,001 sectors");
            RpWorldDestroy(r.world);
            check(RpLightGetWorld(l) == nullptr && tiesOf(l).empty(), "the destroyed world let the light go");
            RpLightDestroy(l);
            RwFrameDestroy(f);
        }
        stopEngine("deep trees");

        // ---- world lights ------------------------------------------------------------------------------------------
        startEngine();
        {
            Spec s;
            s.depth = 4, s.format = rpWORLDSECTORSOVERLAP;
            const Built b = build(s, rng);
            RpWorld* w = readWorld(b.bytes).world;
            RpWorld* w2 = readWorld(build(Spec{}, rng).bytes).world;
            require(w != nullptr && w2 != nullptr, "two worlds");
            const std::vector<Leaf> ls = leaves(w);
            RwFrame* f = nullptr;
            RpLight* l = makeLight(rpLIGHTPOINT, &f);
            RpWorldAddLight(w, l);
            check(inList(w->lightList, l) && RpLightGetWorld(l) == w, "a point light joins the light list");
            unsigned tied = 0;
            for (int i = 0; i < 2000; ++i) {
                const float x = rng.in(b.box[3] - 100, b.box[0] + 100), y = rng.in(b.box[4] - 100, b.box[1] + 100),
                            z = rng.in(b.box[5] - 100, b.box[2] + 100), r = i % 10 == 0 ? 0.0f : rng.in(0, 600);
                RpLightSetRadius(l, r);
                moveLight(f, x, y, z);
                const RwV3d c = RwFrameGetLTM(f)->pos;
                const float inf[3] = {c.x - r, c.y - r, c.z - r}, sup[3] = {c.x + r, c.y + r, c.z + r};
                std::vector<const RpWorldSector*> expect;
                for (const Leaf& leaf : ls) {
                    bool reach = true;
                    for (int a = 0; a < 3; ++a) reach = reach && inf[a] < leaf.sup[a] && leaf.inf[a] < sup[a];
                    if (reach) expect.push_back(leaf.s);
                }
                std::vector<const RpWorldSector*> got = tiesOf(l);
                std::vector<const RpWorldSector*> sortedGot = got, sortedExpect = expect;
                std::sort(sortedGot.begin(), sortedGot.end()), std::sort(sortedExpect.begin(), sortedExpect.end());
                check(sortedGot == sortedExpect, "light " + std::to_string(i) + ": ties exactly the sectors its box reaches");
                for (const RpWorldSector* sec : got) {
                    bool back = false;
                    for (const RwLLLink* k = sec->lightsInWorldSector.link.next; k != &sec->lightsInWorldSector.link; k = k->next) {
                        back = back || reinterpret_cast<const rpLightTie*>(k)->light == l;
                    }
                    check(back, "each tie is in its sector's list too");
                }
                tied += static_cast<unsigned>(got.size());
            }
            RwFrame* fd = nullptr;
            RpLight* d = makeLight(rpLIGHTDIRECTIONAL, &fd);
            RpWorldAddLight(w, d);
            moveLight(fd, 1, 2, 3);
            check(inList(w->directionalLightList, d) && tiesOf(d).empty(), "a directional light: the directional list, no ties");
            // moved to another world (the original links it into both)
            RpWorldAddLight(w2, l);
            moveLight(f, w2->boundingBox.inf.x + 1, w2->boundingBox.inf.y + 1, w2->boundingBox.inf.z + 1);
            check(!inList(w->lightList, l) && inList(w2->lightList, l) && RpLightGetWorld(l) == w2, "a light added to a second world moves");
            for (const RpWorldSector* sec : tiesOf(l)) check(RpWorldSectorGetWorld(sec) == w2, "its ties are the new world's");
            // destroyed in a world: it leaves the list
            RpLightDestroy(d);
            check(w->directionalLightList.link.next == &w->directionalLightList.link, "a light destroyed in a world leaves its list");
            RwFrameDestroy(fd);
            // a world destroyed with a light in it
            RpLightSetRadius(l, 1e6f);
            _rwFrameSyncDirty();
            check(!tiesOf(l).empty(), "ties before the destroy");
            RpWorldDestroy(w2);
            check(RpLightGetWorld(l) == nullptr && l->inWorld.next == nullptr && l->inWorld.prev == nullptr && tiesOf(l).empty(),
                  "a world destroyed with a light in it lets the light go");
            // a tie allocation failing: the ties' free list has blocks of 64; four spot lights fill one in the
            // 16-sector world, then the light's first tie needs a new block
            std::vector<RpLight*> more;
            std::vector<RwFrame*> moreFrames;
            for (int i = 0; i < 4; ++i) {
                RwFrame* mf = nullptr;
                more.push_back(makeLight(rpLIGHTSPOT, &mf));
                moreFrames.push_back(mf);
                RpLightSetRadius(more.back(), 1e6f);
                RpWorldAddLight(w, more.back());
            }
            _rwFrameSyncDirty();
            check(tiesOf(more[0]).size() == ls.size(), "the spot lights tie every sector");
            takeError();
            g_failHint = 0x00030507u;   // the ties' next free-list block (allocated with the tie's hint)
            RpWorldAddLight(w, l);
            _rwFrameSyncDirty();
            const RwError e = takeError();
            check(g_failHint == 0 && e.errorCode == E_RW_NOMEM && tiesOf(l).size() < ls.size(), "a failing tie allocation: E_RW_NOMEM");
            RpLightSetRadius(l, 1e6f);
            _rwFrameSyncDirty();
            check(tiesOf(l).size() == ls.size(), "the next sync ties it everywhere");
            std::printf("  2000 lights: %u ties checked\n", tied);
            RpWorldDestroy(w);
            for (RpLight* x : more) check(RpLightGetWorld(x) == nullptr && tiesOf(x).empty(), "every light let go"), RpLightDestroy(x);
            for (RwFrame* x : moreFrames) RwFrameDestroy(x);
            RpLightDestroy(l);
            RwFrameDestroy(f);
        }
        stopEngine("world lights");
        std::printf("rpworld_world_native_tests: %u checks passed (no allocation outlives a shutdown)\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rpworld_world_native_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
