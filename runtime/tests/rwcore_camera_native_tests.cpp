// Native tests of the RenderWare cameras (rwcore/src/camera.cpp) for what the oracle cannot cover:
// - independent mathematics, from the definition of a RenderWare camera rather than the original's code: a point
//   given by window coordinates (u, v) in [0, 1] and a depth z between the clip planes, placed through the view window
//   (half-extents at distance 1, the eye moved by the view offset) and the frame's LTM, must come out of the view
//   matrix at (u z, v z, z) (perspective) or (u, v, z) (parallel); the frustum corners are those points at the
//   window's corners; each corner lies on its three planes and inside the other three; the normals are unit length
//   (to _rwInvSqrt's precision) and point out; the closest-corner bytes are the normals' signs; the box bounds the
//   corners exactly; spheres inside / across / outside are classified as such; the Z scale and shift map the clip
//   planes onto the device's Z range less 1e-4 of it at each end. Random frames, windows of any shape (0.1 .. 10),
//   offsets, clip ranges up to 1e-3 .. 1e7, device Z ranges.
// - no stretching: a Hor+ window (width = height x the screen's aspect) gives square pixels at 16:9, 16:10, 21:9,
//   32:9 and 32:10 screens (5K included); the same check sees the stretch of a 4:3 window on those screens.
// - repeated attach cycles: a camera moved between frames of moving hierarchies follows only its current frame;
//   detached, it follows none; a destroyed frame releases it.
// - the deliberate differences of rw/core/camera.h: a refused begin-update restores the previous current camera,
//   destroying the current camera clears it, a failing plugin constructor fails the creation (no leak), no camera
//   without an open camera module, a stream struct longer than 32 bytes is read into 32 and skipped, a failed plugin
//   read destroys the camera; plus bad versions, truncated streams, an invalid projection (camera unchanged), the
//   device calls of clear / show raster with their arguments.
// Memory is counted: every part ends with no live allocation.
//
// Usage: rwcore_camera_native_tests
#include "rw/core/binary.h"
#include "rw/core/camera.h"
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/frame.h"
#include "rw/core/memory.h"
#include "rw/core/stream.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
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

constexpr RwUInt32 kPluginID = 0x7006;
int g_constructs = 0, g_destructs = 0, g_fail_construct_at = -1, g_plugin_reads = 0;
bool g_fail_plugin_read = false;
void* pluginCtor(void* object, RwInt32 offset, RwInt32) {
    if (g_constructs++ == g_fail_construct_at) return nullptr;
    *reinterpret_cast<RwUInt32*>(static_cast<char*>(object) + offset) = 0xCA11u;
    return object;
}
void* pluginDtor(void* object, RwInt32 offset, RwInt32) {
    ++g_destructs;
    require(*reinterpret_cast<RwUInt32*>(static_cast<char*>(object) + offset) != 0xDEADu, "a camera destroyed twice");
    *reinterpret_cast<RwUInt32*>(static_cast<char*>(object) + offset) = 0xDEADu;
    return object;
}
RwStream* pluginRead(RwStream* stream, RwInt32 length, void* object, RwInt32 offset, RwInt32) {
    ++g_plugin_reads;
    if (g_fail_plugin_read || length != 4) return nullptr;
    RwUInt32 value = 0;
    if (RwStreamRead(stream, &value, 4) != 4) return nullptr;
    *reinterpret_cast<RwUInt32*>(static_cast<char*>(object) + offset) = value;
    return stream;
}
RwInt32 g_pluginOffset = -1;

void startEngine() {
    RwEngineOpenParams params{nullptr};
    require(RwEngineInit(&kCounting, 0, 0) == 1, "init");
    g_pluginOffset = RwCameraRegisterPlugin(4, kPluginID, pluginCtor, pluginDtor, nullptr);
    require(g_pluginOffset >= static_cast<RwInt32>(sizeof(RwCamera)), "a camera plugin");
    require(RwCameraRegisterPluginStream(kPluginID, pluginRead, nullptr, nullptr) == g_pluginOffset, "its stream");
    require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "start");
}
void stopEngine(const char* what) {
    require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, std::string(what) + ": shutdown");
    require(g_live.empty(), std::string(what) + ": " + std::to_string(g_live.size()) + " allocations leaked");
}

RwUInt32 pluginWord(const RwCamera* c) {
    return *reinterpret_cast<const RwUInt32*>(reinterpret_cast<const char*>(c) + g_pluginOffset);
}

int objectsOn(const RwFrame* f) {
    int n = 0;
    for (const RwLLLink* l = f->objectList.link.next; l != &f->objectList.link; l = l->next) ++n;
    return n;
}

// ---- the device, recorded -----------------------------------------------------------------------------------------
struct DeviceCall {
    int index;
    void* a;
    void* b;
    RwInt32 n;
};
std::vector<DeviceCall> g_calls;
bool g_device_answer = true;
template <int Index>
RwBool recordingStd(void* a, void* b, RwInt32 n) {
    g_calls.push_back({Index, a, b, n});
    return g_device_answer;
}

// ---- geometry in double precision ----------------------------------------------------------------------------------
struct D3 {
    double x, y, z;
};
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double norm(D3 a) { return std::sqrt(dot(a, a)); }
D3 of(const RwV3d& v) { return {v.x, v.y, v.z}; }

/// A frame placed by a random rotation (a normalised random quaternion) at pos; returns the LTM the camera sees.
RwMatrix randomPlacement(std::mt19937_64& g, double posRange) {
    std::normal_distribution<double> n;
    double q[4] = {n(g), n(g), n(g), n(g)};
    const double len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (double& x : q) x /= len;
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    std::uniform_real_distribution<double> p(-posRange, posRange);
    RwMatrix m{};
    m.right = {static_cast<RwReal>(1 - 2 * (y * y + z * z)), static_cast<RwReal>(2 * (x * y + w * z)),
               static_cast<RwReal>(2 * (x * z - w * y))};
    m.up = {static_cast<RwReal>(2 * (x * y - w * z)), static_cast<RwReal>(1 - 2 * (x * x + z * z)),
            static_cast<RwReal>(2 * (y * z + w * x))};
    m.at = {static_cast<RwReal>(2 * (x * z + w * y)), static_cast<RwReal>(2 * (y * z - w * x)),
            static_cast<RwReal>(1 - 2 * (x * x + y * y))};
    m.pos = {static_cast<RwReal>(p(g)), static_cast<RwReal>(p(g)), static_cast<RwReal>(p(g))};
    m.flags = 0;
    return m;
}

void place(RwFrame* f, const RwMatrix& m) {
    f->modelling = m;
    RwFrameUpdateObjects(f);
    _rwFrameSyncDirty();
}

/// The camera-space point of window coordinates (u, v) at depth z, by the definition of the view window: perspective,
/// the ray from the eye (moved by the view offset: -offset.x along right, offset.y along up) through the window's
/// point at distance 1; parallel, the window's point at z, sheared by the offset as z leaves 1.
D3 cameraPoint(const RwCamera& c, double u, double v, double z) {
    const double sr = 1 - 2 * u, su = 1 - 2 * v;
    const double wx = c.viewWindow.x, wy = c.viewWindow.y, ox = c.viewOffset.x, oy = c.viewOffset.y;
    if (c.projectionType == rwPARALLEL) return {sr * wx + (1 - z) * -ox, su * wy + (1 - z) * oy, z};
    return {-ox + (sr * wx + ox) * z, oy + (su * wy - oy) * z, z};
}
D3 world(const RwMatrix& ltm, D3 q) { return of(ltm.pos) + of(ltm.right) * q.x + of(ltm.up) * q.y + of(ltm.at) * q.z; }
/// The view matrix applied in double to the float matrix the camera holds (columns: right.x, up.x, at.x, ...).
D3 view(const RwMatrix& m, D3 p) {
    return {p.x * m.right.x + p.y * m.up.x + p.z * m.at.x + m.pos.x, p.x * m.right.y + p.y * m.up.y + p.z * m.at.y + m.pos.y,
            p.x * m.right.z + p.y * m.up.z + p.z * m.at.z + m.pos.z};
}

constexpr double kEps = 1.0 / 8388608.0;   // 2^-23
unsigned g_checks = 0;
void check(bool condition, const std::string& what) {
    ++g_checks;
    require(condition, what);
}

// ---- 1. the mathematics ---------------------------------------------------------------------------------------------
void mathematics() {
    startEngine();
    std::mt19937_64 g(0x20261101);
    std::uniform_real_distribution<double> unit(0, 1);
    RwFrame* frame = RwFrameCreate();
    RwCamera* camera = RwCameraCreate();
    require(frame != nullptr && camera != nullptr, "camera + frame");
    RwCameraSetFrame(camera, frame);
    RwGlobals& G = rwSrcGlobals();
    unsigned configs = 0, extreme = 0, unresolved = 0, unresolvedOrdinary = 0;
    for (int trial = 0; trial < 20000; ++trial) {
        const bool parallel = trial % 3 == 2, wide = trial % 10 == 9;
        const double posRange = trial % 4 == 0 ? 2000.0 : 50.0;
        RwCameraSetProjection(camera, parallel ? rwPARALLEL : rwPERSPECTIVE);
        const RwV2d window{static_cast<RwReal>(std::exp(std::log(0.1) + unit(g) * std::log(100.0))),
                           static_cast<RwReal>(std::exp(std::log(0.1) + unit(g) * std::log(100.0)))};
        RwCameraSetViewWindow(camera, &window);
        const RwV2d offset = trial % 5 == 0 ? RwV2d{0, 0}
                                            : RwV2d{static_cast<RwReal>(unit(g) * 2 - 1), static_cast<RwReal>(unit(g) * 2 - 1)};
        RwCameraSetViewOffset(camera, &offset);
        double zn, zf;
        if (wide) {   // extreme clip ranges
            zn = parallel ? -std::exp(unit(g) * std::log(1e4)) : std::exp(std::log(1e-3) + unit(g) * std::log(1e3));
            zf = std::exp(std::log(1e5) + unit(g) * std::log(1e2));
            ++extreme;
        } else {
            zn = parallel ? unit(g) * 20 - 10 : 0.05 + unit(g) * 5;
            zf = zn + 1 + unit(g) * 3000;
        }
        RwCameraSetNearClipPlane(camera, static_cast<RwReal>(zn));
        RwCameraSetFarClipPlane(camera, static_cast<RwReal>(zf));
        const float dn = trial % 7 == 0 ? -1.0f : 0.0f, df = trial % 11 == 0 ? 65535.0f : 1.0f;
        G.dOpenDevice.zBufferNear = dn, G.dOpenDevice.zBufferFar = df;
        RwCameraSetNearClipPlane(camera, camera->nearPlane);   // the Z scale / shift follow the device
        const RwMatrix ltm = randomPlacement(g, posRange);
        place(frame, ltm);
        const RwCamera& c = *camera;
        const std::string tag = "trial " + std::to_string(trial) + (parallel ? " parallel" : " perspective");
        zn = c.nearPlane, zf = c.farPlane;
        const double wx = c.viewWindow.x, wy = c.viewWindow.y, ox = std::fabs(c.viewOffset.x), oy = std::fabs(c.viewOffset.y);
        const double pos = norm(of(ltm.pos));
        const double reach = std::max(std::fabs(zn), std::fabs(zf)) * (1 + wx + wy + ox + oy) + 1 + ox + oy;
        // the corners: window corners (u, v) = (0, 0) (1, 0) (1, 1) (0, 1) ... at near, then at far
        const double cu[4] = {0, 1, 1, 0}, cv[4] = {0, 0, 1, 1};
        const double cornerTol = 32 * kEps * (pos + reach);
        for (int i = 0; i < 8; ++i) {
            const D3 e = world(ltm, cameraPoint(c, cu[i % 4], cv[i % 4], i < 4 ? zn : zf));
            check(norm(of(c.frustumCorners[i]) - e) <= cornerTol, tag + ": corner " + std::to_string(i));
        }
        // window points through the view matrix
        for (int k = 0; k < 6; ++k) {
            const double u = unit(g) * 1.4 - 0.2, v = unit(g) * 1.4 - 0.2, z = zn + (zf - zn) * unit(g);
            const D3 q = cameraPoint(c, u, v, z);
            const D3 p = world(ltm, q);
            const D3 s = view(c.viewMatrix, p);
            const double scale = (pos + norm(q) + 1) * (1 / wx + 1 / wy + 2 + (ox + oy) * (1 / wx + 1 / wy));
            const double tol = 64 * kEps * scale, zTol = 64 * kEps * (pos + norm(q) + 1);
            check(std::fabs(s.z - z) <= zTol, tag + ": view z");
            if (parallel) {
                check(std::fabs(s.x - u) <= tol && std::fabs(s.y - v) <= tol, tag + ": parallel window coordinates");
            } else {
                check(std::fabs(s.x / s.z - u) <= tol / z && std::fabs(s.y / s.z - v) <= tol / z, tag + ": perspective window coordinates");
            }
        }
        // the planes: unit outward normals; each holds four corners (three of them define it), the rest inside
        D3 centroid{0, 0, 0};
        for (const RwV3d& v : c.frustumCorners) centroid = centroid + of(v) * 0.125;
        const double planeTol = 4 * kEps * (pos + reach);
        // The side planes come from cross products of corner differences, so they exist only where the float corners
        // resolve the box's width (2 x the smaller half-extent at the far plane): not for the thinnest, longest boxes,
        // whose far corners coincide (as in the original - a limit of world-space float corners; counted, not hidden).
        const double width = 2 * std::min(wx, wy) * (parallel ? 1.0 : std::fabs(zf));
        const bool resolvable = width > 8 * planeTol;
        unresolved += !resolvable;
        unresolvedOrdinary += !resolvable && !wide;
        for (int p = 0; p < 6; ++p) {
            const RwFrustumPlane& fp = c.frustumPlanes[p];
            const D3 n = of(fp.plane.normal);
            check(fp.closestX == !std::signbit(fp.plane.normal.x) && fp.closestY == !std::signbit(fp.plane.normal.y) &&
                      fp.closestZ == !std::signbit(fp.plane.normal.z),
                  tag + ": closest-corner bytes");
            if (p >= 2 && !resolvable) continue;
            check(std::fabs(norm(n) - 1) <= 2e-3, tag + ": plane " + std::to_string(p) + " normal length");
            std::vector<double> d;
            for (const RwV3d& v : c.frustumCorners) d.push_back(dot(of(v), n) - fp.plane.distance);
            std::sort(d.begin(), d.end());
            check(std::fabs(d[4]) <= planeTol && std::fabs(d[7]) <= planeTol, tag + ": four corners on plane " + std::to_string(p));
            check(d[0] < -planeTol, tag + ": the other corners inside plane " + std::to_string(p));
            check(dot(centroid, n) - fp.plane.distance < 0, tag + ": plane " + std::to_string(p) + " points out");
        }
        // the far plane's normal is the frame's at, the near plane's its opposite
        check(norm(of(c.frustumPlanes[0].plane.normal) - of(ltm.at)) <= 4 * kEps &&
                  norm(of(c.frustumPlanes[1].plane.normal) + of(ltm.at)) <= 4 * kEps,
              tag + ": far / near normals");
        // the box: exactly the corners' bounds
        RwV3d lo = c.frustumCorners[0], hi = lo;
        for (const RwV3d& v : c.frustumCorners) {
            lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
            hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
        }
        check(std::memcmp(&c.frustumBoundBox.inf, &lo, sizeof lo) == 0 && std::memcmp(&c.frustumBoundBox.sup, &hi, sizeof hi) == 0,
              tag + ": bounding box");
        // spheres: deep inside (radius 0), across a side, beyond the far plane / a side
        {
            const double z = zn + (zf - zn) * (0.25 + 0.5 * unit(g));
            const auto sphereAt = [&](double u, double v, double zz, double r) {
                const D3 p = world(ltm, cameraPoint(c, u, v, zz));
                const RwSphere s{{static_cast<RwReal>(p.x), static_cast<RwReal>(p.y), static_cast<RwReal>(p.z)}, static_cast<RwReal>(r)};
                return RwCameraFrustumTestSphere(camera, &s);
            };
            const double halfWidthX = parallel ? wx : wx * z;   // the window's half extent at z (perspective: no offset term needed for the margin)
            const double halfWidthY = parallel ? wy : wy * z;
            const double small = std::min(halfWidthX, halfWidthY);
            if (small > 1e4 * planeTol) {
                check(sphereAt(0.3 + 0.4 * unit(g), 0.3 + 0.4 * unit(g), z, 0) == rwSPHEREINSIDE, tag + ": a point inside");
                check(sphereAt(0, 0.5, z, 0.2 * small) == rwSPHEREBOUNDARY, tag + ": a sphere across a side");
                check(sphereAt(1.5 + unit(g), 0.5, z, 0.01 * small) == rwSPHEREOUTSIDE, tag + ": a sphere beyond a side");
                check(sphereAt(0.5, -0.6, z, 0.01 * small) == rwSPHEREOUTSIDE, tag + ": a sphere beyond the bottom");
            }
            const double beyond = zf + 0.1 * (zf - zn);
            if ((zf - zn) > 1e4 * planeTol) {
                check(sphereAt(0.5, 0.5, beyond, 0.01 * (zf - zn)) == rwSPHEREOUTSIDE, tag + ": beyond the far plane");
                check(sphereAt(0.5, 0.5, zf, 0.01 * (zf - zn)) == rwSPHEREBOUNDARY, tag + ": across the far plane");
            }
        }
        // the Z scale and shift: the clip planes onto the device's range less 1e-4 of it at each end
        {
            const double range = static_cast<double>(df) - dn, margin = 1e-4 * range;
            const auto device = [&](double z) { return c.zScale * (parallel ? z : 1 / z) + c.zShift; };
            // tight enough to see a missing 1e-4 margin
            const double w = parallel ? std::max(std::fabs(zn), std::fabs(zf)) : std::max(1 / std::fabs(zn), 1 / std::fabs(zf));
            const double zTol = 64 * kEps * (std::fabs(c.zScale) * w + std::fabs(c.zShift) + std::fabs(range));
            check(std::fabs(device(zn) - (dn + margin)) <= zTol && std::fabs(device(zf) - (df - margin)) <= zTol,
                  tag + ": Z scale / shift");
        }
        ++configs;
    }
    std::printf("  mathematics: %u configurations (%u with extreme clip ranges; side planes unresolvable in %u of them, in %u "
                "others)\n",
                configs, extreme, unresolved, unresolvedOrdinary);
    require(configs == 20000 && extreme >= 1500 && unresolvedOrdinary == 0, "every configuration ran (ordinary ones all resolvable)");
    RwCameraDestroy(camera);
    RwFrameDestroy(frame);
    stopEngine("mathematics");
}

// ---- 2. aspect ratios: square pixels ----------------------------------------------------------------------------
void aspects() {
    startEngine();
    RwFrame* frame = RwFrameCreate();
    RwCamera* camera = RwCameraCreate();
    RwCameraSetFrame(camera, frame);
    RwMatrix identity{};
    identity.right = {1, 0, 0}, identity.up = {0, 1, 0}, identity.at = {0, 0, 1}, identity.pos = {0, 0, 0};
    place(frame, identity);
    struct Screen {
        const char* name;
        double w, h;
    };
    const Screen screens[] = {{"1920x1080 16:9", 1920, 1080},  {"2560x1600 16:10", 2560, 1600}, {"3440x1440 21:9", 3440, 1440},
                              {"5120x1440 32:9", 5120, 1440},  {"3840x1200 32:10", 3840, 1200}, {"5120x2880 16:9 5K", 5120, 2880},
                              {"5120x1600 32:10 5K", 5120, 1600}};
    // the view of the camera-space point (10 t, 10 t, 10): equal tangents across and up (window coordinates times z)
    const auto pixelRatio = [&](double t) { return view(camera->viewMatrix, {t * 10, t * 10, 10}); };
    for (const Screen& s : screens) {
        for (int proj = 0; proj < 2; ++proj) {
            RwCameraSetProjection(camera, proj ? rwPARALLEL : rwPERSPECTIVE);
            const RwReal wy = 0.4142135f;   // a 45 degree vertical field of view
            const RwV2d hor{static_cast<RwReal>(wy * s.w / s.h), wy};
            RwCameraSetViewWindow(camera, &hor);
            _rwFrameSyncDirty();
            for (double t : {0.05, 0.1, 0.2, 0.3}) {
                const D3 v = pixelRatio(t);
                const double z = proj ? 1.0 : v.z;
                const double dx = (v.x / z - 0.5) * s.w, dy = (v.y / z - 0.5) * s.h;
                check(std::fabs(std::fabs(dx) / std::fabs(dy) - 1) < 1e-5, std::string(s.name) + ": square pixels (Hor+)");
            }
            // the same check sees a 4:3 window stretched on a wider screen
            const RwV2d four3{static_cast<RwReal>(wy * 4.0 / 3.0), wy};
            RwCameraSetViewWindow(camera, &four3);
            _rwFrameSyncDirty();
            const D3 v = pixelRatio(0.2);
            const double z = proj ? 1.0 : v.z;
            const double ratio = std::fabs((v.x / z - 0.5) * s.w) / std::fabs((v.y / z - 0.5) * s.h);
            check(std::fabs(ratio - (s.w / s.h) / (4.0 / 3.0)) < 1e-4 && std::fabs(ratio - 1) > 0.1,
                  std::string(s.name) + ": a 4:3 window is seen stretched");
        }
    }
    RwCameraDestroy(camera);
    RwFrameDestroy(frame);
    stopEngine("aspects");
    std::printf("  aspects: %zu screens, both projections\n", std::size(screens));
}

// ---- 3. attach cycles -------------------------------------------------------------------------------------------
void attachCycles() {
    startEngine();
    std::mt19937_64 g(77);
    std::vector<RwFrame*> frames;
    for (int i = 0; i < 6; ++i) frames.push_back(RwFrameCreate());
    RwFrameAddChild(frames[0], frames[1]), RwFrameAddChild(frames[1], frames[2]), RwFrameAddChild(frames[3], frames[4]);
    std::vector<RwCamera*> cameras;
    for (int i = 0; i < 3; ++i) cameras.push_back(RwCameraCreate());
    std::vector<int> on(3, -1);
    for (int cycle = 0; cycle < 3000; ++cycle) {
        const int k = static_cast<int>(g() % 3);
        const int f = static_cast<int>(g() % 7) - 1;   // -1: detach
        RwCameraSetFrame(cameras[k], f < 0 ? nullptr : frames[f]);
        on[k] = f;
        _rwFrameSyncDirty();   // a camera attached to a frame syncs with it
        for (int i = 0; i < 6; ++i) {
            int expected = 0;
            for (int c = 0; c < 3; ++c) expected += on[c] == i;
            check(objectsOn(frames[i]) == expected, "objects on each frame");
        }
        check(RwCameraGetFrame(cameras[k]) == (f < 0 ? nullptr : frames[f]), "RwCameraGetFrame");
        // move one frame of a hierarchy; every camera on it (or below it) follows, others do not
        std::vector<std::vector<RwV3d>> before;
        for (RwCamera* c : cameras) before.emplace_back(c->frustumCorners, c->frustumCorners + 8);
        const int moved = static_cast<int>(g() % 6);
        RwMatrix m = randomPlacement(g, 30);
        place(frames[moved], m);
        for (int c = 0; c < 3; ++c) {
            bool affected = false;
            if (on[c] >= 0) {
                for (const RwFrame* a = frames[on[c]]; a != nullptr; a = static_cast<const RwFrame*>(a->object.parent)) {
                    affected = affected || a == frames[moved];
                }
            }
            const bool changed = std::memcmp(before[c].data(), cameras[c]->frustumCorners, sizeof(RwV3d) * 8) != 0;
            check(changed == affected, "a camera follows exactly its own frame's hierarchy");
            if (affected) {   // and sits where its frame's LTM says
                const RwMatrix& ltm = frames[on[c]]->ltm;
                const D3 e = world(ltm, cameraPoint(*cameras[c], 0, 0, cameras[c]->nearPlane));
                check(norm(of(cameras[c]->frustumCorners[0]) - e) <= 1e-3, "at its frame's LTM");
            }
        }
    }
    // a destroyed frame releases its camera
    RwCameraSetFrame(cameras[0], frames[5]);
    require(RwFrameDestroy(frames[5]) == 1, "destroy a frame with a camera");
    check(RwCameraGetFrame(cameras[0]) == nullptr, "the destroyed frame released the camera");
    for (RwCamera* c : cameras) RwCameraDestroy(c);
    for (int i = 0; i < 5; ++i) check(objectsOn(frames[i]) == 0, "destroyed cameras leave their frames");
    RwFrameDestroyHierarchy(frames[0]);
    RwFrameDestroyHierarchy(frames[3]);
    stopEngine("attach cycles");
}

// ---- 4. lifecycle, device calls, deliberate differences ----------------------------------------------------------
void lifecycle() {
    // no camera module: no camera
    {
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(&kCounting, 0, 0) == 1, "init");
        check(RwCameraCreate() == nullptr && takeError().errorCode == static_cast<RwInt32>(0x80000001u),
              "no camera before the engine opens");
        require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "start");
        stopEngine("unopened");
    }
    startEngine();
    RwGlobals& G = rwSrcGlobals();
    // the defaults
    RwCamera* a = RwCameraCreate();
    {
        const RwCamera& c = *a;
        check(c.object.object.type == rwCAMERA && c.object.object.parent == nullptr && c.projectionType == rwPERSPECTIVE &&
                  c.viewWindow.x == 1 && c.viewWindow.y == 1 && c.recipViewWindow.x == 1 && c.recipViewWindow.y == 1 &&
                  c.viewOffset.x == 0 && c.viewOffset.y == 0 && c.nearPlane == 0.05f && c.farPlane == 10 && c.fogPlane == 5 &&
                  c.frameBuffer == nullptr && c.zBuffer == nullptr && pluginWord(&c) == 0xCA11u,
              "RwCameraCreate's defaults");
        // the device range [0, 1]: 1/z of 0.05 and 10 onto [1e-4, 1 - 1e-4]
        const double s = (1 - 2e-4) / (0.1 - 20.0), t = 0.5 * ((1.0) - s * (0.1 + 20.0));
        check(std::fabs(c.zScale - s) < 1e-6 && std::fabs(c.zShift - t) < 1e-6, "the default Z scale / shift");
    }
    // a failing plugin constructor fails the creation, without a leak
    g_fail_construct_at = g_constructs;
    const int destructsBefore = g_destructs;
    check(RwCameraCreate() == nullptr && g_destructs == destructsBefore, "a failed constructor fails RwCameraCreate");
    g_fail_construct_at = -1;
    // an invalid projection: NULL, E_RW_BADPARAM, the camera unchanged
    {
        std::vector<unsigned char> image(reinterpret_cast<unsigned char*>(a), reinterpret_cast<unsigned char*>(a) + sizeof(RwCamera));
        check(RwCameraSetProjection(a, static_cast<RwCameraProjection>(3)) == nullptr && takeError().errorCode == E_RW_BADPARAM &&
                  std::memcmp(image.data(), a, sizeof(RwCamera)) == 0,
              "an invalid projection changes nothing");
        check(RwCameraSetProjection(a, rwNACAMERAPROJECTION) == nullptr && takeError().errorCode == E_RW_BADPARAM,
              "rwNACAMERAPROJECTION is invalid");
    }
    // every setter marks the frame for an update; a projection change alone remaps Z (z, not 1 / z, for parallel)
    {
        RwFrame* f = RwFrameCreate();
        RwCameraSetFrame(a, f);
        const RwV2d w{2, 1}, o{0.5f, 0.25f};
        const auto marks = [&](auto set, const char* what) {
            _rwFrameSyncDirty();
            require(!RwFrameDirty(f), "synced");
            set();
            check(RwFrameDirty(f) != 0, std::string(what) + " marks the frame");
        };
        marks([&] { RwCameraSetViewWindow(a, &w); }, "RwCameraSetViewWindow");
        marks([&] { RwCameraSetViewOffset(a, &o); }, "RwCameraSetViewOffset");
        marks([&] { RwCameraSetNearClipPlane(a, 1); }, "RwCameraSetNearClipPlane");
        marks([&] { RwCameraSetFarClipPlane(a, 100); }, "RwCameraSetFarClipPlane");
        marks([&] { RwCameraSetProjection(a, rwPARALLEL); }, "RwCameraSetProjection");
        const double margin = 1e-4;
        check(std::fabs(a->zScale * 1 + a->zShift - margin) < 1e-6 && std::fabs(a->zScale * 100 + a->zShift - (1 - margin)) < 1e-6,
              "a projection change remaps Z (parallel: z)");
        RwCameraSetProjection(a, rwPERSPECTIVE);
        check(std::fabs(a->zScale / 1 + a->zShift - margin) < 1e-6 && std::fabs(a->zScale / 100 + a->zShift - (1 - margin)) < 1e-6,
              "and back (perspective: 1 / z)");
        RwCameraSetFrame(a, nullptr);
        RwFrameDestroy(f);
    }
    // begin / end update through the device
    RwCamera* b = RwCameraCreate();
    RwFrame* frame = RwFrameCreate();
    RwCameraSetFrame(b, frame);
    RwMatrix m{};
    m.right = {1, 0, 0}, m.up = {0, 1, 0}, m.at = {0, 0, 1}, m.pos = {5, 6, 7};
    frame->modelling = m;
    RwFrameUpdateObjects(frame);   // dirty: begin-update must sync it
    const RwStandardFunc saved[3] = {G.stdFunc[1], G.stdFunc[10], G.stdFunc[20]};
    const RwStandardFunc savedClear = G.stdFunc[21];
    G.stdFunc[1] = recordingStd<1>, G.stdFunc[10] = recordingStd<10>, G.stdFunc[20] = recordingStd<20>, G.stdFunc[21] = recordingStd<21>;
    G.curCamera = a;
    g_device_answer = false;
    check(RwCameraBeginUpdate(b) == nullptr && G.curCamera == a, "a refused begin-update restores the current camera");
    check(g_calls.size() == 1 && g_calls[0].index == 1 && g_calls[0].a == nullptr && g_calls[0].b == b, "the device's begin");
    check(!RwFrameDirty(frame) && b->frustumCorners[0].x != 0, "begin-update synced the dirty frames first");
    g_device_answer = true;
    g_calls.clear();
    check(RwCameraBeginUpdate(b) == b && G.curCamera == b, "begin-update makes the camera current");
    g_device_answer = false;
    check(RwCameraEndUpdate(b) == nullptr && G.curCamera == b, "a refused end-update keeps it");
    g_device_answer = true;
    check(RwCameraEndUpdate(b) == b && G.curCamera == nullptr, "end-update clears it");
    check(g_calls.size() == 3 && g_calls[1].index == 10 && g_calls[1].b == b && g_calls[2].index == 10, "the device's end");
    g_calls.clear();
    // clear and show raster: the device's arguments and answers
    int colour = 0;
    check(RwCameraClear(b, &colour, rwCAMERACLEARIMAGE | rwCAMERACLEARZ) == b && g_calls.back().index == 21 && g_calls.back().a == b &&
              g_calls.back().b == &colour && g_calls.back().n == 3,
          "RwCameraClear");
    g_device_answer = false;
    check(RwCameraClear(b, &colour, rwCAMERACLEARZ) == nullptr, "a refused clear");
    g_device_answer = true;
    check(RwCameraShowRaster(b, &colour, 1) == nullptr && g_calls.back().index == 20 && g_calls.back().a == nullptr &&
              g_calls.back().b == &colour && g_calls.back().n == 1,
          "no frame buffer: show raster fails, as the original's returns the raster");
    unsigned char raster[16];
    b->frameBuffer = reinterpret_cast<RwRaster*>(raster);
    check(RwCameraShowRaster(b, nullptr, 0) == b && g_calls.back().a == raster, "show raster shows the frame buffer");
    g_device_answer = false;
    check(RwCameraShowRaster(b, nullptr, 0) == nullptr, "a refused show");
    g_device_answer = true;
    b->frameBuffer = nullptr;
    G.stdFunc[1] = saved[0], G.stdFunc[10] = saved[1], G.stdFunc[20] = saved[2], G.stdFunc[21] = savedClear;
    // destroying the current camera clears it; destroying detaches from the frame and runs the destructors
    G.curCamera = b;
    const int d0 = g_destructs;
    check(RwCameraDestroy(b) == 1 && G.curCamera == nullptr && objectsOn(frame) == 0 && g_destructs == d0 + 1,
          "destroying the current camera");
    RwCameraDestroy(a);
    RwFrameDestroy(frame);
    // many cameras: the free list grows and returns everything
    std::vector<RwCamera*> many;
    for (int i = 0; i < 1000; ++i) many.push_back(RwCameraCreate());
    for (RwCamera* c : many) {
        check(c != nullptr && (reinterpret_cast<std::uintptr_t>(c) & 15u) == 0, "cameras are 16-byte aligned");
    }
    for (RwCamera* c : many) RwCameraDestroy(c);
    stopEngine("lifecycle");
}

// ---- 5. streams -------------------------------------------------------------------------------------------------
std::vector<unsigned char> g_bytes;
void put(RwUInt32 w) {
    for (int i = 0; i < 4; ++i) g_bytes.push_back(static_cast<unsigned char>(w >> (8 * i)));
}
constexpr RwUInt32 libraryID(RwUInt32 version) {
    return (((version - 0x30000u) << 14) & 0xFFC00000u) | ((version & 0x3Fu) << 16) | 0x37u;
}
RwCamera* readCamera(bool* positionedAt = nullptr) {
    takeError();   // the first error is held until read: start each case with none
    RwMemory memory{g_bytes.data(), g_bytes.size()};
    RwStream* s = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory);
    require(s != nullptr, "a memory stream");
    RwCamera* c = RwCameraStreamRead(s);
    if (positionedAt != nullptr) {
        RwUInt32 marker = 0;
        *positionedAt = RwStreamRead(s, &marker, 4) == 4 && marker == 0x4D41524Bu;
    }
    RwStreamClose(s, nullptr);
    return c;
}
void cameraStruct(RwUInt32 length, RwUInt32 version, RwUInt32 projection) {
    put(rwID_STRUCT), put(length), put(libraryID(version));
    const RwReal fields[7] = {1.7777778f, 1.0f, 0.25f, -0.5f, 0.5f, 900.0f, 450.0f};
    for (RwUInt32 i = 0; i < length / 4; ++i) put(i < 7 ? std::bit_cast<RwUInt32>(fields[i]) : i == 7 ? projection : 0xEEEEEEEEu);
}
void streams() {
    startEngine();
    // a 40-byte struct (8 bytes beyond the camera's 32), plugin data, then a marker: fields from the first 32 bytes,
    // the stream left after the extension
    g_bytes.clear();
    cameraStruct(40, 0x36003u, rwPARALLEL);
    put(rwID_EXTENSION), put(16), put(libraryID(0x36003u));
    put(kPluginID), put(4), put(libraryID(0x36003u)), put(0xFEEDu);
    put(0x4D41524Bu);
    bool positioned = false;
    RwCamera* c = readCamera(&positioned);
    check(c != nullptr && positioned, "a long struct is read into 32 bytes and skipped");
    check(c->viewWindow.x == 1.7777778f && c->viewOffset.y == -0.5f && c->nearPlane == 0.5f && c->farPlane == 900.0f &&
              c->fogPlane == 450.0f && c->projectionType == rwPARALLEL && pluginWord(c) == 0xFEEDu,
          "the fields and the plugin data");
    RwCameraDestroy(c);
    // a bad version: NULL, E_RW_BADVERSION, no camera made
    g_bytes.clear();
    cameraStruct(32, 0x34000u, rwPERSPECTIVE);
    put(rwID_EXTENSION), put(0), put(libraryID(0x36003u));
    const int made = g_constructs;
    check(readCamera() == nullptr && takeError().errorCode == static_cast<RwInt32>(0x80000004u) && g_constructs == made,
          "a bad version");
    // truncated: the struct says 32 bytes, 12 follow
    g_bytes.clear();
    put(rwID_STRUCT), put(32), put(libraryID(0x36003u)), put(0), put(0), put(0);
    check(readCamera() == nullptr && g_constructs == made, "a truncated struct");
    // a failing plugin read: NULL and the camera destroyed (the original leaks it)
    g_bytes.clear();
    cameraStruct(32, 0x36003u, rwPERSPECTIVE);
    put(rwID_EXTENSION), put(16), put(libraryID(0x36003u));
    put(kPluginID), put(4), put(libraryID(0x36003u)), put(1);
    g_fail_plugin_read = true;
    const int destructs = g_destructs;
    check(readCamera() == nullptr && g_destructs == destructs + 1, "a failed plugin read destroys the camera");
    g_fail_plugin_read = false;
    // an invalid projection in the stream: the camera, with E_RW_BADPARAM (as the original), perspective kept
    g_bytes.clear();
    cameraStruct(32, 0x37002u, 9);
    put(rwID_EXTENSION), put(0), put(libraryID(0x37002u));
    c = readCamera();
    check(c != nullptr && takeError().errorCode == E_RW_BADPARAM && c->projectionType == rwPERSPECTIVE, "an invalid projection");
    RwCameraDestroy(c);
    // a short struct: the missing fields read as zero
    g_bytes.clear();
    cameraStruct(8, 0x36003u, 0);
    put(rwID_EXTENSION), put(0), put(libraryID(0x36003u));
    c = readCamera();
    check(c != nullptr && c->viewWindow.x == 1.7777778f && c->nearPlane == 0 && c->farPlane == 0 && takeError().errorCode == E_RW_BADPARAM,
          "a short struct");
    RwCameraDestroy(c);
    stopEngine("streams");
}

}  // namespace

int main() {
    try {
        mathematics();
        aspects();
        attachCycles();
        lifecycle();
        streams();
        std::printf("rwcore_camera_native_tests: %u checks passed (no allocation outlives a shutdown)\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_camera_native_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
