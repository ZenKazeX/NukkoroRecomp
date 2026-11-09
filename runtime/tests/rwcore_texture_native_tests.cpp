// Native tests of the RenderWare rasters, textures and texture dictionaries (rwcore/src/raster.cpp, texture.cpp) for
// what the oracle cannot cover:
// - the host device's rasters (the oracle scripts the device): 8888 / 888 / default-format textures with and without
//   mip levels (the level count, every level's size, lock pointers inside the storage, pixels written and read back,
//   width / height / stride while locked and restored by the unlock, a second lock refused), unsupported formats and
//   depths refused, camera and Z rasters without pixels, RwTextureSetRaster through the device;
// - the deliberate differences of rw/core/texture.h and raster.h: a dictionary destroyed while one of its textures is
//   referenced elsewhere (the survivor leaves it cleanly), failing raster / texture / dictionary plugin constructors
//   (NULL, nothing leaked, the default dictionary intact), no modules;
// - the read path a converted-asset loader uses: RwTextureSetReadCallBack, the read texture joining the current
//   dictionary, found again by name in any letter case, references; the module's close destroying the default
//   dictionary with its textures.
// Memory is counted: every part ends with no live allocation.
//
// Usage: rwcore_texture_native_tests
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/memory.h"
#include "rw/core/raster.h"
#include "rw/core/texture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::map<void*, std::size_t> g_live;
void* countMalloc(std::size_t size, RwUInt32) {
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

int g_fail[3] = {-1, -1, -1}, g_count[3] = {};
template <int K> void* ctor(void* o, RwInt32, RwInt32) { return g_count[K]++ == g_fail[K] ? nullptr : o; }

void startEngine() {
    RwEngineOpenParams params{nullptr};
    require(RwEngineInit(&kCounting, 0, 0) == 1, "init");
    require(RwRasterRegisterPlugin(4, 0x7201, ctor<0>, nullptr, nullptr) >= 0 &&
                RwTextureRegisterPlugin(4, 0x7202, ctor<1>, nullptr, nullptr) >= 0 &&
                _rwPluginRegistryAddPlugin(&rwTexDictionaryRegistry(), 4, 0x7203, ctor<2>, nullptr, nullptr) >= 0,
            "plugins");
    require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "start");
}
void stopEngine(const char* what) {
    require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, std::string(what) + ": shutdown");
    require(g_live.empty(), std::string(what) + ": " + std::to_string(g_live.size()) + " allocations leaked");
}

unsigned g_checks = 0;
void check(bool c, const std::string& what) {
    ++g_checks;
    require(c, what);
}

RwTexture* g_loaded = nullptr;
RwTexture* loader(const RwChar* name, const RwChar*) {
    if (std::strcmp(name, "missing") == 0) return nullptr;
    RwRaster* r = RwRasterCreate(4, 4, 32, rwRASTERTYPETEXTURE | rwRASTERFORMAT8888);
    g_loaded = RwTextureCreate(r);
    RwTextureSetName(g_loaded, name);
    return g_loaded;
}

}  // namespace

int main() {
    try {
        startEngine();
        // ---- host rasters ----------------------------------------------------------------------------------------
        for (RwInt32 format : {rwRASTERFORMAT8888, rwRASTERFORMAT888, rwRASTERFORMATDEFAULT}) {
            for (bool mips : {false, true}) {
                const RwInt32 w = 300, h = 75;
                RwRaster* r = RwRasterCreate(w, h, 0, rwRASTERTYPETEXTURE | format | (mips ? rwRASTERFORMATMIPMAP : 0));
                const std::string tag = "format " + std::to_string(format) + (mips ? " with mips" : "");
                check(r != nullptr && r->parent == r && r->depth == 32 && RwRasterGetType(r) == rwRASTERTYPETEXTURE &&
                          (RwRasterGetFormat(r) & rwRASTERFORMATPIXELFORMATMASK) ==
                              (format == rwRASTERFORMATDEFAULT ? rwRASTERFORMAT8888 : format),
                      tag + ": RwRasterCreate");
                const RwInt32 levels = RwRasterGetNumLevels(r);
                check(levels == (mips ? 9 : 1), tag + ": level count (300 x 75: 9 levels)");
                const rwHostRaster* host = rwHostRasterOf(r);
                std::size_t total = 0;
                for (RwInt32 l = 0; l < levels; ++l) total += rwHostRasterLevelSize(host, l);
                for (RwInt32 l = 0; l < levels; ++l) {
                    RwUInt8* p = RwRasterLock(r, static_cast<RwUInt8>(l), rwRASTERLOCKWRITE);
                    const RwInt32 lw = (w >> l) > 0 ? w >> l : 1, lh = (h >> l) > 0 ? h >> l : 1;
                    check(p != nullptr && p == r->cpPixels && r->width == lw && r->height == lh && r->stride == lw * 4 &&
                              p >= host->pixels && p + static_cast<std::size_t>(lh) * r->stride <= host->pixels + total,
                          tag + ": level " + std::to_string(l) + " locked");
                    check(RwRasterLock(r, 0, rwRASTERLOCKREAD) == nullptr, tag + ": a second lock is refused");
                    for (RwInt32 y = 0; y < lh; ++y) {
                        for (RwInt32 x = 0; x < lw * 4; ++x) p[y * r->stride + x] = static_cast<RwUInt8>(x + 3 * y + 7 * l);
                    }
                    RwRasterUnlock(r);
                    check(r->width == w && r->height == h && r->cpPixels == nullptr, tag + ": unlock restores level 0");
                }
                for (RwInt32 l = 0; l < levels; ++l) {
                    const RwUInt8* p = RwRasterLock(r, static_cast<RwUInt8>(l), rwRASTERLOCKREAD);
                    bool same = true;
                    for (RwInt32 y = 0; y < r->height; ++y) {
                        for (RwInt32 x = 0; x < r->width * 4; ++x) same = same && p[y * r->stride + x] == static_cast<RwUInt8>(x + 3 * y + 7 * l);
                    }
                    RwRasterUnlock(r);
                    check(same, tag + ": level " + std::to_string(l) + " pixels kept");
                }
                check(RwRasterLock(r, static_cast<RwUInt8>(levels), rwRASTERLOCKREAD) == nullptr, tag + ": no level past the last");
                check(RwRasterDestroy(r) == 1, tag + ": destroy");
            }
        }
        check(RwRasterCreate(16, 16, 16, rwRASTERTYPETEXTURE | rwRASTERFORMAT565) == nullptr, "565 is not supported");
        check(RwRasterCreate(16, 16, 8, rwRASTERTYPETEXTURE | rwRASTERFORMAT8888) == nullptr, "depth 8 for 8888 is refused");
        check(RwRasterCreate(0, 16, 32, rwRASTERTYPETEXTURE | rwRASTERFORMAT8888) == nullptr, "an empty raster is refused");
        {
            RwRaster* cam = RwRasterCreate(5120, 1440, 32, rwRASTERTYPECAMERA);
            RwRaster* z = RwRasterCreate(5120, 1440, 32, rwRASTERTYPEZBUFFER);
            check(cam != nullptr && z != nullptr && rwHostRasterOf(cam)->pixels == nullptr && RwRasterLock(cam, 0, rwRASTERLOCKREAD) == nullptr,
                  "camera / Z rasters have no CPU pixels (the renderer's buffers stand for them)");
            RwRasterDestroy(cam), RwRasterDestroy(z);
        }
        // ---- textures, dictionaries, the read path -------------------------------------------------------------
        {
            RwTexDictionary* def = RwTexDictionaryGetCurrent();
            check(def != nullptr && def->object.type == rwTEXDICTIONARY, "the open made a current default dictionary");
            RwTexture* t = RwTextureCreate(nullptr);
            check(t->refCount == 1 && t->filterAddressing == 0x1101u && RwTextureGetFilterMode(t) == rwFILTERNEAREST &&
                      RwTextureGetAddressingU(t) == rwTEXTUREADDRESSWRAP && RwTextureGetAddressingV(t) == rwTEXTUREADDRESSWRAP &&
                      t->dict == nullptr && t->name[0] == 0,
                  "RwTextureCreate's defaults");
            RwRaster* r = RwRasterCreate(8, 8, 32, rwRASTERTYPETEXTURE | rwRASTERFORMAT8888);
            check(RwTextureSetRaster(t, r) == t && t->raster == r, "RwTextureSetRaster through the host device");
            check(RwTextureSetRaster(t, nullptr) == t && t->raster == nullptr, "RwTextureSetRaster(NULL)");
            RwTextureSetRaster(t, r);
            takeError();
            RwTextureSetName(t, "a_name_that_is_much_longer_than_31_characters");
            check(std::strlen(t->name) == 31 && takeError().errorCode == static_cast<RwInt32>(0x8000001Eu), "a long name is cut");
            RwTextureSetName(t, "Westopolis_Wall01");
            RwTexDictionary* d = RwTexDictionaryCreate();
            RwTexDictionaryAddTexture(d, t);
            check(RwTexDictionaryFindNamedTexture(d, "WESTOPOLIS_wall01") == t && RwTexDictionaryFindNamedTexture(d, "Westopolis_Wall0") == nullptr,
                  "names match in any letter case, not as prefixes");
            RwTexDictionarySetCurrent(nullptr);
            check(RwTextureRead("westopolis_WALL01", nullptr) == t && t->refCount == 2, "no current dictionary: every one is searched");
            RwTextureSetReadCallBack(loader);
            RwTexDictionarySetCurrent(def);
            RwTexture* loaded = RwTextureRead("sky_dome", "sky_mask");
            check(loaded == g_loaded && loaded->dict == def && RwTextureRead("SKY_DOME", nullptr) == loaded && loaded->refCount == 2,
                  "a loaded texture joins the current dictionary and is found again");
            takeError();
            check(RwTextureRead("missing", nullptr) == nullptr && takeError().errorCode == 0x16, "nothing found: E_RW_READTEXMASK");
            // a dictionary destroyed while its texture is referenced elsewhere: the survivor leaves it cleanly
            check(RwTexDictionaryDestroy(d) == 1 && t->refCount == 1 && t->dict == nullptr && t->lInDictionary.next == nullptr,
                  "a referenced texture survives its dictionary, unlinked");
            check(RwTextureDestroy(t) == 1, "and is destroyed later without touching the freed dictionary");
            RwTextureDestroy(loaded);   // one of its two references; the close takes the other with the default dictionary
            check(loaded->refCount == 1 && loaded->dict == def, "references");
            // failing constructors: NULL, nothing leaked
            for (int k = 0; k < 3; ++k) {
                g_fail[k] = g_count[k];
                const bool failed = k == 0 ? RwRasterCreate(4, 4, 32, rwRASTERTYPETEXTURE) == nullptr
                                  : k == 1 ? RwTextureCreate(nullptr) == nullptr
                                           : RwTexDictionaryCreate() == nullptr;
                g_fail[k] = -1;
                check(failed, "a failing plugin constructor fails the creation (" + std::to_string(k) + ")");
            }
            check(RwTexDictionaryGetCurrent() == def, "the default dictionary is still current");
        }
        stopEngine("textures");   // the close destroys the default dictionary and the loaded texture with its raster
        // ---- no modules ------------------------------------------------------------------------------------------
        {
            require(RwEngineInit(&kCounting, 0, 0) == 1, "init");
            check(RwRasterCreate(4, 4, 32, 0) == nullptr && RwTextureCreate(nullptr) == nullptr && RwTexDictionaryCreate() == nullptr &&
                      takeError().errorCode == static_cast<RwInt32>(0x80000001u),
                  "no raster / texture / dictionary before the engine starts");
            require(RwEngineTerm() == 1, "term");
            require(g_live.empty(), "no modules: leaked");
        }
        std::printf("rwcore_texture_native_tests: %u checks passed (no allocation outlives a shutdown)\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_texture_native_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
