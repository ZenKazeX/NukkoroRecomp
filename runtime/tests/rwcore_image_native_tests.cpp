// Native tests of the RenderWare images (rwcore/src/image.cpp, rw/core/image.h) for what the oracle cannot cover:
// - the engine's life: the image module in its slot (open at RwEngineStart, closed at RwEngineStop), the device's
//   gamma applied at RwEngineStart, every allocation of the module's open failing in turn (NULL, nothing leaked, not
//   counted as open, a later start-up succeeds);
// - the definitions behind the oracle's bits: gamma tables against double-precision powers (within one step, gamma 1
//   the identity, monotone), strides and palette placement for every depth, RwImageMakeMask / ApplyMask / Copy /
//   GammaCorrect against what they mean per pixel;
// - the deliberate differences of rw/core/image.h: failing plugin constructors, no image module, sizes whose byte
//   counts would wrap, the conversion copy's second allocation failing in RwImageApplyMask, a refused RwImageCopy
//   leaving the destination as it was, RwImageFindRasterFormat's zeroed raster;
// - the search: full names built along a path (directories, absolute names, no path), RwImageFindFileType and
//   RwImageReadMaskedImage with formats, RwFexist through the file interface and its fopen fallback on a real file;
// - the host device: no image / raster exchange yet (its standard functions say "not supported").
// Memory is counted: every part ends with no live allocation.
//
// Usage: rwcore_image_native_tests
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/image.h"
#include "rw/core/memory.h"
#include "rw/core/raster.h"
#include "rw/core/stream.h"

#include <algorithm>
#include <cmath>
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
    if (g_mallocs++ == g_failAt) return nullptr;
    void* q = std::realloc(p, size);
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

int g_ctorFail = -1, g_ctorCount = 0, g_dtorCount = 0;
void* ctor(void* o, RwInt32, RwInt32) { return g_ctorCount++ == g_ctorFail ? nullptr : o; }
void* dtor(void* o, RwInt32, RwInt32) {
    ++g_dtorCount;
    return o;
}

void startEngine(RwReal deviceGamma = 1.0f) {
    RwEngineOpenParams params{nullptr};
    rwDeviceHandle()->gammaCorrection = deviceGamma;
    require(RwEngineInit(&kCounting, 0, 0) == 1, "init");
    require(_rwPluginRegistryAddPlugin(&rwImageRegistry(), 4, 0x7204, ctor, dtor, nullptr) >= 0, "plugin");
    require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "start");
    rwDeviceHandle()->gammaCorrection = 1.0f;
}
void stopEngine(const char* what) {
    require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, std::string(what) + ": shutdown");
    require(g_live.empty(), std::string(what) + ": " + std::to_string(g_live.size()) + " allocations leaked");
}
rwImageGlobals& globals() { return rwModuleGlobals<rwImageGlobals>(imageModule); }

RwImage* made(RwInt32 w, RwInt32 h, RwInt32 d, unsigned seed) {
    RwImage* image = RwImageCreate(w, h, d);
    require(image != nullptr && RwImageAllocatePixels(image) == image, "an image with pixels");
    const std::size_t bytes = static_cast<std::size_t>(image->stride) * static_cast<std::size_t>(h);
    for (std::size_t i = 0; i < bytes; ++i) image->cpPixels[i] = static_cast<RwUInt8>((seed + i * 37u) % (d == 4 ? 16u : 256u));
    if (image->palette != nullptr) {
        for (int i = 0; i < (1 << d); ++i) image->palette[i] = {static_cast<RwUInt8>(i * 3 + seed), static_cast<RwUInt8>(i * 5),
                                                                static_cast<RwUInt8>(255 - i), static_cast<RwUInt8>(i * 7 + 1)};
    }
    return image;
}
RwRGBA pixel(const RwImage* image, int x, int y) {
    const RwUInt8* row = image->cpPixels + static_cast<std::ptrdiff_t>(y) * image->stride;
    if (image->depth == 32) return {row[4 * x], row[4 * x + 1], row[4 * x + 2], row[4 * x + 3]};
    return image->palette[row[x]];
}

// the file interface's existence answers: a set of names; the calls in order
std::vector<std::string> g_existing, g_asked;
RwBool fakeExist(const RwChar* name) {
    g_asked.emplace_back(name);
    for (const std::string& e : g_existing) if (e == name) return 1;
    return 0;
}
std::vector<std::string> g_readNames;
RwImage* g_toRead[2] = {};
int g_readNext = 0;
RwImage* readCallBack(const RwChar* name) {
    g_readNames.emplace_back(name);
    return g_readNext < 2 ? g_toRead[g_readNext++] : nullptr;
}
rwImageFormat* addFormat(const char* lc, const char* uc, RwImageCallBackRead read) {
    rwImageGlobals& g = globals();
    auto* f = static_cast<rwImageFormat*>(rwSrcGlobals().memoryAlloc(g.imageFormatFreeList, 0x30406u));
    require(f != nullptr, "a format");
    *f = {};
    std::snprintf(f->lcExtension, sizeof f->lcExtension, "%s", lc), std::snprintf(f->ucExtension, sizeof f->ucExtension, "%s", uc);
    f->imageRead = read;
    f->nextFormat = g.imageFormatList;
    g.imageFormatList = f;
    return f;
}

RwBool findNothing(void*, void*, RwInt32) { return 1; }   // a device that fills no field of the raster

}  // namespace

int main() {
    try {
        // ---- the engine's life ----------------------------------------------------------------------------------
        startEngine(2.2f);
        check(imageModule.numInstances == 1 && globals().imageFreeList != nullptr && globals().imagePath != nullptr &&
                  globals().imagePath[0] == 0 && globals().imagePathSize == 256 && globals().scratchMemSize == 256 &&
                  globals().imageFormatList == nullptr,
              "the image module opens with the engine");
        check(globals().gammaVal == 2.2f, "RwEngineStart applies the device's gamma");
        const rwImageGlobals deviceGamma = globals();
        RwImageSetGamma(2.2f);
        check(std::memcmp(deviceGamma.gammaTable, globals().gammaTable, 256) == 0 &&
                  std::memcmp(deviceGamma.invGammaTable, globals().invGammaTable, 256) == 0,
              "the start-up tables are RwImageSetGamma's");
        stopEngine("life");
        check(imageModule.numInstances == 0, "the image module closes with the engine");
        // every allocation of the module's own open failing in turn (in a started engine, the module closed first)
        for (long k = 0;; ++k) {
            startEngine();
            const RwInt32 offset = imageModule.globalsOffset;
            _rwImageClose(RwEngineInstance, offset, sizeof(rwImageGlobals));
            require(imageModule.numInstances == 0, "closed");
            g_mallocs = 0;
            g_failAt = k;
            void* opened = _rwImageOpen(RwEngineInstance, offset, sizeof(rwImageGlobals));
            g_failAt = -1;
            if (opened != nullptr) {
                check(k == 4 && g_mallocs == 4 && imageModule.numInstances == 1, "the open makes exactly four allocations");
                stopEngine("open");
                break;
            }
            check(imageModule.numInstances == 0 && globals().imageFreeList == nullptr &&
                      globals().imageFormatFreeList == nullptr && globals().imagePath == nullptr && globals().scratchMem == nullptr,
                  "a failed open (allocation " + std::to_string(k) + ") is undone and not counted");
            takeError();
            require(_rwImageOpen(RwEngineInstance, offset, sizeof(rwImageGlobals)) == RwEngineInstance, "a later open");
            stopEngine("a failed open");
        }

        // ---- gamma tables against the definition ------------------------------------------------------------------
        startEngine();
        {
            check(globals().gammaVal == 1.0f, "gamma 1 by default");
            for (int i = 0; i < 256; ++i) {
                check(globals().gammaTable[i] == i && globals().invGammaTable[i] == i, "gamma 1 is the identity");
            }
            int worst = 0;
            for (int n = 0; n < 2000; ++n) {
                const float gamma = 0.1f + 0.0025f * static_cast<float>(n);
                RwImageSetGamma(gamma);
                for (int i = 0; i < 256; ++i) {
                    const double x = i / 255.0;
                    const double want = std::floor(0.5 + 255.0 * std::pow(x, 1.0 / gamma));
                    const double wantInv = std::floor(0.5 + 255.0 * std::pow(x, static_cast<double>(gamma)));
                    const int d = static_cast<int>(std::fabs(globals().gammaTable[i] - want));
                    const int dInv = static_cast<int>(std::fabs(globals().invGammaTable[i] - wantInv));
                    worst = std::max(worst, std::max(d, dInv));
                    if (i > 0) {
                        require(globals().gammaTable[i] >= globals().gammaTable[i - 1] &&
                                    globals().invGammaTable[i] >= globals().invGammaTable[i - 1],
                                "the tables are monotone");
                    }
                }
                check(globals().gammaTable[255] == 255 && globals().invGammaTable[255] == 255, "255 maps to 255");
            }
            check(worst <= 1, "every entry within one step of the double-precision power (worst " + std::to_string(worst) + ")");
            std::printf("  gamma tables: worst difference from double precision %d step(s)\n", worst);
            // the original's saturation: 1 / 0 = infinity, 0^infinity = 0, 1^infinity = NaN -> fctiwz's INT_MIN
            RwImageSetGamma(0.0f);
            check(globals().gammaTable[1] == 0 && globals().gammaTable[255] == 0 && globals().invGammaTable[1] == 255,
                  "gamma 0 (1 / 0 = infinity; 1^infinity = NaN gives byte 0; x^0 = 1)");
            RwImageSetGamma(-1.0f);
            check(globals().gammaTable[1] == 0x01 && globals().gammaTable[255] == 255,
                  "a negative gamma: 255 x 255 + 0.5 truncated, its low byte (65025 = 0xFE01)");
            RwImageSetGamma(-0.001f);
            check(globals().gammaTable[1] == 0xFF && globals().gammaTable[255] == 255,
                  "a tiny negative gamma saturates (fctiwz of infinity is INT_MAX, its low byte 0xFF)");
            RwImageSetGamma(1.0f);
        }
        // ---- images: strides, palettes, the operations against their meaning --------------------------------------
        {
            const RwInt32 depths[] = {1, 4, 8, 16, 24, 32};
            for (const RwInt32 d : depths) {
                for (const RwInt32 w : {0, 1, 3, 5, 17}) {
                    RwImage* image = RwImageCreate(w, 3, d);
                    check(image != nullptr && image->flags == 0 && image->stride == 0 && image->cpPixels == nullptr &&
                              image->palette == nullptr,
                          "a new image: no pixels, stride 0 (native)");
                    check(RwImageAllocatePixels(image) == image && (image->flags & rwIMAGEALLOCATED), "pixels");
                    const RwInt32 row = ((d + 7) >> 3) * w;
                    check(image->stride == ((row + 3) & ~3) && image->stride % 4 == 0 && image->stride >= row, "the stride");
                    if (d == 4 || d == 8) {
                        check(reinterpret_cast<RwUInt8*>(image->palette) == image->cpPixels + image->stride * 3 &&
                                  g_live[image->cpPixels] == static_cast<std::size_t>(image->stride * 3 + (4 << d)),
                              "the palette after the pixels, one block");
                    } else {
                        check(image->palette == nullptr && g_live[image->cpPixels] == static_cast<std::size_t>(image->stride * 3),
                              "no palette");
                    }
                    check(RwImageFreePixels(image) == image && image->cpPixels == nullptr && image->palette == nullptr &&
                              !(image->flags & rwIMAGEALLOCATED),
                          "freed");
                    check(RwImageDestroy(image) == 1, "destroyed");
                }
            }
            // make mask: alpha = max(r, g, b)
            RwImage* a = made(7, 4, 32, 11);
            RwImage* p = made(7, 4, 8, 5);
            RwImageMakeMask(a), RwImageMakeMask(p);
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 7; ++x) {
                    const RwRGBA c = pixel(a, x, y);
                    check(c.alpha == std::max(c.red, std::max(c.green, c.blue)), "RwImageMakeMask (32)");
                }
            }
            for (int i = 0; i < 256; ++i) {
                check(p->palette[i].alpha == std::max(p->palette[i].red, std::max(p->palette[i].green, p->palette[i].blue)),
                      "RwImageMakeMask (palette)");
            }
            // apply mask: each pixel keeps its colour and takes the mask's alpha; a palettised image becomes 32-bit
            RwImage* image = made(7, 4, 8, 9);
            std::vector<RwRGBA> before;
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 7; ++x) before.push_back(pixel(image, x, y));
            check(RwImageApplyMask(image, p) == image && image->depth == 32 && image->palette == nullptr, "RwImageApplyMask converts");
            for (int y = 0, i = 0; y < 4; ++y) {
                for (int x = 0; x < 7; ++x, ++i) {
                    const RwRGBA c = pixel(image, x, y);
                    check(c.red == before[i].red && c.green == before[i].green && c.blue == before[i].blue &&
                              c.alpha == pixel(p, x, y).alpha,
                          "RwImageApplyMask's pixels");
                }
            }
            RwImage* small = made(3, 4, 32, 1);
            takeError();
            check(RwImageApplyMask(image, small) == nullptr && takeError().errorCode == static_cast<RwInt32>(0x8000000Au),
                  "sizes differ: E_RW_INVIMAGEMASK");
            RwImage* odd = made(7, 4, 16, 1);
            check(RwImageApplyMask(odd, p) == nullptr && takeError().errorCode == static_cast<RwInt32>(0x80000009u),
                  "an image depth of 16: E_RW_INVIMAGEFORMAT");
            // copies: 4 -> 8 keeps the indices, 8 -> 32 looks them up, the gamma flag travels
            RwImage* four = made(7, 4, 4, 3);
            RwImage* eight = made(7, 4, 8, 4);
            RwImage* full = made(7, 4, 32, 4);
            four->flags |= rwIMAGEGAMMACORRECTED;
            check(RwImageCopy(eight, four) == eight && (eight->flags & rwIMAGEGAMMACORRECTED), "4 -> 8");
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 7; ++x) check(eight->cpPixels[y * eight->stride + x] == four->cpPixels[y * four->stride + x], "4 -> 8 indices");
            }
            check(RwImageCopy(full, p) == full && !(full->flags & rwIMAGEGAMMACORRECTED), "8 -> 32");
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 7; ++x) {
                    const RwRGBA c = pixel(full, x, y), w = pixel(p, x, y);
                    check(std::memcmp(&c, &w, 4) == 0, "8 -> 32 colours");
                }
            }
            // a refused copy (native): NULL, the destination untouched, its flag kept
            std::vector<RwUInt8> keep(four->cpPixels, four->cpPixels + four->stride * 4);
            const RwInt32 flags = four->flags;
            check(RwImageCopy(four, full) == nullptr && takeError().errorCode == static_cast<RwInt32>(0x80000009u) &&
                      four->flags == flags && std::memcmp(keep.data(), four->cpPixels, keep.size()) == 0,
                  "32 -> 4 refused: NULL, E_RW_INVIMAGEFORMAT, the destination as it was");
            // gamma correction applies the table to r, g, b
            RwImageSetGamma(2.2f);
            RwImage* g32 = made(5, 2, 32, 7);
            std::vector<RwUInt8> raw(g32->cpPixels, g32->cpPixels + g32->stride * 2);
            check(RwImageGammaCorrect(g32) == g32 && (g32->flags & rwIMAGEGAMMACORRECTED), "RwImageGammaCorrect");
            for (int i = 0; i < 5 * 2 * 4; ++i) {
                const RwUInt8 want = (i % 4 == 3) ? raw[i] : globals().gammaTable[raw[i]];
                check(g32->cpPixels[i] == want, "gamma-corrected bytes");
            }
            RwImageSetGamma(1.0f);
            RwImage* empty = RwImageCreate(4, 4, 8);
            check(RwImageGammaCorrect(empty) == nullptr && takeError().errorCode == static_cast<RwInt32>(0x80000016u),
                  "no palette: E_RW_NULLP");
            check(RwImageGammaCorrect(odd) == nullptr && takeError().errorCode == static_cast<RwInt32>(0x80000008u),
                  "depth 16: E_RW_INVIMAGEDEPTH");
            for (RwImage* i : {a, p, image, small, odd, four, eight, full, g32, empty}) RwImageDestroy(i);
        }
        // ---- the deliberate differences ---------------------------------------------------------------------------
        {
            const int dtorsBefore = g_dtorCount;
            g_ctorFail = g_ctorCount;
            check(RwImageCreate(4, 4, 32) == nullptr, "a failing plugin constructor fails RwImageCreate");
            g_ctorFail = -1;
            check(g_dtorCount == dtorsBefore, "no destructor for an image never constructed");
            RwImage* big = RwImageCreate(0x7FFFFFFF, 2, 32);
            const long before = g_mallocs;
            takeError();
            check(RwImageAllocatePixels(big) == nullptr && takeError().errorCode == E_RW_NOMEM && g_mallocs == before,
                  "a stride beyond RwInt32 fails without an allocation");
            big->width = -5;
            check(RwImageAllocatePixels(big) == nullptr && takeError().errorCode == E_RW_NOMEM && g_mallocs == before,
                  "a negative size fails without an allocation");
            RwImageDestroy(big);
            // RwImageApplyMask: the second allocation (the image's new pixels) failing
            RwImage* image = made(6, 3, 8, 2);
            RwImage* mask = made(6, 3, 32, 3);
            g_failAt = g_mallocs + 1;
            check(RwImageApplyMask(image, mask) == nullptr && takeError().errorCode == E_RW_NOMEM, "the new pixels fail: NULL");
            g_failAt = -1;
            check(image->depth == 32 && image->cpPixels == nullptr && !(image->flags & rwIMAGEALLOCATED),
                  "the image is left without pixels (not copied into NULL)");
            check(RwImageAllocatePixels(image) == image && RwImageApplyMask(image, mask) == image, "and recovers");
            RwImageDestroy(image), RwImageDestroy(mask);
            // RwImageFindRasterFormat: a device that fills nothing reads the zeroed raster
            RwStandardFunc saved = rwSrcGlobals().stdFunc[9];
            rwSrcGlobals().stdFunc[9] = findNothing;
            RwImage* i = RwImageCreate(2, 2, 32);
            RwInt32 w = -1, h = -1, d = -1, f = -1;
            check(RwImageFindRasterFormat(i, 4, &w, &h, &d, &f) == i && w == 0 && h == 0 && d == 0 && f == 0,
                  "RwImageFindRasterFormat's raster starts zeroed");
            rwSrcGlobals().stdFunc[9] = saved;
            // the host device: no exchange yet
            RwRaster raster{};
            RwRGBA rgba{1, 2, 3, 4};
            check(RwImageSetFromRaster(i, &raster) == nullptr && RwRasterSetFromImage(&raster, i) == nullptr &&
                      RwImageFindRasterFormat(i, 4, &w, &h, &d, &f) == nullptr && RwRGBASetFromPixel(&rgba, 7, 0x500) == &rgba,
                  "the host device answers 'not supported'");
            RwImageDestroy(i);
        }
        // ---- the search -------------------------------------------------------------------------------------------
        {
            rwFileInterface().fexist = fakeExist;
            check(RwImageFindFileType("tex") == nullptr && g_asked.empty(), "no formats: nothing asked");
            addFormat(".png", ".PNG", readCallBack);
            addFormat(".bmp", ".BMP", nullptr);   // newest first
            g_existing = {"tex.PNG"};
            const RwChar* ext = RwImageFindFileType("tex");
            check(ext != nullptr && std::strcmp(ext, ".PNG") == 0 &&
                      g_asked == std::vector<std::string>{"tex.bmp", "tex.BMP", "tex.png", "tex.PNG"},
                  "no path: the name with each extension, the newest format first, lower case first");
            g_asked.clear();
            check(RwImageSetPath("a/;C:\\b\\;;c/") != nullptr && std::strcmp(globals().imagePath, "a/;C:\\b\\;;c/") == 0, "a path");
            g_existing = {"c/tex.png"};
            ext = RwImageFindFileType("tex");
            check(ext != nullptr && std::strcmp(ext, ".png") == 0 && g_asked.size() == 15 && g_asked[0] == "a/tex.bmp" &&
                      g_asked[4] == "C:\\b\\tex.bmp" && g_asked[8] == "tex.bmp" && g_asked[14] == "c/tex.png",
                  "each directory in turn; an empty element is the name alone");
            g_asked.clear();
            g_existing = {};
            check(RwImageFindFileType("C:\\abs\\tex") == nullptr && g_asked.size() == 4 && g_asked[0] == "C:\\abs\\tex.bmp" &&
                      RwImageFindFileType("\\lead") == nullptr && g_asked.size() == 8 && g_asked[4] == "\\lead.bmp",
                  "absolute names are not searched along the path");
            g_asked.clear();
            check(RwImageSetPath("a/;c/") != nullptr, "a shorter path");
            // read: the format of the extension, along the path; the mask applied
            RwImage* image = made(4, 4, 32, 1);
            RwImage* mask = made(4, 4, 8, 2);
            g_toRead[0] = image, g_toRead[1] = mask, g_readNext = 0;
            g_existing = {"c/x.png", "a/m.png"};
            RwImage* read = RwImageReadMaskedImage("sub/x.png", "m.png");
            check(read == nullptr && g_readNames.empty(), "the path is searched with the name as given (sub/x.png: none)");
            read = RwImageReadMaskedImage("x.png", "m.png");
            check(read == image && g_readNames == std::vector<std::string>{"c/x.png", "a/m.png"} && image->depth == 32,
                  "read along the path, the mask applied and destroyed");
            check(RwImageReadMaskedImage("x.bmp", nullptr) == nullptr && RwImageReadMaskedImage("x", nullptr) == nullptr &&
                      RwImageReadMaskedImage("x.PnG", nullptr) == nullptr,
                  "a format without a read callback, no extension, an extension of another case: none");
            g_readNames.clear();
            RwImage* image2 = made(4, 4, 32, 1);
            g_toRead[0] = image2, g_toRead[1] = nullptr, g_readNext = 0;
            check(RwImageReadMaskedImage("x.png", "m.png") == nullptr, "a mask that is not read: NULL (the image destroyed)");
            RwImageDestroy(read);
            // RwFexist through fopen when the interface has no fexist (a real file)
            rwFileInterface().fexist = nullptr;
            const char* probe = "rwcore_image_native_probe.tmp";
            std::remove(probe);
            check(!RwFexist(probe), "RwFexist: no file");
            if (void* f = rwHostFileInterface().fopen(probe, "wb")) rwHostFileInterface().fclose(f);
            check(RwFexist(probe), "RwFexist: the file (fopen fallback)");
            std::remove(probe);
            rwFileInterface() = rwHostFileInterface();
            check(rwFileInterface().fexist != nullptr && !RwFexist(probe), "the host interface's fexist");
            // the path buffer growing, and failing to grow
            const std::string longPath(1000, 'p');
            check(RwImageSetPath(longPath.c_str()) != nullptr && globals().imagePathSize == 1001, "the path buffer grows");
            const std::string longer(2000, 'q');
            g_failAt = g_mallocs;
            check(RwImageSetPath(longer.c_str()) == nullptr && takeError().errorCode == E_RW_NOMEM && globals().imagePathSize == 1001 &&
                      std::strlen(globals().imagePath) == 1000,
                  "a failed growth keeps the old path");
            g_failAt = -1;
        }
        stopEngine("images");   // the close frees the formats, the path and the scratch buffer
        // ---- no module ---------------------------------------------------------------------------------------------
        {
            require(RwEngineInit(&kCounting, 0, 0) == 1, "init");
            check(RwImageCreate(4, 4, 32) == nullptr && takeError().errorCode == static_cast<RwInt32>(0x80000001u),
                  "no image before the engine starts");
            require(RwEngineTerm() == 1, "term");
            require(g_live.empty(), "no module: leaked");
        }
        std::printf("rwcore_image_native_tests: %u checks passed (no allocation outlives a shutdown)\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_image_native_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
