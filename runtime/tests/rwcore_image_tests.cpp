// Oracle for the native RenderWare images (rwcore/src/image.cpp, rw/core/image.h): the ORIGINAL baimage / baimras
// functions of PAL main.dol in the reference interpreter (rwcore_oracle.h) and the native ones run the same random
// sessions, with an image plugin, the device's standard functions (pixel to RGB, image from raster, raster from
// image, raster format of an image) and the file system's RwFexist scripted alike on both sides, and image formats
// with read callbacks that hand out images made before the call. Each session opens the image module (now and then
// with an allocation failing), registers up to three formats (one without a read callback), and runs operations:
// images created (depths 0, 1, 4, 8, 16, 24, 32; sizes 0 .. 40), destroyed, given pixels (filled with the same random
// bytes on both sides; allocations failing), freed; masks made and applied (palettised and 32-bit images and masks,
// sizes that differ, invalid depths, the conversion's allocation failing); copies (every depth pair the original
// converts, and the ones it refuses - those through ImageConvertDepth itself, see below); gamma tables set (1, 2.2,
// 0, -0, negative, huge, tiny, infinity, NaN, random) and applied; search paths set (empty, several directories,
// absolute names, past the 256-byte buffer, reallocation failing); RwImageFindFileType and RwImageReadMaskedImage
// along the path (names with and without extensions, after ':' / '/' / '\\', masks, the read failing or handing out
// images of other sizes and depths); the raster exchange and RwRasterGetOffset. After every operation: every image's
// fields (the stride once set), its pixels and palette bytes, the gamma tables and gamma, the path and buffer sizes,
// the format list, every result, every callback in order (plugin constructors / destructors, RwFexist and read names,
// device calls), the engine's memory requests (pixels, path, scratch: size and hint) and the held error.
// Not generated (the deliberate differences, rwcore_image_native_tests): failing plugin constructors, no image module,
// sizes whose byte counts wrap, RwImageCopy of a pair ImageConvertDepth refuses (the original then writes through
// NULL - compared instead as ImageConvertDepth's own result and error against the native RwImageCopy's), the second
// allocation of RwImageApplyMask failing (the original then copies into NULL pixels).
// liveness-roots: reference-interpreter-only
//
// Usage: rwcore_image_tests <main.dol>
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/image.h"
#include "rw/core/memory.h"
#include "rw/core/raster.h"
#include "rw/core/stream.h"
#include "rwcore_oracle.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kImageOpen{"_rwImageOpen", 0x80488964u, 0x29Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kImageClose{"_rwImageClose", 0x80488C00u, 0x158u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kImageCreate{"RwImageCreate", 0x80488D58u, 0xA8u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kImageDestroy{"RwImageDestroy", 0x80488E00u, 0x68u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kAllocatePixels{"RwImageAllocatePixels", 0x80488E68u, 0x138u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kFreePixels{"RwImageFreePixels", 0x80488FA0u, 0x58u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMakeMask{"RwImageMakeMask", 0x80488FF8u, 0xE0u, 0x80A3000Cu, 0x4E800020u};
constexpr Original kApplyMask{"RwImageApplyMask", 0x804890D8u, 0x554u, 0x9421FFC0u, 0x4E800020u};
constexpr Original kSetPath{"RwImageSetPath", 0x8048962Cu, 0xF0u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kForAllFullNames{"ImagePathForAllFullNames", 0x8048971Cu, 0x338u, 0x9421FFC0u, 0x4E800020u};
constexpr Original kAttempRead{"ImageAttempRead", 0x80489A54u, 0x6Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kDetermineExtender{"ImageDetermineExtender", 0x80489AC0u, 0xE8u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kFindFileType{"RwImageFindFileType", 0x80489BA8u, 0x3Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kReadMasked{"RwImageReadMaskedImage", 0x80489BE4u, 0x584u, 0x9421FFD0u, 0x4E800020u};
constexpr Original kSetFromPixel{"RwRGBASetFromPixel", 0x8048A168u, 0x44u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kConvertDepth{"ImageConvertDepth", 0x8048A1ACu, 0x25Cu, 0x9421FFD0u, 0x4E800020u};
constexpr Original kImageCopy{"RwImageCopy", 0x8048A408u, 0x100u, 0x9421FFD0u, 0x4E800020u};
constexpr Original kGammaCorrect{"RwImageGammaCorrect", 0x8048A508u, 0x378u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kSetGamma{"RwImageSetGamma", 0x8048A880u, 0x178u, 0x9421FF70u, 0x4E800020u};
constexpr Original kImageFromRaster{"RwImageSetFromRaster", 0x8048A9F8u, 0x74u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRasterFromImage{"RwRasterSetFromImage", 0x8048AA6Cu, 0x74u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kFindRasterFormat{"RwImageFindRasterFormat", 0x8048AAE0u, 0x9Cu, 0x9421FFA0u, 0x4E800020u};
constexpr Original kRasterGetOffset{"RwRasterGetOffset", 0x8048AB7Cu, 0x14u, 0xA803001Cu, 0x4E800020u};
constexpr Original kPathIsAbsolute{"_rwpathisabsolute", 0x80484494u, 0x58u, 0x88030001u, 0x4E800020u};
constexpr Original kPow{"pow", 0x803B0444u, 0x830u, 0x9421FF50u, 0x4E800020u};
constexpr Original kStringOpen{"_rwStringOpen", 0x80483E70u, 0x140u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kMemoryOpen{"_rwMemoryOpen", 0x804812C4u, 0x118u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRegOpen{"_rwPluginRegistryOpen", 0x80482BE8u, 0x5Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};
constexpr Original kErrorGet{"RwErrorGet", 0x8047EC34u, 0x44u, 0x808D615Cu, 0x4E800020u};
constexpr u32 kFreeListAlloc = 0x80480E00u, kFreeListFree = 0x80480FA0u, kAddPlugin = 0x80482E18u;
constexpr u32 kFexist = 0x8047B6CCu;   // RwFexist: the GameCube's disc file system, stubbed
constexpr u32 kImageModule = 0x805F2668u, kImageTKList = 0x8056F0C0u;
constexpr u32 kGuestErrorOffset = 0x200u, kGuestImageOffset = 0x300u, kGuestImageSize = 0x220u;
constexpr u32 kImg = kEngine + kGuestImageOffset;
// stubs
constexpr u32 kCtor = 0x817F0100u, kDtor = 0x817F0110u, kDevPixel = 0x817F0120u, kDevImageGet = 0x817F0130u,
              kDevRasterSet = 0x817F0140u, kDevFindFormat = 0x817F0150u, kReadA = 0x817F0160u, kReadB = 0x817F0170u;
constexpr u32 kFuncs = kBuffer, kErr = kBuffer + 0x20u, kName = kBuffer + 0x100u, kMaskName = kBuffer + 0x300u,
              kPathBuf = kBuffer + 0x500u, kOut = kBuffer + 0x1000u;
constexpr RwUInt32 kTrackedHints[] = {0x00030018u, 0x01040406u, 0x01040018u};   // pixels, path, scratch

struct Seen {
    int kind;   // 0 ctor, 1 dtor, 2 fexist (a = answer), 3 read (a = format), 4 pixel to RGB, 5 image from raster,
                // 6 raster from image, 7 raster format
    int a, b;
    std::string s;
    bool operator==(const Seen&) const = default;
};
std::string show(const std::vector<Seen>& v) {
    std::string out;
    for (const Seen& x : v) out += " " + std::to_string(x.kind) + "/" + std::to_string(x.a) + "/" + std::to_string(x.b) + "/" + x.s;
    return out;
}

std::vector<u32> g_script;
u32 script(std::size_t& n) { return g_script[n++ % g_script.size()]; }

// ---- the native side's callbacks and memory --------------------------------------------------------------------
std::vector<Seen> g_nseen;
std::size_t g_nnext = 0;
std::map<const void*, int> g_nids;
int nid(const void* p) {
    if (p == nullptr) return -10;
    const auto it = g_nids.find(p);
    return it == g_nids.end() ? -1 : it->second;
}
struct Request {
    std::size_t size;
    RwUInt32 hint;
    bool operator==(const Request&) const = default;
};
std::vector<Request> g_nmallocs, g_nreallocs;
long g_nfailMalloc = -1, g_nfailRealloc = -1;
void* nMalloc(std::size_t size, RwUInt32 hint) {
    const long index = static_cast<long>(g_nmallocs.size());
    g_nmallocs.push_back({size, hint});
    return index == g_nfailMalloc ? nullptr : std::malloc(size ? size : 1);
}
void nFree(void* p) { std::free(p); }
void* nRealloc(void* p, std::size_t size, RwUInt32 hint) {
    const long index = static_cast<long>(g_nreallocs.size());
    g_nreallocs.push_back({size, hint});
    return index == g_nfailRealloc ? nullptr : std::realloc(p, size ? size : 1);
}
void* nCalloc(std::size_t n, std::size_t size, RwUInt32) { return std::calloc(n, size); }

u32 fnv(const std::string& s) {
    u32 h = 2166136261u;
    for (char c : s) h = (h ^ static_cast<u8>(c)) * 16777619u;
    return h;
}
// about three names in eight exist, and every name with "_e" in it (the reads use those to reach their images)
bool exists(const std::string& s) { return (fnv(s) & 7u) < 3u || s.find("_e") != std::string::npos; }

void* nCtor(void* o, RwInt32, RwInt32) {
    g_nseen.push_back({0, nid(o), 0, {}});
    return o;
}
void* nDtor(void* o, RwInt32, RwInt32) {
    g_nseen.push_back({1, nid(o), 0, {}});
    return o;
}
RwBool nFexist(const RwChar* name) {
    const bool e = exists(name);
    g_nseen.push_back({2, e ? 1 : 0, 0, name});
    return e;
}
std::vector<RwImage*> g_ncandidates;
std::size_t g_ngiven = 0;
RwImage* nRead(int format, const RwChar* name) {
    g_nseen.push_back({3, format, 0, name});
    if (script(g_nnext) % 4u == 0 || g_ngiven >= g_ncandidates.size()) return nullptr;
    return g_ncandidates[g_ngiven++];
}
RwImage* nReadA(const RwChar* name) { return nRead(0, name); }
RwImage* nReadB(const RwChar* name) { return nRead(1, name); }
RwBool nDevPixel(void* out, void* in, RwInt32 format) {
    const u32 pixel = *static_cast<RwUInt32*>(in);
    g_nseen.push_back({4, static_cast<int>(pixel), format, {}});
    const u32 v = (pixel * 2654435761u) ^ static_cast<u32>(format);
    std::memcpy(out, &v, 4);
    return 1;
}
RwBool nDevImageGet(void* image, void* raster, RwInt32) {
    g_nseen.push_back({5, nid(image), nid(raster), {}});
    return script(g_nnext) % 3u != 0;
}
RwBool nDevRasterSet(void* raster, void* image, RwInt32) {
    g_nseen.push_back({6, nid(raster), nid(image), {}});
    return script(g_nnext) % 3u != 0;
}
RwBool nDevFindFormat(void* raster, void* image, RwInt32 type) {
    g_nseen.push_back({7, nid(image), type, {}});
    const u32 s = script(g_nnext);
    if (s % 4u == 0) return 0;
    auto* r = static_cast<RwRaster*>(raster);
    r->width = static_cast<RwInt32>(s % 300u), r->height = static_cast<RwInt32>((s >> 9) % 300u);
    r->depth = static_cast<RwInt32>((s >> 18) % 33u), r->cType = static_cast<RwUInt8>(s >> 3);
    r->cFormat = static_cast<RwUInt8>(s >> 11);
    return 1;
}

std::string randomPath(Rng& rng) {
    static const char* const dirs[] = {"textures/", "C:\\games\\shadow\\", "", "a/b/c/", "\\root\\", "x.y/", "D:", "/abs/"};
    const u32 n = rng.next() % 5u;
    std::string p;
    for (u32 i = 0; i < n; ++i) {
        if (i) p += ';';
        if (rng.next() % 11u == 0) {
            p += std::string(150 + rng.next() % 200u, 'd') + "/";   // past the 256-byte buffers
        } else {
            p += dirs[rng.next() % 8u];
        }
    }
    if (n && rng.next() % 6u == 0) p += ';';
    return p;
}
std::string randomName(Rng& rng) {
    static const char* const prefixes[] = {"", "", "", "sub/", "C:\\abs\\", "\\lead\\", "d:", "x.y/", "a:b/c\\", "/slash/"};
    static const char* const bases[] = {"tex", "wall", "a.b", "sky", "f", "n.m.o"};
    static const char* const exts[] = {".bmp", ".BMP", ".png", ".PNG", ".ras", ".RAS", ".xyz", "", ".Bmp", ".a/b"};
    std::string n = prefixes[rng.next() % 10u];
    n += bases[rng.next() % 6u];
    n += exts[rng.next() % 10u];
    if (rng.next() % 13u == 0) n = std::string(200 + rng.next() % 100u, 'n') + ".png";
    return n;
}
float randomGamma(Rng& rng) {
    // the four after the denormal: gammas where pow's result kept in double would change a table byte (found by
    // search; RwImageSetGamma rounds it to float first)
    static const u32 decisive[] = {0x4060D61Du, 0x408C8B7Eu, 0x40AD38B6u, 0x3FB8BECAu};
    const u32 r = rng.next() % 20u;
    if (r >= 16u) return std::bit_cast<float>(decisive[r - 16u]);
    switch (r) {
        case 0: return 1.0f;
        case 1: return 2.2f;
        case 2: return 0.45f;
        case 3: return 0.0f;
        case 4: return -0.0f;
        case 5: return -1.5f;
        case 6: return std::numeric_limits<float>::infinity();
        case 7: return std::bit_cast<float>(0x7FC12345u);   // a quiet NaN
        case 8: return 1e-30f;
        case 9: return 1e30f;
        case 10: return std::bit_cast<float>(0x00000123u);   // a denormal
        default: return 0.05f + static_cast<float>(rng.next() % 100000u) * 5e-5f;
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_image_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kImageOpen, kImageClose, kImageCreate, kImageDestroy, kAllocatePixels, kFreePixels,
                                  kMakeMask, kApplyMask, kSetPath, kForAllFullNames, kAttempRead, kDetermineExtender,
                                  kFindFileType, kReadMasked, kSetFromPixel, kConvertDepth, kImageCopy, kGammaCorrect,
                                  kSetGamma, kImageFromRaster, kRasterFromImage, kFindRasterFormat, kRasterGetOffset,
                                  kPathIsAbsolute, kPow, kStringOpen, kMemoryOpen, kRegOpen, kErrorOpen, kErrorGet}) {
            g.check_pinned(f);
        }
        GuestMemory& m = g.memory();
        Rng rng;
        for (unsigned i = 0; i < 97; ++i) g_script.push_back(rng.next());
        std::map<u32, int> gids;
        const auto gid = [&](u32 a) {
            if (a == 0) return -10;
            const auto it = gids.find(a);
            return it == gids.end() ? -1 : it->second;
        };
        const auto readString = [&](u32 at) {
            std::string s;
            for (u8 c; (c = m.peek8(at)) != 0; ++at) s.push_back(static_cast<char>(c));
            return s;
        };
        const auto writeString = [&](u32 at, const std::string& s) {
            for (std::size_t i = 0; i <= s.size(); ++i) m.write8(at + static_cast<u32>(i), i < s.size() ? static_cast<u8>(s[i]) : 0);
        };
        std::vector<Seen> gseen;
        std::size_t gnext = 0;
        std::vector<u32> gcandidates;
        std::size_t ggiven = 0;
        g.stub(kCtor, [&](Cpu& c) { gseen.push_back({0, gid(c.gpr[3]), 0, {}}); });
        g.stub(kDtor, [&](Cpu& c) { gseen.push_back({1, gid(c.gpr[3]), 0, {}}); });
        g.stub(kFexist, [&](Cpu& c) {
            const std::string name = readString(c.gpr[3]);
            const bool e = exists(name);
            gseen.push_back({2, e ? 1 : 0, 0, name});
            c.gpr[3] = e ? 1u : 0u;
        });
        for (int f = 0; f < 2; ++f) {
            g.stub(f == 0 ? kReadA : kReadB, [&, f](Cpu& c) {
                gseen.push_back({3, f, 0, readString(c.gpr[3])});
                if (script(gnext) % 4u == 0 || ggiven >= gcandidates.size()) {
                    c.gpr[3] = 0;
                } else {
                    c.gpr[3] = gcandidates[ggiven++];
                }
            });
        }
        g.stub(kDevPixel, [&](Cpu& c) {
            const u32 pixel = m.peek32(c.gpr[4]);
            gseen.push_back({4, static_cast<int>(pixel), static_cast<int>(c.gpr[5]), {}});
            const u32 v = (pixel * 2654435761u) ^ c.gpr[5];
            for (u32 i = 0; i < 4; ++i) m.write8(c.gpr[3] + i, static_cast<u8>(v >> (8 * i)));   // host byte order
            c.gpr[3] = 1;
        });
        g.stub(kDevImageGet, [&](Cpu& c) {
            gseen.push_back({5, gid(c.gpr[3]), gid(c.gpr[4]), {}});
            c.gpr[3] = script(gnext) % 3u != 0;
        });
        g.stub(kDevRasterSet, [&](Cpu& c) {
            gseen.push_back({6, gid(c.gpr[3]), gid(c.gpr[4]), {}});
            c.gpr[3] = script(gnext) % 3u != 0;
        });
        g.stub(kDevFindFormat, [&](Cpu& c) {
            gseen.push_back({7, gid(c.gpr[4]), static_cast<int>(c.gpr[5]), {}});
            const u32 s = script(gnext);
            if (s % 4u == 0) {
                c.gpr[3] = 0;
                return;
            }
            const u32 r = c.gpr[3];
            m.write32(r + 0xC, s % 300u), m.write32(r + 0x10, (s >> 9) % 300u), m.write32(r + 0x14, (s >> 18) % 33u);
            m.write8(r + 0x20, static_cast<u8>(s >> 3)), m.write8(r + 0x23, static_cast<u8>(s >> 11));
            c.gpr[3] = 1;
        });

        // ---- the guest engine -----------------------------------------------------------------------------------
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
        g.call(kAddPlugin, {kImageTKList, 8, 0x7104, kCtor, kDtor, 0});
        for (u32 i = 0; i < 29; ++i) m.write32(kEngine + 0x48u + 4u * i, 0);
        m.write32(kEngine + 0x48u + 4u * 3u, kDevPixel), m.write32(kEngine + 0x48u + 4u * 6u, kDevImageGet);
        m.write32(kEngine + 0x48u + 4u * 7u, kDevRasterSet), m.write32(kEngine + 0x48u + 4u * 9u, kDevFindFormat);

        // ---- the native engine ----------------------------------------------------------------------------------
        const RwMemoryFunctions funcs{nMalloc, nFree, nRealloc, nCalloc};
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(&funcs, 0, 0) == 1, "native init");
        require(_rwPluginRegistryAddPlugin(&rwImageRegistry(), 8, 0x7104, nCtor, nDtor, nullptr) >= 0, "native plugin");
        require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "native start");
        RwGlobals& G = rwSrcGlobals();
        G.stdFunc[3] = nDevPixel, G.stdFunc[6] = nDevImageGet, G.stdFunc[7] = nDevRasterSet, G.stdFunc[9] = nDevFindFormat;
        rwFileInterface().fexist = nFexist;
        rwImageGlobals& N = rwModuleGlobals<rwImageGlobals>(imageModule);
        const RwInt32 nativeOffset = imageModule.globalsOffset;
        void* const nativeInstance = RwEngineInstance;
        // the native module is open (RwEngineStart): close it, the sessions open it as the guest's are
        _rwImageClose(nativeInstance, nativeOffset, sizeof(rwImageGlobals));

        struct Img {
            u32 g;
            RwImage* n;
            bool strideKnown;
        };
        std::vector<Img> images;
        int nextId = 0;
        u64 instructions = 0;
        unsigned checks = 0, sessions = 0, failedOpens = 0, reads = 0, readsHit = 0, masked = 0, typesFound = 0,
                 copies = 0, refusedCopies = 0, applied = 0, gammaSets = 0, corrected = 0, allocFailures = 0,
                 pathGrowths = 0;
        std::size_t gmallocSeen = 0, greallocSeen = 0;
        // the image free list's block size on each side (learned at each open): a block it allocates when it has none is
        // a free-list request (its size differs: native images are larger), not one of the module's own
        u32 gImageBlock = 0;
        std::size_t nImageBlock = 0;
        const auto tracked = [](RwUInt32 hint) {
            return std::find(std::begin(kTrackedHints), std::end(kTrackedHints), hint) != std::end(kTrackedHints);
        };
        const auto sameRequests = [&](const std::string& what) {
            std::vector<Request> gm, nm, gr, nr;
            for (std::size_t i = gmallocSeen; i < g.mallocs.size(); ++i) {
                if (tracked(g.mallocs[i].hint) && g.mallocs[i].size != gImageBlock) gm.push_back({g.mallocs[i].size, g.mallocs[i].hint});
            }
            for (const Request& r : g_nmallocs) if (tracked(r.hint) && r.size != nImageBlock) nm.push_back(r);
            for (std::size_t i = greallocSeen; i < g.reallocs.size(); ++i) gr.push_back({g.reallocs[i].size, g.reallocs[i].hint});
            nr = g_nreallocs;
            const auto list = [](const std::vector<Request>& v) {
                std::string s;
                for (const Request& r : v) s += " " + std::to_string(r.size) + "/" + std::to_string(r.hint);
                return s;
            };
            require(gm == nm, what + ": memory requests (malloc) differ; original" + list(gm) + ", native" + list(nm));
            require(gr == nr, what + ": memory requests (realloc) differ; original" + list(gr) + ", native" + list(nr));
            gmallocSeen = g.mallocs.size(), greallocSeen = g.reallocs.size();
            g_nmallocs.clear(), g_nreallocs.clear();
            g.fail_call = -1, g.fail_realloc = -1, g_nfailMalloc = -1, g_nfailRealloc = -1;
        };
        const auto sameCallbacks = [&](const std::string& what) {
            if (gseen != g_nseen) require(false, what + ": callbacks differ; original" + show(gseen) + ", native" + show(g_nseen));
            gseen.clear(), g_nseen.clear();
            instructions += g.call(kErrorGet.entry, {kErr});
            RwError e{};
            RwErrorGet(&e);
            char buf[96];
            std::snprintf(buf, sizeof buf, ": errors (original %08X/%08X, native %08X/%08X)", m.peek32(kErr), m.peek32(kErr + 4),
                          static_cast<u32>(e.pluginID), static_cast<u32>(e.errorCode));
            require(m.peek32(kErr) == static_cast<u32>(e.pluginID) && m.peek32(kErr + 4) == static_cast<u32>(e.errorCode), what + buf);
            sameRequests(what);
        };
        const auto sameImage = [&](const Img& p, const std::string& what) {
            const RwImage& n = *p.n;
            require(m.peek32(p.g) == static_cast<u32>(n.flags) && m.peek32(p.g + 4) == static_cast<u32>(n.width) &&
                        m.peek32(p.g + 8) == static_cast<u32>(n.height) && m.peek32(p.g + 0xC) == static_cast<u32>(n.depth),
                    what + ": image fields");
            if (p.strideKnown) require(m.peek32(p.g + 0x10) == static_cast<u32>(n.stride), what + ": image stride");
            const u32 gp = m.peek32(p.g + 0x14), gpal = m.peek32(p.g + 0x18);
            require((gp == 0) == (n.cpPixels == nullptr) && (gpal == 0) == (n.palette == nullptr), what + ": image pixels / palette presence");
            if (gp != 0 && gpal != 0) {
                require(static_cast<std::ptrdiff_t>(gpal - gp) == reinterpret_cast<RwUInt8*>(n.palette) - n.cpPixels,
                        what + ": palette offset");
            }
            std::vector<u8> bytes;
            if (gp != 0 && p.strideKnown) {
                bytes.resize(static_cast<std::size_t>(n.stride) * static_cast<std::size_t>(n.height));
                m.peek_block(gp, bytes.data(), bytes.size());
                require(std::memcmp(bytes.data(), n.cpPixels, bytes.size()) == 0, what + ": pixel bytes");
            }
            if (gpal != 0 && (n.depth == 4 || n.depth == 8)) {
                bytes.resize(std::size_t{4} << n.depth);
                m.peek_block(gpal, bytes.data(), bytes.size());
                require(std::memcmp(bytes.data(), n.palette, bytes.size()) == 0, what + ": palette bytes");
            }
        };
        const auto sameModule = [&](const std::string& what) {
            for (u32 i = 0; i < 256; ++i) {
                require(m.peek8(kImg + 0xC + i) == N.gammaTable[i], what + ": gamma table entry " + std::to_string(i));
                require(m.peek8(kImg + 0x10C + i) == N.invGammaTable[i], what + ": inverse gamma table entry " + std::to_string(i));
            }
            require(m.peek32(kImg + 0x20C) == std::bit_cast<u32>(N.gammaVal), what + ": gamma");
            require(m.peek32(kImg + 8) == N.imagePathSize && m.peek32(kImg + 0x214) == N.scratchMemSize, what + ": buffer sizes");
            require((m.peek32(kImg + 4) == 0) == (N.imagePath == nullptr), what + ": path presence");
            if (N.imagePath != nullptr) require(readString(m.peek32(kImg + 4)) == N.imagePath, what + ": path");
            for (const Img& p : images) sameImage(p, what);
        };
        const auto addImage = [&](u32 ga, RwImage* na, bool strideKnown) {
            const int id = nextId++;
            gids[ga] = id, g_nids[na] = id;
            images.push_back({ga, na, strideKnown});
        };
        const auto forget = [&](std::size_t k) {
            gids.erase(images[k].g), g_nids.erase(images[k].n);
            images.erase(images.begin() + static_cast<long>(k));
        };
        const auto fill = [&](const Img& p) {   // the same random bytes in the pixels and palette of both
            const RwImage& n = *p.n;
            std::size_t bytes = static_cast<std::size_t>(n.stride) * static_cast<std::size_t>(n.height);
            if (n.palette != nullptr) bytes += std::size_t{4} << n.depth;
            const u32 gp = m.peek32(p.g + 0x14);
            for (std::size_t i = 0; i < bytes; ++i) {
                u8 b = static_cast<u8>(rng.next());
                if (n.depth == 4 && i < static_cast<std::size_t>(n.stride) * n.height) b &= 0x0F;   // indices < 16
                n.cpPixels[i] = b;
            }
            m.write_block(gp, n.cpPixels, bytes);
        };
        // an image made on both sides (created, maybe given pixels and filled); its constructor calls compared
        const auto make = [&](RwInt32 w, RwInt32 h, RwInt32 d, bool pixels, const std::string& what) -> int {
            instructions += g.call(kImageCreate.entry, {static_cast<u32>(w), static_cast<u32>(h), static_cast<u32>(d)});
            const u32 gi = g.r3();
            RwImage* ni = RwImageCreate(w, h, d);
            require(gi != 0 && ni != nullptr, what + ": RwImageCreate");
            const int id = nextId;
            for (Seen& s : gseen) if (s.kind == 0 && s.a == -1) s.a = id;
            for (Seen& s : g_nseen) if (s.kind == 0 && s.a == -1) s.a = id;
            addImage(gi, ni, false);
            if (pixels) {
                instructions += g.call(kAllocatePixels.entry, {gi});
                require(g.r3() == gi && RwImageAllocatePixels(ni) == ni, what + ": RwImageAllocatePixels");
                images.back().strideKnown = true;
                fill(images.back());
            }
            sameCallbacks(what + " (made)");
            return static_cast<int>(images.size()) - 1;
        };
        const auto destroyAt = [&](std::size_t k, const std::string& what) {
            instructions += g.call(kImageDestroy.entry, {images[k].g});
            require(g.r3() == 1 && RwImageDestroy(images[k].n) == 1, what + ": RwImageDestroy");
            forget(k);
        };
        // widths and heights that repeat with the other one different (a mask must match both)
        const RwInt32 sizes[][2] = {{1, 1}, {3, 2}, {7, 5}, {16, 9}, {33, 4}, {0, 3}, {5, 0}, {40, 40}, {7, 3}, {9, 5}};
        const RwInt32 depths[] = {32, 8, 4, 32, 8, 4, 16, 24, 1, 0, 32};
        const auto randomSize = [&](RwInt32& w, RwInt32& h) {
            const auto& s = sizes[rng.next() % 10u];
            w = s[0], h = s[1];
        };
        const auto allocated = [&](const Img& p) { return (p.n->flags & rwIMAGEALLOCATED) != 0; };

        // rasters for the exchange: guest blocks and native rasters
        struct Ras {
            u32 g;
            RwRaster* n;
        };
        std::vector<Ras> rasters;
        std::vector<RwRaster> nativeRasters(3);
        for (int i = 0; i < 3; ++i) {
            const u32 a = g.heap_alloc(0x40);
            for (u32 b = 0; b < 0x40; b += 4) m.write32(a + b, 0);
            rasters.push_back({a, &nativeRasters[static_cast<std::size_t>(i)]});
            gids[a] = 5000 + i, g_nids[rasters.back().n] = 5000 + i;
        }

        // the set-up's own requests (the engines' start-up) are not compared
        gmallocSeen = g.mallocs.size(), greallocSeen = g.reallocs.size();
        g_nmallocs.clear(), g_nreallocs.clear();
        for (unsigned session = 0; session < 100; ++session) {
            const std::string sw = "session " + std::to_string(session);
            // ---- open (every fifth with one of its four allocations failing) --------------------------------------
            if (session % 5u == 4u) {
                const int k = static_cast<int>(rng.next() % 4u);
                g.fail_call = static_cast<int>(g.mallocs.size()) + k;
                g_nfailMalloc = k;
                instructions += g.call(kImageOpen.entry, {kEngine, kGuestImageOffset, kGuestImageSize});
                require(g.r3() == 0 && _rwImageOpen(nativeInstance, nativeOffset, sizeof(rwImageGlobals)) == nullptr,
                        sw + ": a failing _rwImageOpen");
                require(m.peek32(kImg) == 0 && m.peek32(kImg + 0x218) == 0 && m.peek32(kImg + 4) == 0, sw + ": the failed open's leftovers");
                require(N.imageFreeList == nullptr && N.imageFormatFreeList == nullptr && N.imagePath == nullptr, sw + ": the native failed open's leftovers");
                ++failedOpens;
                sameCallbacks(sw + " (failed open)");
            }
            instructions += g.call(kImageOpen.entry, {kEngine, kGuestImageOffset, kGuestImageSize});
            require(g.r3() == kEngine && _rwImageOpen(nativeInstance, nativeOffset, sizeof(rwImageGlobals)) == nativeInstance,
                    sw + ": _rwImageOpen");
            require(g.mallocs.size() - gmallocSeen == 4 && g_nmallocs.size() == 4, sw + ": the open's four allocations");
            gImageBlock = g.mallocs[gmallocSeen].size, nImageBlock = g_nmallocs[0].size;
            require(gImageBlock % 4u != 0 && nImageBlock % 4u != 0, sw + ": block sizes cannot be pixel sizes (multiples of 4)");
            sameCallbacks(sw + " (open)");
            sameModule(sw + " (open)");
            // ---- formats: up to three, the newest first; the last kind has no read callback ---------------------
            const unsigned formats = rng.next() % 5u;
            for (unsigned f = 0; f < formats; ++f) {
                // the fourth extension holds a '/': only the name after the last '/' has an extension
                static const char* const lc[] = {".bmp", ".png", ".ras", ".a/b"};
                static const char* const uc[] = {".BMP", ".PNG", ".RAS", ".A/B"};
                instructions += g.call(kFreeListAlloc, {m.peek32(kImg + 0x218), 0x30406u});
                const u32 gf = g.r3();
                auto* nf = static_cast<rwImageFormat*>(rwSrcGlobals().memoryAlloc(N.imageFormatFreeList, 0x30406u));
                require(gf != 0 && nf != nullptr, sw + ": a format");
                for (u32 i = 0; i < 0x34; i += 4) m.write32(gf + i, 0);
                *nf = {};
                writeString(gf, lc[f]), writeString(gf + 0x14, uc[f]);
                std::snprintf(nf->lcExtension, sizeof nf->lcExtension, "%s", lc[f]), std::snprintf(nf->ucExtension, sizeof nf->ucExtension, "%s", uc[f]);
                m.write32(gf + 0x28, f == 0 || f == 3 ? kReadA : f == 1 ? kReadB : 0u);
                nf->imageRead = f == 0 || f == 3 ? nReadA : f == 1 ? nReadB : nullptr;
                m.write32(gf + 0x30, m.peek32(kImg + 0x21C)), m.write32(kImg + 0x21C, gf);
                nf->nextFormat = N.imageFormatList, N.imageFormatList = nf;
            }
            sameCallbacks(sw + " (formats)");

            for (unsigned step = 0; step < 300; ++step) {
                const u32 op = rng.next() % 100u;
                const std::string what = sw + " op " + std::to_string(op) + " step " + std::to_string(step);
                if (op < 10 && images.size() < 24) {                                                    // create
                    RwInt32 w, h;
                    randomSize(w, h);
                    make(w, h, depths[rng.next() % 11u], false, what);
                } else if (op < 16 && !images.empty()) {                                                // destroy
                    destroyAt(rng.next() % images.size(), what);
                } else if (op < 26 && !images.empty()) {                                                // allocate
                    const std::size_t k = rng.next() % images.size();
                    Img& p = images[k];
                    if (allocated(p)) continue;
                    const bool fail = rng.next() % 6u == 0;
                    if (fail) {
                        g.fail_call = static_cast<int>(g.mallocs.size());
                        g_nfailMalloc = 0;
                        ++allocFailures;
                    }
                    instructions += g.call(kAllocatePixels.entry, {p.g});
                    const u32 gr = g.r3();
                    RwImage* nr = RwImageAllocatePixels(p.n);
                    require(gid(gr) == nid(nr) && (gr == 0) == fail, what + ": RwImageAllocatePixels");
                    p.strideKnown = true;
                    if (!fail) fill(p);
                } else if (op < 30 && !images.empty()) {                                                // free pixels
                    Img& p = images[rng.next() % images.size()];
                    if (!allocated(p) && p.n->cpPixels != nullptr) continue;
                    instructions += g.call(kFreePixels.entry, {p.g});
                    require(g.r3() == p.g && RwImageFreePixels(p.n) == p.n, what + ": RwImageFreePixels");
                } else if (op < 35 && !images.empty()) {                                                // make mask
                    Img& p = images[rng.next() % images.size()];
                    const bool safe = allocated(p) || (p.n->depth != 4 && p.n->depth != 8 && p.n->depth != 32) ||
                                      (p.n->depth == 32 && (p.n->height == 0 || p.n->width == 0));
                    if (!safe) continue;
                    instructions += g.call(kMakeMask.entry, {p.g});
                    require(g.r3() == p.g && RwImageMakeMask(p.n) == p.n, what + ": RwImageMakeMask");
                } else if (op < 43) {                                                                   // apply mask
                    // an image and a mask with pixels (made now or alive), the same size or not
                    RwInt32 w, h;
                    randomSize(w, h);
                    if (images.size() > 20) continue;
                    const RwInt32 imageDepths[] = {32, 8, 4, 16};
                    const RwInt32 maskDepths[] = {32, 8, 4, 16, 24};
                    const int ki = make(w, h, imageDepths[rng.next() % 4u], true, what + " image");
                    RwInt32 mw = w, mh = h;
                    if (rng.next() % 8u == 0) randomSize(mw, mh);
                    const bool self = rng.next() % 10u == 0;
                    const int km = self ? ki : make(mw, mh, maskDepths[rng.next() % 5u], true, what + " mask");
                    const Img pi = images[static_cast<std::size_t>(ki)], pm = images[static_cast<std::size_t>(km)];
                    const bool fail = rng.next() % 7u == 0 && (pi.n->depth == 4 || pi.n->depth == 8);
                    if (fail) {   // the conversion copy's pixels (the first allocation)
                        g.fail_call = static_cast<int>(g.mallocs.size());
                        g_nfailMalloc = 0;
                        ++allocFailures;
                    }
                    instructions += g.call(kApplyMask.entry, {pi.g, pm.g});
                    const u32 gr = g.r3();
                    RwImage* nr = RwImageApplyMask(pi.n, pm.n);
                    require(gid(gr) == nid(nr), what + ": RwImageApplyMask");
                    applied += gr != 0;
                } else if (op < 52) {                                                                   // copy
                    RwInt32 w, h;
                    randomSize(w, h);
                    if (images.size() > 20) continue;
                    // the pairs the original converts: equal depths, 4 -> 8, 4 / 8 -> 32; and refused ones
                    const RwInt32 pairs[][2] = {{32, 32}, {8, 8}, {4, 4}, {16, 16}, {24, 24}, {4, 8}, {4, 32}, {8, 32},
                                                {8, 4}, {32, 8}, {32, 4}, {16, 32}, {32, 16}};
                    const auto& pr = pairs[rng.next() % 13u];
                    const int ks = make(w, h, pr[0], true, what + " source");
                    const int kd = make(w, h, pr[1], true, what + " destination");
                    const Img ps = images[static_cast<std::size_t>(ks)], pd = images[static_cast<std::size_t>(kd)];
                    if (rng.next() % 3u == 0) {   // the gamma-corrected flag travels
                        m.write32(ps.g, m.peek32(ps.g) | 2u), ps.n->flags |= 2;
                    }
                    if (rng.next() % 3u == 0) {   // and replaces the destination's
                        m.write32(pd.g, m.peek32(pd.g) | 2u), pd.n->flags |= 2;
                    }
                    const bool converts = pr[0] == pr[1] || (pr[0] == 4 && pr[1] == 8) || (pr[1] == 32 && (pr[0] == 4 || pr[0] == 8));
                    if (converts) {
                        instructions += g.call(kImageCopy.entry, {pd.g, ps.g});
                        const u32 gr = g.r3();
                        require(gr == pd.g && RwImageCopy(pd.n, ps.n) == pd.n, what + ": RwImageCopy");
                        ++copies;
                    } else {
                        instructions += g.call(kConvertDepth.entry, {pd.g, ps.g});
                        require(g.r3() == 0 && RwImageCopy(pd.n, ps.n) == nullptr, what + ": a refused RwImageCopy");
                        ++refusedCopies;
                    }
                } else if (op < 58 && !images.empty()) {                                                // gamma correct
                    // a palette / pixels exist exactly when the image owns them, so every image is a valid input
                    Img& p = images[rng.next() % images.size()];
                    instructions += g.call(kGammaCorrect.entry, {p.g});
                    const u32 gr = g.r3();
                    RwImage* nr = RwImageGammaCorrect(p.n);
                    require(gid(gr) == nid(nr), what + ": RwImageGammaCorrect");
                    corrected += gr != 0;
                } else if (op < 62) {                                                                   // set gamma
                    const float gamma = randomGamma(rng);
                    instructions += g.call(kSetGamma.entry, {}, {static_cast<double>(gamma)});
                    require(g.r3() == 1 && RwImageSetGamma(gamma) == 1, what + ": RwImageSetGamma");
                    ++gammaSets;
                } else if (op < 68) {                                                                   // set path
                    const std::string path = randomPath(rng);
                    writeString(kPathBuf, path);
                    const bool fail = path.size() + 1 > N.imagePathSize && rng.next() % 4u == 0;
                    if (fail) {
                        g.fail_realloc = static_cast<int>(g.reallocs.size());
                        g_nfailRealloc = 0;
                    }
                    pathGrowths += path.size() + 1 > N.imagePathSize;
                    instructions += g.call(kSetPath.entry, {kPathBuf});
                    const u32 gr = g.r3();
                    const RwChar* nr = RwImageSetPath(path.c_str());
                    require((gr == kPathBuf) == (nr == path.c_str()) && (gr == 0) == (nr == nullptr) && (gr == 0) == fail,
                            what + ": RwImageSetPath");
                } else if (op < 76) {                                                                   // find file type
                    const std::string name = randomName(rng);
                    writeString(kName, name);
                    if (rng.next() % 9u == 0) {   // the scratch buffer cannot grow
                        g.fail_realloc = static_cast<int>(g.reallocs.size());
                        g_nfailRealloc = 0;
                    }
                    instructions += g.call(kFindFileType.entry, {kName});
                    const u32 gr = g.r3();
                    const RwChar* nr = RwImageFindFileType(name.c_str());
                    require((gr == 0) == (nr == nullptr), what + ": RwImageFindFileType");
                    if (gr != 0) {
                        require(readString(gr) == nr, what + ": RwImageFindFileType's extension");
                        ++typesFound;
                    }
                } else if (op < 92) {                                                                   // read
                    if (images.size() > 18) continue;
                    // the images the read callbacks hand out, made first on both sides
                    const RwInt32 readDepths[] = {32, 32, 8, 4, 16};
                    RwInt32 w, h;
                    randomSize(w, h);
                    gcandidates.clear(), g_ncandidates.clear();
                    ggiven = 0, g_ngiven = 0;
                    std::vector<int> candidateIds;
                    for (int c = 0; c < 3; ++c) {
                        RwInt32 cw = w, ch = h;
                        if (rng.next() % 8u == 0) randomSize(cw, ch);
                        const int k = make(cw, ch, readDepths[rng.next() % 5u], true, what + " candidate");
                        gcandidates.push_back(images[static_cast<std::size_t>(k)].g);
                        g_ncandidates.push_back(images[static_cast<std::size_t>(k)].n);
                        candidateIds.push_back(nextId - 1);
                    }
                    // mostly names a registered format reads and the file system has, else any name
                    const auto readName = [&]() {
                        static const char* const exts[] = {".bmp", ".png", ".ras"};
                        if (formats == 0 || rng.next() % 4u == 0) return randomName(rng);
                        std::string n = rng.next() % 3u == 0 ? "sub/" : "";
                        n += rng.next() % 4u == 0 ? "plain" : "wall_e";
                        if (formats == 4u && rng.next() % 3u == 0) return std::string("c.a/b");   // that extension, whole
                        return n + exts[rng.next() % (formats < 2u ? formats : 2u)];   // the formats with a read callback
                    };
                    const std::string name = readName();
                    const bool withMask = rng.next() % 2u == 0;
                    const std::string maskName = rng.next() % 6u == 0 ? std::string() : readName();
                    writeString(kName, name), writeString(kMaskName, maskName);
                    instructions += g.call(kReadMasked.entry, {kName, withMask ? kMaskName : 0u});
                    const u32 gr = g.r3();
                    RwImage* nr = RwImageReadMaskedImage(name.c_str(), withMask ? maskName.c_str() : nullptr);
                    require(gid(gr) == nid(nr) && ggiven == g_ngiven, what + ": RwImageReadMaskedImage");
                    ++reads;
                    readsHit += gr != 0;
                    masked += gr != 0 && withMask && !maskName.empty();
                    // the candidates handed out are either the result or destroyed inside; the rest go now
                    for (std::size_t c = 0; c < candidateIds.size(); ++c) {
                        const int id = candidateIds[c];
                        std::size_t k = 0;
                        while (k < images.size() && gids[images[k].g] != id) ++k;
                        require(k < images.size(), what + ": candidate bookkeeping");
                        if (c < ggiven) {
                            if (images[k].g != gr) forget(k);
                        } else {
                            sameCallbacks(what + " (before the spare's destroy)");
                            destroyAt(k, what + " spare");
                        }
                    }
                    gcandidates.clear(), g_ncandidates.clear();
                } else {                                                                                // the device
                    const u32 kind = rng.next() % 5u;
                    const Ras& r = rasters[rng.next() % rasters.size()];
                    const u8 privateFlags = static_cast<u8>(rng.next());
                    m.write8(r.g + 0x22, privateFlags), r.n->privateFlags = privateFlags;
                    if (kind == 0 && !images.empty()) {
                        const Img& p = images[rng.next() % images.size()];
                        instructions += g.call(kImageFromRaster.entry, {p.g, r.g});
                        const u32 gr = g.r3();
                        require(gid(gr) == nid(RwImageSetFromRaster(p.n, r.n)), what + ": RwImageSetFromRaster");
                    } else if (kind == 1 && !images.empty()) {
                        const Img& p = images[rng.next() % images.size()];
                        instructions += g.call(kRasterFromImage.entry, {r.g, p.g});
                        const u32 gr = g.r3();
                        require(gid(gr) == nid(RwRasterSetFromImage(r.n, p.n)), what + ": RwRasterSetFromImage");
                    } else if (kind == 2 && !images.empty()) {
                        const Img& p = images[rng.next() % images.size()];
                        const u32 type = rng.next() % 8u;
                        for (u32 i = 0; i < 4; ++i) m.write32(kOut + 4 * i, 0xDEAD0000u + i);
                        RwInt32 out[4] = {static_cast<RwInt32>(0xDEAD0000u), static_cast<RwInt32>(0xDEAD0001u),
                                          static_cast<RwInt32>(0xDEAD0002u), static_cast<RwInt32>(0xDEAD0003u)};
                        instructions += g.call(kFindRasterFormat.entry, {p.g, type, kOut, kOut + 4, kOut + 8, kOut + 12});
                        const u32 gr = g.r3();
                        RwImage* nr = RwImageFindRasterFormat(p.n, static_cast<RwInt32>(type), &out[0], &out[1], &out[2], &out[3]);
                        require(gid(gr) == nid(nr), what + ": RwImageFindRasterFormat");
                        for (u32 i = 0; i < 4; ++i) require(m.peek32(kOut + 4 * i) == static_cast<u32>(out[i]), what + ": RwImageFindRasterFormat's outputs");
                    } else if (kind == 3) {
                        const u32 pixel = rng.next(), format = rng.next() % 0x10000u;
                        instructions += g.call(kSetFromPixel.entry, {kOut, pixel, format});
                        RwRGBA rgba{};
                        require(g.r3() == kOut && RwRGBASetFromPixel(&rgba, pixel, static_cast<RwInt32>(format)) == &rgba,
                                what + ": RwRGBASetFromPixel");
                        require(m.peek8(kOut) == rgba.red && m.peek8(kOut + 1) == rgba.green && m.peek8(kOut + 2) == rgba.blue &&
                                    m.peek8(kOut + 3) == rgba.alpha,
                                what + ": RwRGBASetFromPixel's colour");
                    } else {
                        const u16 x = static_cast<u16>(rng.next()), y = static_cast<u16>(rng.next());
                        m.write32(r.g + 0x1C, (u32(x) << 16) | y);
                        r.n->nOffsetX = static_cast<RwInt16>(x), r.n->nOffsetY = static_cast<RwInt16>(y);
                        instructions += g.call(kRasterGetOffset.entry, {r.g, kOut, kOut + 2});
                        RwInt16 nx = 0, ny = 0;
                        require(g.r3() == r.g && RwRasterGetOffset(r.n, &nx, &ny) == r.n, what + ": RwRasterGetOffset");
                        require(m.peek16(kOut) == static_cast<u16>(nx) && m.peek16(kOut + 2) == static_cast<u16>(ny),
                                what + ": RwRasterGetOffset's offsets");
                    }
                    require(m.peek8(r.g + 0x22) == r.n->privateFlags, what + ": raster flags");
                }
                sameCallbacks(what);
                sameModule(what);
                ++checks;
            }
            // ---- close: every image destroyed, then the module ------------------------------------------------
            while (!images.empty()) destroyAt(images.size() - 1, sw + " (close)");
            sameCallbacks(sw + " (images destroyed)");
            instructions += g.call(kImageClose.entry, {kEngine, kGuestImageOffset, kGuestImageSize});
            require(g.r3() == kEngine && _rwImageClose(nativeInstance, nativeOffset, sizeof(rwImageGlobals)) == nativeInstance,
                    sw + ": _rwImageClose");
            require(m.peek32(kImg) == 0 && m.peek32(kImg + 4) == 0 && m.peek32(kImg + 0x210) == 0 && m.peek32(kImg + 0x218) == 0 &&
                        m.peek32(kImg + 0x21C) == 0 && m.peek32(kImg + 8) == 0 && m.peek32(kImg + 0x214) == 0,
                    sw + ": the original's closed module");
            require(N.imageFreeList == nullptr && N.imagePath == nullptr && N.scratchMem == nullptr &&
                        N.imageFormatFreeList == nullptr && N.imageFormatList == nullptr && N.imagePathSize == 0 && N.scratchMemSize == 0,
                    sw + ": the native closed module");
            sameCallbacks(sw + " (close)");
            ++sessions;
        }
        // the native module open again for the engine's own close
        require(_rwImageOpen(nativeInstance, nativeOffset, sizeof(rwImageGlobals)) == nativeInstance, "the final open");
        rwFileInterface().fexist = nullptr;
        require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, "native shutdown");
        std::printf("  sessions %u (%u failed opens), reads %u (%u images, %u masked), types found %u, copies %u (+%u refused),"
                    " masks applied %u, gamma sets %u, corrections %u, allocation failures %u, path growths %u\n",
                    sessions, failedOpens, reads, readsHit, masked, typesFound, copies, refusedCopies, applied, gammaSets,
                    corrected, allocFailures, pathGrowths);
        require(readsHit > 150 && masked > 20 && typesFound > 800 && copies > 250 && refusedCopies > 150 && applied > 250 &&
                    gammaSets > 1000 && corrected > 600 && allocFailures > 150 && pathGrowths > 100 && failedOpens >= 20,
                "the sessions must reach every case");
        std::printf("rwcore_image_tests: %u operations identical to the original (%llu original instructions)\n", checks,
                    static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_image_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
