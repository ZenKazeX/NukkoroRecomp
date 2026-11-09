// RenderWare Graphics 3.7 world plugin: lights (module balight), native x64.
//
// A light is an object with a frame: ambient and directional lights light everything they reach; point and spot
// lights have a radius, spot lights a cone (kept as -cos of its half-angle). Colours are RGBA reals; a grey colour
// (r == g == b) is flagged so lighting can skip the colour channels apart. The world plugin's light extension (the
// world-light sync) ties a light to the world and its sectors; that comes with the world module.
//
// Written from PAL main.dol balight 0x8045B7B0..0x8045BF74, every arithmetic step in the original's order (the cone
// angle's inlined fdlibm acosf included). Proof: runtime/tests/rpworld_light_tests.cpp (oracle) and
// rpworld_light_native_tests.cpp. Deliberate differences: rwcore/README.md "Native contracts" -> Lights.
// Not linked by the game, so not provided: RpLightClone, RpLightStreamWrite / GetSize, RpLightGetPluginOffset,
// RpLightSetFreeListCreateParams.
#pragma once

#include "rw/core/engine.h"
#include "rw/core/frame.h"
#include "rw/core/plugin.h"
#include "rw/core/stream.h"
#include "rw/core/types.h"

inline constexpr RwUInt8 rpLIGHT = 3;   ///< RwObject::type of a light

enum RpLightType : RwInt32 {
    rpNALIGHTTYPE = 0,
    rpLIGHTDIRECTIONAL = 1,
    rpLIGHTAMBIENT = 2,
    rpLIGHTPOINT = 0x80,
    rpLIGHTSPOT = 0x81,
    rpLIGHTSPOTSOFT = 0x82,
};

enum RpLightFlag : RwUInt8 {
    rpLIGHTLIGHTATOMICS = 0x01,
    rpLIGHTLIGHTWORLD = 0x02,
};

inline constexpr RwUInt8 rpLIGHTPRIVATENOCHROMA = 0x01;   ///< privateFlags: r == g == b

/// A light (rpworld.h RpLight).
struct RpLight {
    RwObjectHasFrame object;      ///< type rpLIGHT, subType = RpLightType, flags = RpLightFlag
    RwReal radius;
    RwRGBAReal color;
    RwReal minusCosAngle;         ///< -cos of a spot light's cone half-angle
    RwLinkList WorldSectorsInLight;   ///< the world plugin's sector ties (an empty list until the world module)
    RwLLLink inWorld;             ///< the world's list of global lights (unlinked: both NULL)
    RwUInt16 lightFrame;          ///< RwGlobals::lightFrame - 1 when created
    RwUInt16 pad;
};

/// The light module's globals: the free list of lights.
struct rpLightGlobals {
    RwFreeList* lights;
};

extern RwModuleInfo lightModule;

/// The lights' plugin registry (lightTKList, 0x8056E8E8).
RwPluginRegistry& rpLightRegistry();

/// Opens the module: the light free list (entries of the registry's size, 32 to a block, one preallocated, a static
/// header, hint rwMEMHINTDUR_GLOBAL | rwID_LIGHT). NULL when it cannot be made. Original: 0x8045BEE8.
void* _rpLightOpen(void* instance, RwInt32 offset, RwInt32 size);

/// Destroys every light still alive (LightTidyDestroyLight 0x8045B7B0 over the free list), then the free list.
/// Returns instance. Original: 0x8045BE70.
void* _rpLightClose(void* instance, RwInt32 offset, RwInt32 size);

/// A light of the given type: radius 0, white, cone 0 (minusCosAngle 0), flags rpLIGHTLIGHTATOMICS |
/// rpLIGHTLIGHTWORLD, grey, no frame, not in a world, the plugins' constructors run. NULL when the allocation or
/// (native) a plugin constructor fails, or no light module is open. Original: 0x8045BD84.
RpLight* RpLightCreate(RwInt32 type);

/// Runs the plugin destructors, detaches the light from its frame and frees it. TRUE. Original: 0x8045BD24.
RwBool RpLightDestroy(RpLight* light);

/// The radius; the frame is marked for an update. Returns light. Original: 0x8045B7D4.
RpLight* RpLightSetRadius(RpLight* light, RwReal radius);

/// The colour; the grey flag follows (r == g == b, compared as reals). Returns light. Original: 0x8045B814.
RpLight* RpLightSetColor(RpLight* light, const RwRGBAReal* color);

/// The cone half-angle in radians, acos(-minusCosAngle) by the original's inlined fdlibm acosf (|x| >= 1 gives 0
/// or pi; its square roots are _rwSqrt's). Original: 0x8045B868.
RwReal RpLightGetConeAngle(const RpLight* light);

/// A cone half-angle in radians, 0 .. pi / 2 (else NULL, nothing changed): minusCosAngle = -cos(angle) (MSL cos in
/// double, rounded). Original: 0x8045BA94.
RpLight* RpLightSetConeAngle(RpLight* light, RwReal angle);

/// The light's frame, type, flags (RenderWare's macros over the object).
RpLight* RpLightSetFrame(RpLight* light, RwFrame* frame);
RwFrame* RpLightGetFrame(const RpLight* light);
inline RpLightType RpLightGetType(const RpLight* light) { return static_cast<RpLightType>(light->object.object.subType); }
inline RwUInt32 RpLightGetFlags(const RpLight* light) { return light->object.object.flags; }
inline RpLight* RpLightSetFlags(RpLight* light, RwUInt32 flags) {
    light->object.object.flags = static_cast<RwUInt8>(flags);
    return light;
}

/// A light plugin (before the engine opens). Its offset or -1. Original: 0x8045BAF0.
RwInt32 RpLightRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                              RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);

/// A light plugin's stream callbacks. Its offset or -1. Original: 0x8045BB30.
RwInt32 RpLightRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB,
                                    RwPluginDataChunkWriteCallBack writeCB, RwPluginDataChunkGetSizeCallBack getSizeCB);

/// A light from a stream: its struct chunk {radius, red, green, blue, cone, flags | type << 16} (versions 3.5.0.0 ..
/// 3.7.0.2, else E_RW_BADVERSION; before 3.3 the cone was a tangent) and plugin data. NULL on a read failure
/// (native: the light is destroyed; the original leaks it; a struct chunk longer than 24 bytes is read into 24 and
/// skipped - the original reads it all into a 24-byte stack buffer). Original: 0x8045BB6C.
RpLight* RpLightStreamRead(RwStream* stream);
