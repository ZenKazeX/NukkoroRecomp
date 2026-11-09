// RenderWare lights (module balight; see rw/world/light.h). Every floating-point step is the original's: fmadds /
// fmsubs / fnmsubs round once (std::fmaf), fmuls / fadds / fsubs / fdivs are single operations.
#include "rw/world/light.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "math/msl.h"
#include "rw/core/binary.h"
#include "rw/core/error.h"
#include "rw/core/memory.h"
#include "rw/core/vector.h"

RwModuleInfo lightModule{};

namespace {

// The free list's creation parameters (balight statics 0x805EEE60 / 0x805EEE64; RpLightSetFreeListCreateParams is
// not linked) and its in-place header (0x805E2688).
constexpr RwInt32 kLightsPerBlock = 32;
constexpr RwInt32 kPreallocBlocks = 1;
constexpr RwUInt32 kLightHint = 0x00030000u | rwID_LIGHT;   // rwMEMHINTDUR_EVENT, as the original passes it
constexpr RwUInt32 rwID_WORLDPLUGIN = 0x02;                 // the plugin id the original's errors carry
RwFreeList lightFreeListSpace{};
RwPluginRegistry lightTKList = rwPLUGINREGISTRYINIT(sizeof(RpLight));   // 0x8056E8E8

// The constants, by their bits (r2 + 0x1A18 .. 0x1A60)
const RwReal kZero = std::bit_cast<RwReal>(0x00000000u);
const RwReal kPi = std::bit_cast<RwReal>(0x40490FDBu);
const RwReal kHalfPi = std::bit_cast<RwReal>(0x3FC90FDBu);
const RwReal kPio2Hi = std::bit_cast<RwReal>(0x3FC90FDAu);
const RwReal kPio2Lo = std::bit_cast<RwReal>(0x33A22168u);
const RwReal kPS0 = std::bit_cast<RwReal>(0x3E2AAAABu);
const RwReal kPS1 = std::bit_cast<RwReal>(0xBEA6B090u);
const RwReal kPS2 = std::bit_cast<RwReal>(0x3E4E0AA8u);
const RwReal kPS3 = std::bit_cast<RwReal>(0xBD241146u);
const RwReal kPS4 = std::bit_cast<RwReal>(0x3A4F7F04u);
const RwReal kPS5 = std::bit_cast<RwReal>(0x3811EF08u);
const RwReal kOne = std::bit_cast<RwReal>(0x3F800000u);
const RwReal kQS1 = std::bit_cast<RwReal>(0xC019D139u);
const RwReal kQS2 = std::bit_cast<RwReal>(0x4001572Du);
const RwReal kQS3 = std::bit_cast<RwReal>(0xBF303361u);
const RwReal kQS4 = std::bit_cast<RwReal>(0x3D9DC62Eu);
const RwReal kHalf = std::bit_cast<RwReal>(0x3F000000u);
const RwReal kPiLow = std::bit_cast<RwReal>(0x40490FDAu);   // pi as acosf's last branch takes it (one ulp below kPi)
const RwReal kTwo = std::bit_cast<RwReal>(0x40000000u);

RwFreeList*& lightFreeList() { return rwModuleGlobals<rpLightGlobals>(lightModule).lights; }

inline RwReal fmadds(RwReal a, RwReal c, RwReal b) { return std::fmaf(a, c, b); }      // a * c + b
inline RwReal fmsubs(RwReal a, RwReal c, RwReal b) { return std::fmaf(a, c, -b); }     // a * c - b
inline RwReal fnmsubs(RwReal a, RwReal c, RwReal b) { return -std::fmaf(a, c, -b); }   // -(a * c - b)

/// fdlibm acosf's rational approximation: p(z) = z * (pS0 + z * (... pS5)), q(z) = 1 + z * (qS1 + ... qS4), each a
/// chain of fused multiply-adds as the original evaluates them.
RwReal acosP(RwReal z) {
    return z * fmadds(z, fmadds(z, fmadds(z, fmadds(z, fmadds(kPS5, z, kPS4), kPS3), kPS2), kPS1), kPS0);
}
RwReal acosQ(RwReal z) { return fmadds(z, fmadds(z, fmadds(z, fmadds(kQS4, z, kQS3), kQS2), kQS1), kOne); }

/// The light's sync (0x8045B7D0): nothing to do until the world plugin replaces it.
RwObjectHasFrame* LightSync(RwObjectHasFrame* object) { return object; }

/// LightTidyDestroyLight (0x8045B7B0): the close's callback over every light still in the free list.
void LightTidyDestroyLight(void* light, void*) { RpLightDestroy(static_cast<RpLight*>(light)); }

void setChroma(RpLight* light) {
    const RwRGBAReal& c = light->color;
    light->object.object.privateFlags = (c.red == c.green && c.red == c.blue) ? rpLIGHTPRIVATENOCHROMA : 0;
}

}  // namespace

RwPluginRegistry& rpLightRegistry() { return lightTKList; }

void* _rpLightOpen(void* instance, RwInt32 offset, RwInt32 /*size*/) {
    lightModule.globalsOffset = offset;
    lightFreeList() = RwFreeListCreateAndPreallocateSpace(lightTKList.sizeOfStruct, kLightsPerBlock, alignof(RpLight),
                                                          kPreallocBlocks, &lightFreeListSpace,
                                                          rwMEMHINTDUR_GLOBAL | rwID_LIGHT);
    if (lightFreeList() == nullptr) return nullptr;
    ++lightModule.numInstances;
    return instance;
}

void* _rpLightClose(void* instance, RwInt32 /*offset*/, RwInt32 /*size*/) {
    if (lightFreeList() != nullptr) {   // native: an open that failed leaves no list to walk
        RwFreeListForAllUsed(lightFreeList(), LightTidyDestroyLight, nullptr);
        RwFreeListDestroy(lightFreeList());
        lightFreeList() = nullptr;
    }
    --lightModule.numInstances;
    return instance;
}

RpLight* RpLightCreate(RwInt32 type) {
    if (lightModule.numInstances <= 0) {   // native: no light module open
        RwError error{rwID_COREPLUGIN, _rwerror(static_cast<RwInt32>(0x80000001u))};   // E_RW_BADENGINESTATE
        RwErrorSet(&error);
        return nullptr;
    }
    auto* light = static_cast<RpLight*>(rwSrcGlobals().memoryAlloc(lightFreeList(), kLightHint));
    if (light == nullptr) return nullptr;
    std::memset(light, 0, sizeof *light);   // native: the original leaves the frame link as memory held it
    light->object.object = {rpLIGHT, static_cast<RwUInt8>(type), 0, 0, nullptr};
    light->object.sync = LightSync;
    light->radius = kZero;
    light->minusCosAngle = kZero;
    light->color = {kOne, kOne, kOne, kOne};
    light->object.object.privateFlags = rpLIGHTPRIVATENOCHROMA;
    light->WorldSectorsInLight.link = {&light->WorldSectorsInLight.link, &light->WorldSectorsInLight.link};
    light->inWorld = {nullptr, nullptr};
    light->lightFrame = static_cast<RwUInt16>(rwSrcGlobals().lightFrame - 1);
    light->object.object.flags = rpLIGHTLIGHTATOMICS | rpLIGHTLIGHTWORLD;
    if (_rwPluginRegistryInitObject(&lightTKList, light) == nullptr) {
        // native: a failed plugin constructor fails the creation (the original returns the half-built light)
        rwSrcGlobals().memoryFree(lightFreeList(), light);
        return nullptr;
    }
    return light;
}

RwBool RpLightDestroy(RpLight* light) {
    _rwPluginRegistryDeInitObject(&lightTKList, light);
    _rwObjectHasFrameReleaseFrame(light);
    rwSrcGlobals().memoryFree(lightFreeList(), light);
    return 1;
}

RpLight* RpLightSetRadius(RpLight* light, RwReal radius) {
    light->radius = radius;
    if (RwFrame* frame = RpLightGetFrame(light)) RwFrameUpdateObjects(frame);
    return light;
}

RpLight* RpLightSetColor(RpLight* light, const RwRGBAReal* color) {
    light->color = *color;
    setChroma(light);
    return light;
}

RwReal RpLightGetConeAngle(const RpLight* light) {
    // acosf(x), x = -minusCosAngle (fdlibm e_acosf.c as the original inlines it)
    const RwReal x = -light->minusCosAngle;
    const RwInt32 hx = std::bit_cast<RwInt32>(x);
    const RwInt32 ix = hx & 0x7FFFFFFF;
    if (ix >= 0x3F800000) return hx > 0 ? kZero : kPi;   // |x| >= 1 (NaN too): 0 or pi
    if (ix < 0x3F000000) {                                // |x| < 0.5
        if (ix <= 0x23000000) return kHalfPi;
        const RwReal z = x * x;
        const RwReal r = acosP(z) / acosQ(z);
        return kPio2Hi - (x - fnmsubs(x, r, kPio2Lo));    // pio2_hi - (x - (pio2_lo - x * r))
    }
    if (hx < 0) {                                         // x <= -0.5
        const RwReal z = kHalf * (kOne + x);
        const RwReal s = _rwSqrt(z);
        const RwReal r = acosP(z) / acosQ(z);
        const RwReal w = fmsubs(r, s, kPio2Lo);
        return fnmsubs(kTwo, s + w, kPiLow);              // pi - 2 * (s + w)
    }
    const RwReal z = kHalf * (kOne - x);                  // x >= 0.5
    const RwReal s = _rwSqrt(z);
    const RwReal df = std::bit_cast<RwReal>(std::bit_cast<RwUInt32>(s) & 0xFFFFF000u);
    const RwReal c = fnmsubs(df, df, z) / (s + df);       // (z - df * df) / (s + df)
    const RwReal r = acosP(z) / acosQ(z);
    const RwReal w = fmadds(r, s, c);
    return kTwo * (df + w);
}

RpLight* RpLightSetConeAngle(RpLight* light, RwReal angle) {
    if (angle < kZero || angle > kHalfPi) return nullptr;
    light->minusCosAngle = -static_cast<RwReal>(shadow::math::msl::cos(angle));   // MSL cos, frsp, fneg
    return light;
}

RpLight* RpLightSetFrame(RpLight* light, RwFrame* frame) {
    _rwObjectHasFrameSetFrame(light, frame);
    return light;
}

RwFrame* RpLightGetFrame(const RpLight* light) { return static_cast<RwFrame*>(light->object.object.parent); }

RwInt32 RpLightRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                              RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB) {
    return _rwPluginRegistryAddPlugin(&lightTKList, size, pluginID, constructCB, destructCB, copyCB);
}

RwInt32 RpLightRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB,
                                    RwPluginDataChunkWriteCallBack writeCB, RwPluginDataChunkGetSizeCallBack getSizeCB) {
    return _rwPluginRegistryAddPluginStream(&lightTKList, pluginID, readCB, writeCB, getSizeCB);
}

RpLight* RpLightStreamRead(RwStream* stream) {
    RwUInt32 length = 0, version = 0;
    if (!RwStreamFindChunk(stream, rwID_STRUCT, &length, &version)) return nullptr;
    if (version < rwLIBRARYBASEVERSION || version > rwLIBRARYCURRENTVERSION) {
        RwError error{rwID_WORLDPLUGIN, _rwerror(static_cast<RwInt32>(0x80000004u))};   // E_RW_BADVERSION
        RwErrorSet(&error);
        return nullptr;
    }
    // {radius, red, green, blue, minusCosAngle (a tangent before 3.3.0.0), flags | type << 16}: 24 bytes
    RwUInt32 data[6] = {};
    const RwUInt32 take = length < sizeof data ? length : static_cast<RwUInt32>(sizeof data);
    if (RwStreamRead(stream, data, take) != take) return nullptr;
    if (length > take && RwStreamSkip(stream, length - take) == nullptr) return nullptr;   // native: the rest skipped
    RwMemNative32(data, sizeof data);
    RpLight* light = RpLightCreate(static_cast<RwInt32>((data[5] >> 16) & 0xFFu));
    if (light == nullptr) return nullptr;
    light->radius = std::bit_cast<RwReal>(data[0]);
    light->color.red = std::bit_cast<RwReal>(data[1]);
    light->color.green = std::bit_cast<RwReal>(data[2]);
    light->color.blue = std::bit_cast<RwReal>(data[3]);
    const RwReal cone = std::bit_cast<RwReal>(data[4]);
    if (version < 0x30300u) {   // unreachable after the version check above, as in the original
        light->minusCosAngle = -_rwSqrt(kOne / (kOne + cone * cone));
    } else {
        light->minusCosAngle = cone;
    }
    setChroma(light);
    light->object.object.flags = static_cast<RwUInt8>(data[5]);
    if (_rwPluginRegistryReadDataChunks(&lightTKList, stream, light) == nullptr) {
        RpLightDestroy(light);   // native: the original leaks it
        return nullptr;
    }
    return light;
}
