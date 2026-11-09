// The engine's life and its device (module badevice; see rw/core/engine.h), written from PAL main.dol
// 0x80486C8C..0x804878D4. Differences from the original, all for robustness (the original's debug build asserts the
// same states): each step checks the engine state it needs (the original calls whatever device function the static
// RwGlobals hold, NULL before an open); RwEngineInit checks the state before touching anything; the new instance is
// zeroed (the original leaves the module globals to their open callbacks); a device that fails to register fails
// the open (the original carries on with no device function and crashes at the next request); a device without the
// standard functions gets "not supported" ones; a device that refuses to stop still leaves the engine opened (its
// modules are closed by then; the original stays "started" with closed modules, a state nothing can leave). Errors raised before the error module is
// open go to the error module's fallback (rw/core/error.h), where the original writes over RwGlobals::curCamera.
#include <cstring>

#include "rw/core/engine.h"
#include "rw/core/camera.h"
#include "rw/core/error.h"
#include "rw/core/frame.h"
#include "rw/core/image.h"
#include "rw/core/matrix.h"
#include "rw/core/memory.h"
#include "rw/core/plugin.h"
#include "rw/core/raster.h"
#include "rw/core/resources.h"
#include "rw/core/stream.h"
#include "rw/core/texture.h"
#include "rw/core/vector.h"

RwModuleInfo colourModule{};

namespace {

constexpr RwInt32 E_RW_BADENGINESTATE = static_cast<RwInt32>(0x80000001u);
constexpr RwInt32 E_RW_NULLP = static_cast<RwInt32>(0x80000016u);
constexpr RwInt32 E_RW_SYSTEMREQUEST = 0x18;
constexpr RwUInt32 kVersion = 0x37002u;

// the core modules' plugin IDs (rwplcore.h rwID_*MODULE)
constexpr RwUInt32 rwID_CAMERAMODULE = 0x405;
constexpr RwUInt32 rwID_IMAGEMODULE = 0x406;
constexpr RwUInt32 rwID_RASTERMODULE = 0x407;
constexpr RwUInt32 rwID_TEXTUREMODULE = 0x408;
constexpr RwUInt32 rwID_MATRIXMODULE = 0x402, rwID_FRAMEMODULE = 0x403, rwID_STREAMMODULE = 0x404, rwID_COLORMODULE = 0x40D,
                   rwID_ERRORMODULE = 0x40F;

RwGlobals g_staticGlobals{};                                                  // 0x805E3C08
RwPluginRegistry engineTKList = rwPLUGINREGISTRYINIT(sizeof(RwGlobals));     // 0x8056F070

void systemError(RwInt32 request) {
    RwError error{rwID_COREPLUGIN, _rwerror(E_RW_SYSTEMREQUEST, request)};
    RwErrorSet(&error);
}

void engineError(RwInt32 code) {
    RwError error{rwID_COREPLUGIN, _rwerror(code)};
    RwErrorSet(&error);
}

/// The open device's function (the copy RwEngineOpen registered in RwGlobals).
RwBool deviceRequest(RwInt32 request) {
    return rwSrcGlobals().dOpenDevice.fpSystem(request, nullptr, nullptr, 0);
}

// ---- the host device ------------------------------------------------------------------------------------------
RwBool hostStandardNotSupported(void*, void*, RwInt32) { return 0; }
RwBool hostStandardSucceed(void*, void*, RwInt32) { return 1; }

/// The standard functions the host device hands the engine (rwplcore.h rwSTANDARD*): a renderer installs its own
/// before RwEngineOpen (rwHostDeviceStandards). Without one, beginning and ending a camera update succeed (nothing
/// to render into) and everything else reports "not supported".
RwStandardFunc* hostStandards() {
    static RwStandardFunc table[rwSTANDARDNUMOFSTANDARD] = {};
    static bool initialised = false;
    if (!initialised) {
        for (RwStandardFunc& f : table) f = hostStandardNotSupported;
        table[1] = hostStandardSucceed;    // rwSTANDARDCAMERABEGINUPDATE
        table[10] = hostStandardSucceed;   // rwSTANDARDCAMERAENDUPDATE
        initialised = true;
    }
    return table;
}

RwBool hostDeviceSystem(RwInt32 request, void* pOut, void* /*pInOut*/, RwInt32 numIn);

RwDevice g_hostDevice{1.0f, hostDeviceSystem, 0.0f, 1.0f, nullptr, nullptr, nullptr, nullptr,
                      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
RwDevice* g_device = &g_hostDevice;

/// The port's device: opens, starts, stops and closes (the renderer attaches to it separately); registering copies
/// the device block into the engine; the standard functions all answer "not supported" until a renderer supplies
/// them; everything else is left to _rwDeviceSystemRequest's defaults.
RwBool hostDeviceSystem(RwInt32 request, void* pOut, void* /*pInOut*/, RwInt32 numIn) {
    switch (request) {
        case rwDEVICESYSTEMOPEN:
        case rwDEVICESYSTEMCLOSE:
        case rwDEVICESYSTEMSTART:
        case rwDEVICESYSTEMSTOP:
        case rwDEVICESYSTEMFINALIZESTART:
        case rwDEVICESYSTEMINITIATESTOP:
            return 1;
        case rwDEVICESYSTEMREGISTER:
            *static_cast<RwDevice*>(pOut) = g_hostDevice;
            return 1;
        case 0x16: {   // rwDEVICESYSTEMGETID: none of RenderWare's device ids - dictionaries of any device are read
            *static_cast<RwUInt16*>(pOut) = 0;
            return 1;
        }
        case rwDEVICESYSTEMSTANDARDS: {
            auto* functions = static_cast<RwStandardFunc*>(pOut);
            for (RwInt32 i = 0; i < numIn && i < rwSTANDARDNUMOFSTANDARD; ++i) functions[i] = hostStandards()[i];
            return 1;
        }
        default:
            return 0;
    }
}

// ---- engine plugins of the core: the colour module has no globals, only its count (0x8047EB60 / 0x8047EB74) ----
void* colourOpen(void* instance, RwInt32, RwInt32) {
    ++colourModule.numInstances;
    return instance;
}
void* colourClose(void* instance, RwInt32, RwInt32) {
    --colourModule.numInstances;
    return instance;
}

// rwENGINEINITNOFREELISTS: free-list requests go straight to the engine's malloc / free (0x80486C1C / 0x80486C4C)
void* MallocWrapper(RwFreeList* fl, RwUInt32 hint) { return RwMalloc(fl->entrySize, hint); }
RwFreeList* FreeWrapper(RwFreeList* fl, void* pData) {
    RwFree(pData);
    return fl;
}

bool engineIs(RwEngineStatus status) { return RwEngineInstance != nullptr && rwSrcGlobals().engineStatus == status; }

}  // namespace

RwDevice*& rwDeviceHandle() { return g_device; }

RwStandardFunc* rwHostDeviceStandards() { return hostStandards(); }

RwEngineStatus rwEngineGetStatus() {
    return RwEngineInstance != nullptr ? rwSrcGlobals().engineStatus : g_staticGlobals.engineStatus;
}

RwBool _rwDeviceSystemRequest(RwDevice* device, RwInt32 requestID, void* pOut, void* pInOut, RwInt32 numIn) {
    RwBool result = device->fpSystem(requestID, pOut, pInOut, numIn);
    if (!result) {
        switch (requestID) {
            case rwDEVICESYSTEMGETNUMSUBSYSTEMS:
                *static_cast<RwInt32*>(pOut) = 1;
                result = 1;
                break;
            case rwDEVICESYSTEMGETSUBSYSTEMINFO:
                result = numIn == 0;
                if (result) {
                    rwSrcGlobals().stringFuncs.vecStrcpy(static_cast<RwSubSystemInfo*>(pOut)->name,
                                                         "Only rendering sub system");
                }
                break;
            case rwDEVICESYSTEMGETCURRENTSUBSYSTEM:
                *static_cast<RwInt32*>(pOut) = 0;
                result = 1;
                break;
            case rwDEVICESYSTEMSETSUBSYSTEM:
                result = numIn == 0;
                break;
            case rwDEVICESYSTEMFINALIZESTART:
            case rwDEVICESYSTEMINITIATESTOP:
                result = 1;
                break;
            default:
                break;
        }
    }
    if (!result) systemError(requestID);
    return result;
}

RwUInt32 RwEngineGetVersion() { return kVersion; }

RwInt32 RwEngineRegisterPlugin(RwInt32 size, RwUInt32 pluginID, void* (*initCB)(void*, RwInt32, RwInt32),
                               void* (*termCB)(void*, RwInt32, RwInt32)) {
    return _rwPluginRegistryAddPlugin(&engineTKList, size, pluginID, initCB, termCB, nullptr);
}

RwInt32 RwEngineGetPluginOffset(RwUInt32 pluginID) { return _rwPluginRegistryGetPluginOffset(&engineTKList, pluginID); }

RwBool RwEngineInit(const RwMemoryFunctions* memoryFuncs, RwUInt32 initFlags, RwUInt32 resArenaSize) {
    if (g_staticGlobals.engineStatus != rwENGINESTATUSIDLE) return 0;
    RwEngineInstance = &g_staticGlobals;
    if (initFlags & rwENGINEINITNOFREELISTS) {
        rwSrcGlobals().memoryAlloc = MallocWrapper;
        rwSrcGlobals().memoryFree = FreeWrapper;
        _rwFreeListEnable(0);
    } else {
        rwSrcGlobals().memoryAlloc = _rwFreeListAllocReal;
        rwSrcGlobals().memoryFree = _rwFreeListFreeReal;
        _rwFreeListEnable(1);
    }
    rwSrcGlobals().resArenaInitSize = resArenaSize;
    if (!_rwStringOpen()) return 0;
    if (!_rwMemoryOpen(memoryFuncs)) {
        _rwStringClose();
        return 0;
    }
    if (!_rwPluginRegistryOpen()) {
        _rwMemoryClose();
        _rwStringClose();
        return 0;
    }
    // The core modules in the original's order. Not ported yet (registered there in these slots, with their IDs):
    // pipeline 0x409, chunk group 0x412, immediate mode 0x40A between texture and resources; then the GameCube
    // driver's plugins (the host device attaches its raster storage instead).
    RwInt32 offsets = 0;
    offsets |= RwEngineRegisterPlugin(sizeof(RwError), rwID_ERRORMODULE, _rwErrorOpen, _rwErrorClose);
    offsets |= RwEngineRegisterPlugin(sizeof(rwVectorGlobals), rwID_VECTORMODULE, _rwVectorOpen, _rwVectorClose);
    offsets |= RwEngineRegisterPlugin(0, rwID_COLORMODULE, colourOpen, colourClose);
    offsets |= RwEngineRegisterPlugin(sizeof(rwMatrixGlobals), rwID_MATRIXMODULE, _rwMatrixOpen, _rwMatrixClose);
    offsets |= RwEngineRegisterPlugin(sizeof(rwFrameGlobals), rwID_FRAMEMODULE, _rwFrameOpen, _rwFrameClose);
    offsets |= RwEngineRegisterPlugin(sizeof(rwStreamGlobals), rwID_STREAMMODULE, _rwStreamModuleOpen,
                                      _rwStreamModuleClose);
    offsets |= RwEngineRegisterPlugin(sizeof(rwCameraGlobals), rwID_CAMERAMODULE, _rwCameraOpen, _rwCameraClose);
    offsets |= RwEngineRegisterPlugin(sizeof(rwImageGlobals), rwID_IMAGEMODULE, _rwImageOpen, _rwImageClose);
    offsets |= RwEngineRegisterPlugin(sizeof(rwRasterGlobals), rwID_RASTERMODULE, _rwRasterOpen, _rwRasterClose);
    offsets |= RwEngineRegisterPlugin(sizeof(rwTextureGlobals), rwID_TEXTUREMODULE, _rwTextureOpen, _rwTextureClose);
    offsets |= RwEngineRegisterPlugin(sizeof(rwResourcesGlobals), rwID_RESOURCESMODULE, _rwResourcesOpen,
                                      _rwResourcesClose);
    if (offsets >= 0 && !rwHostRasterAttach(hostStandards())) offsets = -1;
    if (offsets < 0) {
        _rwPluginRegistryClose();
        _rwMemoryClose();
        _rwStringClose();
        return 0;
    }
    rwSrcGlobals().engineStatus = rwENGINESTATUSINITED;
    return 1;
}

RwBool RwEngineOpen(const RwEngineOpenParams* initParams) {
    if (RwEngineInstance == nullptr) RwEngineInstance = &g_staticGlobals;
    if (!engineIs(rwENGINESTATUSINITED)) {
        engineError(E_RW_BADENGINESTATE);
        return 0;
    }
    if (initParams == nullptr) {
        engineError(E_RW_NULLP);
        return 0;
    }
    RwDevice* device = rwDeviceHandle();
    if (device == nullptr) return 0;
    const std::size_t size = static_cast<std::size_t>(engineTKList.sizeOfStruct);
    void* instance = RwMalloc(size, rwMEMHINTDUR_GLOBAL);
    if (instance == nullptr) {
        RwError error{rwID_COREPLUGIN, _rwerror(E_RW_NOMEM, static_cast<RwInt32>(size))};
        RwErrorSet(&error);
        return 0;
    }
    std::memset(instance, 0, size);
    *static_cast<RwGlobals*>(instance) = g_staticGlobals;
    RwEngineInstance = instance;
    RwGlobals& globals = rwSrcGlobals();
    const auto undo = [&](RwInt32 request) {
        systemError(request);
        RwEngineInstance = &g_staticGlobals;
        g_staticGlobals = globals;
        RwFree(instance);
        return 0;
    };
    if (!device->fpSystem(rwDEVICESYSTEMREGISTER, &globals.dOpenDevice, &globals.memoryFuncs, 0) ||
        globals.dOpenDevice.fpSystem == nullptr) {
        return undo(rwDEVICESYSTEMREGISTER);
    }
    if (!device->fpSystem(rwDEVICESYSTEMOPEN, nullptr, const_cast<RwEngineOpenParams*>(initParams), 0)) {
        return undo(rwDEVICESYSTEMOPEN);
    }
    if (!device->fpSystem(rwDEVICESYSTEMSTANDARDS, globals.stdFunc, nullptr, rwSTANDARDNUMOFSTANDARD)) {
        systemError(rwDEVICESYSTEMSTANDARDS);
        for (RwStandardFunc& f : globals.stdFunc) f = hostStandardNotSupported;
    }
    ++rwNumEngineInstances;
    globals.engineStatus = rwENGINESTATUSOPENED;
    return 1;
}

RwBool RwEngineStart() {
    if (!engineIs(rwENGINESTATUSOPENED)) {
        engineError(E_RW_BADENGINESTATE);
        return 0;
    }
    if (!deviceRequest(rwDEVICESYSTEMSTART)) {
        systemError(rwDEVICESYSTEMSTART);
        return 0;
    }
    if (_rwPluginRegistryInitObject(&engineTKList, RwEngineInstance) == nullptr) {
        if (!deviceRequest(rwDEVICESYSTEMSTOP)) systemError(rwDEVICESYSTEMSTOP);
        return 0;
    }
    RwImageSetGamma(rwSrcGlobals().dOpenDevice.gammaCorrection);   // as the original (0x804870B8)
    deviceRequest(rwDEVICESYSTEMFINALIZESTART);   // a device without the step succeeds, as in the original
    rwSrcGlobals().engineStatus = rwENGINESTATUSSTARTED;
    return 1;
}

RwBool RwEngineStop() {
    if (!engineIs(rwENGINESTATUSSTARTED)) {
        engineError(E_RW_BADENGINESTATE);
        return 0;
    }
    deviceRequest(rwDEVICESYSTEMINITIATESTOP);
    _rwPluginRegistryDeInitObject(&engineTKList, RwEngineInstance);
    const RwBool stopped = deviceRequest(rwDEVICESYSTEMSTOP);
    if (!stopped) systemError(rwDEVICESYSTEMSTOP);
    rwSrcGlobals().engineStatus = rwENGINESTATUSOPENED;   // the modules are closed either way
    return stopped;
}

RwBool RwEngineClose() {
    if (!engineIs(rwENGINESTATUSOPENED)) {
        engineError(E_RW_BADENGINESTATE);
        return 0;
    }
    if (!deviceRequest(rwDEVICESYSTEMCLOSE)) {
        systemError(rwDEVICESYSTEMCLOSE);
        return 0;
    }
    void* instance = RwEngineInstance;
    RwEngineInstance = &g_staticGlobals;
    g_staticGlobals = *static_cast<RwGlobals*>(instance);
    RwFree(instance);
    --rwNumEngineInstances;
    g_staticGlobals.engineStatus = rwENGINESTATUSINITED;
    return 1;
}

RwBool RwEngineTerm() {
    if (rwNumEngineInstances != 0 || g_staticGlobals.engineStatus != rwENGINESTATUSINITED) return 0;
    _rwPluginRegistryClose();
    _rwMemoryClose();
    g_staticGlobals.engineStatus = rwENGINESTATUSIDLE;
    return 1;
}
