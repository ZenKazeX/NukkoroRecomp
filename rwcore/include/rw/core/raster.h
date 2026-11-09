// RenderWare Graphics 3.7 core: rasters (module baraster, CPU side), native x64.
//
// A raster is an image the device owns: a texture's pixels, a camera's frame or Z buffer. RenderWare's raster module
// is a thin layer over the device's standard functions (create, destroy, lock, unlock, mip levels); the GameCube's
// driver keeps its rasters in its own formats. The port's host device keeps a raster's pixels in CPU memory, every mip
// level, in the RenderWare format the raster was created with (rwRASTERFORMAT8888 and 888: 4 bytes a pixel, R G B A in
// memory - the renderer's R8G8B8A8 textures; the offline converter writes them so) - the renderer uploads them. Camera and Z rasters have
// no pixels: the renderer's swap chain and depth buffers stand for them.
//
// Written from PAL main.dol baraster 0x8048AB90..0x8048B1C4: RwRasterCreate / Destroy / Lock / Unlock / GetNumLevels /
// RegisterPlugin and the module's open / close, which the game links. Not linked, so not provided: the raster context
// stack (RwRasterPushContext / PopContext / GetCurrentContext, whose state the original's open initialises), sub
// rasters, RwRasterRender*, RwRasterClear*, palettes. Proof: runtime/tests/rwcore_texture_tests.cpp (oracle) and
// rwcore_texture_native_tests.cpp.
#pragma once

#include "rw/core/engine.h"
#include "rw/core/plugin.h"
#include "rw/core/types.h"

#include <cstddef>

/// RwRasterType (rwcore.h): plain RwInt32 values, as RenderWare ORs types, formats and flags together.
using RwRasterType = RwInt32;
inline constexpr RwInt32 rwRASTERTYPENORMAL = 0x00;
inline constexpr RwInt32 rwRASTERTYPEZBUFFER = 0x01;
inline constexpr RwInt32 rwRASTERTYPECAMERA = 0x02;
inline constexpr RwInt32 rwRASTERTYPETEXTURE = 0x04;
inline constexpr RwInt32 rwRASTERTYPECAMERATEXTURE = 0x05;
inline constexpr RwInt32 rwRASTERTYPEMASK = 0x07;
inline constexpr RwInt32 rwRASTERDONTALLOCATE = 0x80;

/// RwRasterFormat (rwcore.h): plain RwInt32 values, as RenderWare ORs types, formats and flags together.
using RwRasterFormat = RwInt32;
inline constexpr RwInt32 rwRASTERFORMATDEFAULT = 0x0000;
inline constexpr RwInt32 rwRASTERFORMAT1555 = 0x0100;
inline constexpr RwInt32 rwRASTERFORMAT565 = 0x0200;
inline constexpr RwInt32 rwRASTERFORMAT4444 = 0x0300;
inline constexpr RwInt32 rwRASTERFORMATLUM8 = 0x0400;
inline constexpr RwInt32 rwRASTERFORMAT8888 = 0x0500;
inline constexpr RwInt32 rwRASTERFORMAT888 = 0x0600;
inline constexpr RwInt32 rwRASTERFORMAT16 = 0x0700;
inline constexpr RwInt32 rwRASTERFORMAT24 = 0x0800;
inline constexpr RwInt32 rwRASTERFORMAT32 = 0x0900;
inline constexpr RwInt32 rwRASTERFORMAT555 = 0x0A00;
inline constexpr RwInt32 rwRASTERFORMATAUTOMIPMAP = 0x1000;
inline constexpr RwInt32 rwRASTERFORMATPAL8 = 0x2000;
inline constexpr RwInt32 rwRASTERFORMATPAL4 = 0x4000;
inline constexpr RwInt32 rwRASTERFORMATMIPMAP = 0x8000;
inline constexpr RwInt32 rwRASTERFORMATPIXELFORMATMASK = 0x0F00;
inline constexpr RwInt32 rwRASTERFORMATMASK = 0xFF00;

/// RwRasterLockMode (rwcore.h): plain RwInt32 values, as RenderWare ORs types, formats and flags together.
using RwRasterLockMode = RwInt32;
inline constexpr RwInt32 rwRASTERLOCKWRITE = 0x01;
inline constexpr RwInt32 rwRASTERLOCKREAD = 0x02;
inline constexpr RwInt32 rwRASTERLOCKNOFETCH = 0x04;
inline constexpr RwInt32 rwRASTERLOCKRAW = 0x08;

/// A raster (rwcore.h RwRaster). Native pointers and sizes; the fields keep RenderWare's meaning.
struct RwRaster {
    RwRaster* parent;          ///< itself unless a sub raster
    RwUInt8* cpPixels;         ///< the locked level's pixels (NULL unless locked)
    RwUInt8* palette;
    RwInt32 width;             ///< the locked level's, else level 0's
    RwInt32 height;
    RwInt32 depth;             ///< bits per pixel
    RwInt32 stride;            ///< bytes per row of the locked level
    RwInt16 nOffsetX;
    RwInt16 nOffsetY;
    RwUInt8 cType;             ///< RwRasterType
    RwUInt8 cFlags;
    RwUInt8 privateFlags;
    RwUInt8 cFormat;           ///< RwRasterFormat >> 8
    RwUInt8* originalPixels;
    RwInt32 originalWidth;     ///< level 0's while a level is locked
    RwInt32 originalHeight;
    RwInt32 originalStride;
};

/// The raster module's globals: the free list of rasters.
struct rwRasterGlobals {
    RwFreeList* rasters;
};

extern RwModuleInfo rasterModule;

/// The rasters' plugin registry (rasterTKList, 0x8056F0D8).
RwPluginRegistry& rwRasterRegistry();

/// Opens the module: the raster free list (128 to a block, one preallocated, a static header, hint
/// rwMEMHINTDUR_GLOBAL | rwID_RASTERMODULE). NULL when it cannot be made. Original: 0x8048B07C.
void* _rwRasterOpen(void* instance, RwInt32 offset, RwInt32 size);

/// Destroys the raster free list. Returns instance. Original: 0x8048B010.
void* _rwRasterClose(void* instance, RwInt32 offset, RwInt32 size);

/// A raster of width x height x depth, flags = RwRasterType | RwRasterFormat: parent itself, no pixels locked, the
/// device's raster create, then the plugins' constructors. NULL when the allocation or the device fails (native:
/// also when a plugin constructor fails, or no raster module is open). Original: 0x8048AEC4.
RwRaster* RwRasterCreate(RwInt32 width, RwInt32 height, RwInt32 depth, RwInt32 flags);

/// The plugins' destructors, the device's raster destroy, the raster freed. TRUE. Original: 0x8048AC2C.
RwBool RwRasterDestroy(RwRaster* raster);

/// Locks mip level `level` for lockMode (RwRasterLockMode): the device sets cpPixels, width, height and stride to the
/// level's. The pixels or NULL. Original: 0x8048AFC0.
RwUInt8* RwRasterLock(RwRaster* raster, RwUInt8 level, RwInt32 lockMode);

/// Unlocks the raster through the device. Returns raster. Original: 0x8048AB90.
RwRaster* RwRasterUnlock(RwRaster* raster);

/// 1 without rwRASTERFORMATMIPMAP, else the device's level count (-1 when it fails). Original: 0x8048AD34.
RwInt32 RwRasterGetNumLevels(RwRaster* raster);

/// A raster plugin (before the engine opens). Its offset or -1. Original: 0x8048ACA4.
RwInt32 RwRasterRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                               RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);

/// The raster's offset in its parent. Returns raster. Original: 0x8048AB7C.
inline RwRaster* RwRasterGetOffset(RwRaster* raster, RwInt16* xOffset, RwInt16* yOffset) {
    *xOffset = raster->nOffsetX;
    *yOffset = raster->nOffsetY;
    return raster;
}

inline RwInt32 RwRasterGetWidth(const RwRaster* r) { return r->width; }
inline RwInt32 RwRasterGetHeight(const RwRaster* r) { return r->height; }
inline RwInt32 RwRasterGetStride(const RwRaster* r) { return r->stride; }
inline RwInt32 RwRasterGetDepth(const RwRaster* r) { return r->depth; }
inline RwInt32 RwRasterGetType(const RwRaster* r) { return r->cType & rwRASTERTYPEMASK; }
inline RwInt32 RwRasterGetFormat(const RwRaster* r) { return (static_cast<RwInt32>(r->cFormat) << 8) & rwRASTERFORMATMASK; }

// ---- the host device's rasters (native; rwHostDeviceStandards installs them) --------------------------------------

/// The host device's per-raster storage, a raster plugin slot: every level's pixels, contiguous, level 0 first.
struct rwHostRaster {
    RwUInt8* pixels;           ///< all levels; NULL for camera / Z rasters
    RwInt32 width;             ///< level 0's
    RwInt32 height;
    RwInt32 levels;
    RwInt32 lockedLevel;       ///< -1 when unlocked
};

/// The host device's raster functions, installed in the given standard-function table: raster create / destroy,
/// lock / unlock, mip levels, texture set raster. Registers the host raster plugin (call between RwEngineInit and
/// RwEngineOpen; RwEngineInit does). FALSE when the plugin cannot be registered.
RwBool rwHostRasterAttach(RwStandardFunc* standards);

/// The host storage of a raster (its plugin slot), or NULL before rwHostRasterAttach.
rwHostRaster* rwHostRasterOf(RwRaster* raster);

/// Bytes of mip level `level` of host storage (4 bytes a pixel, max(width >> level, 1) x max(height >> level, 1)).
std::size_t rwHostRasterLevelSize(const rwHostRaster* host, RwInt32 level);
