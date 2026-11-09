// RenderWare cameras (modules bacamera, babincam, babbox; see rw/core/camera.h). Every floating-point step is the
// original's: fmadds / fmsubs / fnmsubs round once (std::fmaf), fmuls / fadds / fsubs / fdivs are single operations,
// each written as its own statement or expression where the original rounds in between.
#include "rw/core/camera.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "rw/core/binary.h"
#include "rw/core/error.h"
#include "rw/core/memory.h"
#include "rw/core/resources.h"
#include "rw/core/vector.h"

RwModuleInfo cameraModule{};

namespace {

// The free list's creation parameters (bacamera statics 0x805EEF88 / 0x805EEF8C; RwCameraSetFreeListCreateParams is
// not linked) and its in-place header (0x805E3BE0).
constexpr RwInt32 kCamerasPerBlock = 4;
constexpr RwInt32 kPreallocBlocks = 1;
constexpr RwUInt32 kCameraHint = 0x00030000u | rwID_CAMERA;   // rwMEMHINTDUR_EVENT, as the original passes it
RwFreeList cameraFreeListSpace{};
RwPluginRegistry cameraTKList = rwPLUGINREGISTRYINIT(sizeof(RwCamera));   // 0x8056F030

// The constants, by their bits (r2 + 0x1BD8 .. 0x1BF8)
const RwReal kOne = std::bit_cast<RwReal>(0x3F800000u);
const RwReal kZEpsilon = std::bit_cast<RwReal>(0x38D1B717u);   // 1e-4 of the device's Z range kept off each end
const RwReal kHalf = std::bit_cast<RwReal>(0x3F000000u);
const RwReal kTwo = std::bit_cast<RwReal>(0x40000000u);
const RwReal kMinusHalf = std::bit_cast<RwReal>(0xBF000000u);
const RwReal kZero = std::bit_cast<RwReal>(0x00000000u);
const RwReal kDefaultNear = std::bit_cast<RwReal>(0x3D4CCCCDu);   // 0.05
const RwReal kDefaultFar = std::bit_cast<RwReal>(0x41200000u);    // 10
const RwReal kDefaultFog = std::bit_cast<RwReal>(0x40A00000u);    // 5

constexpr RwInt32 rwSTANDARDCAMERABEGINUPDATE = 1, rwSTANDARDCAMERAENDUPDATE = 10, rwSTANDARDRASTERSHOWRASTER = 20,
                  rwSTANDARDCAMERACLEAR = 21;

RwFreeList*& cameraFreeList() { return rwModuleGlobals<rwCameraGlobals>(cameraModule).cameras; }

inline RwReal fmadds(RwReal a, RwReal c, RwReal b) { return std::fmaf(a, c, b); }       // a * c + b
inline RwReal fmsubs(RwReal a, RwReal c, RwReal b) { return std::fmaf(a, c, -b); }      // a * c - b
inline RwReal fnmadds(RwReal a, RwReal c, RwReal b) { return -std::fmaf(a, c, b); }     // -(a * c + b)
inline RwReal fnmsubs(RwReal a, RwReal c, RwReal b) { return -std::fmaf(a, c, -b); }    // -(a * c - b)

RwFrame* frameOf(const RwCamera* camera) { return static_cast<RwFrame*>(camera->object.object.parent); }

void markFrame(RwCamera* camera) {
    if (RwFrame* frame = frameOf(camera)) RwFrameUpdateObjects(frame);
}

/// CameraUpdateZShiftScale (inlined at its callers in the original): z (parallel) or 1 / z (otherwise) mapped onto
/// the device's Z range, less 1e-4 of it at each end.
void updateZShiftScale(RwCamera* camera) {
    const RwDevice& device = rwSrcGlobals().dOpenDevice;
    const RwReal zNear = device.zBufferNear, zFar = device.zBufferFar;
    RwReal a, b;
    if (camera->projectionType == rwPARALLEL) {
        a = camera->farPlane;
        b = camera->nearPlane;
    } else {
        a = kOne / camera->farPlane;
        b = kOne / camera->nearPlane;
    }
    const RwReal range = zFar - zNear;
    const RwReal diff = a - b;
    const RwReal sum = a + b;
    const RwReal margin = kZEpsilon * range;
    const RwReal top = zFar - margin;
    const RwReal bottom = zNear + margin;
    const RwReal span = top - bottom;
    const RwReal total = top + bottom;
    camera->zScale = span / diff;
    camera->zShift = kHalf * fnmsubs(camera->zScale, sum, total);   // (total - zScale * sum) / 2, fused
}

/// dot(p, n) as the clip-plane builders form it: z * n.z + (x * n.x + y * n.y), fused.
RwReal planeDot(const RwV3d& p, const RwV3d& n) { return fmadds(p.z, n.z, fmadds(p.x, n.x, p.y * n.y)); }

/// p x q as the builders form it (fmsubs over a plain product).
RwV3d cross(const RwV3d& p, const RwV3d& q) {
    return {fmsubs(p.y, q.z, p.z * q.y), fmsubs(p.z, q.x, p.x * q.z), fmsubs(p.x, q.y, p.y * q.x)};
}

RwV3d minus(const RwV3d& a, const RwV3d& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

/// The closest-corner byte of a normal component: 0 when its sign bit is set (-0 included), else 1.
RwUInt8 closest(RwReal v) { return (std::bit_cast<RwUInt32>(v) >> 31) ? 0 : 1; }

void setPlane(RwFrustumPlane& p, const RwV3d& normal, const RwV3d& through) {
    p.plane.normal = normal;
    p.plane.distance = planeDot(through, normal);
    p.closestX = closest(normal.x);
    p.closestY = closest(normal.y);
    p.closestZ = closest(normal.z);
}

/// A side plane: n normalized with _rwInvSqrt of z^2 + (x^2 + y^2), through a corner.
void setSidePlane(RwFrustumPlane& p, RwV3d n, const RwV3d& through) {
    const RwReal xx = n.x * n.x;
    const RwReal yy = n.y * n.y;
    const RwReal zz = n.z * n.z;
    const RwReal xy = xx + yy;
    const RwReal inv = _rwInvSqrt(zz + xy);
    n = {n.x * inv, n.y * inv, n.z * inv};
    setPlane(p, n, through);
}

/// The far and near planes (normals at / -at) and the four sides from the corners, as both builders end.
void buildPlanes(RwCamera* camera, const RwV3d& at, bool parallel) {
    const RwV3d* c = camera->frustumCorners;
    RwFrustumPlane* planes = camera->frustumPlanes;
    setPlane(planes[0], at, c[4]);
    setPlane(planes[1], {-at.x, -at.y, -at.z}, c[0]);
    const RwV3d a = minus(c[1], c[5]);
    setSidePlane(planes[2], cross(a, minus(c[6], c[5])), c[1]);
    setSidePlane(planes[3], cross(minus(c[4], c[5]), a), c[1]);
    if (parallel) {   // the opposite sides of a box are parallel: the original negates planes 2 and 3
        const RwV3d n2 = planes[2].plane.normal, n3 = planes[3].plane.normal;
        setPlane(planes[4], {-n2.x, -n2.y, -n2.z}, c[3]);
        setPlane(planes[5], {-n3.x, -n3.y, -n3.z}, c[3]);
    } else {
        const RwV3d b = minus(c[3], c[7]);
        setSidePlane(planes[4], cross(b, minus(c[4], c[7])), c[3]);
        setSidePlane(planes[5], cross(minus(c[6], c[7]), b), c[3]);
    }
}

/// The view matrix of a perspective camera (0x80485AFC): x and y scaled by -1/2 of the reciprocal view window and
/// shifted so the window spans [0, 1] at distance 1, z along the frame's at.
void buildPerspViewMatrix(RwCamera* camera) {
    const RwMatrix& ltm = frameOf(camera)->ltm;
    RwMatrix& v = camera->viewMatrix;
    // x
    const RwReal sx = kMinusHalf * camera->recipViewWindow.x;
    const RwReal ox = fnmsubs(sx, camera->viewOffset.x, kHalf);   // 0.5 - sx * offset.x, fused
    v.right.x = fmadds(ltm.at.x, ox, ltm.right.x * sx);
    v.up.x = fmadds(ltm.at.y, ox, ltm.right.y * sx);
    v.at.x = fmadds(ltm.at.z, ox, ltm.right.z * sx);
    v.pos.x = kHalf - (ox + fmadds(ltm.pos.z, v.at.x, fmadds(ltm.pos.x, v.right.x, ltm.pos.y * v.up.x)));
    // y
    const RwReal sy = kMinusHalf * camera->recipViewWindow.y;
    const RwReal oy = fmadds(sy, camera->viewOffset.y, kHalf);    // sy * offset.y + 0.5, fused
    v.right.y = fmadds(ltm.at.x, oy, ltm.up.x * sy);
    v.up.y = fmadds(ltm.at.y, oy, ltm.up.y * sy);
    v.at.y = fmadds(ltm.at.z, oy, ltm.up.z * sy);
    v.pos.y = kHalf - (oy + fmadds(ltm.pos.z, v.at.y, fmadds(ltm.pos.x, v.right.y, ltm.pos.y * v.up.y)));
    // z
    v.right.z = ltm.at.x;
    v.up.z = ltm.at.y;
    v.at.z = ltm.at.z;
    v.pos.z = fnmadds(ltm.pos.z, ltm.at.z, fmadds(ltm.pos.x, ltm.at.x, ltm.pos.y * ltm.at.y));
    RwMatrixOptimize(&v, nullptr);
}

/// The view matrix of a parallel camera (0x80486174): as the perspective one with the offset shifts unscaled by
/// distance (and, as in the original, negated in x only).
void buildParallelViewMatrix(RwCamera* camera) {
    const RwMatrix& ltm = frameOf(camera)->ltm;
    RwMatrix& v = camera->viewMatrix;
    // x
    const RwReal sx = kMinusHalf * camera->recipViewWindow.x;
    const RwReal ox = -(sx * camera->viewOffset.x);
    v.right.x = fmadds(ltm.at.x, ox, ltm.right.x * sx);
    v.up.x = fmadds(ltm.at.y, ox, ltm.right.y * sx);
    v.at.x = fmadds(ltm.at.z, ox, ltm.right.z * sx);
    v.pos.x = kHalf - (ox + fmadds(ltm.pos.z, v.at.x, fmadds(ltm.pos.x, v.right.x, ltm.pos.y * v.up.x)));
    // y
    const RwReal sy = kMinusHalf * camera->recipViewWindow.y;
    const RwReal oy = sy * camera->viewOffset.y;
    v.right.y = fmadds(ltm.at.x, oy, ltm.up.x * sy);
    v.up.y = fmadds(ltm.at.y, oy, ltm.up.y * sy);
    v.at.y = fmadds(ltm.at.z, oy, ltm.up.z * sy);
    v.pos.y = kHalf - (oy + fmadds(ltm.pos.z, v.at.y, fmadds(ltm.pos.x, v.right.y, ltm.pos.y * v.up.y)));
    // z
    v.right.z = ltm.at.x;
    v.up.z = ltm.at.y;
    v.at.z = ltm.at.z;
    v.pos.z = fnmadds(ltm.pos.z, ltm.at.z, fmadds(ltm.pos.x, ltm.at.x, ltm.pos.y * ltm.at.y));
    RwMatrixOptimize(&v, nullptr);
}

/// CameraBuildPerspClipPlanes (0x80485318): the view window's corners at distance 1 (accumulated as the original
/// adds them), each pushed to the near and far planes from the offset point, then the planes.
void buildPerspClipPlanes(RwCamera* camera) {
    const RwMatrix& ltm = frameOf(camera)->ltm;
    const RwReal wx = camera->viewWindow.x, wy = camera->viewWindow.y;
    const RwV3d r{ltm.right.x * wx, ltm.right.y * wx, ltm.right.z * wx};
    const RwV3d u{ltm.up.x * wy, ltm.up.y * wy, ltm.up.z * wy};
    const RwV3d r2{kTwo * r.x, kTwo * r.y, kTwo * r.z};
    const RwV3d u2{kTwo * u.x, kTwo * u.y, kTwo * u.z};
    const RwV3d centre{ltm.at.x + r.x, ltm.at.y + r.y, ltm.at.z + r.z};
    RwV3d* c = camera->frustumCorners;
    c[0] = {centre.x + u.x, centre.y + u.y, centre.z + u.z};
    c[1] = {c[0].x - r2.x, c[0].y - r2.y, c[0].z - r2.z};
    c[2] = {c[1].x - u2.x, c[1].y - u2.y, c[1].z - u2.z};
    c[3] = {c[2].x + r2.x, c[2].y + r2.y, c[2].z + r2.z};
    const RwReal minusOffsetX = -camera->viewOffset.x, offsetY = camera->viewOffset.y;
    const RwV3d o{fmadds(ltm.up.x, offsetY, ltm.right.x * minusOffsetX), fmadds(ltm.up.y, offsetY, ltm.right.y * minusOffsetX),
                  fmadds(ltm.up.z, offsetY, ltm.right.z * minusOffsetX)};
    for (int i = 0; i < 4; ++i) {
        const RwV3d d = minus(c[i], o);
        const RwV3d base{o.x + ltm.pos.x, o.y + ltm.pos.y, o.z + ltm.pos.z};
        c[i] = {fmadds(d.x, camera->nearPlane, base.x), fmadds(d.y, camera->nearPlane, base.y),
                fmadds(d.z, camera->nearPlane, base.z)};
        c[i + 4] = {fmadds(d.x, camera->farPlane, base.x), fmadds(d.y, camera->farPlane, base.y),
                    fmadds(d.z, camera->farPlane, base.z)};
    }
    buildPlanes(camera, ltm.at, false);
}

/// CameraBuildParallelClipPlanes (0x80485C60): the box's corners in camera space, moved by the frame's LTM.
void buildParallelClipPlanes(RwCamera* camera) {
    const RwMatrix& ltm = frameOf(camera)->ltm;
    const RwReal wx = camera->viewWindow.x, wy = camera->viewWindow.y;
    const RwReal zn = camera->nearPlane, zf = camera->farPlane;
    const RwReal minusOffsetX = -camera->viewOffset.x, offsetY = camera->viewOffset.y;
    const RwReal nearShift = kOne - zn, farShift = kOne - zf;
    const RwReal nx = nearShift * minusOffsetX, ny = nearShift * offsetY;
    const RwReal fx = farShift * minusOffsetX, fy = farShift * offsetY;
    RwV3d* c = camera->frustumCorners;
    c[0] = {wx + nx, wy + ny, zn};
    c[1] = {-wx + nx, wy + ny, zn};
    c[2] = {-wx + nx, -wy + ny, zn};
    c[3] = {wx + nx, -wy + ny, zn};
    c[4] = {wx + fx, wy + fy, zf};
    c[5] = {-wx + fx, wy + fy, zf};
    c[6] = {-wx + fx, -wy + fy, zf};
    c[7] = {wx + fx, -wy + fy, zf};
    RwV3dTransformPoints(c, c, 8, &ltm);
    buildPlanes(camera, ltm.at, true);
}

/// CameraSync (0x804862DC): the frame's hierarchy has synced - rebuild the view matrix, the frustum and its box.
RwObjectHasFrame* CameraSync(RwObjectHasFrame* object) {
    auto* camera = reinterpret_cast<RwCamera*>(object);
    if (camera->projectionType == rwPERSPECTIVE) {
        buildPerspViewMatrix(camera);
        buildPerspClipPlanes(camera);
    } else {
        buildParallelViewMatrix(camera);
        buildParallelClipPlanes(camera);
    }
    RwBBoxCalculate(&camera->frustumBoundBox, camera->frustumCorners, 8);
    return object;
}

RwCamera* CameraBeginUpdate(RwCamera* camera) {
    RwGlobals& g = rwSrcGlobals();
    void* const previous = g.curCamera;
    g.curCamera = camera;
    _rwFrameSyncDirty();
    if (g.stdFunc[rwSTANDARDCAMERABEGINUPDATE](nullptr, camera, 0)) return camera;
    g.curCamera = previous;   // native: the original leaves the refused camera current
    return nullptr;
}

RwCamera* CameraEndUpdate(RwCamera* camera) {
    RwGlobals& g = rwSrcGlobals();
    if (!g.stdFunc[rwSTANDARDCAMERAENDUPDATE](nullptr, camera, 0)) return nullptr;
    g.curCamera = nullptr;
    return camera;
}

}  // namespace

RwBBox* RwBBoxCalculate(RwBBox* boundBox, const RwV3d* verts, RwInt32 numVertices) {
    boundBox->inf = verts[0];
    boundBox->sup = verts[0];
    for (RwInt32 i = 1; i < numVertices; ++i) {
        const RwV3d& v = verts[i];
        if (boundBox->inf.x > v.x) boundBox->inf.x = v.x;
        if (boundBox->inf.y > v.y) boundBox->inf.y = v.y;
        if (boundBox->inf.z > v.z) boundBox->inf.z = v.z;
        if (boundBox->sup.x < v.x) boundBox->sup.x = v.x;
        if (boundBox->sup.y < v.y) boundBox->sup.y = v.y;
        if (boundBox->sup.z < v.z) boundBox->sup.z = v.z;
    }
    return boundBox;
}

RwPluginRegistry& rwCameraRegistry() { return cameraTKList; }

void* _rwCameraOpen(void* instance, RwInt32 offset, RwInt32 /*size*/) {
    cameraModule.globalsOffset = offset;
    cameraFreeList() = RwFreeListCreateAndPreallocateSpace(cameraTKList.sizeOfStruct, kCamerasPerBlock, alignof(RwCamera),
                                                           kPreallocBlocks, &cameraFreeListSpace,
                                                           rwMEMHINTDUR_GLOBAL | rwID_CAMERA);
    if (cameraFreeList() == nullptr) return nullptr;
    ++cameraModule.numInstances;
    return instance;
}

void* _rwCameraClose(void* instance, RwInt32 /*offset*/, RwInt32 /*size*/) {
    if (cameraFreeList() != nullptr) {
        RwFreeListDestroy(cameraFreeList());
        cameraFreeList() = nullptr;
    }
    --cameraModule.numInstances;
    return instance;
}

RwCamera* RwCameraCreate() {
    if (cameraModule.numInstances <= 0) {   // native: no camera module open
        RwError error{rwID_COREPLUGIN, _rwerror(static_cast<RwInt32>(0x80000001u))};   // E_RW_BADENGINESTATE
        RwErrorSet(&error);
        return nullptr;
    }
    auto* camera = static_cast<RwCamera*>(rwSrcGlobals().memoryAlloc(cameraFreeList(), kCameraHint));
    if (camera == nullptr) return nullptr;
    std::memset(camera, 0, sizeof *camera);   // native: the original leaves the matrices and frustum as memory held them
    camera->object.object = {rwCAMERA, 0, 0, 0, nullptr};
    camera->object.sync = CameraSync;
    camera->beginUpdate = CameraBeginUpdate;
    camera->endUpdate = CameraEndUpdate;
    camera->viewWindow = {kOne, kOne};
    camera->recipViewWindow = {kOne, kOne};
    camera->viewOffset = {kZero, kZero};
    camera->nearPlane = kDefaultNear;
    camera->farPlane = kDefaultFar;
    camera->fogPlane = kDefaultFog;
    camera->frameBuffer = nullptr;
    camera->zBuffer = nullptr;
    camera->projectionType = rwPERSPECTIVE;
    updateZShiftScale(camera);
    camera->viewMatrix.flags = 0;
    if (_rwPluginRegistryInitObject(&cameraTKList, camera) == nullptr) {
        // native: a failed plugin constructor fails the creation (the original returns the half-built camera)
        rwSrcGlobals().memoryFree(cameraFreeList(), camera);
        return nullptr;
    }
    return camera;
}

RwBool RwCameraDestroy(RwCamera* camera) {
    _rwPluginRegistryDeInitObject(&cameraTKList, camera);
    _rwObjectHasFrameReleaseFrame(camera);
    if (rwSrcGlobals().curCamera == camera) rwSrcGlobals().curCamera = nullptr;   // native: no dangling current camera
    rwSrcGlobals().memoryFree(cameraFreeList(), camera);
    return 1;
}

RwCamera* RwCameraSetFrame(RwCamera* camera, RwFrame* frame) {
    _rwObjectHasFrameSetFrame(camera, frame);
    return camera;
}

RwFrame* RwCameraGetFrame(const RwCamera* camera) { return frameOf(camera); }

RwCamera* RwCameraBeginUpdate(RwCamera* camera) { return camera->beginUpdate(camera); }

RwCamera* RwCameraEndUpdate(RwCamera* camera) { return camera->endUpdate(camera); }

RwCamera* RwCameraSetViewWindow(RwCamera* camera, const RwV2d* viewWindow) {
    camera->viewWindow = *viewWindow;
    camera->recipViewWindow.x = kOne / camera->viewWindow.x;
    camera->recipViewWindow.y = kOne / camera->viewWindow.y;
    markFrame(camera);
    return camera;
}

RwCamera* RwCameraSetViewOffset(RwCamera* camera, const RwV2d* offset) {
    camera->viewOffset = *offset;
    markFrame(camera);
    return camera;
}

RwCamera* RwCameraSetNearClipPlane(RwCamera* camera, RwReal nearClip) {
    camera->nearPlane = nearClip;
    updateZShiftScale(camera);
    markFrame(camera);
    return camera;
}

RwCamera* RwCameraSetFarClipPlane(RwCamera* camera, RwReal farClip) {
    camera->farPlane = farClip;
    updateZShiftScale(camera);
    markFrame(camera);
    return camera;
}

RwCamera* RwCameraSetProjection(RwCamera* camera, RwCameraProjection projection) {
    if (projection != rwPERSPECTIVE && projection != rwPARALLEL) {
        RwError error{rwID_COREPLUGIN, _rwerror(E_RW_BADPARAM, "Invalid projection type")};
        RwErrorSet(&error);
        return nullptr;
    }
    camera->projectionType = projection;
    markFrame(camera);
    updateZShiftScale(camera);
    return camera;
}

RwFrustumTestResult RwCameraFrustumTestSphere(const RwCamera* camera, const RwSphere* sphere) {
    RwFrustumTestResult result = rwSPHEREINSIDE;
    const RwReal radius = sphere->radius, minusRadius = -sphere->radius;
    for (const RwFrustumPlane& p : camera->frustumPlanes) {
        const RwReal dist = planeDot(sphere->center, p.plane.normal) - p.plane.distance;
        if (dist > radius) return rwSPHEREOUTSIDE;
        if (dist > minusRadius) result = rwSPHEREBOUNDARY;
    }
    return result;
}

RwCamera* RwCameraClear(RwCamera* camera, void* colour, RwInt32 clearMode) {
    return rwSrcGlobals().stdFunc[rwSTANDARDCAMERACLEAR](camera, colour, clearMode) ? camera : nullptr;
}

RwCamera* RwCameraShowRaster(RwCamera* camera, void* dev, RwUInt32 flags) {
    // RwRasterShowRaster (0x8048AD94): the resources arena's end-of-frame purge, then the device shows the raster. It
    // returns the raster (NULL when the device refuses), so a camera without a frame buffer always fails, as in the
    // original.
    RwRaster* const raster = camera->frameBuffer;
    _rwResourcesPurge();
    const bool shown = rwSrcGlobals().stdFunc[rwSTANDARDRASTERSHOWRASTER](raster, dev, static_cast<RwInt32>(flags));
    return shown && raster != nullptr ? camera : nullptr;
}

RwInt32 RwCameraRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                               RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB) {
    return _rwPluginRegistryAddPlugin(&cameraTKList, size, pluginID, constructCB, destructCB, copyCB);
}

RwInt32 RwCameraRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB,
                                     RwPluginDataChunkWriteCallBack writeCB, RwPluginDataChunkGetSizeCallBack getSizeCB) {
    return _rwPluginRegistryAddPluginStream(&cameraTKList, pluginID, readCB, writeCB, getSizeCB);
}

RwCamera* RwCameraStreamRead(RwStream* stream) {
    RwUInt32 length = 0, version = 0;
    if (!RwStreamFindChunk(stream, rwID_STRUCT, &length, &version)) return nullptr;
    if (version < rwLIBRARYBASEVERSION || version > rwLIBRARYCURRENTVERSION) {
        RwError error{rwID_COREPLUGIN, _rwerror(static_cast<RwInt32>(0x80000004u))};   // E_RW_BADVERSION
        RwErrorSet(&error);
        return nullptr;
    }
    // {viewWindow, viewOffset, near, far, fog, projection}: 32 bytes
    RwUInt32 data[8] = {};
    const RwUInt32 take = length < sizeof data ? length : static_cast<RwUInt32>(sizeof data);
    if (RwStreamRead(stream, data, take) != take) return nullptr;
    if (length > take && RwStreamSkip(stream, length - take) == nullptr) return nullptr;   // native: the rest skipped
    RwMemNative32(data, sizeof data);
    RwCamera* camera = RwCameraCreate();
    if (camera == nullptr) return nullptr;
    if (_rwPluginRegistryReadDataChunks(&cameraTKList, stream, camera) == nullptr) {
        RwCameraDestroy(camera);   // native: the original leaks it
        return nullptr;
    }
    const RwV2d window{std::bit_cast<RwReal>(data[0]), std::bit_cast<RwReal>(data[1])};
    const RwV2d offset{std::bit_cast<RwReal>(data[2]), std::bit_cast<RwReal>(data[3])};
    RwCameraSetViewWindow(camera, &window);
    RwCameraSetViewOffset(camera, &offset);
    RwCameraSetNearClipPlane(camera, std::bit_cast<RwReal>(data[4]));
    RwCameraSetFarClipPlane(camera, std::bit_cast<RwReal>(data[5]));
    camera->fogPlane = std::bit_cast<RwReal>(data[6]);
    RwCameraSetProjection(camera, static_cast<RwCameraProjection>(data[7]));
    return camera;
}
