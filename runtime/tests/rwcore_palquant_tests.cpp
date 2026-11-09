// Oracle for the native RenderWare palette quantiser (rwcore/src/palquant.cpp, rw/core/palquant.h): the ORIGINAL
// RwPalQuantInit (0x80490F28), RwPalQuantAddImage (0x8048EF00), RwPalQuantResolvePalette (0x80490620),
// RwPalQuantMatchImage (0x80490B00) and RwPalQuantTerm (0x80491448) of PAL main.dol - and through them every static
// function of the module - in the reference interpreter (rwcore_oracle.h) against the native ones.
//
// Each of 240 cases: a zeroed quantiser on both sides, Init, one to three images added (depth 32, 8 or 4 with
// palettes; 1 .. 24 pixels a side, strides with padding; colours from a few to hundreds of base colours, with or
// without noise inside and across cells, or random; weights 1, other fractions, random, now and then 0 or negative),
// the palette resolved for maxcols 1 .. 32 (64 or 100 in a tenth of the cases, with images up to 16 a side; 255 or
// 256 in three, over one random image of 17 or 18 a side; now and then 0 or negative), each image matched (one index a byte, or 4-bit
// packed with dstPacked TRUE, FALSE or 2), now and then resolved twice or added to after resolving (the boxes cleared
// first on both sides: past the last box it makes the original reads stale ones, see below), then Term.
// After every call: the tree (which nodes exist; every leaf's weight, colour sums, variance and index, after
// resolving every node's), the quantiser's 256 boxes, variances and box statistics, the palette (with what lies
// past it), the indices (with the padding), the result, and the node allocations and frees (count, hint).
// Floats compare by bits (NaN with NaN: the GameCube's default NaN is positive, the PC's negative).
// The free list itself is stubbed on the guest side (RwFreeListCreate / Destroy and the engine's memoryAlloc /
// memoryFree; proven in rwcore_memory_tests); the native side runs without free lists (rwENGINEINITNOFREELISTS), so
// every node is one RwMalloc.
// Not generated (the deliberate differences, rwcore_palquant_native_tests): failed allocations (the original writes
// through NULL), maxcols over 256 (it overruns its boxes), images with sizes below 1, colours matched that were
// never added (it reads through NULL), and a cut stopping short with boxes left from an earlier resolve (it assigns
// those).
// liveness-roots: reference-interpreter-only
//
// Usage: rwcore_palquant_tests <main.dol>
#include "rw/core/engine.h"
#include "rw/core/palquant.h"
#include "rwcore_oracle.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kFromMatchSpace{"FromMatchSpace", 0x8048E8ACu, 0x24u, 0xC0430000u, 0x4E800020u};
constexpr Original kInitLeaf{"InitLeaf", 0x8048E8D0u, 0x28u, 0x38000000u, 0x4E800020u};
constexpr Original kLeafCalcStats{"LeafCalcStats", 0x8048E8F8u, 0x120u, 0x9421FFD0u, 0x4E800020u};
constexpr Original kStatsAdd{"StatsAdd", 0x8048EA18u, 0x100u, 0xC0440014u, 0x4E800020u};
constexpr Original kRepresentativeColor{"RepresentativeColor", 0x8048EB18u, 0xDCu, 0x9421FFD0u, 0x4E800020u};
constexpr Original kInitBranch{"InitBranch", 0x8048EBF4u, 0x48u, 0x38000000u, 0x4E800020u};
constexpr Original kAllocateToLeaf{"AllocateToLeaf", 0x8048EC3Cu, 0x2C4u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kAddImage{"RwPalQuantAddImage", 0x8048EF00u, 0x450u, 0x9421FF50u, 0x4E800020u};
constexpr Original kAssignindex{"assignindex", 0x8048F350u, 0x17Cu, 0x9421FFD0u, 0x4E800020u};
constexpr Original kAddvolume{"addvolume", 0x8048F4CCu, 0x2CCu, 0x9421FFB0u, 0x4E800020u};
constexpr Original kFindBestCut{"FindBestCut", 0x8048F798u, 0x240u, 0x9421FF40u, 0x4E800020u};
constexpr Original kNCut{"nCut", 0x8048F9D8u, 0x19Cu, 0x9421FFC0u, 0x4E800020u};
constexpr Original kCalcNodeWeights{"CalcNodeWeights", 0x8048FB74u, 0x3D8u, 0x9421FFA0u, 0x4E800020u};
constexpr Original kCountLeafs{"CountLeafs", 0x8048FF4Cu, 0x2E8u, 0x9421FF80u, 0x4E800020u};
constexpr Original kExtractNodes{"ExtractNodes", 0x80490234u, 0x2D0u, 0x9421FFA0u, 0x4E800020u};
constexpr Original kGetIndex{"GetIndex", 0x80490504u, 0x11Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kResolvePalette{"RwPalQuantResolvePalette", 0x80490620u, 0x4E0u, 0x9421FF20u, 0x4E800020u};
constexpr Original kMatchImage{"RwPalQuantMatchImage", 0x80490B00u, 0x428u, 0x9421FFC0u, 0x4E800020u};
constexpr Original kInit{"RwPalQuantInit", 0x80490F28u, 0x1FCu, 0x9421FFD0u, 0x4E800020u};
constexpr Original kDeleteOctTree{"DeleteOctTree", 0x80491124u, 0x324u, 0x9421FF80u, 0x4E800020u};
constexpr Original kTerm{"RwPalQuantTerm", 0x80491448u, 0x44u, 0x9421FFF0u, 0x4E800020u};
constexpr u32 kFreeListCreate = 0x80480C28u, kFreeListDestroy = 0x80480D2Cu;

constexpr u32 kNodeHint = 0x00030411u, kGuestNode = 0x5Cu, kGuestPalQuant = 0x4008u;
constexpr int kDepth = 6;

// ---- the native side's memory: node allocations (and their frees) counted -----------------------------------------
std::unordered_set<void*> g_nodes;
long g_nodeAllocs = 0, g_nodeFrees = 0, g_badHints = 0;
void* nMalloc(std::size_t size, RwUInt32 hint) {
    void* const p = std::malloc(size ? size : 1);
    if (size == sizeof(_rwPalQuantOctNode)) {
        if (hint != kNodeHint) ++g_badHints;
        std::memset(p, 0xA5, size);   // nothing the original leaves unset may matter
        g_nodes.insert(p), ++g_nodeAllocs;
    }
    return p;
}
void nFree(void* p) {
    if (g_nodes.erase(p)) ++g_nodeFrees;
    std::free(p);
}
void* nRealloc(void* p, std::size_t size, RwUInt32) { return std::realloc(p, size ? size : 1); }
void* nCalloc(std::size_t n, std::size_t size, RwUInt32) { return std::calloc(n, size); }

bool sameFloat(float a, float b) {
    return std::bit_cast<u32>(a) == std::bit_cast<u32>(b) || (std::isnan(a) && std::isnan(b));
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_palquant_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kFromMatchSpace, kInitLeaf, kLeafCalcStats, kStatsAdd, kRepresentativeColor, kInitBranch,
                                  kAllocateToLeaf, kAddImage, kAssignindex, kAddvolume, kFindBestCut, kNCut,
                                  kCalcNodeWeights, kCountLeafs, kExtractNodes, kGetIndex, kResolvePalette, kMatchImage,
                                  kInit, kDeleteOctTree, kTerm}) {
            g.check_pinned(f);
        }
        GuestMemory& m = g.memory();
        Rng rng;
        u64 instructions = 0, longest = 0;
        unsigned checks = 0;
        std::string longestAt;
        // a guest call, named in a failure; the median cut of 256 colours runs past the default step limit
        const auto gcall = [&](const std::string& at, u32 entry, std::initializer_list<u32> gprs,
                               std::initializer_list<double> fprs = {}) {
            u64 n = 0;
            try {
                n = g.call(entry, gprs, fprs, 400'000'000u);
            } catch (const std::exception& e) {
                throw std::runtime_error(at + ": " + e.what());
            }
            if (n > longest) longest = n, longestAt = at;
            instructions += n;
            return n;
        };

        // the guest's free list, stubbed: creation and destruction recorded, entries from the guest heap
        struct Create {
            u32 entrySize, perBlock, alignment, hint;
        };
        std::vector<Create> creates;
        u32 destroys = 0, gNodeAllocs = 0, gNodeFrees = 0, gBadAllocs = 0;
        const u32 kList = g.heap_alloc(64);
        g.stub(kFreeListCreate, [&](Cpu& c) {
            creates.push_back({c.gpr[3], c.gpr[4], c.gpr[5], c.gpr[6]});
            c.gpr[3] = kList;
        });
        g.stub(kFreeListDestroy, [&](Cpu& c) {
            if (c.gpr[3] == kList) ++destroys;
            c.gpr[3] = 1;
        });
        g.stub(kMemoryAllocStub, [&](Cpu& c) {
            if (c.gpr[3] != kList || c.gpr[4] != kNodeHint) ++gBadAllocs;
            const u32 at = g.heap_alloc(kGuestNode);
            for (u32 i = 0; i < kGuestNode; i += 4) m.write32(at + i, 0xA5A5A5A5u);
            ++gNodeAllocs;
            c.gpr[3] = at;
        });
        g.stub(kMemoryFreeStub, [&](Cpu& c) {
            if (c.gpr[3] != kList) ++gBadAllocs;
            g.heap_free(c.gpr[4]);
            ++gNodeFrees;
            c.gpr[3] = kList;
        });

        const RwMemoryFunctions funcs{nMalloc, nFree, nRealloc, nCalloc};
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(&funcs, rwENGINEINITNOFREELISTS, 0) == 1, "native init");
        require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "native start");

        // ---- comparisons ---------------------------------------------------------------------------------------
        const auto leafSame = [&](u32 at, const _rwPalQuantLeafNode& n) {
            return sameFloat(g.read_float(at), n.weight) && sameFloat(g.read_float(at + 4), n.ac.red) &&
                   sameFloat(g.read_float(at + 8), n.ac.green) && sameFloat(g.read_float(at + 12), n.ac.blue) &&
                   sameFloat(g.read_float(at + 16), n.ac.alpha) && sameFloat(g.read_float(at + 20), n.var) &&
                   static_cast<u8>(m.peek32(at + 24) >> 24) == n.palIndex;
        };
        // the trees: the same nodes, the leaves' (all nodes' when full) statistics
        unsigned nodesCompared = 0;
        const auto compareTree = [&](auto& self, u32 gn, const _rwPalQuantOctNode* n, int depth, bool full,
                                     const std::string& what) -> void {
            require((gn == 0) == (n == nullptr), what + ": a node on one side only (depth " + std::to_string(depth) + ")");
            if (n == nullptr) return;
            ++nodesCompared;
            if (full || depth == 0) require(leafSame(gn, n->Leaf), what + ": node statistics (depth " + std::to_string(depth) + ")");
            if (depth == 0) return;
            for (u32 i = 0; i < 16; ++i) self(self, m.peek32(gn + 0x1Cu + 4u * i), n->Branch.dir[i], depth - 1, full, what);
        };
        const auto compareQuantiser = [&](u32 gpq, const RwPalQuant& pq, bool full, const std::string& what) {
            compareTree(compareTree, m.peek32(gpq + 0x4000u), pq.root, kDepth, full, what);
            for (u32 j = 0; j < 256; ++j) {
                for (u32 c = 0; c < 4; ++c) {
                    require(static_cast<RwInt32>(m.peek32(gpq + 32u * j + 4u * c)) == pq.Mcube[j].col0[c] &&
                                static_cast<RwInt32>(m.peek32(gpq + 32u * j + 16u + 4u * c)) == pq.Mcube[j].col1[c],
                            what + ": box " + std::to_string(j));
                }
                require(sameFloat(g.read_float(gpq + 0x2000u + 4u * j), pq.Mvv[j]), what + ": box variance " + std::to_string(j));
                require(leafSame(gpq + 0x2400u + 28u * j, pq.Mvol[j]), what + ": box statistics " + std::to_string(j));
            }
            require(static_cast<long>(gNodeAllocs) == g_nodeAllocs && static_cast<long>(gNodeFrees) == g_nodeFrees,
                    what + ": node allocations and frees");
        };

        // ---- images ----------------------------------------------------------------------------------------------
        struct Image {
            RwInt32 width, height, depth, stride;
            std::vector<u8> pixels;
            std::vector<u8> palette;   // 4 << depth bytes for 4 and 8
        };
        unsigned depth4 = 0, depth8 = 0, depth32 = 0, noisy = 0, randomImages = 0;
        // side: the largest width and height; full: a random 32-bit image of side x side (one cell a pixel, nearly)
        const auto makeImage = [&](u32 side, bool full) {
            Image im;
            const u32 pick = rng.next() % 10u;
            im.depth = full || pick < 6 ? 32 : pick < 8 ? 8 : 4;
            im.width = full ? static_cast<RwInt32>(side) : 1 + static_cast<RwInt32>(rng.next() % side);
            im.height = full ? static_cast<RwInt32>(side) : 1 + static_cast<RwInt32>(rng.next() % side);
            const RwInt32 bpp = im.depth == 32 ? 4 : 1;
            im.stride = bpp * im.width + (rng.next() % 3u == 0 ? static_cast<RwInt32>(rng.next() % 9u) : 0);
            im.pixels.resize(static_cast<std::size_t>(im.stride) * static_cast<std::size_t>(im.height));
            for (u8& b : im.pixels) b = static_cast<u8>(rng.next());   // padding included
            // base colours: few, some, many; spread over the cube or within a small region
            const u32 kind = rng.next() % 8u;
            u32 bases = kind < 3 ? 1 + rng.next() % 6u : kind < 6 ? 7 + rng.next() % 70u : 80 + rng.next() % 300u;
            if (im.depth == 8) bases = std::min(bases, 256u);
            if (im.depth == 4) bases = std::min(bases, 16u);
            const bool region = rng.next() % 3u == 0;
            const u32 noise = rng.next() % 4u;   // 0: none, 1: inside the cell, 2: across cells, 3: none
            const bool random = full || (im.depth == 32 && rng.next() % 12u == 0);
            std::vector<u8> base(4u * bases);
            for (u8& b : base) b = region ? static_cast<u8>(96u + rng.next() % 40u) : static_cast<u8>(rng.next());
            const auto colour = [&](u8* out, u32 b) {
                for (u32 c = 0; c < 4; ++c) {
                    u32 v = base[4u * b + c];
                    if (noise == 1) v = (v & ~3u) | (rng.next() & 3u);
                    if (noise == 2) v = std::min(255u, v + (rng.next() & 7u));
                    out[c] = static_cast<u8>(v);
                }
            };
            if (noise == 1 || noise == 2) ++noisy;
            if (im.depth == 32) {
                ++depth32;
                if (random) ++randomImages;
                for (RwInt32 y = 0; y < im.height; ++y) {
                    for (RwInt32 x = 0; x < im.width; ++x) {
                        u8* const p = &im.pixels[static_cast<std::size_t>(y * im.stride + 4 * x)];
                        if (random) for (u32 c = 0; c < 4; ++c) p[c] = static_cast<u8>(rng.next());
                        else colour(p, rng.next() % bases);
                    }
                }
            } else {
                (im.depth == 8 ? depth8 : depth4)++;
                const u32 entries = 1u << im.depth;
                im.palette.resize(4u * entries);
                for (u32 e = 0; e < entries; ++e) colour(&im.palette[4u * e], e % bases);
                for (RwInt32 y = 0; y < im.height; ++y) {
                    for (RwInt32 x = 0; x < im.width; ++x) {
                        im.pixels[static_cast<std::size_t>(y * im.stride + x)] = static_cast<u8>(rng.next() % std::min(entries, bases + bases / 2u));
                    }
                }
            }
            return im;
        };
        const auto tieImage = [&]() {
            Image im;
            im.depth = 32, im.width = 4, im.height = 4, im.stride = 16;
            im.pixels.assign(64, 0);
            static constexpr u8 kColour[4][4] = {{0, 100, 0, 0}, {0, 104, 0, 0}, {0, 0, 0, 100}, {0, 0, 0, 104}};
            for (u32 i = 0; i < 16; ++i) std::memcpy(&im.pixels[4u * i], kColour[i % 4], 4);
            return im;
        };
        const auto toGuest = [&](const Image& im) {
            const u32 gp = g.heap_alloc(static_cast<u32>(im.pixels.size()));
            m.write_block(gp, im.pixels.data(), im.pixels.size());
            u32 gpal = 0;
            if (!im.palette.empty()) {
                gpal = g.heap_alloc(static_cast<u32>(im.palette.size()));
                m.write_block(gpal, im.palette.data(), im.palette.size());
            }
            const u32 gi = g.heap_alloc(0x1C);
            m.write32(gi, 0), m.write32(gi + 4, static_cast<u32>(im.width)), m.write32(gi + 8, static_cast<u32>(im.height));
            m.write32(gi + 0xC, static_cast<u32>(im.depth)), m.write32(gi + 0x10, static_cast<u32>(im.stride));
            m.write32(gi + 0x14, gp), m.write32(gi + 0x18, gpal);
            return gi;
        };
        const auto freeGuest = [&](u32 gi) {
            g.heap_free(m.peek32(gi + 0x14));
            if (m.peek32(gi + 0x18)) g.heap_free(m.peek32(gi + 0x18));
            g.heap_free(gi);
        };
        const auto native = [](Image& im) {
            return RwImage{0, im.width, im.height, im.depth, im.stride, im.pixels.data(),
                           im.palette.empty() ? nullptr : reinterpret_cast<RwRGBA*>(im.palette.data())};
        };
        {   // the tie is real: the two boxes after the first cut have bit-identical variances
            Image im = tieImage();
            RwImage ni = native(im);
            auto* const probe = new RwPalQuant{};
            require(RwPalQuantInit(probe) == 1, "tie probe: Init");
            RwPalQuantAddImage(probe, &ni, 1.0f);
            RwRGBA pal[2];
            require(RwPalQuantResolvePalette(pal, 2, probe) == 2 && probe->Mvv[0] > 0.0f &&
                        std::bit_cast<u32>(probe->Mvv[0]) == std::bit_cast<u32>(probe->Mvv[1]),
                    "tie probe: the two boxes after the first cut have bit-identical variances");
            RwPalQuantTerm(probe);
            delete probe;
            require(g_nodeAllocs == g_nodeFrees && g_nodes.empty(), "tie probe: every node freed");
            g_nodeAllocs = g_nodeFrees = 0;   // the cases compare their own allocations with the guest's
        }

        // ---- the cases -------------------------------------------------------------------------------------------
        unsigned cases = 0, added = 0, extracted = 0, cut = 0, cutShort = 0, allBoxes = 0, zeroWeights = 0,
                 negativeWeights = 0, oddWeights = 0, matched = 0, packed = 0, twice = 0, addAfter = 0, nonPositive = 0,
                 ties = 0;
        u64 resolveInstructions = 0;
        for (unsigned c = 0; c < 240; ++c) {
            const std::string what = "case " + std::to_string(c);
            ties += c % 60u == 30u;
            // maxcols first (the median cut's cost grows with it and the tree: 255 / 256 colours over a tree of
            // about 300 leaves take some 200 million original instructions, so only a few cases have them)
            static constexpr RwInt32 kSmall[] = {1, 2, 3, 4, 5, 8, 13, 16, 16, 24, 32};
            // tie: two clusters mirrored between green and alpha - after the first cut the two boxes' variances are
            // equal to the bit (checked below), so the next box to cut is decided by the tie rule alone
            const bool tie = c % 60u == 30u;
            const bool all = !tie && c % 80u == 5u, large = !all && !tie && rng.next() % 10u == 0;
            RwInt32 caseMax = all ? 255 + static_cast<RwInt32>(rng.next() % 2u)
                              : tie ? 3 : large ? (rng.next() % 2u ? 64 : 100) : kSmall[rng.next() % std::size(kSmall)];
            if (!all && !tie && rng.next() % 40u == 0) caseMax = -static_cast<RwInt32>(rng.next() % 3u), ++nonPositive;
            const u32 side = all ? 17u + rng.next() % 2u : large ? 16u : 24u;
            const u32 gpq = g.heap_alloc(kGuestPalQuant);
            {
                std::vector<u8> zero(kGuestPalQuant, 0);
                m.write_block(gpq, zero.data(), zero.size());
            }
            auto* const pq = new RwPalQuant{};
            creates.clear();
            gcall(what + " Init", kInit.entry, {gpq});
            require(g.r3() == 1 && RwPalQuantInit(pq) == 1, what + ": Init");
            require(creates.size() == 1 && creates[0].entrySize == kGuestNode && creates[0].perBlock == 0x400u &&
                        creates[0].alignment == 4u && creates[0].hint == kNodeHint,
                    what + ": the guest's free list");
            compareQuantiser(gpq, *pq, false, what + " after Init");

            std::vector<Image> images;
            std::vector<u32> guestImages;
            const auto addOne = [&](const std::string& at) {
                images.push_back(tie ? tieImage() : makeImage(side, all && images.empty()));
                Image& im = images.back();
                guestImages.push_back(toGuest(im));
                const u32 wk = all || tie ? 19u : rng.next() % 20u;
                float w = 1.0f;
                if (wk < 3) w = std::array<float, 4>{0.5f, 2.0f, 3.25f, 0.125f}[rng.next() % 4u], ++oddWeights;
                else if (wk < 6) w = 0.01f + static_cast<float>(rng.next() % 100000u) * 1e-4f, ++oddWeights;
                else if (wk == 6) w = 0.0f, ++zeroWeights;
                else if (wk == 7 && rng.next() % 3u == 0) w = -1.0f, ++negativeWeights;
                gcall(at, kAddImage.entry, {gpq, guestImages.back()}, {static_cast<double>(w)});
                RwImage ni = native(im);
                RwPalQuantAddImage(pq, &ni, w);
                ++added;
                compareQuantiser(gpq, *pq, false, at);
            };
            const unsigned count = all || tie ? 1u : 1 + rng.next() % 3u;
            for (unsigned i = 0; i < count; ++i) addOne(what + " after image " + std::to_string(i));

            const auto resolve = [&](const std::string& at) {
                const RwInt32 maxcols = caseMax;
                const u32 room = 256u + 8u;
                const u32 gpal = g.heap_alloc(4u * room);
                std::vector<u8> pal(4u * room);
                for (u8& b : pal) b = static_cast<u8>(rng.next());
                m.write_block(gpal, pal.data(), pal.size());
                resolveInstructions += gcall(at + " (maxcols " + std::to_string(maxcols) + ")", kResolvePalette.entry,
                                             {gpal, static_cast<u32>(maxcols), gpq});
                const RwInt32 gr = static_cast<RwInt32>(g.r3());
                const RwInt32 nr = RwPalQuantResolvePalette(reinterpret_cast<RwRGBA*>(pal.data()), maxcols, pq);
                require(gr == nr, at + ": the result (original " + std::to_string(gr) + ", native " + std::to_string(nr) + ")");
                std::vector<u8> gb(pal.size());
                m.peek_block(gpal, gb.data(), gb.size());
                for (std::size_t i = 0; i < pal.size(); ++i) {
                    require(gb[i] == pal[i], at + ": palette byte " + std::to_string(i) + " (maxcols " + std::to_string(maxcols) + ")");
                }
                compareQuantiser(gpq, *pq, true, at);
                g.heap_free(gpal);
                // which path: the leaves themselves, or boxes (all of them made, or fewer)
                RwInt32 made = 0;
                for (const _rwPalQuantRGBABox& b : pq->Mcube) made += b.col1[0] != 0;
                if (maxcols > 0 && nr <= maxcols && made == 0) ++extracted;
                else if (maxcols > 0) {
                    ++cut;
                    if (made < maxcols) ++cutShort;
                    if (maxcols >= 255) ++allBoxes;
                }
                ++checks;
            };
            // before resolving again, the boxes cleared on both sides: the original reads the boxes past the last it
            // makes when its cut stops short (the deliberate difference), which a fresh quantiser has zeroed
            const auto clearBoxes = [&] {
                std::vector<u8> zero(0x4000u, 0);
                m.write_block(gpq, zero.data(), zero.size());
                std::memset(pq->Mcube, 0, sizeof pq->Mcube), std::memset(pq->Mvv, 0, sizeof pq->Mvv);
                std::memset(pq->Mvol, 0, sizeof pq->Mvol);
            };
            resolve(what + " resolved");
            if (!all && !tie && rng.next() % 12u == 0) clearBoxes(), resolve(what + " resolved again"), ++twice;
            if (!all && !tie && rng.next() % 12u == 0) {
                addOne(what + " added after resolving"), ++addAfter;
                clearBoxes(), resolve(what + " resolved after adding");
            }

            // every image matched: one index a byte, or packed
            for (std::size_t i = 0; i < images.size(); ++i) {
                Image& im = images[i];
                const u32 mode = rng.next() % 4u;
                const RwInt32 dstdepth = mode == 0 ? 8 : 4;
                const RwBool dstPacked = mode == 1 ? 1 : mode == 2 ? 0 : mode == 3 ? 2 : static_cast<RwBool>(rng.next() % 2u);
                const bool isPacked = dstdepth == 4 && dstPacked == 1;
                const RwInt32 dststride = (isPacked ? (im.width + 1) / 2 : im.width) + static_cast<RwInt32>(rng.next() % 3u);
                std::vector<u8> dst(static_cast<std::size_t>(dststride) * static_cast<std::size_t>(im.height));
                for (u8& b : dst) b = static_cast<u8>(rng.next());
                const u32 gdst = g.heap_alloc(static_cast<u32>(dst.size()));
                m.write_block(gdst, dst.data(), dst.size());
                gcall(what + " match", kMatchImage.entry, {gdst, static_cast<u32>(dststride), static_cast<u32>(dstdepth),
                                                            static_cast<u32>(dstPacked), gpq, guestImages[i]});
                RwImage ni = native(im);
                RwPalQuantMatchImage(dst.data(), dststride, dstdepth, dstPacked, pq, &ni);
                std::vector<u8> gb(dst.size());
                m.peek_block(gdst, gb.data(), gb.size());
                for (std::size_t b = 0; b < dst.size(); ++b) {
                    require(gb[b] == dst[b], what + ": index byte " + std::to_string(b) + " of image " + std::to_string(i));
                }
                g.heap_free(gdst);
                ++matched, ++checks;
                if (isPacked) ++packed;
            }

            gcall(what + " Term", kTerm.entry, {gpq});
            RwPalQuantTerm(pq);
            require(m.peek32(gpq + 0x4000u) == 0 && pq->root == nullptr && destroys == 1, what + ": Term");
            require(static_cast<long>(gNodeAllocs) == g_nodeAllocs && static_cast<long>(gNodeFrees) == g_nodeFrees &&
                        g_nodeAllocs == g_nodeFrees && g_nodes.empty(),
                    what + ": every node freed");
            require(gBadAllocs == 0 && g_badHints == 0, what + ": the node requests (list, hint)");
            destroys = 0;
            for (u32 gi : guestImages) freeGuest(gi);
            g.heap_free(gpq);
            delete pq;
            ++cases, ++checks;
            if (cases % 40u == 0) {
                std::fprintf(stderr, "  %u cases, %llu original instructions (the longest call %llu, %s)\n", cases,
                             static_cast<unsigned long long>(instructions), static_cast<unsigned long long>(longest),
                             longestAt.c_str());
            }
        }
        require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, "native shutdown");
        std::printf("  %u quantisers: %u images added (%u of depth 32, %u random; %u of depth 8, %u of depth 4; %u with "
                    "noise), weights %u odd / %u zero / %u negative\n",
                    cases, added, depth32, randomImages, depth8, depth4, noisy, oddWeights, zeroWeights, negativeWeights);
        std::printf("  palettes: %u from the leaves, %u cut into boxes (%u for 255 / 256 colours, %u stopped short), %u for "
                    "maxcols <= 0, %u resolved twice, %u added to after resolving, %u with tied boxes; %u images matched "
                    "(%u packed); %u nodes compared\n",
                    extracted, cut, allBoxes, cutShort, nonPositive, twice, addAfter, ties, matched, packed, nodesCompared);
        std::printf("  (%llu original instructions resolving; the longest call %llu, %s)\n",
                    static_cast<unsigned long long>(resolveInstructions), static_cast<unsigned long long>(longest),
                    longestAt.c_str());
        require(ties >= 4 && extracted > 40 && cut > 80 && allBoxes >= 3 && cutShort > 3 && nonPositive > 2 && twice > 8 &&
                    addAfter > 8 && packed > 80 && depth4 > 50 && depth8 > 60 && randomImages > 15 && zeroWeights > 8 &&
                    negativeWeights > 2,
                "every case must be reached");
        std::printf("rwcore_palquant_tests: %u operations identical to the original (%llu original instructions)\n", checks,
                    static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_palquant_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
