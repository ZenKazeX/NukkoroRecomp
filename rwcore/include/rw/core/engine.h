// RenderWare Graphics 3.7 engine instance: RwGlobals, the memory functions and module globals (rwplcore.h), native x64.
//
// RenderWare keeps all engine state in one block, RwEngineInstance: RwGlobals followed by every module's globals at
// the offset its registration returned (RwModuleInfo::globalsOffset). The structure and field names are the header's;
// on x64 the pointer fields are 8 bytes, so the offsets differ from the GameCube's (memoryFuncs is at +0x108 there),
// which nothing depends on: modules reach their globals through RwModuleInfo, as the originals do.
//
// Memory: RwMemoryFunctions take size_t sizes (RenderWare's own signature). The default functions are the host C++
// allocator - no fixed arena, no 32-bit size or address arithmetic - so the engine can use all of the machine's
// memory. An application may install its own, as with RwEngineInit.
#pragma once

#include <cstdarg>
#include <cstddef>

#include "rw/core/types.h"

struct RwFreeList;
struct RwMetrics;
struct RwIm2DVertex;
enum RwRenderState : RwInt32;
enum RwPrimitiveType : RwInt32;
using RwImVertexIndex = RwUInt16;

using RwSystemFunc = RwBool (*)(RwInt32 nOption, void* pOut, void* pInOut, RwInt32 nIn);
using RwRenderStateSetFunction = RwBool (*)(RwRenderState nState, void* pParam);
using RwRenderStateGetFunction = RwBool (*)(RwRenderState nState, void* pParam);
using RwIm2DRenderLineFunction = RwBool (*)(RwIm2DVertex* vertices, RwInt32 numVertices, RwInt32 vert1, RwInt32 vert2);
using RwIm2DRenderTriangleFunction = RwBool (*)(RwIm2DVertex* vertices, RwInt32 numVertices, RwInt32 vert1,
                                                RwInt32 vert2, RwInt32 vert3);
using RwIm2DRenderPrimitiveFunction = RwBool (*)(RwPrimitiveType primType, RwIm2DVertex* vertices,
                                                 RwInt32 numVertices);
using RwIm2DRenderIndexedPrimitiveFunction = RwBool (*)(RwPrimitiveType primType, RwIm2DVertex* vertices,
                                                        RwInt32 numVertices, RwImVertexIndex* indices,
                                                        RwInt32 numIndices);
using RwIm3DRenderLineFunction = RwBool (*)(RwInt32 vert1, RwInt32 vert2);
using RwIm3DRenderTriangleFunction = RwBool (*)(RwInt32 vert1, RwInt32 vert2, RwInt32 vert3);
using RwIm3DRenderPrimitiveFunction = RwBool (*)(RwPrimitiveType primType);
using RwIm3DRenderIndexedPrimitiveFunction = RwBool (*)(RwPrimitiveType primtype, RwImVertexIndex* indices,
                                                        RwInt32 numIndices);

/// The device block of the engine (rwplcore.h RwDevice). The renderer behind it is the project's, not GameCube's.
struct RwDevice {
    RwReal gammaCorrection;
    RwSystemFunc fpSystem;
    RwReal zBufferNear;
    RwReal zBufferFar;
    RwRenderStateSetFunction fpRenderStateSet;
    RwRenderStateGetFunction fpRenderStateGet;
    RwIm2DRenderLineFunction fpIm2DRenderLine;
    RwIm2DRenderTriangleFunction fpIm2DRenderTriangle;
    RwIm2DRenderPrimitiveFunction fpIm2DRenderPrimitive;
    RwIm2DRenderIndexedPrimitiveFunction fpIm2DRenderIndexedPrimitive;
    RwIm3DRenderLineFunction fpIm3DRenderLine;
    RwIm3DRenderTriangleFunction fpIm3DRenderTriangle;
    RwIm3DRenderPrimitiveFunction fpIm3DRenderPrimitive;
    RwIm3DRenderIndexedPrimitiveFunction fpIm3DRenderIndexedPrimitive;
};

using RwStandardFunc = RwBool (*)(void* pOut, void* pInOut, RwInt32 nI);
inline constexpr int rwSTANDARDNUMOFSTANDARD = 29;

/// The string functions (rwplcore.h RwStringFunctions).
struct RwStringFunctions {
    int (*vecSprintf)(RwChar* buffer, const RwChar* format, ...);
    int (*vecVsprintf)(RwChar* buffer, const RwChar* format, std::va_list argptr);
    RwChar* (*vecStrcpy)(RwChar* dest, const RwChar* srce);
    RwChar* (*vecStrncpy)(RwChar* dest, const RwChar* srce, std::size_t size);
    RwChar* (*vecStrcat)(RwChar* dest, const RwChar* srce);
    RwChar* (*vecStrncat)(RwChar* dest, const RwChar* srce, std::size_t size);
    RwChar* (*vecStrrchr)(const RwChar* string, int findThis);
    RwChar* (*vecStrchr)(const RwChar* string, int findThis);
    RwChar* (*vecStrstr)(const RwChar* string, const RwChar* findThis);
    int (*vecStrcmp)(const RwChar* string1, const RwChar* string2);
    int (*vecStrncmp)(const RwChar* string1, const RwChar* string2, std::size_t max_size);
    int (*vecStricmp)(const RwChar* string1, const RwChar* string2);
    std::size_t (*vecStrlen)(const RwChar* string);
    RwChar* (*vecStrupr)(RwChar* string);
    RwChar* (*vecStrlwr)(RwChar* string);
    RwChar* (*vecStrtok)(RwChar* string, const RwChar* delimit);
    int (*vecSscanf)(const RwChar* buffer, const RwChar* format, ...);
};

/// The memory functions (rwplcore.h RwMemoryFunctions). Sizes are size_t.
struct RwMemoryFunctions {
    void* (*rwmalloc)(std::size_t size, RwUInt32 hint);
    void (*rwfree)(void* mem);
    void* (*rwrealloc)(void* mem, std::size_t newSize, RwUInt32 hint);
    void* (*rwcalloc)(std::size_t numObj, std::size_t sizeObj, RwUInt32 hint);
};

using RwMemoryAllocFn = void* (*)(RwFreeList* fl, RwUInt32 hint);
using RwMemoryFreeFn = RwFreeList* (*)(RwFreeList* fl, void* pData);

enum RwEngineStatus : RwInt32 {
    rwENGINESTATUSIDLE = 0,
    rwENGINESTATUSINITED = 1,
    rwENGINESTATUSOPENED = 2,
    rwENGINESTATUSSTARTED = 3,
};

/// The root of the engine instance (rwplcore.h RwGlobals, release layout).
struct RwGlobals {
    void* curCamera;
    void* curWorld;
    RwUInt16 renderFrame;
    RwUInt16 lightFrame;
    RwUInt16 pad[2];
    RwDevice dOpenDevice;
    RwStandardFunc stdFunc[rwSTANDARDNUMOFSTANDARD];
    RwLinkList dirtyFrameList;
    RwStringFunctions stringFuncs;
    RwMemoryFunctions memoryFuncs;
    RwMemoryAllocFn memoryAlloc;
    RwMemoryFreeFn memoryFree;
    RwMetrics* metrics;
    RwEngineStatus engineStatus;
    RwUInt32 resArenaInitSize;
};

/// A module's place in the engine instance (rwplcore.h RwModuleInfo).
struct RwModuleInfo {
    RwInt32 globalsOffset;
    RwInt32 numInstances;
};

/// The engine instance: RwGlobals, then the module globals (RenderWare's RwEngineInstance).
extern void* RwEngineInstance;

/// How many engines are open (RwEngineOpen counts up, RwEngineClose down; GameCube 0x805F2658, beside
/// RwEngineInstance). The name is descriptive: the variable is static in the original.
extern RwInt32 rwNumEngineInstances;

/// The number of open engines (plugins can only be registered while it is 0). Original: 0x80486DB4.
inline RwInt32 _rwGetNumEngineInstances() { return rwNumEngineInstances; }

/// RWSRCGLOBAL(variable): the engine's RwGlobals.
inline RwGlobals& rwSrcGlobals() { return *static_cast<RwGlobals*>(RwEngineInstance); }

/// RWPLUGINOFFSET(T, RwEngineInstance, module.globalsOffset): a module's globals.
template <class T> T& rwModuleGlobals(const RwModuleInfo& module) {
    return *reinterpret_cast<T*>(static_cast<char*>(RwEngineInstance) + module.globalsOffset);
}

/// RwMalloc / RwFree / RwRealloc / RwCalloc: through the engine's memory functions.
inline void* RwMalloc(std::size_t size, RwUInt32 hint) { return rwSrcGlobals().memoryFuncs.rwmalloc(size, hint); }
inline void RwFree(void* mem) { rwSrcGlobals().memoryFuncs.rwfree(mem); }
inline void* RwRealloc(void* mem, std::size_t size, RwUInt32 hint) {
    return rwSrcGlobals().memoryFuncs.rwrealloc(mem, size, hint);
}
inline void* RwCalloc(std::size_t num, std::size_t size, RwUInt32 hint) {
    return rwSrcGlobals().memoryFuncs.rwcalloc(num, size, hint);
}

/// The host allocator as RenderWare memory functions (the default): no arena, no size limit but the machine's.
RwMemoryFunctions rwHostMemoryFunctions();

/// RenderWare's memory hints (rwplcore.h RwMemoryHintDuration / RwMemoryHintFlag, as the binary passes them).
inline constexpr RwUInt32 rwMEMHINTDUR_GLOBAL = 0x00040000u;
inline constexpr RwUInt32 rwID_VECTORMODULE = 0x00000401u;  ///< rwID_VECTORMODULE (rwcore module id)

// ---- the engine's life (module badevice; originals 0x80486C8C..0x804878D4) ----------------------------------------
// RwEngineInit (memory, plugin registry, the core modules registered as engine plugins) -> RwEngineOpen (the engine
// instance allocated, the device opened) -> RwEngineStart (every module's open callback, in registration order) ->
// RwEngineStop -> RwEngineClose -> RwEngineTerm. Plugins register between Init and Open. The device is the port's
// (rwDeviceHandle): the GameCube driver is not ported; the renderer behind the device is the project's.

enum RwEngineInitFlag : RwUInt32 {
    rwENGINEINITFREELISTS = 0x0,
    rwENGINEINITNOFREELISTS = 0x1,  ///< memoryAlloc / memoryFree straight to RwMalloc / RwFree (no free lists)
};

/// The device requests (rwplcore.h RwCoreDeviceSystemFn).
enum RwCoreDeviceSystemFn : RwInt32 {
    rwDEVICESYSTEMOPEN = 0x00,
    rwDEVICESYSTEMCLOSE,
    rwDEVICESYSTEMSTART,
    rwDEVICESYSTEMSTOP,
    rwDEVICESYSTEMREGISTER,
    rwDEVICESYSTEMGETNUMMODES,
    rwDEVICESYSTEMGETMODEINFO,
    rwDEVICESYSTEMUSEMODE,
    rwDEVICESYSTEMFOCUS,
    rwDEVICESYSTEMINITPIPELINE,
    rwDEVICESYSTEMGETMODE,
    rwDEVICESYSTEMSTANDARDS,
    rwDEVICESYSTEMGETTEXMEMSIZE,
    rwDEVICESYSTEMGETNUMSUBSYSTEMS,
    rwDEVICESYSTEMGETSUBSYSTEMINFO,
    rwDEVICESYSTEMGETCURRENTSUBSYSTEM,
    rwDEVICESYSTEMSETSUBSYSTEM,
    rwDEVICESYSTEMFINALIZESTART,
    rwDEVICESYSTEMINITIATESTOP,
    rwDEVICESYSTEMGETMAXTEXTURESIZE,
};

/// What RwEngineOpen hands the device (rwplcore.h RwEngineOpenParams).
struct RwEngineOpenParams {
    void* displayID;
};

/// A rendering sub-system's description (rwplcore.h RwSubSystemInfo).
struct RwSubSystemInfo {
    RwChar name[80];
};

/// The device the engine opens (_rwDeviceGetHandle's result): the port's host device unless replaced, as a test does.
/// Descriptive name: the GameCube's comes from its driver (0x804960F0).
RwDevice*& rwDeviceHandle();

/// The host device's standard functions (rwplcore.h rwSTANDARD* indices, rwSTANDARDNUMOFSTANDARD of them), copied
/// into RwGlobals::stdFunc when an engine opens: the renderer behind the device installs its own here first. By
/// default camera begin / end update succeed and the rest report "not supported". Descriptive name (the port's).
RwStandardFunc* rwHostDeviceStandards();

/// The string module: the engine's RwStringFunctions become the host C library's. Originals: 0x80483E70, 0x80483FB0.
RwBool _rwStringOpen();
void _rwStringClose();

/// device->fpSystem(request, ...), with RenderWare's answers when the device has none (one sub-system named "Only
/// rendering sub system", number 0; start / stop steps succeed); E_RW_SYSTEMREQUEST when neither answers.
/// Original: 0x80486C8C.
RwBool _rwDeviceSystemRequest(RwDevice* device, RwInt32 requestID, void* pOut, void* pInOut, RwInt32 numIn);

/// The RenderWare version the game reports: 0x37002 (3.7.0.2). Original: 0x80486DBC.
RwUInt32 RwEngineGetVersion();

/// Registers an engine plugin (a module): its globals of size bytes in every engine instance, initCB / termCB run
/// at RwEngineStart / RwEngineStop. Returns its offset or -1. Original: 0x80486DC8.
RwInt32 RwEngineRegisterPlugin(RwInt32 size, RwUInt32 pluginID, void* (*initCB)(void*, RwInt32, RwInt32),
                               void* (*termCB)(void*, RwInt32, RwInt32));

/// The offset of an engine plugin's globals, or -1. Original: 0x80486E08.
RwInt32 RwEngineGetPluginOffset(RwUInt32 pluginID);

/// Initialises the engine: memory (memoryFuncs, or the host's when NULL), the plugin registry, the string functions
/// and the core modules as engine plugins. FALSE when the engine is not idle or a step fails (everything undone).
/// Native: the engine state is checked before anything is touched (the original first resets RwEngineInstance).
/// Original: 0x80487548.
RwBool RwEngineInit(const RwMemoryFunctions* memoryFuncs, RwUInt32 initFlags, RwUInt32 resArenaSize);

/// Allocates the engine instance (RwGlobals + every plugin's globals, zeroed natively) and opens the device. FALSE
/// (E_RW_BADENGINESTATE / E_RW_NULLP / E_RW_NOMEM / the device's) leaves the engine initialised. Original: 0x80487254.
RwBool RwEngineOpen(const RwEngineOpenParams* initParams);

/// Starts the device and runs every module's open callback; when one fails, the opened ones are closed again (in
/// reverse) and the device stopped. Original: 0x80487030.
RwBool RwEngineStart();

/// Runs every module's close callback (last registered first) and stops the device; FALSE when the device refuses
/// (the engine is opened either way: its modules are closed). Original: 0x80486F44.
RwBool RwEngineStop();

/// Closes the device and frees the engine instance (RwGlobals go back to the static block). Original: 0x80487188.
RwBool RwEngineClose();

/// Frees the plugin registry and memory; FALSE while an engine is open. Original: 0x804874FC.
RwBool RwEngineTerm();

/// The engine's state (rwENGINESTATUS*). The original reads RwGlobals::engineStatus; for inspection.
RwEngineStatus rwEngineGetStatus();
