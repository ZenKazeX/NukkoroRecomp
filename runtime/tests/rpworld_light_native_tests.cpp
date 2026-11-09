// Native tests of the RenderWare lights (rpworld/src/light.cpp) for what the oracle cannot cover:
// - the cone angle against the definition: RpLightGetConeAngle(x) against acos in double precision over [-1, 1]
//   (worst error printed and bounded: the original's acosf uses _rwSqrt, whose table limits the |x| >= 0.5
//   branches), RpLightSetConeAngle -> Get round trips over 0 .. pi / 2, the refused range;
// - the deliberate differences of rw/world/light.h: a failing plugin constructor fails the creation (no leak), no
//   light without a light module, a stream struct longer than 24 bytes read into 24 and skipped, a failed plugin read
//   destroys the light; plus the defaults, the grey flag, the radius marking the frame, the module's close
//   destroying live lights, RpWorldPluginAttach.
// Memory is counted: every part ends with no live allocation.
//
// Usage: rpworld_light_native_tests
#include "rw/core/binary.h"
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/frame.h"
#include "rw/core/memory.h"
#include "rw/core/stream.h"
#include "rw/world/light.h"
#include "rw/world/world.h"

#include <bit>
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

constexpr RwUInt32 kPluginID = 0x7013;
int g_constructs = 0, g_destructs = 0, g_fail_construct_at = -1;
bool g_fail_read = false;
RwInt32 g_offset = -1;
void* ctor(void* o, RwInt32 offset, RwInt32) {
    if (g_constructs++ == g_fail_construct_at) return nullptr;
    *reinterpret_cast<RwUInt32*>(static_cast<char*>(o) + offset) = 0x11u;
    return o;
}
void* dtor(void* o, RwInt32 offset, RwInt32) {
    ++g_destructs;
    require(*reinterpret_cast<RwUInt32*>(static_cast<char*>(o) + offset) != 0xDEADu, "a light destroyed twice");
    *reinterpret_cast<RwUInt32*>(static_cast<char*>(o) + offset) = 0xDEADu;
    return o;
}
RwStream* pluginRead(RwStream* stream, RwInt32 length, void* o, RwInt32 offset, RwInt32) {
    if (g_fail_read || length != 4) return nullptr;
    return RwStreamRead(stream, static_cast<char*>(o) + offset, 4) == 4 ? stream : nullptr;
}

void startEngine() {
    RwEngineOpenParams params{nullptr};
    require(RwEngineInit(&kCounting, 0, 0) == 1, "init");
    require(RpWorldPluginAttach() == 1, "RpWorldPluginAttach");
    g_offset = RpLightRegisterPlugin(4, kPluginID, ctor, dtor, nullptr);
    require(g_offset >= static_cast<RwInt32>(sizeof(RpLight)), "a light plugin");
    require(RpLightRegisterPluginStream(kPluginID, pluginRead, nullptr, nullptr) == g_offset, "its stream");
    require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "start");
    require(lightModule.numInstances == 1, "the light module opened with the engine");
}
void stopEngine(const char* what) {
    require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, std::string(what) + ": shutdown");
    require(lightModule.numInstances == 0, std::string(what) + ": the light module closed");
    require(g_live.empty(), std::string(what) + ": " + std::to_string(g_live.size()) + " allocations leaked");
}

unsigned g_checks = 0;
void check(bool c, const std::string& what) {
    ++g_checks;
    require(c, what);
}

std::vector<unsigned char> g_bytes;
void put(RwUInt32 w) {
    for (int i = 0; i < 4; ++i) g_bytes.push_back(static_cast<unsigned char>(w >> (8 * i)));
}
constexpr RwUInt32 libraryID(RwUInt32 v) { return (((v - 0x30000u) << 14) & 0xFFC00000u) | ((v & 0x3Fu) << 16) | 0x37u; }
RpLight* readLight(bool* positioned = nullptr) {
    takeError();
    RwMemory memory{g_bytes.data(), g_bytes.size()};
    RwStream* s = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory);
    RpLight* l = RpLightStreamRead(s);
    if (positioned != nullptr) {
        RwUInt32 marker = 0;
        *positioned = RwStreamRead(s, &marker, 4) == 4 && marker == 0x4D41524Bu;
    }
    RwStreamClose(s, nullptr);
    return l;
}

}  // namespace

int main() {
    try {
        // ---- the cone angle against acos ---------------------------------------------------------------------------
        startEngine();
        {
            RpLight* l = RpLightCreate(rpLIGHTSPOT);
            double worst = 0, worstAt = 0;
            for (int i = 0; i <= 2000000; ++i) {
                const float x = -1.0f + 2.0f * static_cast<float>(i) / 2000000.0f;
                l->minusCosAngle = -x;
                const double e = std::fabs(static_cast<double>(RpLightGetConeAngle(l)) - std::acos(static_cast<double>(x)));
                if (e > worst) worst = e, worstAt = x;
            }
            std::printf("  cone angle vs acos over [-1, 1]: worst error %.3g rad at x = %.7g\n", worst, worstAt);
            check(worst < 2e-4, "RpLightGetConeAngle within 2e-4 rad of acos (measured 8.8e-5)");
            l->minusCosAngle = -1.0f;
            check(RpLightGetConeAngle(l) == 0.0f, "x = 1: 0");
            l->minusCosAngle = 1.0f;
            check(RpLightGetConeAngle(l) == std::bit_cast<float>(0x40490FDBu), "x = -1: pi");
            l->minusCosAngle = -2.0f;
            check(RpLightGetConeAngle(l) == 0.0f, "x > 1: 0 (no NaN)");
            double worstTrip = 0;
            for (int i = 0; i <= 100000; ++i) {
                const float a = 1.5707963f * static_cast<float>(i) / 100000.0f;
                check(RpLightSetConeAngle(l, a) == l, "a cone angle in range");
                check(std::fabs(l->minusCosAngle + std::cos(static_cast<double>(a))) < 1e-6, "minusCosAngle = -cos");
                worstTrip = std::fmax(worstTrip, std::fabs(static_cast<double>(RpLightGetConeAngle(l)) - a));
            }
            std::printf("  set / get cone round trip: worst %.3g rad\n", worstTrip);
            check(worstTrip < 5e-4, "set / get round trip (measured 2.4e-4)");
            const float before = l->minusCosAngle;
            check(RpLightSetConeAngle(l, -0.01f) == nullptr && RpLightSetConeAngle(l, 1.6f) == nullptr &&
                      l->minusCosAngle == before,
                  "angles outside 0 .. pi / 2 are refused, nothing changed");
            RpLightDestroy(l);
        }
        // ---- defaults, grey flag, radius, frames -----------------------------------------------------------------
        {
            rwSrcGlobals().lightFrame = 77;
            RpLight* l = RpLightCreate(rpLIGHTPOINT);
            check(l->object.object.type == rpLIGHT && RpLightGetType(l) == rpLIGHTPOINT &&
                      RpLightGetFlags(l) == (rpLIGHTLIGHTATOMICS | rpLIGHTLIGHTWORLD) &&
                      l->object.object.privateFlags == rpLIGHTPRIVATENOCHROMA && l->radius == 0 && l->minusCosAngle == 0 &&
                      l->color.red == 1 && l->color.green == 1 && l->color.blue == 1 && l->color.alpha == 1 &&
                      l->lightFrame == 76 && RpLightGetFrame(l) == nullptr &&
                      l->WorldSectorsInLight.link.next == &l->WorldSectorsInLight.link && l->inWorld.next == nullptr &&
                      (reinterpret_cast<std::uintptr_t>(l) & (alignof(RpLight) - 1)) == 0,
                  "RpLightCreate's defaults");
            const RwRGBAReal grey{0.5f, 0.5f, 0.5f, 0.25f}, red{1, 0, 0, 1}, nearGrey{0.5f, 0.5f, std::nextafter(0.5f, 1.0f), 1};
            check(RpLightSetColor(l, &grey) == l && l->object.object.privateFlags == rpLIGHTPRIVATENOCHROMA &&
                      l->color.alpha == 0.25f,
                  "grey colour");
            check(RpLightSetColor(l, &red)->object.object.privateFlags == 0, "a coloured light");
            check(RpLightSetColor(l, &nearGrey)->object.object.privateFlags == 0, "one ulp off grey is coloured");
            RwFrame* f = RwFrameCreate();
            RpLightSetFrame(l, f);
            _rwFrameSyncDirty();
            check(RpLightSetRadius(l, 12.5f) == l && l->radius == 12.5f && RwFrameDirty(f) != 0, "radius marks the frame");
            RpLightSetFlags(l, rpLIGHTLIGHTWORLD);
            check(RpLightGetFlags(l) == rpLIGHTLIGHTWORLD, "flags");
            // a failed constructor fails the creation, nothing leaks
            g_fail_construct_at = g_constructs;
            const int d = g_destructs;
            check(RpLightCreate(rpLIGHTAMBIENT) == nullptr && g_destructs == d, "a failed constructor fails RpLightCreate");
            g_fail_construct_at = -1;
            // the close destroys the lights still alive (here l and two more), running their destructors
            RpLightCreate(rpLIGHTDIRECTIONAL), RpLightCreate(rpLIGHTAMBIENT);
            const int before = g_destructs;
            RwFrameDestroy(f);   // releases l first
            check(RpLightGetFrame(l) == nullptr, "a destroyed frame releases its light");
            stopEngine("defaults");
            check(g_destructs == before + 3, "the module's close destroyed the three live lights");
        }
        // ---- no light module -----------------------------------------------------------------------------------
        {
            RwEngineOpenParams params{nullptr};
            require(RwEngineInit(&kCounting, 0, 0) == 1, "init");
            check(RpLightCreate(rpLIGHTAMBIENT) == nullptr && takeError().errorCode == static_cast<RwInt32>(0x80000001u),
                  "no light without the light module");
            require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "start");
            check(RpLightCreate(rpLIGHTAMBIENT) == nullptr, "nor without RpWorldPluginAttach");
            require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, "shutdown");
            require(g_live.empty(), "no light module: leaked");
        }
        // ---- streams ----------------------------------------------------------------------------------------------
        startEngine();
        {
            const auto lightStruct = [](RwUInt32 length, RwUInt32 version, RwUInt32 type, RwUInt32 flags) {
                put(rwID_STRUCT), put(length), put(libraryID(version));
                const RwUInt32 f[6] = {std::bit_cast<RwUInt32>(250.0f), std::bit_cast<RwUInt32>(0.25f), std::bit_cast<RwUInt32>(0.5f),
                                       std::bit_cast<RwUInt32>(0.75f), std::bit_cast<RwUInt32>(-0.5f), (type << 16) | flags};
                for (RwUInt32 i = 0; i < length / 4; ++i) put(i < 6 ? f[i] : 0xEEEEEEEEu);
            };
            // a 32-byte struct (8 bytes beyond the light's 24), plugin data, then a marker
            g_bytes.clear();
            lightStruct(32, 0x36003u, rpLIGHTSPOT, 1);
            put(rwID_EXTENSION), put(16), put(libraryID(0x36003u)), put(kPluginID), put(4), put(libraryID(0x36003u)), put(0xBEEFu);
            put(0x4D41524Bu);
            bool positioned = false;
            RpLight* l = readLight(&positioned);
            check(l != nullptr && positioned && RpLightGetType(l) == rpLIGHTSPOT && RpLightGetFlags(l) == 1 && l->radius == 250 &&
                      l->color.red == 0.25f && l->color.green == 0.5f && l->color.blue == 0.75f && l->color.alpha == 1 &&
                      l->minusCosAngle == -0.5f && l->object.object.privateFlags == 0 &&
                      *reinterpret_cast<RwUInt32*>(reinterpret_cast<char*>(l) + g_offset) == 0xBEEFu,
                  "a long struct is read into 24 bytes and skipped; fields and plugin data");
            RpLightDestroy(l);
            // bad versions: NULL, E_RW_BADVERSION, no light made
            for (RwUInt32 v : {0x34000u, 0x37003u}) {
                g_bytes.clear();
                lightStruct(24, v, rpLIGHTPOINT, 3);
                put(rwID_EXTENSION), put(0), put(libraryID(0x36003u));
                const int made = g_constructs;
                check(readLight() == nullptr && takeError().errorCode == static_cast<RwInt32>(0x80000004u) && g_constructs == made,
                      "a bad version");
            }
            // truncated
            g_bytes.clear();
            put(rwID_STRUCT), put(24), put(libraryID(0x36003u)), put(0), put(0);
            check(readLight() == nullptr, "a truncated struct");
            // a failing plugin read destroys the light (the original leaks it)
            g_bytes.clear();
            lightStruct(24, 0x36003u, rpLIGHTPOINT, 3);
            put(rwID_EXTENSION), put(16), put(libraryID(0x36003u)), put(kPluginID), put(4), put(libraryID(0x36003u)), put(1);
            g_fail_read = true;
            const int d = g_destructs;
            check(readLight() == nullptr && g_destructs == d + 1, "a failed plugin read destroys the light");
            g_fail_read = false;
            // a short struct: missing fields read as zero (type 0)
            g_bytes.clear();
            lightStruct(8, 0x36003u, 0, 0);
            put(rwID_EXTENSION), put(0), put(libraryID(0x36003u));
            l = readLight();
            check(l != nullptr && l->radius == 250 && l->color.red == 0.25f && l->color.green == 0 && RpLightGetType(l) == 0 &&
                      RpLightGetFlags(l) == 0,
                  "a short struct");
            RpLightDestroy(l);
        }
        stopEngine("streams");
        std::printf("rpworld_light_native_tests: %u checks passed (no allocation outlives a shutdown)\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rpworld_light_native_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
