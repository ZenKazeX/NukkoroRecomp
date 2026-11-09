// Native tests of the RenderWare palette quantiser (rwcore/src/palquant.cpp, rw/core/palquant.h), apart from the
// oracle (rwcore_palquant_tests, which proves the arithmetic against the original):
// - the definition behind the oracle's bits: every palette entry is the weighted mean colour of the pixels given its
//   index (within one step); when the image's cells (4 x 4 x 4 x 4 byte values) are no more than maxcols, each cell
//   has its own entry and the count is returned, else maxcols entries; pixels of one cell share an index; packed
//   output puts the first pixel in the low 4 bits and keeps the other bits;
// - the deliberate differences of rw/core/palquant.h: Init failing cleanly, a node that cannot be allocated leaving
//   its pixel out, maxcols over 256, colours never added, sizes below 1, other depths, the median cut stopping short
//   (images of weight 0), and Term freeing every node (no leak in any case).
#include "rw/core/engine.h"
#include "rw/core/palquant.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr RwUInt32 kNodeHint = 0x00030411u;

long g_live = 0, g_hinted = 0, g_failHinted = -1, g_failFrom = -1;
bool g_failListHeader = false;   // the next rwMEMHINTDUR_EVENT request (a free list's header without free lists)
void* nMalloc(std::size_t size, RwUInt32 hint) {
    if (hint == 0x00030000u && g_failListHeader) {
        g_failListHeader = false;
        return nullptr;
    }
    if (hint == kNodeHint) {
        const long i = g_hinted++;
        if (i == g_failHinted || (g_failFrom >= 0 && i >= g_failFrom)) return nullptr;
    }
    ++g_live;
    return std::malloc(size ? size : 1);
}
void nFree(void* p) {
    if (p != nullptr) --g_live;
    std::free(p);
}
void* nRealloc(void* p, std::size_t size, RwUInt32) { return std::realloc(p, size ? size : 1); }
void* nCalloc(std::size_t n, std::size_t size, RwUInt32) {
    ++g_live;
    return std::calloc(n, size);
}

unsigned g_checks = 0;
void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) throw std::runtime_error(what);
}

struct Rng {
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    std::uint32_t next() {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        return static_cast<std::uint32_t>(s >> 11);
    }
} rng;

/// A 32-bit image (stride with padding).
struct Image {
    std::vector<RwUInt8> pixels;
    RwImage image{};
    Image(RwInt32 w, RwInt32 h, RwInt32 pad = 0) : pixels(static_cast<std::size_t>((4 * w + pad) * (h > 0 ? h : 1))) {
        image = RwImage{0, w, h, 32, 4 * w + pad, pixels.data(), nullptr};
    }
    RwUInt8* at(RwInt32 x, RwInt32 y) { return &pixels[static_cast<std::size_t>(y * image.stride + 4 * x)]; }
};

std::uint32_t cellOf(const RwUInt8* c) {
    return (static_cast<std::uint32_t>(c[0] >> 2) << 18) | (static_cast<std::uint32_t>(c[1] >> 2) << 12) |
           (static_cast<std::uint32_t>(c[2] >> 2) << 6) | (c[3] >> 2);
}

/// Quantises im (weight 1) into maxcols colours and checks the definition; returns the result.
RwInt32 quantiseAndCheck(Image& im, RwInt32 maxcols, const std::string& what) {
    RwPalQuant* const pq = new RwPalQuant{};
    check(RwPalQuantInit(pq) == 1, what + ": Init");
    RwPalQuantAddImage(pq, &im.image, 1.0f);
    std::vector<RwRGBA> palette(260, RwRGBA{1, 2, 3, 4});
    const RwInt32 n = RwPalQuantResolvePalette(palette.data(), maxcols, pq);
    const RwInt32 w = im.image.width, h = im.image.height;
    std::vector<RwUInt8> index(static_cast<std::size_t>(w * h));
    RwPalQuantMatchImage(index.data(), w, 8, 0, pq, &im.image);
    std::set<std::uint32_t> cells;
    std::map<std::uint32_t, RwUInt8> cellIndex;
    std::map<RwUInt8, std::vector<double>> sums;
    for (RwInt32 y = 0; y < h; ++y) {
        for (RwInt32 x = 0; x < w; ++x) {
            const RwUInt8* const c = im.at(x, y);
            const RwUInt8 i = index[static_cast<std::size_t>(y * w + x)];
            cells.insert(cellOf(c));
            const auto [it, fresh] = cellIndex.emplace(cellOf(c), i);
            check(fresh || it->second == i, what + ": pixels of one cell share an index");
            std::vector<double>& s = sums[i];
            s.resize(5, 0.0);
            for (int k = 0; k < 4; ++k) s[static_cast<std::size_t>(k)] += c[k];
            s[4] += 1.0;
        }
    }
    const RwInt32 cap = std::min(maxcols, 256);   // maxcols over 256 is taken as 256
    const RwInt32 expect = static_cast<RwInt32>(cells.size()) <= cap ? static_cast<RwInt32>(cells.size()) : cap;
    check(n == expect, what + ": the count (" + std::to_string(n) + ", expected " + std::to_string(expect) + ")");
    if (static_cast<RwInt32>(cells.size()) <= cap) {
        std::set<RwUInt8> distinct;
        for (const auto& [cell, i] : cellIndex) distinct.insert(i);
        check(distinct.size() == cells.size(), what + ": one entry a cell");
    }
    for (const auto& [i, s] : sums) {
        check(i < n, what + ": an index within the palette");
        const RwUInt8* const p = &palette[i].red;
        for (int k = 0; k < 4; ++k) {
            const double mean = s[static_cast<std::size_t>(k)] / s[4];
            check(std::fabs(p[k] - mean) <= 1.0, what + ": entry " + std::to_string(i) + " channel " + std::to_string(k) +
                                                     " is " + std::to_string(p[k]) + ", its pixels' mean " + std::to_string(mean));
        }
    }
    for (std::size_t i = static_cast<std::size_t>(maxcols > 0 ? std::min(maxcols, 256) : 0); i < palette.size(); ++i) {
        check(palette[i].red == 1 && palette[i].green == 2 && palette[i].blue == 3 && palette[i].alpha == 4,
              what + ": nothing written past maxcols");
    }
    RwPalQuantTerm(pq);
    delete pq;
    return n;
}

}  // namespace

int main() {
    try {
        const RwMemoryFunctions funcs{nMalloc, nFree, nRealloc, nCalloc};
        RwEngineOpenParams params{nullptr};
        check(RwEngineInit(&funcs, rwENGINEINITNOFREELISTS, 0) == 1, "init");
        check(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "start");
        const long baseline = g_live;

        // ---- the definition ------------------------------------------------------------------------------------
        unsigned leafPalettes = 0, cutPalettes = 0;
        for (unsigned c = 0; c < 300; ++c) {
            const RwInt32 w = 1 + static_cast<RwInt32>(rng.next() % 40u), h = 1 + static_cast<RwInt32>(rng.next() % 40u);
            Image im(w, h, static_cast<RwInt32>(rng.next() % 5u));
            const std::uint32_t bases = 1 + rng.next() % (c % 3u == 0 ? 8u : 400u);
            std::vector<RwUInt8> base(4u * bases);
            for (RwUInt8& b : base) b = static_cast<RwUInt8>(rng.next());
            for (RwInt32 y = 0; y < h; ++y) {
                for (RwInt32 x = 0; x < w; ++x) {
                    const std::uint32_t b = rng.next() % bases;
                    for (int k = 0; k < 4; ++k) im.at(x, y)[k] = static_cast<RwUInt8>((base[4u * b + static_cast<std::uint32_t>(k)] & ~3u) | (rng.next() & 3u));
                }
            }
            static constexpr RwInt32 kMax[] = {1, 2, 4, 7, 16, 32, 64, 256};
            const RwInt32 maxcols = kMax[rng.next() % std::size(kMax)];
            const RwInt32 n = quantiseAndCheck(im, maxcols, "image " + std::to_string(c));
            (n < maxcols ? leafPalettes : cutPalettes)++;
        }
        check(leafPalettes > 50 && cutPalettes > 100, "both paths reached");
        check(g_live == baseline, "nothing leaked by the definition cases");
        std::printf("  definition: %u palettes from the cells, %u cut into boxes\n", leafPalettes, cutPalettes);

        // one colour a cell, each exactly reproduced
        {
            Image im(16, 1);
            for (RwInt32 x = 0; x < 16; ++x) {
                for (int k = 0; k < 4; ++k) im.at(x, 0)[k] = static_cast<RwUInt8>(16 * x + 4 * k + 1);
            }
            RwPalQuant* const pq = new RwPalQuant{};
            check(RwPalQuantInit(pq) == 1, "exact: Init");
            RwPalQuantAddImage(pq, &im.image, 1.0f);
            std::vector<RwRGBA> palette(16);
            check(RwPalQuantResolvePalette(palette.data(), 16, pq) == 16, "exact: 16 cells");
            RwUInt8 index[16];
            RwPalQuantMatchImage(index, 16, 8, 0, pq, &im.image);
            for (RwInt32 x = 0; x < 16; ++x) {
                check(std::memcmp(&palette[index[x]], im.at(x, 0), 4) == 0, "exact: each colour its own entry");
            }
            // packed: the first pixel in the low 4 bits, the other bits kept for an odd width
            RwUInt8 packed[3] = {0xAA, 0xAA, 0xAA};
            Image odd(3, 1);
            std::memcpy(odd.at(0, 0), im.at(2, 0), 4), std::memcpy(odd.at(1, 0), im.at(5, 0), 4), std::memcpy(odd.at(2, 0), im.at(9, 0), 4);
            RwPalQuantMatchImage(packed, 3, 4, 1, pq, &odd.image);
            check(packed[0] == static_cast<RwUInt8>((index[5] << 4) | index[2]) &&
                      packed[1] == static_cast<RwUInt8>(0xA0 | index[9]) && packed[2] == 0xAA,
                  "packed: low nibble first, other bits kept");
            RwUInt8 bytes[3] = {0xAA, 0xAA, 0xAA};
            RwPalQuantMatchImage(bytes, 3, 4, 2, pq, &odd.image);
            check(bytes[0] == index[2] && bytes[1] == index[5] && bytes[2] == index[9], "dstPacked 2: one index a byte");
            // a colour never added: index 0
            Image other(2, 1);
            std::memcpy(other.at(0, 0), im.at(7, 0), 4);
            for (int k = 0; k < 4; ++k) other.at(1, 0)[k] = 0xFE;   // a cell the image never had
            RwUInt8 two[2] = {0x55, 0x55};
            RwPalQuantMatchImage(two, 2, 8, 0, pq, &other.image);
            check(two[0] == index[7] && two[1] == 0, "a colour never added matches index 0");
            // sizes below 1 and other depths: nothing added, nothing written
            const long before = g_hinted;
            Image empty(0, 4), negative(3, 1);
            negative.image.height = -5;
            RwPalQuantAddImage(pq, &empty.image, 1.0f), RwPalQuantAddImage(pq, &negative.image, 1.0f);
            Image deep(4, 4);
            for (RwUInt8& b : deep.pixels) b = static_cast<RwUInt8>(rng.next());
            deep.image.depth = 16;
            RwPalQuantAddImage(pq, &deep.image, 1.0f);
            check(g_hinted == before, "sizes below 1 and depth 16 add nothing");
            RwUInt8 untouched[16];
            std::memset(untouched, 0x77, sizeof untouched);
            RwPalQuantMatchImage(untouched, 4, 8, 0, pq, &negative.image), RwPalQuantMatchImage(untouched, 4, 8, 0, pq, &deep.image);
            bool same = true;
            for (RwUInt8 b : untouched) same = same && b == 0x77;
            check(same, "sizes below 1 and depth 16 write nothing");
            RwPalQuantTerm(pq);
            delete pq;
        }
        check(g_live == baseline, "nothing leaked by the exact case");

        // ---- maxcols over 256 ------------------------------------------------------------------------------------
        {
            Image im(30, 20);
            for (RwInt32 i = 0; i < 600; ++i) {   // 600 cells
                RwUInt8* const p = im.at(i % 30, i / 30);
                p[0] = static_cast<RwUInt8>((i % 64) << 2), p[1] = static_cast<RwUInt8>((i / 64) << 2);
                p[2] = static_cast<RwUInt8>(i * 7), p[3] = 0xFF;
            }
            for (RwInt32 maxcols : {257, 400, 1000}) {
                const RwInt32 n = quantiseAndCheck(im, maxcols, "maxcols " + std::to_string(maxcols));
                check(n == 256, "maxcols over 256 is taken as 256");
            }
        }
        check(g_live == baseline, "nothing leaked by maxcols over 256");

        // ---- Init failing ------------------------------------------------------------------------------------------
        unsigned initFailures = 0;
        for (long f = 0;; ++f) {
            g_hinted = 0, g_failHinted = f;
            RwPalQuant* const pq = new RwPalQuant{};
            const RwBool ok = RwPalQuantInit(pq);
            g_failHinted = -1;
            if (ok) {
                RwPalQuantTerm(pq);
                delete pq;
                break;
            }
            check(g_live == baseline && pq->root == nullptr && pq->cubefreelist == nullptr,
                  "a failed Init releases what it made (allocation " + std::to_string(f) + ")");
            delete pq;
            ++initFailures;
        }
        check(initFailures >= 1, "Init's allocations fail in turn");
        {
            RwPalQuant* const pq = new RwPalQuant{};
            g_failListHeader = true;
            check(RwPalQuantInit(pq) == 0 && !g_failListHeader, "Init fails when its free list cannot be made");
            check(g_live == baseline && pq->root == nullptr && pq->cubefreelist == nullptr, "nothing made without the list");
            delete pq;
            ++initFailures;
        }

        // ---- nodes that cannot be allocated ----------------------------------------------------------------------
        unsigned leftOut = 0;
        for (long f = 1; f < 40; f += 3) {
            Image im(8, 8);
            for (RwUInt8& b : im.pixels) b = static_cast<RwUInt8>(rng.next());
            RwPalQuant* const pq = new RwPalQuant{};
            g_hinted = 0;
            check(RwPalQuantInit(pq) == 1, "nodes: Init");
            g_failFrom = g_hinted + f;   // the first f nodes, then none
            RwPalQuantAddImage(pq, &im.image, 1.0f);
            g_failFrom = -1;
            std::vector<RwRGBA> palette(256);
            const RwInt32 n = RwPalQuantResolvePalette(palette.data(), 256, pq);
            std::vector<RwUInt8> index(64);
            RwPalQuantMatchImage(index.data(), 8, 8, 0, pq, &im.image);
            // each pixel: its leaf's index where its leaf was made, else 0; the count is the leaves'
            std::set<const _rwPalQuantOctNode*> leaves;
            unsigned missing = 0;
            for (RwInt32 p = 0; p < 64; ++p) {
                const RwUInt8* const c = im.at(p % 8, p / 8);
                const _rwPalQuantOctNode* node = pq->root;
                for (int level = 5; level >= 0 && node != nullptr; --level) {
                    const int dir = (((c[0] >> (2 + level)) & 1) << 3) | (((c[1] >> (2 + level)) & 1) << 2) |
                                    (((c[2] >> (2 + level)) & 1) << 1) | ((c[3] >> (2 + level)) & 1);
                    node = node->Branch.dir[dir];
                }
                if (node != nullptr) leaves.insert(node);
                else ++missing;
                check(index[static_cast<std::size_t>(p)] == (node != nullptr ? node->Leaf.palIndex : 0),
                      "nodes: a pixel's index is its leaf's, 0 without one");
            }
            check(n == static_cast<RwInt32>(leaves.size()), "nodes: one entry a leaf made");
            if (missing > 0) ++leftOut;
            RwPalQuantTerm(pq);
            delete pq;
            check(g_live == baseline, "nodes: everything freed after failed allocations");
        }
        check(leftOut > 5, "pixels left out match index 0");

        // ---- the median cut stopping short: weight 0 --------------------------------------------------------------
        {
            Image im(16, 16);
            for (RwUInt8& b : im.pixels) b = static_cast<RwUInt8>(rng.next());
            RwPalQuant* const pq = new RwPalQuant{};
            check(RwPalQuantInit(pq) == 1, "weight 0: Init");
            RwPalQuantAddImage(pq, &im.image, 0.0f);
            std::vector<RwRGBA> palette(16, RwRGBA{9, 9, 9, 9});
            check(RwPalQuantResolvePalette(palette.data(), 16, pq) == 16, "weight 0: maxcols returned");
            bool cleared = true;
            for (const RwRGBA& p : palette) cleared = cleared && p.red == 0 && p.green == 0 && p.blue == 0 && p.alpha == 0;
            check(cleared, "weight 0: the entries cleared (no box could be cut)");
            std::vector<RwUInt8> index(256, 0x33);
            RwPalQuantMatchImage(index.data(), 16, 8, 0, pq, &im.image);
            bool zero = true;
            for (RwUInt8 i : index) zero = zero && i == 0;
            check(zero, "weight 0: every pixel in the one box");
            for (std::size_t j = 1; j < 256; ++j) {
                check(pq->Mcube[j].col1[0] == 0, "weight 0: no box made past the first");
            }
            RwPalQuantTerm(pq);
            delete pq;
        }
        check(g_live == baseline, "nothing leaked by weight 0");

        // ---- a second resolve stopping short: the first one's boxes are not used ----------------------------------
        {
            Image im(16, 16), zero(4, 4);
            for (RwUInt8& b : im.pixels) b = static_cast<RwUInt8>(rng.next());
            for (RwUInt8& b : zero.pixels) b = static_cast<RwUInt8>(rng.next());
            RwPalQuant* const pq = new RwPalQuant{};
            check(RwPalQuantInit(pq) == 1, "stale: Init");
            RwPalQuantAddImage(pq, &im.image, 1.0f);
            std::vector<RwRGBA> palette(64);
            check(RwPalQuantResolvePalette(palette.data(), 64, pq) == 64, "stale: the first resolve cuts 64 boxes");
            check(pq->Mcube[63].col1[0] != 0, "stale: the first resolve made every box");
            RwPalQuantAddImage(pq, &zero.image, 0.0f);   // leaves of weight 0: variance NaN up to the root
            check(RwPalQuantResolvePalette(palette.data(), 64, pq) == 64, "stale: the second resolve returns maxcols");
            bool cleared = true;
            for (std::size_t j = 1; j < 64; ++j) {
                cleared = cleared && palette[j].red == 0 && palette[j].green == 0 && palette[j].blue == 0 && palette[j].alpha == 0;
            }
            check(cleared, "stale: the entries past the one box made stay cleared");
            std::vector<RwUInt8> index(256, 0x33);
            RwPalQuantMatchImage(index.data(), 16, 8, 0, pq, &im.image);
            bool first = true;
            for (RwUInt8 i : index) first = first && i == 0;
            check(first, "stale: every pixel in the one box made");
            RwPalQuantTerm(pq);
            delete pq;
        }
        check(g_live == baseline, "nothing leaked by the second resolve");

        check(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, "shutdown");
        std::printf("  contracts: %u Init failures, %u images with pixels left out\n", initFailures, leftOut);
        std::printf("rwcore_palquant_native_tests: %u checks passed\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_palquant_native_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
