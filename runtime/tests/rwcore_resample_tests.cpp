// Oracle for the native RenderWare resampling (rwcore/src/resample.cpp, rw/core/image.h "baresamp"): the ORIGINAL
// RwImageResample (0x8048B7E0, with ImageResampleGetAvgPixel 0x8048B55C and ImageResampleGetSpan 0x8048B1C8) and
// RwImageCreateResample (0x8048B9BC) of PAL main.dol in the reference interpreter (rwcore_oracle.h) against the native
// ones.
//
// Part 1, RwImageResample on images built alike on both sides: 32-bit sources of 1 .. 40 pixels a side (now and then
// up to 700 in one direction), random bytes, flat 0 / 255 and gradients, strides with padding, the gamma flag set or
// not; destinations of 1 .. 40 a side, 1 .. 12 for the long sources (both directions of scaling, equal sizes, 1-pixel
// rows and columns), a few widths over 32768 times the source's (a step of 1: every span 0 wide, the original's NaNs);
// empty destinations.
// After each call: the result, the destination's flags and every pixel byte (padding included).
// Part 2, RwImageCreateResample through the image module (the guest's opened as in rwcore_image_tests): sources of
// depth 32, 8 and 4 (palettes: through RwImageCopy) at random sizes into random sizes, the gamma flag, each pixel
// allocation failing in turn. After each call: NULL or not, the new image's fields and pixels, the pixel requests
// (size, hint) and how many the call freed, the held error.
// Not generated (the deliberate differences, rwcore_resample_native_tests): steps below 1 (the original loops on a
// wrapped count), depths other than 32 for RwImageResample and other than 4 / 8 / 32 for RwImageCreateResample (the
// original reads or resamples memory that is not the image's), sizes below 1, sources over 32766 pixels, and steps
// whose float rounding makes the last box reach past the source (the original reads after the buffer: the cases are
// predicted from the step and skipped, and counted).
// liveness-roots: reference-interpreter-only
//
// Usage: rwcore_resample_tests <main.dol>
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/image.h"
#include "rwcore_oracle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kSpan{"ImageResampleGetSpan", 0x8048B1C8u, 0x394u, 0x7C042850u, 0x4E800020u};
constexpr Original kAvgPixel{"ImageResampleGetAvgPixel", 0x8048B55Cu, 0x284u, 0x9421FFB0u, 0x4E800020u};
constexpr Original kResample{"RwImageResample", 0x8048B7E0u, 0x1DCu, 0x9421FF60u, 0x4E800020u};
constexpr Original kCreateResample{"RwImageCreateResample", 0x8048B9BCu, 0x474u, 0x9421FF50u, 0x4E800020u};
constexpr Original kImageOpen{"_rwImageOpen", 0x80488964u, 0x29Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kImageCreate{"RwImageCreate", 0x80488D58u, 0xA8u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kImageDestroy{"RwImageDestroy", 0x80488E00u, 0x68u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kAllocatePixels{"RwImageAllocatePixels", 0x80488E68u, 0x138u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kFreePixels{"RwImageFreePixels", 0x80488FA0u, 0x58u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kStringOpen{"_rwStringOpen", 0x80483E70u, 0x140u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kMemoryOpen{"_rwMemoryOpen", 0x804812C4u, 0x118u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRegOpen{"_rwPluginRegistryOpen", 0x80482BE8u, 0x5Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};
constexpr Original kErrorGet{"RwErrorGet", 0x8047EC34u, 0x44u, 0x808D615Cu, 0x4E800020u};
constexpr u32 kFreeListAlloc = 0x80480E00u, kFreeListFree = 0x80480FA0u;
constexpr u32 kImageModule = 0x805F2668u;
constexpr u32 kGuestErrorOffset = 0x200u, kGuestImageOffset = 0x300u, kGuestImageSize = 0x220u;
constexpr u32 kFuncs = kBuffer, kErr = kBuffer + 0x20u, kDst = kBuffer + 0x40u, kSrc = kBuffer + 0x80u;
constexpr RwUInt32 kPixelsHint = 0x00030018u;

// ---- the native side's memory ----------------------------------------------------------------------------------
struct Request {
    std::size_t size;
    RwUInt32 hint;
    bool operator==(const Request&) const = default;
};
std::vector<Request> g_nmallocs;
long g_nfailMalloc = -1, g_nfrees = 0;
void* nMalloc(std::size_t size, RwUInt32 hint) {
    const long index = static_cast<long>(g_nmallocs.size());
    g_nmallocs.push_back({size, hint});
    return index == g_nfailMalloc ? nullptr : std::malloc(size ? size : 1);
}
void nFree(void* p) {
    ++g_nfrees;   // NULL too: the original's RwImageFreePixels calls the free function whatever it holds
    std::free(p);
}
void* nRealloc(void* p, std::size_t size, RwUInt32) { return std::realloc(p, size ? size : 1); }
void* nCalloc(std::size_t n, std::size_t size, RwUInt32) { return std::calloc(n, size); }

/// The x (or y) step of the original and whether its last box reaches past the source.
bool reachesPast(RwInt32 src, RwInt32 dst) {
    const float q = static_cast<float>(src) / static_cast<float>(dst);
    const float s = 65536.0f * q;
    const long long step = static_cast<long long>(s);
    return ((static_cast<long long>(dst) * step - 1) >> 16) >= src;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_resample_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kSpan, kAvgPixel, kResample, kCreateResample, kImageOpen, kImageCreate, kImageDestroy,
                                  kAllocatePixels, kFreePixels, kStringOpen, kMemoryOpen, kRegOpen, kErrorOpen, kErrorGet}) {
            g.check_pinned(f);
        }
        GuestMemory& m = g.memory();
        Rng rng;
        u64 instructions = 0;
        unsigned checks = 0;

        const auto fillPixels = [&](std::vector<u8>& bytes, int kind) {
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] = kind == 0 ? static_cast<u8>(rng.next()) : kind == 1 ? u8{0} : kind == 2 ? u8{255}
                                                                       : static_cast<u8>(i * 7u + (i >> 5));
            }
        };

        // ---- part 1: RwImageResample -----------------------------------------------------------------------------
        unsigned resamples = 0, skipped = 0, upscales = 0, downscales = 0, stepOne = 0, empty = 0, padded = 0;
        for (unsigned c = 0; c < 2500; ++c) {
            const std::string what = "resample " + std::to_string(c);
            const u32 shape = rng.next() % 20u;
            RwInt32 sw = 1 + static_cast<RwInt32>(rng.next() % 40u), sh = 1 + static_cast<RwInt32>(rng.next() % 40u);
            RwInt32 dw = 1 + static_cast<RwInt32>(rng.next() % 40u), dh = 1 + static_cast<RwInt32>(rng.next() % 40u);
            if (shape == 0 || shape == 1) dw = 1 + dw % 12, dh = 1 + dh % 12;   // long sources, small destinations
            if (shape == 0) sw = 64 + static_cast<RwInt32>(rng.next() % 640u), sh = 1 + static_cast<RwInt32>(rng.next() % 3u);
            if (shape == 1) sh = 64 + static_cast<RwInt32>(rng.next() % 640u), sw = 1 + static_cast<RwInt32>(rng.next() % 3u);
            if (shape == 2) dw = sw, dh = sh;
            if (shape == 3) dw = 1;
            if (shape == 4) dh = 1;
            if (c % 500u == 7u) sw = 1, sh = 1, dw = 32769 + static_cast<RwInt32>(rng.next() % 30000u), dh = 1;
            if (shape == 6) (rng.next() % 2u ? dw : dh) = 0;
            if (dw > 0 && dh > 0 && (reachesPast(sw, dw) || reachesPast(sh, dh))) {
                ++skipped;
                continue;
            }
            const RwInt32 spad = rng.next() % 4u == 0 ? static_cast<RwInt32>(rng.next() % 3u) : 0;
            const RwInt32 dpad = rng.next() % 4u == 0 ? static_cast<RwInt32>(rng.next() % 3u) : 0;
            const RwInt32 sstride = 4 * (sw + spad), dstride = 4 * (dw + dpad);
            std::vector<u8> src(static_cast<std::size_t>(sstride) * static_cast<std::size_t>(sh)),
                dst(static_cast<std::size_t>(dstride) * static_cast<std::size_t>(dh));
            fillPixels(src, static_cast<int>(rng.next() % 6u) % 4);
            fillPixels(dst, 0);
            const RwInt32 sflags = static_cast<RwInt32>(rng.next() % 4u), dflags = static_cast<RwInt32>(rng.next() % 4u);
            g.reset_engine();
            const u32 gs = g.heap_alloc(static_cast<u32>(src.size())), gd = g.heap_alloc(static_cast<u32>(dst.size()) + 4u);
            m.write_block(gs, src.data(), src.size());
            if (!dst.empty()) m.write_block(gd, dst.data(), dst.size());
            const auto writeImage = [&](u32 at, RwInt32 flags, RwInt32 w, RwInt32 h, RwInt32 stride, u32 pixels) {
                m.write32(at, static_cast<u32>(flags)), m.write32(at + 4, static_cast<u32>(w)), m.write32(at + 8, static_cast<u32>(h));
                m.write32(at + 0xC, 32), m.write32(at + 0x10, static_cast<u32>(stride)), m.write32(at + 0x14, pixels);
                m.write32(at + 0x18, 0);
            };
            writeImage(kSrc, sflags, sw, sh, sstride, gs);
            writeImage(kDst, dflags, dw, dh, dstride, gd);
            RwImage ns{sflags, sw, sh, 32, sstride, src.data(), nullptr};
            RwImage nd{dflags, dw, dh, 32, dstride, dst.data(), nullptr};
            instructions += g.call(kResample.entry, {kDst, kSrc});
            require(g.r3() == kDst && RwImageResample(&nd, &ns) == &nd, what + ": the result");
            require(static_cast<RwInt32>(m.peek32(kDst)) == nd.flags, what + ": the flags");
            std::vector<u8> gbytes(dst.size());
            if (!dst.empty()) m.peek_block(gd, gbytes.data(), gbytes.size());
            for (std::size_t i = 0; i < dst.size(); ++i) {
                if (gbytes[i] != dst[i]) {
                    char buf[200];
                    std::snprintf(buf, sizeof buf, ": byte %zu (pixel %zu of %dx%d from %dx%d) original %02X native %02X",
                                  i, i / 4, dw, dh, sw, sh, gbytes[i], dst[i]);
                    require(false, what + buf);
                }
            }
            ++resamples, ++checks;
            if (dw == 0 || dh == 0) ++empty;
            else if (dw > 32768 * sw) ++stepOne;
            else if (dw * dh > sw * sh) ++upscales;
            else ++downscales;
            if (spad || dpad) ++padded;
        }
        std::printf("  RwImageResample: %u calls (%u enlarging, %u reducing, %u with a step of 1, %u empty, %u padded), "
                    "%u predicted to read past the source skipped\n",
                    resamples, upscales, downscales, stepOne, empty, padded, skipped);
        std::printf("  (part 1: %llu original instructions)\n", static_cast<unsigned long long>(instructions));
        require(resamples > 2300 && upscales > 700 && downscales > 700 && stepOne >= 2 && empty > 50 && padded > 500,
                "part 1 must reach every case");

        // ---- part 2: RwImageCreateResample through the image module -------------------------------------------------
        g.reset_engine();
        for (u32 a : {kErrorModule, kErrorModule + 4u, kImageModule, kImageModule + 4u}) m.write32(a, 0);
        m.write32(kFuncs, kMallocStub), m.write32(kFuncs + 4, kFreeStub), m.write32(kFuncs + 8, kReallocStub);
        m.write32(kFuncs + 12, 0);
        g.call(kMemoryOpen.entry, {kFuncs});
        m.write32(kEngine + 0x118u, kFreeListAlloc), m.write32(kEngine + 0x11Cu, kFreeListFree);
        g.call(kErrorOpen.entry, {kEngine, kGuestErrorOffset, 8});
        g.call(kRegOpen.entry, {});
        g.call(kStringOpen.entry, {});
        m.write32(0x805F2658u, 0);
        instructions += g.call(kImageOpen.entry, {kEngine, kGuestImageOffset, kGuestImageSize});
        require(g.r3() == kEngine, "the guest image module");

        const RwMemoryFunctions funcs{nMalloc, nFree, nRealloc, nCalloc};
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(&funcs, 0, 0) == 1, "native init");
        require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "native start");
        {
            RwError e{};
            RwErrorGet(&e);
        }
        const auto pixelRequests = [](const std::vector<Request>& v, std::size_t from) {
            std::vector<Request> out;
            for (std::size_t i = from; i < v.size(); ++i) if (v[i].hint == kPixelsHint) out.push_back(v[i]);
            return out;
        };
        unsigned created = 0, failed = 0, converted = 0, gammas = 0;
        for (unsigned c = 0; c < 1500; ++c) {
            const RwInt32 depth = (rng.next() % 3u == 0) ? (rng.next() % 2u ? 8 : 4) : 32;
            const std::string what = "create " + std::to_string(c) + " (depth " + std::to_string(depth) + ")";
            const RwInt32 sw = 1 + static_cast<RwInt32>(rng.next() % 32u), sh = 1 + static_cast<RwInt32>(rng.next() % 32u);
            RwInt32 dw = 1 + static_cast<RwInt32>(rng.next() % 32u), dh = 1 + static_cast<RwInt32>(rng.next() % 32u);
            if (rng.next() % 5u == 0) dw = (sw + 1) / 2, dh = (sh + 1) / 2;   // as the mipmap builder asks
            if (reachesPast(sw, dw) || reachesPast(sh, dh)) continue;
            const bool gamma = rng.next() % 3u == 0;
            // the source on both sides: created by the module, pixels and palette from the same bytes
            instructions += g.call(kImageCreate.entry, {static_cast<u32>(sw), static_cast<u32>(sh), static_cast<u32>(depth)});
            const u32 gsrc = g.r3();
            instructions += g.call(kAllocatePixels.entry, {gsrc});
            RwImage* const nsrc = RwImageCreate(sw, sh, depth);
            require(gsrc != 0 && nsrc != nullptr && RwImageAllocatePixels(nsrc) != nullptr, what + ": the source");
            const RwInt32 gstride = static_cast<RwInt32>(m.peek32(gsrc + 0x10));
            require(gstride == nsrc->stride, what + ": the source stride");
            std::vector<u8> bytes(static_cast<std::size_t>(gstride) * static_cast<std::size_t>(sh));
            fillPixels(bytes, static_cast<int>(rng.next() % 5u) % 4);
            if (depth == 4) for (u8& v : bytes) v &= 15u;   // indices into the 16-entry palette
            m.write_block(m.peek32(gsrc + 0x14), bytes.data(), bytes.size());
            std::memcpy(nsrc->cpPixels, bytes.data(), bytes.size());
            if (depth != 32) {
                std::vector<u8> pal(4u << depth);
                fillPixels(pal, 0);
                m.write_block(m.peek32(gsrc + 0x18), pal.data(), pal.size());
                std::memcpy(nsrc->palette, pal.data(), pal.size());
            }
            if (gamma) {
                m.write32(gsrc, m.peek32(gsrc) | 2u);
                nsrc->flags |= rwIMAGEGAMMACORRECTED;
            }
            // one of the call's pixel allocations failing now and then
            const std::size_t gFrom = g.mallocs.size(), gFreed = g.frees.size();
            g_nmallocs.clear(), g_nfrees = 0;
            const int fail = rng.next() % 6u == 0 ? static_cast<int>(rng.next() % (depth == 32 ? 1u : 2u)) : -1;
            if (fail >= 0) g.fail_call = static_cast<int>(gFrom) + fail, g_nfailMalloc = fail;
            instructions += g.call(kCreateResample.entry, {gsrc, static_cast<u32>(dw), static_cast<u32>(dh)});
            RwImage* const n = RwImageCreateResample(nsrc, dw, dh);
            g.fail_call = -1, g_nfailMalloc = -1;
            const u32 gr = g.r3();
            require((gr == 0) == (n == nullptr), what + ": NULL or not");
            std::vector<Request> gm;
            for (std::size_t i = gFrom; i < g.mallocs.size(); ++i) gm.push_back({g.mallocs[i].size, g.mallocs[i].hint});
            require(pixelRequests(gm, 0) == pixelRequests(g_nmallocs, 0), what + ": the pixel requests");
            require(fail < 0 || (static_cast<std::size_t>(fail) < gm.size() && gm[static_cast<std::size_t>(fail)].hint == kPixelsHint &&
                                 static_cast<std::size_t>(fail) < g_nmallocs.size() &&
                                 g_nmallocs[static_cast<std::size_t>(fail)].hint == kPixelsHint),
                    what + ": the failed allocation is a pixel allocation on both sides");
            require(static_cast<long>(g.frees.size() - gFreed) == g_nfrees, what + ": the frees");
            instructions += g.call(kErrorGet.entry, {kErr});
            RwError e{};
            RwErrorGet(&e);
            require(m.peek32(kErr) == static_cast<u32>(e.pluginID) && m.peek32(kErr + 4) == static_cast<u32>(e.errorCode),
                    what + ": the held error");
            if (gr) {
                require(m.peek32(gr) == static_cast<u32>(n->flags) && m.peek32(gr + 4) == static_cast<u32>(n->width) &&
                            m.peek32(gr + 8) == static_cast<u32>(n->height) && m.peek32(gr + 0xC) == static_cast<u32>(n->depth) &&
                            m.peek32(gr + 0x10) == static_cast<u32>(n->stride),
                        what + ": the new image's fields");
                std::vector<u8> gp(static_cast<std::size_t>(n->stride) * static_cast<std::size_t>(n->height));
                m.peek_block(m.peek32(gr + 0x14), gp.data(), gp.size());
                require(std::memcmp(gp.data(), n->cpPixels, gp.size()) == 0, what + ": the new image's pixels");
                instructions += g.call(kFreePixels.entry, {gr});
                instructions += g.call(kImageDestroy.entry, {gr});
                RwImageFreePixels(n);
                RwImageDestroy(n);
                ++created;
                if (depth != 32) ++converted;
                if (gamma) ++gammas;
            } else {
                ++failed;
            }
            instructions += g.call(kFreePixels.entry, {gsrc});
            instructions += g.call(kImageDestroy.entry, {gsrc});
            RwImageFreePixels(nsrc);
            RwImageDestroy(nsrc);
            ++checks;
        }
        require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, "native shutdown");
        std::printf("  RwImageCreateResample: %u images (%u converted from 4 / 8 bits, %u gamma-corrected), %u failed "
                    "allocations\n",
                    created, converted, gammas, failed);
        require(created > 1000 && converted > 300 && gammas > 300 && failed > 100, "part 2 must reach every case");
        std::printf("rwcore_resample_tests: %u operations identical to the original (%llu original instructions)\n", checks,
                    static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_resample_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
