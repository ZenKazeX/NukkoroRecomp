// RenderWare rasters (module baraster, CPU side; see rw/core/raster.h) and the host device's raster functions.
#include "rw/core/raster.h"

#include <cstring>

#include "rw/core/error.h"
#include "rw/core/memory.h"
#include "rw/core/texture.h"

RwModuleInfo rasterModule{};

namespace {

// The free list's creation parameters (baraster statics 0x805EEFA8 / 0x805EEFAC) and its in-place header
// (0x805E3DA8).
constexpr RwInt32 kRastersPerBlock = 128;
constexpr RwInt32 kPreallocBlocks = 1;
constexpr RwUInt32 rwID_RASTERMODULE = 0x407u;
constexpr RwUInt32 kRasterHint = 0x00030000u | rwID_RASTERMODULE;   // rwMEMHINTDUR_EVENT, as the original passes it
RwFreeList rasterFreeListSpace{};
RwPluginRegistry rasterTKList = rwPLUGINREGISTRYINIT(sizeof(RwRaster));   // 0x8056F0D8

constexpr RwInt32 rwSTANDARDRASTERCREATE = 4, rwSTANDARDRASTERDESTROY = 5, rwSTANDARDTEXTURESETRASTER = 8,
                  rwSTANDARDRASTERLOCK = 15, rwSTANDARDRASTERUNLOCK = 16, rwSTANDARDRASTERGETMIPLEVELS = 28;

RwFreeList*& rasterFreeList() { return rwModuleGlobals<rwRasterGlobals>(rasterModule).rasters; }

// ---- the host device ------------------------------------------------------------------------------------------
constexpr RwUInt32 kHostRasterPluginID = 0x7FFF0407u;   // the port's own (no RenderWare plugin has this id)
RwInt32 g_hostRasterOffset = -1;

void* hostRasterConstruct(void* object, RwInt32 offset, RwInt32) {
    auto* host = reinterpret_cast<rwHostRaster*>(static_cast<char*>(object) + offset);
    *host = {nullptr, 0, 0, 0, -1};
    return object;
}

RwInt32 levelDimension(RwInt32 size, RwInt32 level) { return (size >> level) > 0 ? size >> level : 1; }

RwBool hostRasterCreate(void*, void* object, RwInt32 flags) {
    auto* raster = static_cast<RwRaster*>(object);
    rwHostRaster* host = rwHostRasterOf(raster);
    const RwInt32 type = flags & rwRASTERTYPEMASK;
    const RwInt32 format = flags & rwRASTERFORMATMASK;
    raster->cType = static_cast<RwUInt8>(type);
    raster->cFormat = static_cast<RwUInt8>(format >> 8);
    raster->stride = 0;
    if (host == nullptr || raster->width <= 0 || raster->height <= 0) return 0;
    *host = {nullptr, raster->width, raster->height, 1, -1};
    if (type == rwRASTERTYPECAMERA || type == rwRASTERTYPEZBUFFER || type == rwRASTERTYPECAMERATEXTURE ||
        (flags & rwRASTERDONTALLOCATE) != 0) {
        return 1;   // the renderer's swap chain / depth buffers stand for these
    }
    const RwInt32 pixel = format & rwRASTERFORMATPIXELFORMATMASK;
    if (pixel != rwRASTERFORMATDEFAULT && pixel != rwRASTERFORMAT8888 && pixel != rwRASTERFORMAT888) return 0;
    if (raster->depth != 0 && raster->depth != 24 && raster->depth != 32) return 0;
    if (pixel == rwRASTERFORMATDEFAULT) raster->cFormat = static_cast<RwUInt8>((format | rwRASTERFORMAT8888) >> 8);
    if (raster->depth == 0) raster->depth = 32;
    if ((format & rwRASTERFORMATMIPMAP) != 0) {
        RwInt32 largest = raster->width > raster->height ? raster->width : raster->height;
        host->levels = 1;
        while (largest > 1) largest >>= 1, ++host->levels;
    }
    std::size_t bytes = 0;
    for (RwInt32 level = 0; level < host->levels; ++level) bytes += rwHostRasterLevelSize(host, level);
    host->pixels = static_cast<RwUInt8*>(RwMalloc(bytes, kRasterHint));
    if (host->pixels == nullptr) return 0;
    std::memset(host->pixels, 0, bytes);
    return 1;
}

RwBool hostRasterDestroy(void*, void* object, RwInt32) {
    rwHostRaster* host = rwHostRasterOf(static_cast<RwRaster*>(object));
    if (host != nullptr && host->pixels != nullptr) {
        RwFree(host->pixels);
        host->pixels = nullptr;
    }
    return 1;
}

RwBool hostRasterLock(void* pixelsOut, void* object, RwInt32 flags) {
    auto* raster = static_cast<RwRaster*>(object);
    rwHostRaster* host = rwHostRasterOf(raster);
    const RwInt32 level = (flags >> 8) & 0xFF;
    if (host == nullptr || host->pixels == nullptr || level >= host->levels || host->lockedLevel >= 0) return 0;
    RwUInt8* at = host->pixels;
    for (RwInt32 l = 0; l < level; ++l) at += rwHostRasterLevelSize(host, l);
    raster->originalWidth = raster->width;
    raster->originalHeight = raster->height;
    raster->originalStride = raster->stride;
    raster->originalPixels = raster->cpPixels;
    raster->width = levelDimension(host->width, level);
    raster->height = levelDimension(host->height, level);
    raster->stride = raster->width * 4;
    raster->cpPixels = at;
    host->lockedLevel = level;
    *static_cast<RwUInt8**>(pixelsOut) = at;
    return 1;
}

RwBool hostRasterUnlock(void*, void* object, RwInt32) {
    auto* raster = static_cast<RwRaster*>(object);
    rwHostRaster* host = rwHostRasterOf(raster);
    if (host == nullptr || host->lockedLevel < 0) return 1;
    raster->width = raster->originalWidth;
    raster->height = raster->originalHeight;
    raster->stride = raster->originalStride;
    raster->cpPixels = raster->originalPixels;
    host->lockedLevel = -1;
    return 1;
}

RwBool hostRasterGetMipLevels(void* levelsOut, void* object, RwInt32) {
    rwHostRaster* host = rwHostRasterOf(static_cast<RwRaster*>(object));
    if (host == nullptr) return 0;
    *static_cast<RwInt32*>(levelsOut) = host->levels;
    return 1;
}

RwBool hostTextureSetRaster(void* texture, void* raster, RwInt32) {
    *static_cast<RwRaster**>(texture) = static_cast<RwRaster*>(raster);   // RwTexture::raster, its first field
    return 1;
}

}  // namespace

RwPluginRegistry& rwRasterRegistry() { return rasterTKList; }

void* _rwRasterOpen(void* instance, RwInt32 offset, RwInt32 /*size*/) {
    rasterModule.globalsOffset = offset;
    rasterFreeList() = RwFreeListCreateAndPreallocateSpace(rasterTKList.sizeOfStruct, kRastersPerBlock, alignof(RwRaster),
                                                           kPreallocBlocks, &rasterFreeListSpace,
                                                           rwMEMHINTDUR_GLOBAL | rwID_RASTERMODULE);
    if (rasterFreeList() == nullptr) return nullptr;
    ++rasterModule.numInstances;
    return instance;
}

void* _rwRasterClose(void* instance, RwInt32 /*offset*/, RwInt32 /*size*/) {
    if (rasterFreeList() != nullptr) {
        RwFreeListDestroy(rasterFreeList());
        rasterFreeList() = nullptr;
    }
    --rasterModule.numInstances;
    return instance;
}

RwRaster* RwRasterCreate(RwInt32 width, RwInt32 height, RwInt32 depth, RwInt32 flags) {
    if (rasterModule.numInstances <= 0) {   // native: no raster module open
        RwError error{rwID_COREPLUGIN, _rwerror(static_cast<RwInt32>(0x80000001u))};   // E_RW_BADENGINESTATE
        RwErrorSet(&error);
        return nullptr;
    }
    auto* raster = static_cast<RwRaster*>(rwSrcGlobals().memoryAlloc(rasterFreeList(), kRasterHint));
    if (raster == nullptr) return nullptr;
    std::memset(raster, 0, sizeof *raster);   // native: the original leaves the format and original fields to the device
    raster->privateFlags = 0;
    raster->cFlags = 0;
    raster->width = width;
    raster->height = height;
    raster->nOffsetX = 0;
    raster->nOffsetY = 0;
    raster->depth = depth;
    raster->parent = raster;
    raster->cpPixels = nullptr;
    raster->palette = nullptr;
    if (g_hostRasterOffset >= 0) {   // the host slot starts empty before the device sees the raster
        hostRasterConstruct(raster, g_hostRasterOffset, sizeof(rwHostRaster));
    }
    if (!rwSrcGlobals().stdFunc[rwSTANDARDRASTERCREATE](nullptr, raster, flags)) {
        rwSrcGlobals().memoryFree(rasterFreeList(), raster);
        return nullptr;
    }
    if (_rwPluginRegistryInitObject(&rasterTKList, raster) == nullptr) {
        // native: a failed plugin constructor fails the creation (the original returns the half-built raster)
        rwSrcGlobals().stdFunc[rwSTANDARDRASTERDESTROY](nullptr, raster, 0);
        rwSrcGlobals().memoryFree(rasterFreeList(), raster);
        return nullptr;
    }
    return raster;
}

RwBool RwRasterDestroy(RwRaster* raster) {
    _rwPluginRegistryDeInitObject(&rasterTKList, raster);
    rwSrcGlobals().stdFunc[rwSTANDARDRASTERDESTROY](nullptr, raster, 0);
    rwSrcGlobals().memoryFree(rasterFreeList(), raster);
    return 1;
}

RwUInt8* RwRasterLock(RwRaster* raster, RwUInt8 level, RwInt32 lockMode) {
    RwUInt8* pixels = nullptr;
    const RwInt32 flags = lockMode + (static_cast<RwInt32>(level) << 8);
    return rwSrcGlobals().stdFunc[rwSTANDARDRASTERLOCK](&pixels, raster, flags) ? pixels : nullptr;
}

RwRaster* RwRasterUnlock(RwRaster* raster) {
    rwSrcGlobals().stdFunc[rwSTANDARDRASTERUNLOCK](nullptr, raster, 0);
    return raster;
}

RwInt32 RwRasterGetNumLevels(RwRaster* raster) {
    if ((raster->cFormat & (rwRASTERFORMATMIPMAP >> 8)) == 0) return 1;
    RwInt32 levels = 0;
    return rwSrcGlobals().stdFunc[rwSTANDARDRASTERGETMIPLEVELS](&levels, raster, 0) ? levels : -1;
}

RwInt32 RwRasterRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                               RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB) {
    return _rwPluginRegistryAddPlugin(&rasterTKList, size, pluginID, constructCB, destructCB, copyCB);
}

RwBool rwHostRasterAttach(RwStandardFunc* standards) {
    if (g_hostRasterOffset < 0 || _rwPluginRegistryGetPluginOffset(&rasterTKList, kHostRasterPluginID) < 0) {
        // no constructor: RwRasterCreate empties the slot before the device's create fills it (the plugin
        // constructors run after the device, and must not drop what it allocated)
        g_hostRasterOffset = RwRasterRegisterPlugin(sizeof(rwHostRaster), kHostRasterPluginID, nullptr, nullptr, nullptr);
    }
    if (g_hostRasterOffset < 0) return 0;
    standards[rwSTANDARDRASTERCREATE] = hostRasterCreate;
    standards[rwSTANDARDRASTERDESTROY] = hostRasterDestroy;
    standards[rwSTANDARDTEXTURESETRASTER] = hostTextureSetRaster;
    standards[rwSTANDARDRASTERLOCK] = hostRasterLock;
    standards[rwSTANDARDRASTERUNLOCK] = hostRasterUnlock;
    standards[rwSTANDARDRASTERGETMIPLEVELS] = hostRasterGetMipLevels;
    standards[26] = rwHostNativeTextureRead;   // rwSTANDARDNATIVETEXTUREREAD: the converter's 'HOST' textures
    return 1;
}

rwHostRaster* rwHostRasterOf(RwRaster* raster) {
    if (g_hostRasterOffset < 0) return nullptr;
    return reinterpret_cast<rwHostRaster*>(reinterpret_cast<char*>(raster) + g_hostRasterOffset);
}

std::size_t rwHostRasterLevelSize(const rwHostRaster* host, RwInt32 level) {
    return static_cast<std::size_t>(levelDimension(host->width, level)) *
           static_cast<std::size_t>(levelDimension(host->height, level)) * 4u;
}
