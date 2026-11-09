// RenderWare core vectors (see rw/core/vector.h).
//
// The tables: entry i of a table covers the floats x whose (bits >> 12) & 0xFFF is i - the exponent's lowest bit and
// the top 11 mantissa bits - so x in [1, 2) fills entries 2048..4095 and x in [2, 4) entries 0..2047. An entry holds
// the result's bits minus the exponent part the lookup adds back ((bits >> 1) & 0x3FC00000, or that of ~bits for the
// inverse). The originals compute each square root as x * rsqrt(x): frsqrte refined by two Newton steps in double,
// rounded to single. frsqrte is the reference interpreter's model, 1 / sqrt(x) in double (an estimate the manual
// permits; the GameCube's own estimate is coarser before the Newton steps), so the tables are proven against that.
// Under that model the two Newton steps change no entry (one step, measured, gives the same tables): the oracle cannot
// tell them apart, and they are kept because the original runs them and the hardware estimate needs them.
#include "rw/core/vector.h"

#include <bit>
#include <cmath>

#include "rw/core/error.h"

RwModuleInfo vectorModule{};

namespace {

constexpr std::size_t kTableBytes = 0x4000;
constexpr RwUInt32 kTableHint = rwMEMHINTDUR_GLOBAL | rwID_VECTORMODULE;

/// x * rsqrt(x) as the original's loop computes it, rounded to single. The tables only take x in [1, 4); the
/// original's branches for x <= 0 are never reached.
RwReal tableSqrt(RwReal x) {
    const double v = x;
    double y = 1.0 / std::sqrt(v);                                   // frsqrte
    for (int step = 0; step < 2; ++step) {                           // y = 0.5y * (3 - v*y*y), fnmsub fused
        const double yy = y * y;
        const double half = 0.5 * y;
        const double t = -std::fma(v, yy, -3.0);
        y = half * t;
    }
    return static_cast<RwReal>(v * y);                               // frsp
}

/// One entry of the square-root (inverse = false) or inverse table for x.
RwUInt32 entry(RwReal x, bool inverse, RwUInt32 exponentPart) {
    const RwReal s = tableSqrt(x);
    const RwReal r = inverse ? 1.0f / s : s;                         // fdivs
    return std::bit_cast<RwUInt32>(r) - exponentPart;
}

/// A table: x from 1.0 in mantissa steps of 2^-11 over [1, 4).
RwUInt32* buildTable(bool inverse) {
    auto* table = static_cast<RwUInt32*>(RwMalloc(kTableBytes, kTableHint));
    if (table == nullptr) return nullptr;
    RwUInt32 bits = std::bit_cast<RwUInt32>(1.0f);
    // [1, 2) -> upper half, [2, 4) -> lower half; the exponent part differs by the halved exponent's rounding
    const RwUInt32 upper = inverse ? 0x20000000u : 0x1FC00000u;
    const RwUInt32 lower = inverse ? 0x1FC00000u : 0x20000000u;
    for (int i = 0; i < 2048; ++i, bits += 0x1000u) table[2048 + i] = entry(std::bit_cast<RwReal>(bits), inverse, upper);
    for (int i = 0; i < 2048; ++i, bits += 0x1000u) table[i] = entry(std::bit_cast<RwReal>(bits), inverse, lower);
    return table;
}

void reportNoMemory() {
    RwError error{rwID_COREPLUGIN, _rwerror(E_RW_NOMEM, static_cast<RwInt32>(kTableBytes))};
    RwErrorSet(&error);
}

/// The table lookup of _rwSqrt / _rwInvSqrt.
RwReal lookup(RwReal x, const RwUInt32* table, bool inverse) {
    RwUInt32 u = std::bit_cast<RwUInt32>(x);
    if (u == 0) return x;
    u += 0x800u;                                                     // round to the table's 11 mantissa bits
    const RwUInt32 exponent = ((inverse ? ~u : u) >> 1) & 0x3FC00000u;
    return std::bit_cast<RwReal>(exponent + table[(u >> 12) & 0xFFFu]);
}

inline RwReal combine(RwReal x, RwReal y, RwReal z, RwReal right, RwReal up, RwReal at) {
    return std::fmaf(z, at, std::fmaf(y, up, x * right));
}

inline RwReal combinePoint(RwReal x, RwReal y, RwReal z, RwReal right, RwReal up, RwReal at, RwReal pos) {
    return std::fmaf(z, at, std::fmaf(y, up, std::fmaf(x, right, pos)));
}

inline RwV3d pointTimes(const RwV3d& p, const RwMatrix& m) {
    return {combinePoint(p.x, p.y, p.z, m.right.x, m.up.x, m.at.x, m.pos.x),
            combinePoint(p.x, p.y, p.z, m.right.y, m.up.y, m.at.y, m.pos.y),
            combinePoint(p.x, p.y, p.z, m.right.z, m.up.z, m.at.z, m.pos.z)};
}

inline RwV3d vectorTimes(const RwV3d& v, const RwMatrix& m) {
    return {combine(v.x, v.y, v.z, m.right.x, m.up.x, m.at.x),
            combine(v.x, v.y, v.z, m.right.y, m.up.y, m.at.y),
            combine(v.x, v.y, v.z, m.right.z, m.up.z, m.at.z)};
}

}  // namespace

void* _rwVectorOpen(void* instance, RwInt32 offset, RwInt32 /*size*/) {
    vectorModule.globalsOffset = offset;
    RwUInt32* sqrtTab = buildTable(false);
    if (sqrtTab == nullptr) {
        reportNoMemory();
        return nullptr;
    }
    rwModuleGlobals<rwVectorGlobals>(vectorModule).SqrtTab = sqrtTab;
    RwUInt32* invSqrtTab = buildTable(true);
    if (invSqrtTab == nullptr) {         // the original leaks the square-root table here (no close follows a failed open)
        reportNoMemory();
        RwFree(sqrtTab);
        rwModuleGlobals<rwVectorGlobals>(vectorModule).SqrtTab = nullptr;
        return nullptr;
    }
    rwModuleGlobals<rwVectorGlobals>(vectorModule).InvSqrtTab = invSqrtTab;
    ++vectorModule.numInstances;
    return instance;
}

void* _rwVectorClose(void* instance, RwInt32 /*offset*/, RwInt32 /*size*/) {
    rwVectorGlobals& globals = rwModuleGlobals<rwVectorGlobals>(vectorModule);
    if (globals.InvSqrtTab != nullptr) {
        RwFree(globals.InvSqrtTab);
        globals.InvSqrtTab = nullptr;
    }
    if (globals.SqrtTab != nullptr) {
        RwFree(globals.SqrtTab);
        globals.SqrtTab = nullptr;
    }
    --vectorModule.numInstances;
    return instance;
}

RwReal _rwSqrt(RwReal x) { return lookup(x, rwModuleGlobals<rwVectorGlobals>(vectorModule).SqrtTab, false); }

RwReal _rwInvSqrt(RwReal x) { return lookup(x, rwModuleGlobals<rwVectorGlobals>(vectorModule).InvSqrtTab, true); }

RwReal RwV2dLength(const RwV2d* in) {
    const RwReal xx = in->x * in->x;
    const RwReal yy = in->y * in->y;
    const RwReal sum = xx + yy;
    return _rwSqrt(sum);
}

namespace {

/// x * x + y * y + z * z as the originals sum it: z * z + (x * x + y * y), single operations, not fused.
RwReal lengthSquared(const RwV3d& v) {
    const RwReal xx = v.x * v.x;
    const RwReal yy = v.y * v.y;
    const RwReal zz = v.z * v.z;
    const RwReal sum = xx + yy;
    return zz + sum;
}

}  // namespace

RwReal RwV3dNormalize(RwV3d* out, const RwV3d* in) {
    const RwReal squared = lengthSquared(*in);
    const RwReal length = _rwSqrt(squared);
    const RwReal invLength = _rwInvSqrt(squared);
    const RwV3d v = *in;                                             // all three read before out is written
    out->x = v.x * invLength;
    out->y = v.y * invLength;
    out->z = v.z * invLength;
    if (length <= 0.0f) {
        RwError error{rwID_COREPLUGIN, _rwerror(E_RW_ZEROLENGTH)};
        RwErrorSet(&error);
    }
    return length;
}

RwReal RwV3dLength(const RwV3d* in) { return _rwSqrt(lengthSquared(*in)); }

RwV3d* RwV3dTransformPoint(RwV3d* pointOut, const RwV3d* pointIn, const RwMatrix* matrix) {
    *pointOut = pointTimes(*pointIn, *matrix);
    return pointOut;
}

RwV3d* RwV3dTransformPoints(RwV3d* pointsOut, const RwV3d* pointsIn, RwInt32 numPoints, const RwMatrix* matrix) {
    if (numPoints <= 0) return pointsOut;
    RwV3d current = pointsIn[0];
    for (RwInt32 i = 0; i < numPoints; ++i) {
        const RwV3d next = i + 1 < numPoints ? pointsIn[i + 1] : RwV3d{};  // read before the store, as the original
        pointsOut[i] = pointTimes(current, *matrix);
        current = next;
    }
    return pointsOut;
}

RwV3d* RwV3dTransformVector(RwV3d* vectorOut, const RwV3d* vectorIn, const RwMatrix* matrix) {
    *vectorOut = vectorTimes(*vectorIn, *matrix);
    return vectorOut;
}
