// Native tests of the RenderWare resampling (rwcore/src/resample.cpp, rw/core/image.h "baresamp") for what the oracle
// cannot cover:
// - the definition behind the oracle's bits: every destination byte within one step of the exact box filter (the
//   coverage-weighted average computed in double from the same 16.16 boxes), equal sizes and flat images unchanged,
//   halving the mean of each 2 x 2 block (within one step), the gamma flag carried;
// - the deliberate differences of rw/core/image.h: depths other than 32, missing pixels, steps below 1, sources over
//   32766 pixels, RwImageCreateResample with sizes below 1 and sources RwImageCopy cannot convert (NULL, nothing
//   written, nothing leaked); boxes whose float step reaches past the source (found by search, images of hundreds of
//   pixels) read only the image (run under AddressSanitizer, rwcore/README.md) and match the clamped definition; at
//   the largest weight past the source (3942 rows into 3921) the last row is unchanged by row 0 and by the memory
//   after the pixels (the read past the source is the last row's);
// - RwImageCreateResample's allocations failing in turn (NULL, nothing leaked), depths 4 / 8 / 32, with the image
//   module's free list and without free lists (then the images' own allocations fail too).
// Memory is counted: every part ends with no live allocation.
//
// Usage: rwcore_resample_native_tests
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/image.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
unsigned g_checks = 0;
void check(bool condition, const std::string& message) {
    require(condition, message);
    ++g_checks;
}

std::map<void*, std::size_t> g_live;
long g_mallocs = 0, g_failAt = -1;
void* countMalloc(std::size_t size, RwUInt32) {
    if (g_mallocs++ == g_failAt) return nullptr;
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
    void* q = std::realloc(p, size ? size : 1);
    g_live.erase(p);
    g_live[q] = size;
    return q;
}
void* countCalloc(std::size_t n, std::size_t size, RwUInt32 hint) {
    void* p = countMalloc(n * size, hint);
    if (p != nullptr) std::memset(p, 0, n * size);
    return p;
}

struct Rng {
    std::uint64_t s = 0x2026110220261103ull;
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<std::uint32_t>(s >> 16);
    }
};

/// An image on the stack with its own pixel buffer.
struct Owned {
    std::vector<RwUInt8> bytes;
    RwImage image{};
    Owned(RwInt32 w, RwInt32 h, RwInt32 pad = 0) : bytes(static_cast<std::size_t>(4 * (w + pad)) * static_cast<std::size_t>(h)) {
        image = RwImage{0, w, h, 32, 4 * (w + pad), bytes.data(), nullptr};
    }
    const RwUInt8* at(RwInt32 x, RwInt32 y) const {
        return bytes.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(image.stride) + 4u * static_cast<std::size_t>(x);
    }
};

std::int32_t step(RwInt32 src, RwInt32 dst) {
    return static_cast<std::int32_t>(65536.0f * (static_cast<float>(src) / static_cast<float>(dst)));
}

/// The exact box average of channel c over [x0, x1] x [y0, y1] (16.16, inclusive ends as the original: a box's weight
/// in a pixel is its covered fraction, the last pixel frac(x1)), source coordinates clamped to the image.
double exactBox(const Owned& s, std::int64_t x0, std::int64_t x1, std::int64_t y0, std::int64_t y1, int c) {
    const auto weights = [](std::int64_t a, std::int64_t b) {
        std::vector<std::pair<std::int64_t, double>> w;
        const std::int64_t first = a >> 16, last = b >> 16;
        if (first == last) {
            w.push_back({first, static_cast<double>(b - a) / 65536.0});
        } else {
            w.push_back({first, static_cast<double>(((first + 1) << 16) - a) / 65536.0});
            for (std::int64_t i = first + 1; i < last; ++i) w.push_back({i, 1.0});
            w.push_back({last, static_cast<double>(b - (last << 16)) / 65536.0});
        }
        return w;
    };
    double sum = 0, total = 0;
    for (const auto& [iy, wy] : weights(y0, y1)) {
        double row = 0, rowTotal = 0;
        for (const auto& [ix, wx] : weights(x0, x1)) {
            const RwInt32 cy = static_cast<RwInt32>(std::min<std::int64_t>(iy, s.image.height - 1));
            const RwInt32 cx = static_cast<RwInt32>(std::min<std::int64_t>(ix, s.image.width - 1));
            row += wx * s.at(cx, cy)[c] / 255.0;
            rowTotal += wx;
        }
        (void)rowTotal;
        sum += wy * row / (static_cast<double>(x1 - x0) / 65536.0);
        total += wy;
    }
    (void)total;
    return sum / (static_cast<double>(y1 - y0) / 65536.0);
}

bool reachesPast(RwInt32 src, RwInt32 dst) { return ((static_cast<std::int64_t>(dst) * step(src, dst) - 1) >> 16) >= src; }

void againstDefinition() {
    Rng rng;
    // sizes whose float step reaches past the source (a scan: the first 40 pairs)
    std::vector<std::pair<RwInt32, RwInt32>> pastPairs;
    for (RwInt32 s = 2; s < 4000 && pastPairs.size() < 40; ++s) {
        for (RwInt32 d = 2; d < 4000 && pastPairs.size() < 40; ++d) {
            if (reachesPast(s, d)) pastPairs.push_back({s, d});
        }
    }
    check(!pastPairs.empty(), "pairs reaching past the source exist");
    unsigned compared = 0, past = 0;
    for (unsigned c = 0; c < 600; ++c) {
        RwInt32 sw = 1 + static_cast<RwInt32>(rng.next() % 40u), sh = 1 + static_cast<RwInt32>(rng.next() % 40u);
        RwInt32 dw = 1 + static_cast<RwInt32>(rng.next() % 40u), dh = 1 + static_cast<RwInt32>(rng.next() % 40u);
        if (c % 15u == 0) {   // the step reaches past the source: across, then down
            const auto [s, d] = pastPairs[(c / 15u) % pastPairs.size()];
            if ((c / 15u) % 2u) sw = s, dw = d, sh = 1 + static_cast<RwInt32>(rng.next() % 3u), dh = 1 + static_cast<RwInt32>(rng.next() % 3u);
            else sh = s, dh = d, sw = 1 + static_cast<RwInt32>(rng.next() % 3u), dw = 1 + static_cast<RwInt32>(rng.next() % 3u);
        }
        Owned s(sw, sh, static_cast<RwInt32>(rng.next() % 2u)), d(dw, dh);
        for (auto& b : s.bytes) b = static_cast<RwUInt8>(rng.next());
        s.image.flags = static_cast<RwInt32>(rng.next() % 4u);
        check(RwImageResample(&d.image, &s.image) == &d.image && (d.image.flags & 2) == (s.image.flags & 2),
              "resample " + std::to_string(c));
        const std::int32_t xs = step(sw, dw), ys = step(sh, dh);
        if ((((static_cast<std::int64_t>(dw) * xs - 1) >> 16) >= sw) || (((static_cast<std::int64_t>(dh) * ys - 1) >> 16) >= sh)) ++past;
        for (RwInt32 y = 0; y < dh; ++y) {
            for (RwInt32 x = 0; x < dw; ++x) {
                for (int ch = 0; ch < 4; ++ch) {
                    const std::int64_t x0 = static_cast<std::int64_t>(x) * xs, y0 = static_cast<std::int64_t>(y) * ys;
                    if (xs == 1 || ys == 1) continue;
                    const double v = exactBox(s, x0, x0 + xs - 1, y0, y0 + ys - 1, ch) * 255.0 + 0.5;
                    const int got = d.at(x, y)[ch];
                    require(std::abs(got - std::floor(v)) <= 1.0,
                            "resample " + std::to_string(c) + ": a byte off the box filter by more than one step");
                    ++compared;
                }
            }
        }
    }
    check(past > 20, "boxes reaching past the source were found (" + std::to_string(past) + ")");
    std::printf("  definition: %u bytes within one step of the exact box filter (%u resamples reaching past the source)\n",
                compared, past);
}

/// The read past the source is the last row's: with 3942 rows into 3921 (the largest weight past the source, 2.3e-4,
/// of all sizes below 4000), the last destination row is the same whatever row 0 and the memory after the pixel buffer
/// hold - changing them flips its bytes when that read goes to either (some 6 % of the bytes at that weight).
void pastTheSource() {
    constexpr RwInt32 w = 256, sh = 3942, dh = 3921;
    check(reachesPast(sh, dh), "3942 rows into 3921 reach past the source");
    Rng rng;
    std::vector<RwUInt8> bytes(static_cast<std::size_t>(4 * w) * (sh + 1));   // one row more than the image
    for (RwUInt8& b : bytes) b = static_cast<RwUInt8>(rng.next());
    RwImage src{0, w, sh, 32, 4 * w, bytes.data(), nullptr};
    Owned a(w, dh), b(w, dh);
    check(RwImageResample(&a.image, &src) == &a.image, "past the source: the first resample");
    for (RwInt32 x = 0; x < 4 * w; ++x) {
        bytes[static_cast<std::size_t>(x)] ^= 0xFF;                                     // row 0
        bytes[static_cast<std::size_t>(4 * w) * sh + static_cast<std::size_t>(x)] ^= 0xFF;   // after the buffer
    }
    check(RwImageResample(&b.image, &src) == &b.image, "past the source: the second resample");
    const std::size_t last = static_cast<std::size_t>(4 * w) * (dh - 1);
    check(std::memcmp(a.bytes.data() + last, b.bytes.data() + last, static_cast<std::size_t>(4 * w)) == 0,
          "past the source: the last row reads neither row 0 nor the memory after the pixels");
}

void identities() {
    Rng rng;
    for (unsigned c = 0; c < 200; ++c) {
        const RwInt32 w = 1 + static_cast<RwInt32>(rng.next() % 50u), h = 1 + static_cast<RwInt32>(rng.next() % 50u);
        Owned s(w, h), d(w, h), flat(w, h), fd(3 + w / 2, 2 + h / 3);
        for (auto& b : s.bytes) b = static_cast<RwUInt8>(rng.next());
        RwImageResample(&d.image, &s.image);
        check(d.bytes == s.bytes, "equal sizes copy the pixels");
        const RwUInt8 v[4] = {static_cast<RwUInt8>(rng.next()), static_cast<RwUInt8>(rng.next()), static_cast<RwUInt8>(rng.next()),
                              static_cast<RwUInt8>(rng.next())};
        for (std::size_t i = 0; i < flat.bytes.size(); ++i) flat.bytes[i] = v[i % 4];
        RwImageResample(&fd.image, &flat.image);
        bool same = true;
        for (std::size_t i = 0; i < fd.bytes.size(); ++i) same = same && std::abs(fd.bytes[i] - v[i % 4]) <= 1;
        check(same, "a flat image stays flat (within one step)");
        // halving: each pixel the mean of its 2 x 2 block
        Owned big(2 * w, 2 * h), half(w, h);
        for (auto& b : big.bytes) b = static_cast<RwUInt8>(rng.next());
        RwImageResample(&half.image, &big.image);
        for (RwInt32 y = 0; y < h; ++y) {
            for (RwInt32 x = 0; x < w; ++x) {
                for (int ch = 0; ch < 4; ++ch) {
                    const double mean = (big.at(2 * x, 2 * y)[ch] + big.at(2 * x + 1, 2 * y)[ch] + big.at(2 * x, 2 * y + 1)[ch] +
                                         big.at(2 * x + 1, 2 * y + 1)[ch]) / 4.0;
                    require(std::abs(half.at(x, y)[ch] - mean) <= 1.0, "halving gives the 2 x 2 mean");
                }
            }
        }
        ++g_checks;
    }
}

void contracts() {
    Owned s(8, 8), d(4, 4);
    for (auto& b : s.bytes) b = 0x5A;
    std::fill(d.bytes.begin(), d.bytes.end(), RwUInt8{0xC3});
    const auto untouched = [&] {
        return std::all_of(d.bytes.begin(), d.bytes.end(), [](RwUInt8 b) { return b == 0xC3; }) && d.image.flags == 0;
    };
    s.image.flags = rwIMAGEGAMMACORRECTED;
    for (RwInt32 depth : {4, 8, 16, 24, 0}) {
        s.image.depth = depth;
        check(RwImageResample(&d.image, &s.image) == nullptr && untouched(), "a source of depth " + std::to_string(depth) + " refused");
        s.image.depth = 32;
        d.image.depth = depth;
        check(RwImageResample(&d.image, &s.image) == nullptr && untouched(), "a destination of depth " + std::to_string(depth) + " refused");
        d.image.depth = 32;
    }
    RwUInt8* const keep = s.image.cpPixels;
    s.image.cpPixels = nullptr;
    check(RwImageResample(&d.image, &s.image) == nullptr && untouched(), "a source without pixels refused");
    s.image.cpPixels = keep;
    // steps below 1: a destination over 65536 times the source
    {
        Owned one(1, 1), wide(70000, 1);
        check(RwImageResample(&wide.image, &one.image) == nullptr, "a step of 0 refused");
        Owned zero(1, 1);
        zero.image.width = 0;
        check(RwImageResample(&d.image, &zero.image) == nullptr && untouched(), "a source of width 0 refused");
    }
    // an empty destination: nothing to fill, the flag carried (as the original)
    {
        Owned e(4, 4);
        e.image.width = 0;
        check(RwImageResample(&e.image, &s.image) == &e.image && (e.image.flags & 2), "an empty destination");
    }
    // sources over 32766 pixels (their 16.16 coordinates would overflow)
    {
        Owned big(32767, 1), small(2, 1);
        check(RwImageResample(&small.image, &big.image) == nullptr, "a source of 32767 pixels refused");
        Owned ok(32766, 1), small2(2, 1);
        for (std::size_t i = 0; i < ok.bytes.size(); ++i) ok.bytes[i] = static_cast<RwUInt8>(i % 4 == 3 ? 255 : 100);
        check(RwImageResample(&small2.image, &ok.image) == &small2.image && small2.at(1, 0)[0] == 100 && small2.at(1, 0)[3] == 255,
              "a source of 32766 pixels resampled");
    }
}

/// initFlags 0: images from the module's free list (only pixel allocations can fail); rwENGINEINITNOFREELISTS: every
/// image an RwMalloc too, so the images' own allocations fail in turn as well. Returns the failures.
unsigned createResample(RwUInt32 initFlags) {
    const RwMemoryFunctions funcs{countMalloc, countFree, countRealloc, countCalloc};
    RwEngineOpenParams params{nullptr};
    require(RwEngineInit(&funcs, initFlags, 0) == 1 && RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "engine");
    Rng rng;
    unsigned failures = 0, made = 0;
    for (RwInt32 depth : {32, 8, 4}) {
        for (unsigned c = 0; c < 60; ++c) {
            const RwInt32 w = 1 + static_cast<RwInt32>(rng.next() % 30u), h = 1 + static_cast<RwInt32>(rng.next() % 30u);
            RwImage* const src = RwImageCreate(w, h, depth);
            require(src != nullptr && RwImageAllocatePixels(src) != nullptr, "a source");
            for (RwInt32 i = 0; i < src->stride * h; ++i) src->cpPixels[i] = static_cast<RwUInt8>(rng.next() & (depth == 4 ? 15 : 255));
            if (depth != 32) for (int i = 0; i < (1 << depth); ++i) src->palette[i] = RwRGBA{static_cast<RwUInt8>(rng.next()), 7, 9, 255};
            const RwInt32 dw = 1 + static_cast<RwInt32>(rng.next() % 30u), dh = 1 + static_cast<RwInt32>(rng.next() % 30u);
            // every allocation of the call failing in turn: NULL and nothing leaked, then success (within its at most
            // four allocations)
            for (long k = 0;; ++k) {
                check(k <= 4, "RwImageCreateResample succeeds once its allocations do");
                const std::size_t live = g_live.size();
                g_failAt = g_mallocs + k;
                RwImage* const r = RwImageCreateResample(src, dw, dh);
                g_failAt = -1;
                if (r != nullptr) {
                    check(r->width == dw && r->height == dh && r->depth == 32, "the new image");
                    // the same as resampling a 32-bit copy
                    if (depth != 32) {
                        RwImage* const copy = RwImageCreate(w, h, 32);
                        RwImageAllocatePixels(copy);
                        RwImageCopy(copy, src);
                        RwImage* const direct = RwImageCreateResample(copy, dw, dh);
                        check(std::memcmp(direct->cpPixels, r->cpPixels, static_cast<std::size_t>(r->stride * dh)) == 0,
                              "through the 32-bit copy");
                        RwImageFreePixels(direct), RwImageDestroy(direct), RwImageFreePixels(copy), RwImageDestroy(copy);
                    }
                    RwImageFreePixels(r);
                    RwImageDestroy(r);
                    ++made;
                    break;
                }
                check(g_live.size() == live, "a failed RwImageCreateResample leaks nothing");
                RwError e{};
                RwErrorGet(&e);
                ++failures;
            }
            RwImageFreePixels(src);
            RwImageDestroy(src);
        }
    }
    // sizes below 1 and sources RwImageCopy cannot convert
    RwImage* const src = RwImageCreate(4, 4, 32);
    RwImageAllocatePixels(src);
    const std::size_t live = g_live.size();
    for (auto [w, h] : {std::pair{0, 4}, std::pair{4, 0}, std::pair{-1, 4}, std::pair{4, -3}}) {
        check(RwImageCreateResample(src, w, h) == nullptr && g_live.size() == live, "sizes below 1 refused");
    }
    src->depth = 16;
    check(RwImageCreateResample(src, 2, 2) == nullptr && g_live.size() == live, "a 16-bit source (RwImageCopy refuses) fails");
    {
        RwError e{};
        RwErrorGet(&e);
    }
    src->depth = 32;
    RwImageFreePixels(src);
    RwImageDestroy(src);
    require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, "shutdown");
    check(g_live.empty(), "nothing left");
    std::printf("  RwImageCreateResample (%s): %u images, %u failed allocations without a leak\n",
                initFlags ? "without free lists" : "free lists", made, failures);
    return failures;
}

}  // namespace

int main() {
    try {
        againstDefinition();
        pastTheSource();
        identities();
        contracts();
        require(createResample(0) >= 250, "every pixel allocation failed in turn");
        require(createResample(rwENGINEINITNOFREELISTS) >= 500, "every allocation failed in turn");
        std::printf("rwcore_resample_native_tests: %u checks passed\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_resample_native_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
