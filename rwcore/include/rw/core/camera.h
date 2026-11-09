// RenderWare Graphics 3.7 core: cameras (modules bacamera, babincam), native x64.
//
// A camera is an object with a frame: its frame's LTM places it; its view window (half-extents at distance 1),
// offset, near / far planes and projection define the frustum. Whenever its frame's hierarchy syncs, the camera
// rebuilds its view matrix, the eight frustum corners in world space, the six clip planes (far, near, then the four
// sides) and their bounding box. RwCameraBeginUpdate / EndUpdate bracket rendering through the device's standard
// functions; the mathematics knows nothing of the device and assumes no aspect ratio (the view window's x and y are
// independent - 16:9, 21:9, 32:10 are just other windows).
//
// Written from PAL main.dol bacamera 0x80485318..0x80486C1C and babincam 0x804845A4 / 0x804845E0, every arithmetic
// operation in the original's order (the perspective and parallel builders, the Z scale / shift). Proof:
// runtime/tests/rwcore_camera_tests.cpp (oracle) and rwcore_camera_native_tests.cpp. Deliberate differences:
// rwcore/README.md "Native contracts" -> Cameras.
// Not linked by the game, so not provided: RwCameraClone, RwCameraGetPluginOffset, RwCameraSetFreeListCreateParams,
// RwCameraStreamWrite / GetSize / ChunkInfoRead. The camera validation module is compiled out of the game (the empty
// stubs 0x8049C190 / 0x8049C198, by their call sites _rwCameraValOpen / _rwCameraValRender): nothing here.
#pragma once

#include "rw/core/bbox.h"
#include "rw/core/engine.h"
#include "rw/core/frame.h"
#include "rw/core/matrix.h"
#include "rw/core/plugin.h"
#include "rw/core/stream.h"
#include "rw/core/types.h"

struct RwRaster;

inline constexpr RwUInt8 rwCAMERA = 4;   ///< RwObject::type of a camera

enum RwCameraProjection : RwInt32 {
    rwNACAMERAPROJECTION = 0,
    rwPERSPECTIVE = 1,
    rwPARALLEL = 2,
};

enum RwFrustumTestResult : RwInt32 {
    rwSPHEREOUTSIDE = 0,
    rwSPHEREBOUNDARY = 1,
    rwSPHEREINSIDE = 2,
};

enum RwCameraClearMode : RwInt32 {
    rwCAMERACLEARIMAGE = 0x1,
    rwCAMERACLEARZ = 0x2,
    rwCAMERACLEARSTENCIL = 0x4,
};

/// A clip plane with the box corner closest to it per axis (1: the maximum, 0: the minimum; rwcore.h).
struct RwFrustumPlane {
    RwPlane plane;
    RwUInt8 closestX;
    RwUInt8 closestY;
    RwUInt8 closestZ;
    RwUInt8 pad;
};

struct RwCamera;
using RwCameraBeginUpdateFunc = RwCamera* (*)(RwCamera* camera);
using RwCameraEndUpdateFunc = RwCamera* (*)(RwCamera* camera);

/// A camera (rwcore.h RwCamera). 16-byte aligned (the view matrix with it).
struct alignas(16) RwCamera {
    RwObjectHasFrame object;
    RwCameraProjection projectionType;
    RwCameraBeginUpdateFunc beginUpdate;
    RwCameraEndUpdateFunc endUpdate;
    RwMatrix viewMatrix;       ///< world -> camera, x and y mapped so the view window spans [0, 1] (rows: columns)
    RwRaster* frameBuffer;
    RwRaster* zBuffer;
    RwV2d viewWindow;
    RwV2d recipViewWindow;
    RwV2d viewOffset;
    RwReal nearPlane;
    RwReal farPlane;
    RwReal fogPlane;
    RwReal zScale;             ///< z (parallel) or 1 / z (perspective) -> the device's Z range
    RwReal zShift;
    RwFrustumPlane frustumPlanes[6];   ///< far, near, then the four sides; normals point out of the frustum
    RwBBox frustumBoundBox;
    RwV3d frustumCorners[8];   ///< near 0..3, far 4..7, world space
};

/// The camera module's globals: the free list of cameras.
struct rwCameraGlobals {
    RwFreeList* cameras;
};

extern RwModuleInfo cameraModule;

/// The cameras' plugin registry (cameraTKList).
RwPluginRegistry& rwCameraRegistry();

/// Opens the module: the camera free list (entries of the registry's size, 4 to a block, one preallocated, a static
/// header, hint rwMEMHINTDUR_GLOBAL | rwID_CAMERA). NULL when the free list cannot be made. Original: 0x80486474.
void* _rwCameraOpen(void* instance, RwInt32 offset, RwInt32 size);

/// Destroys the camera free list. Returns instance. Original: 0x80486410.
void* _rwCameraClose(void* instance, RwInt32 offset, RwInt32 size);

/// A perspective camera: view window 1 x 1, offset 0, near 0.05, far 10, fog 5, no rasters, no frame, the plugins'
/// constructors run. NULL when the allocation or (native) a plugin constructor fails, or no camera module is open.
/// Original: 0x80486AA8.
RwCamera* RwCameraCreate();

/// Runs the plugin destructors, detaches the camera from its frame and frees it (native: also no longer the current
/// camera). TRUE. Original: 0x80486A48.
RwBool RwCameraDestroy(RwCamera* camera);

/// The camera's frame (RenderWare's RwCameraSetFrame / RwCameraGetFrame, macros over the object's frame).
RwCamera* RwCameraSetFrame(RwCamera* camera, RwFrame* frame);
RwFrame* RwCameraGetFrame(const RwCamera* camera);

/// Starts rendering: the camera becomes RwGlobals::curCamera, every dirty frame hierarchy is synced (so this camera's
/// planes are current), and the device's camera begin-update runs. NULL when the device refuses (native: curCamera
/// is then restored). Originals: 0x8048652C -> CameraBeginUpdate 0x804863A4.
RwCamera* RwCameraBeginUpdate(RwCamera* camera);

/// Ends rendering through the device; curCamera becomes NULL. NULL when the device refuses. Originals: 0x80486504 ->
/// CameraEndUpdate 0x80486340.
RwCamera* RwCameraEndUpdate(RwCamera* camera);

/// The view window (and its reciprocal); the frame is marked for an update. Returns camera. Original: 0x804869A0.
RwCamera* RwCameraSetViewWindow(RwCamera* camera, const RwV2d* viewWindow);

/// The view offset; the frame is marked for an update. Returns camera. Original: 0x80486554.
RwCamera* RwCameraSetViewOffset(RwCamera* camera, const RwV2d* offset);

/// The near / far plane; the Z scale and shift follow (the original's CameraUpdateZShiftScale, inlined there); the
/// frame is marked for an update. Return camera. Originals: 0x804865A0, 0x8048665C.
RwCamera* RwCameraSetNearClipPlane(RwCamera* camera, RwReal nearClip);
RwCamera* RwCameraSetFarClipPlane(RwCamera* camera, RwReal farClip);

/// rwPERSPECTIVE or rwPARALLEL (the frame marked, Z scale / shift updated); another value: NULL with E_RW_BADPARAM
/// "Invalid projection type". Original: 0x8048689C.
RwCamera* RwCameraSetProjection(RwCamera* camera, RwCameraProjection projection);

/// Where a sphere is against the six clip planes (plane by plane: outside as soon as one has it beyond its radius,
/// boundary when one cuts it). Original: 0x80486718.
RwFrustumTestResult RwCameraFrustumTestSphere(const RwCamera* camera, const RwSphere* sphere);

/// Clears the camera's rasters through the device (clearMode: rwCAMERACLEAR*). camera, or NULL when the device
/// refuses. Original: 0x8048680C.
RwCamera* RwCameraClear(RwCamera* camera, void* colour, RwInt32 clearMode);

/// Ends the frame: the resources arena purged (_rwResourcesPurge), then the camera's frame buffer shown through the
/// device. camera, or NULL when the device refuses or the camera has no frame buffer (RwRasterShowRaster returns the
/// raster). Original: 0x80486858 (-> RwRasterShowRaster 0x8048AD94).
RwCamera* RwCameraShowRaster(RwCamera* camera, void* dev, RwUInt32 flags);

/// A camera plugin (before the camera module opens). Its offset or -1. Original: 0x80486A08.
RwInt32 RwCameraRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                               RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);

/// The stream callbacks of a camera plugin. Its offset or -1. Original: 0x804845A4.
RwInt32 RwCameraRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB,
                                     RwPluginDataChunkWriteCallBack writeCB, RwPluginDataChunkGetSizeCallBack getSizeCB);

/// A camera from a stream: its struct chunk (view window, offset, near, far, fog, projection; versions 3.5.0.0 ..
/// 3.7.0.2, else E_RW_BADVERSION) and plugin data, applied through the setters. NULL on a read failure (native: the
/// camera is destroyed; the original leaks it; a struct chunk longer than 32 bytes is read into 32 and skipped - the
/// original reads it all into a 32-byte stack buffer). As in the original, an invalid projection still returns the
/// camera (the error is set). Original: 0x804845E0.
RwCamera* RwCameraStreamRead(RwStream* stream);
