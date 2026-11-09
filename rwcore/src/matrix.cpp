// RenderWare core matrices (see rw/core/matrix.h).
//
// Arithmetic order is the original's, operation by operation. The paired-single RwMatrixMultiply multiplies the first
// term on its own and accumulates the others with fused multiply-adds (ps_muls / ps_madds), so each component is
//     fma(z, M.at, fma(y, M.up, x * M.right))           for a direction row,
//     fma(z, M.at, fma(y, M.up, fma(x, M.right, M.pos))) for a point (translation row).
// The scalar MatrixMultiply and the other functions use their own orders, written out below. fmadds / fmsubs /
// fnmadds / fnmsubs round once (std::fmaf); fmuls / fadds / fsubs / fdivs are plain single operations.
#include "rw/core/matrix.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "math/msl.h"
#include "rw/core/error.h"
#include "rw/core/memory.h"
#include "rw/core/vector.h"

RwModuleInfo matrixModule{};

namespace {

inline RwReal fmadds(RwReal a, RwReal c, RwReal b) { return std::fmaf(a, c, b); }       // a * c + b
inline RwReal fmsubs(RwReal a, RwReal c, RwReal b) { return std::fmaf(a, c, -b); }      // a * c - b
inline RwReal fnmadds(RwReal a, RwReal c, RwReal b) { return -std::fmaf(a, c, b); }     // -(a * c + b)
inline RwReal fnmsubs(RwReal a, RwReal c, RwReal b) { return -std::fmaf(a, c, -b); }    // -(a * c - b)

rwMatrixGlobals& globals() { return rwModuleGlobals<rwMatrixGlobals>(matrixModule); }

/// One component of (x, y, z) * M without translation (paired-single order).
inline RwReal combine(RwReal x, RwReal y, RwReal z, RwReal right, RwReal up, RwReal at) {
    return std::fmaf(z, at, std::fmaf(y, up, x * right));
}

/// One component of (x, y, z, 1) * M (paired-single order).
inline RwReal combinePoint(RwReal x, RwReal y, RwReal z, RwReal right, RwReal up, RwReal at, RwReal pos) {
    return std::fmaf(z, at, std::fmaf(y, up, std::fmaf(x, right, pos)));
}

inline RwV3d rowTimes(const RwV3d& row, const RwMatrix& m) {
    return {combine(row.x, row.y, row.z, m.right.x, m.up.x, m.at.x),
            combine(row.x, row.y, row.z, m.right.y, m.up.y, m.at.y),
            combine(row.x, row.y, row.z, m.right.z, m.up.z, m.at.z)};
}

inline RwV3d pointTimes(const RwV3d& p, const RwMatrix& m) {
    return {combinePoint(p.x, p.y, p.z, m.right.x, m.up.x, m.at.x, m.pos.x),
            combinePoint(p.x, p.y, p.z, m.right.y, m.up.y, m.at.y, m.pos.y),
            combinePoint(p.x, p.y, p.z, m.right.z, m.up.z, m.at.z, m.pos.z)};
}

/// The scalar MatrixMultiply's row: fma(z, at, fma(x, right, y * up)).
inline RwV3d scalarRow(const RwV3d& r, const RwMatrix& b) {
    return {fmadds(r.z, b.at.x, fmadds(r.x, b.right.x, r.y * b.up.x)),
            fmadds(r.z, b.at.y, fmadds(r.x, b.right.y, r.y * b.up.y)),
            fmadds(r.z, b.at.z, fmadds(r.x, b.right.z, r.y * b.up.z))};
}

inline RwReal lengthSquared(const RwV3d& v) {             // z^2 + (x^2 + y^2)
    const RwReal xx = v.x * v.x;
    const RwReal yy = v.y * v.y;
    const RwReal zz = v.z * v.z;
    const RwReal xy = xx + yy;
    return zz + xy;
}

inline RwReal normalError(const RwMatrix& m) {
    const RwReal up = lengthSquared(m.up) - 1.0f;
    const RwReal right = lengthSquared(m.right) - 1.0f;
    const RwReal at = lengthSquared(m.at) - 1.0f;
    return fmadds(at, at, fmadds(right, right, up * up));
}

inline RwReal orthogonalError(const RwMatrix& m) {
    const RwReal atRight = fmadds(m.at.z, m.right.z, fmadds(m.at.x, m.right.x, m.at.y * m.right.y));
    const RwReal upAt = fmadds(m.up.z, m.at.z, fmadds(m.up.x, m.at.x, m.up.y * m.at.y));
    const RwReal rightUp = fmadds(m.right.z, m.up.z, fmadds(m.right.x, m.up.x, m.right.y * m.up.y));
    return fmadds(rightUp, rightUp, fmadds(upAt, upAt, atRight * atRight));
}

/// RwMatrixOptimize's identity error, summed as the original does.
inline RwReal identityError(const RwMatrix& m) {
    const RwReal upY = m.up.y - 1.0f;
    const RwReal rightX = m.right.x - 1.0f;
    const RwReal atZ = m.at.z - 1.0f;
    const RwReal up = m.up.z * m.up.z + fmadds(upY, upY, m.up.x * m.up.x);
    const RwReal right = m.right.z * m.right.z + fmadds(rightX, rightX, m.right.y * m.right.y);
    const RwReal at = fmadds(atZ, atZ, m.at.x * m.at.x + m.at.y * m.at.y);
    const RwReal posXY = m.pos.x * m.pos.x + m.pos.y * m.pos.y;
    const RwReal pos = m.pos.z * m.pos.z + posXY;
    return pos + (at + (right + up));
}

inline RwV3d scaled(const RwV3d& v, RwReal s) { return {v.x * s, v.y * s, v.z * s}; }

/// p x q as MatrixOrthoNormalize computes it (fmsubs).
inline RwV3d cross(const RwV3d& p, const RwV3d& q) {
    return {fmsubs(p.y, q.z, p.z * q.y), fmsubs(p.z, q.x, p.x * q.z), fmsubs(p.x, q.y, p.y * q.x)};
}

inline RwReal absolute(RwReal v) { return v >= 0.0f ? v : -v; }   // the original's "if (!(v >= 0)) v = -v"

/// The core's identity rows and pos (RwMatrixCreate / RwMatrixScale / RwMatrixTranslate), stored in their order.
void storeIdentity(RwMatrix* m) {
    m->at.z = 1.0f;
    m->up.y = 1.0f;
    m->right.x = 1.0f;
    m->up.x = 0.0f;
    m->right.z = 0.0f;
    m->right.y = 0.0f;
    m->at.y = 0.0f;
    m->at.x = 0.0f;
    m->up.z = 0.0f;
    m->pos.z = 0.0f;
    m->pos.y = 0.0f;
    m->pos.x = 0.0f;
}

void badCombination() {
    RwError error{rwID_COREPLUGIN, _rwerror(E_RW_BADPARAM, "Invalid combination type")};
    RwErrorSet(&error);
}

/// The four rows and flags of src into dst; dst's pads become 0 (the originals copy a stack temporary's).
void copyRowsClearPads(RwMatrix* dst, const RwMatrix& src) {
    *dst = src;
    dst->pad1 = dst->pad2 = dst->pad3 = 0;
}

}  // namespace

RwMatrix* RwMatrixMultiply(RwMatrix* dst, const RwMatrix* a, const RwMatrix* b) {
    // all four rows are computed before any is stored, so dst may be a or b (RenderWare forbids the overlap; the
    // original's result with dst == b is undefined): the product is the same for every overlap
    const RwV3d right = rowTimes(a->right, *b), up = rowTimes(a->up, *b), at = rowTimes(a->at, *b);
    const RwV3d pos = pointTimes(a->pos, *b);
    const RwUInt32 flags = a->flags & b->flags;
    dst->right = right;
    dst->up = up;
    dst->at = at;
    dst->pos = pos;
    dst->flags = flags;
    return dst;
}

void MatrixMultiply(RwMatrix* dst, const RwMatrix* a, const RwMatrix* b) {
    // every input word is read before the store that could overwrite it in the original: locals are equivalent
    const RwV3d right = scalarRow(a->right, *b);
    const RwV3d up = scalarRow(a->up, *b);
    const RwV3d at = scalarRow(a->at, *b);
    const RwV3d p = scalarRow(a->pos, *b);
    const RwV3d pos{b->pos.x + p.x, b->pos.y + p.y, b->pos.z + p.z};
    dst->right = right;
    dst->up = up;
    dst->at = at;
    dst->pos = pos;
}

RwMatrix* RwMatrixUpdate(RwMatrix* matrix) {
    constexpr RwUInt32 modified = RwUInt32{rwMATRIXTYPEMASK} | RwUInt32{rwMATRIXINTERNALIDENTITY};
    matrix->flags &= ~modified;
    return matrix;
}

RwReal _rwMatrixDeterminant(const RwMatrix* m) {
    const RwReal a = fmsubs(m->up.z, m->at.x, m->up.x * m->at.z);
    const RwReal b = fmsubs(m->up.y, m->at.z, m->up.z * m->at.y);
    const RwReal c = fmsubs(m->up.x, m->at.y, m->up.y * m->at.x);
    return fmadds(c, m->right.z, fmadds(b, m->right.x, a * m->right.y));
}

RwReal _rwMatrixOrthogonalError(const RwMatrix* matrix) { return orthogonalError(*matrix); }

RwReal _rwMatrixNormalError(const RwMatrix* matrix) { return normalError(*matrix); }

const RwMatrix* RwMatrixOptimize(RwMatrix* matrix, const RwMatrixTolerance* tolerance) {
    if (tolerance == nullptr) tolerance = &globals().tolerance;
    const bool normal = tolerance->Normal >= normalError(*matrix);
    const bool orthogonal = tolerance->Orthogonal >= orthogonalError(*matrix);
    const bool identity = normal && orthogonal && tolerance->Identity >= identityError(*matrix);
    RwUInt32 flags = matrix->flags;
    flags = normal ? flags | rwMATRIXTYPENORMAL : flags & ~RwUInt32{rwMATRIXTYPENORMAL};
    flags = orthogonal ? flags | rwMATRIXTYPEORTHOGONAL : flags & ~RwUInt32{rwMATRIXTYPEORTHOGONAL};
    flags = identity ? flags | rwMATRIXINTERNALIDENTITY : flags & ~RwUInt32{rwMATRIXINTERNALIDENTITY};
    matrix->flags = flags;
    return matrix;
}

RwBool RwEngineSetMatrixTolerances(const RwMatrixTolerance* tolerance) {
    globals().tolerance = *tolerance;
    return 1;
}

RwMatrix* RwMatrixOrthoNormalize(RwMatrix* dst, const RwMatrix* src) {
    RwV3d right = src->right, up = src->up, at = src->at;
    const RwV3d pos = src->pos;
    const RwReal invRight = _rwInvSqrt(lengthSquared(right));
    right = scaled(right, invRight);
    const RwReal invUp = _rwInvSqrt(lengthSquared(up));
    up = scaled(up, invUp);
    const RwReal invAt = _rwInvSqrt(lengthSquared(at));
    at = scaled(at, invAt);

    // keep p, then r = p x q and q = r x p; the pair closest to orthogonal is (p, q)
    RwV3d *p, *q, *r;
    if (!(invRight > 0.0f)) {
        p = &up, q = &at, r = &right;
    } else if (!(invUp > 0.0f)) {
        p = &at, q = &right, r = &up;
    } else if (!(invAt > 0.0f)) {
        p = &right, q = &up, r = &at;
    } else {
        const RwReal upAt = absolute(fmadds(up.z, at.z, fmadds(up.x, at.x, up.y * at.y)));
        const RwReal atRight = absolute(fmadds(at.z, right.z, fmadds(at.x, right.x, at.y * right.y)));
        const RwReal rightUp = absolute(fmadds(right.z, up.z, fmadds(right.x, up.x, right.y * up.y)));
        if (!(upAt >= atRight)) {
            if (!(upAt >= rightUp)) {
                p = &up, q = &at, r = &right;
            } else {
                p = &right, q = &up, r = &at;
            }
        } else if (!(atRight >= rightUp)) {
            p = &at, q = &right, r = &up;
        } else {
            p = &right, q = &up, r = &at;
        }
    }
    *r = cross(*p, *q);
    *r = scaled(*r, _rwInvSqrt(lengthSquared(*r)));
    *q = cross(*r, *p);
    *q = scaled(*q, _rwInvSqrt(lengthSquared(*q)));

    dst->right = right;
    dst->up = up;
    dst->at = at;
    dst->pos = pos;
    dst->flags = (dst->flags | rwMATRIXTYPEORTHONORMAL) & ~RwUInt32{rwMATRIXINTERNALIDENTITY};
    return dst;
}

RwMatrix* RwMatrixRotateOneMinusCosineSine(RwMatrix* matrix, const RwV3d* unitAxis, RwReal oneMinusCosine,
                                           RwReal sine, RwOpCombineType combineOp) {
    const RwReal x = unitAxis->x, y = unitAxis->y, z = unitAxis->z;
    RwMatrix rotation{};
    rotation.flags = rwMATRIXTYPEORTHONORMAL;
    rotation.right.x = 1.0f - (1.0f - x * x) * oneMinusCosine;
    rotation.up.y = 1.0f - (1.0f - y * y) * oneMinusCosine;
    rotation.at.z = 1.0f - (1.0f - z * z) * oneMinusCosine;
    const RwReal yz = y * z * oneMinusCosine;
    const RwReal zx = z * x * oneMinusCosine;
    const RwReal xy = x * y * oneMinusCosine;
    const RwReal xs = x * sine, ys = y * sine, zs = z * sine;
    rotation.up.z = yz + xs;
    rotation.at.y = yz - xs;
    rotation.right.z = zx - ys;
    rotation.at.x = zx + ys;
    rotation.right.y = xy + zs;
    rotation.up.x = xy - zs;
    RwMatrix product{};
    switch (combineOp) {
        case rwCOMBINEREPLACE:
            copyRowsClearPads(matrix, rotation);
            return matrix;
        case rwCOMBINEPRECONCAT:
            RwMatrixMultiply(&product, &rotation, matrix);
            copyRowsClearPads(matrix, product);
            return matrix;
        case rwCOMBINEPOSTCONCAT:
            RwMatrixMultiply(&product, matrix, &rotation);
            copyRowsClearPads(matrix, product);
            return matrix;
    }
    badCombination();
    return nullptr;
}

RwMatrix* RwMatrixRotate(RwMatrix* matrix, const RwV3d* axis, RwReal angle, RwOpCombineType combineOp) {
    // kF32_0_01745329 (0x805FC320), by its bits: the decimal 0.01745329f rounds one ulp lower
    const RwReal kDegreesToRadians = std::bit_cast<RwReal>(0x3C8EFA35u);
    const RwReal radians = kDegreesToRadians * angle;
    const RwReal inverse = _rwInvSqrt(lengthSquared(*axis));
    const RwV3d unit = scaled(*axis, inverse);
    const RwReal sine = static_cast<RwReal>(shadow::math::msl::sin(radians));     // MSL sin, then frsp
    const RwReal cosine = static_cast<RwReal>(shadow::math::msl::cos(radians));   // MSL cos, then frsp
    RwMatrixRotateOneMinusCosineSine(matrix, &unit, 1.0f - cosine, sine, combineOp);
    return matrix;
}

RwMatrix* RwMatrixInvert(RwMatrix* dst, const RwMatrix* src) {
    // RenderWare forbids dst == src; the original then reads elements it has already overwritten and returns a wrong
    // inverse. Reading from a copy gives the right inverse for any overlap and the same bits as the original for
    // separate matrices (its loads and stores below keep their order, which no longer matters).
    const RwMatrix source = *src;
    src = &source;
    const RwUInt32 srcFlags = src->flags;
    if (srcFlags & static_cast<RwUInt32>(globals().matrixOptimizations) & rwMATRIXINTERNALIDENTITY) {
        RwUInt32 words[16];                                   // the original copies all 16 words, flags and pads
        std::memcpy(words, src, sizeof words);
        std::memcpy(dst, words, sizeof words);
        return dst;
    }
    if ((srcFlags & rwMATRIXTYPEMASK) == rwMATRIXTYPEORTHONORMAL) {
        // the transpose, in the original's load / store order
        const RwReal rx = src->right.x;
        const RwReal ux = src->up.x;
        dst->right.x = rx;
        const RwReal ax = src->at.x;
        dst->right.y = ux;
        const RwReal ry = src->right.y;
        dst->right.z = ax;
        const RwReal py = src->pos.y;
        const RwReal uy = src->up.y;
        dst->up.x = ry;
        RwReal sumX = py * ry;
        const RwReal ay = src->at.y;
        RwReal sumY = py * uy;
        dst->up.y = uy;
        const RwReal px = src->pos.x;
        RwReal sumZ = py * ay;
        dst->up.z = ay;
        const RwReal rz = src->right.z;
        sumX = fmadds(px, rx, sumX);
        const RwReal pz = src->pos.z;
        sumY = fmadds(px, ux, sumY);
        const RwReal uz = src->up.z;
        sumZ = fmadds(px, ax, sumZ);
        dst->at.x = rz;
        const RwReal az = src->at.z;
        sumX = fnmadds(pz, rz, sumX);
        dst->at.y = uz;
        sumY = fnmadds(pz, uz, sumY);
        sumZ = fnmadds(pz, az, sumZ);
        dst->at.z = az;
        dst->pos.x = sumX;
        dst->pos.y = sumY;
        dst->pos.z = sumZ;
        dst->flags = rwMATRIXTYPEORTHONORMAL;
        return dst;
    }
    // general: cofactors, in the original's load / store order
    {
        const RwReal uz = src->up.z;
        const RwReal ay = src->at.y;
        const RwReal rz = src->right.z;
        const RwReal uy = src->up.y;
        const RwReal uzay = uz * ay;
        const RwReal az = src->at.z;
        const RwReal rzay = rz * ay;
        const RwReal ry = src->right.y;
        const RwReal rzuy = rz * uy;
        const RwReal c0 = fmsubs(uy, az, uzay);
        const RwReal c1 = fnmsubs(ry, az, rzay);
        const RwReal ux = src->up.x;
        const RwReal c2 = fmsubs(ry, uz, rzuy);
        const RwReal rx = src->right.x;
        dst->right.x = c0;
        const RwReal ax = src->at.x;
        dst->right.y = c1;
        dst->right.z = c2;
        const RwReal d1 = dst->right.y;
        const RwReal d0 = dst->right.x;
        const RwReal t = d1 * ux;
        const RwReal d2 = dst->right.z;
        const RwReal determinant = fmadds(d2, ax, fmadds(d0, rx, t));
        const RwReal inverse = std::bit_cast<RwUInt32>(determinant) != 0 ? 1.0f / determinant : 1.0f;

        const RwReal e0 = dst->right.x;
        const RwReal uz2 = src->up.z;
        const RwReal r0 = e0 * inverse;
        const RwReal ax2 = src->at.x;
        const RwReal rz2 = src->right.z;
        const RwReal ux2 = src->up.x;
        RwReal k5 = uz2 * ax2;
        dst->right.x = r0;
        const RwReal ry2 = src->right.y;
        const RwReal k4 = rz2 * ax2;
        const RwReal e1 = dst->right.y;
        const RwReal k3 = rz2 * ux2;
        const RwReal az2 = src->at.z;
        RwReal k2 = ry2 * ax2;
        const RwReal r1 = e1 * inverse;
        const RwReal rx2 = src->right.x;
        k5 = fnmsubs(ux2, az2, k5);
        const RwReal uy2 = src->up.y;
        RwReal k1 = ry2 * ux2;
        dst->right.y = r1;
        const RwReal k10 = fmsubs(rx2, az2, k4);
        const RwReal ay2 = src->at.y;
        const RwReal e2 = dst->right.z;
        RwReal k6 = uy2 * ax2;
        const RwReal k9 = fnmsubs(rx2, uz2, k3);
        const RwReal py = src->pos.y;
        const RwReal r2 = e2 * inverse;
        const RwReal px = src->pos.x;
        const RwReal u0 = inverse * k5;
        const RwReal pz = src->pos.z;
        k6 = fmsubs(ux2, ay2, k6);
        dst->right.z = r2;
        const RwReal u1 = inverse * k10;
        dst->up.x = u0;
        const RwReal u2 = inverse * k9;
        k2 = fnmsubs(rx2, ay2, k2);
        dst->up.y = u1;
        const RwReal a0 = inverse * k6;
        k1 = fmsubs(rx2, uy2, k1);
        dst->up.z = u2;
        const RwReal a1 = inverse * k2;
        dst->at.x = a0;
        const RwReal a2 = inverse * k1;
        dst->at.y = a1;
        dst->at.z = a2;

        dst->pos.x = fnmadds(pz, dst->at.x, fmadds(px, dst->right.x, py * dst->up.x));
        dst->pos.y = fnmadds(pz, dst->at.y, fmadds(px, dst->right.y, py * dst->up.y));
        dst->pos.z = fnmadds(pz, dst->at.z, fmadds(px, dst->right.z, py * dst->up.z));
        dst->flags = 0;
    }
    return dst;
}

RwMatrix* RwMatrixScale(RwMatrix* matrix, const RwV3d* scale, RwOpCombineType combineOp) {
    switch (combineOp) {
        case rwCOMBINEREPLACE: {
            matrix->at.z = 1.0f;
            const RwReal sx = scale->x;
            matrix->up.y = 1.0f;
            const RwReal sy = scale->y;
            matrix->right.x = 1.0f;
            const RwReal sz = scale->z;
            matrix->up.x = 0.0f;
            matrix->right.z = 0.0f;
            matrix->right.y = 0.0f;
            matrix->at.y = 0.0f;
            matrix->at.x = 0.0f;
            matrix->up.z = 0.0f;
            matrix->pos.z = 0.0f;
            matrix->pos.y = 0.0f;
            matrix->pos.x = 0.0f;
            matrix->flags |= RwUInt32{rwMATRIXINTERNALIDENTITY} | RwUInt32{rwMATRIXTYPEORTHONORMAL};
            matrix->right.x = sx;
            matrix->up.y = sy;
            matrix->at.z = sz;
            break;
        }
        case rwCOMBINEPRECONCAT: {
            const RwReal right = matrix->right.x;
            const RwReal sx = scale->x, sy = scale->y, sz = scale->z;
            matrix->right.x = right * sx;
            matrix->right.y = matrix->right.y * sx;
            matrix->right.z = matrix->right.z * sx;
            matrix->up.x = matrix->up.x * sy;
            matrix->up.y = matrix->up.y * sy;
            matrix->up.z = matrix->up.z * sy;
            matrix->at.x = matrix->at.x * sz;
            matrix->at.y = matrix->at.y * sz;
            matrix->at.z = matrix->at.z * sz;
            break;
        }
        case rwCOMBINEPOSTCONCAT: {
            const RwReal right = matrix->right.x;
            const RwReal sx = scale->x, sy = scale->y, sz = scale->z;
            matrix->right.x = right * sx;
            matrix->right.y = matrix->right.y * sy;
            matrix->right.z = matrix->right.z * sz;
            matrix->up.x = matrix->up.x * sx;
            matrix->up.y = matrix->up.y * sy;
            matrix->up.z = matrix->up.z * sz;
            matrix->at.x = matrix->at.x * sx;
            matrix->at.y = matrix->at.y * sy;
            matrix->at.z = matrix->at.z * sz;
            matrix->pos.x = matrix->pos.x * sx;
            matrix->pos.y = matrix->pos.y * sy;
            matrix->pos.z = matrix->pos.z * sz;
            break;
        }
        default:
            badCombination();
            return nullptr;
    }
    matrix->flags &= ~(RwUInt32{rwMATRIXINTERNALIDENTITY} | RwUInt32{rwMATRIXTYPEMASK});
    return matrix;
}

RwMatrix* RwMatrixTranslate(RwMatrix* matrix, const RwV3d* translation, RwOpCombineType combineOp) {
    switch (combineOp) {
        case rwCOMBINEREPLACE: {
            matrix->at.z = 1.0f;
            const RwReal tx = translation->x;
            matrix->up.y = 1.0f;
            const RwReal ty = translation->y;
            matrix->right.x = 1.0f;
            const RwReal tz = translation->z;
            matrix->up.x = 0.0f;
            matrix->right.z = 0.0f;
            matrix->right.y = 0.0f;
            matrix->at.y = 0.0f;
            matrix->at.x = 0.0f;
            matrix->up.z = 0.0f;
            matrix->pos.z = 0.0f;
            matrix->pos.y = 0.0f;
            matrix->pos.x = 0.0f;
            matrix->flags |= RwUInt32{rwMATRIXINTERNALIDENTITY} | RwUInt32{rwMATRIXTYPEORTHONORMAL};
            matrix->pos.x = tx;
            matrix->pos.y = ty;
            matrix->pos.z = tz;
            break;
        }
        case rwCOMBINEPRECONCAT: {
            const RwReal ty = translation->y, tx = translation->x, tz = translation->z;
            matrix->pos.x = matrix->pos.x + fmadds(tz, matrix->at.x, fmadds(tx, matrix->right.x, ty * matrix->up.x));
            matrix->pos.y = matrix->pos.y + fmadds(tz, matrix->at.y, fmadds(tx, matrix->right.y, ty * matrix->up.y));
            matrix->pos.z = matrix->pos.z + fmadds(tz, matrix->at.z, fmadds(tx, matrix->right.z, ty * matrix->up.z));
            break;
        }
        case rwCOMBINEPOSTCONCAT: {
            const RwReal tx = translation->x, ty = translation->y, tz = translation->z;
            matrix->pos.x = matrix->pos.x + tx;
            matrix->pos.y = matrix->pos.y + ty;
            matrix->pos.z = matrix->pos.z + tz;
            break;
        }
        default:
            badCombination();
            return nullptr;
    }
    matrix->flags &= ~RwUInt32{rwMATRIXINTERNALIDENTITY};
    return matrix;
}

RwMatrix* RwMatrixTransform(RwMatrix* matrix, const RwMatrix* transform, RwOpCombineType combineOp) {
    RwMatrix product{};
    switch (combineOp) {
        case rwCOMBINEREPLACE: {
            RwUInt32 words[16];
            std::memcpy(words, transform, sizeof words);
            std::memcpy(matrix, words, sizeof words);
            return matrix;
        }
        case rwCOMBINEPRECONCAT:
            RwMatrixMultiply(&product, transform, matrix);
            copyRowsClearPads(matrix, product);
            return matrix;
        case rwCOMBINEPOSTCONCAT:
            RwMatrixMultiply(&product, matrix, transform);
            copyRowsClearPads(matrix, product);
            return matrix;
    }
    badCombination();
    return nullptr;
}

RwMatrix* RwMatrixCreate() {
    auto* matrix = static_cast<RwMatrix*>(rwSrcGlobals().memoryAlloc(globals().matrixFreeList, rwMATRIXCREATEHINT));
    if (matrix == nullptr) return nullptr;
    matrix->flags = rwMATRIXTYPEORTHONORMAL;
    storeIdentity(matrix);
    matrix->flags |= RwUInt32{rwMATRIXINTERNALIDENTITY} | RwUInt32{rwMATRIXTYPEORTHONORMAL};
    return matrix;
}

RwBool RwMatrixDestroy(RwMatrix* matrix) {
    rwSrcGlobals().memoryFree(globals().matrixFreeList, matrix);
    return 1;
}

namespace {

// The free list's creation parameters (bamatrix statics 0x805EEF48 / 0x805EEF4C; RwMatrixSetFreeListCreateParams,
// which would change them, is not linked) and its in-place header (0x805E3B18).
constexpr RwInt32 kFreeListBlockSize = 256;
constexpr RwInt32 kFreeListPreallocBlocks = 1;
constexpr RwInt32 kFreeListAlignment = 4;
constexpr RwUInt32 kFreeListHint = rwMEMHINTDUR_GLOBAL | 0x0000000Du;  // | rwID_MATRIX
RwFreeList matrixFreeListSpace{};

}  // namespace

void* _rwMatrixOpen(void* instance, RwInt32 offset, RwInt32 /*size*/) {
    matrixModule.globalsOffset = offset;
    globals().matrixFreeList =
        RwFreeListCreateAndPreallocateSpace(sizeof(RwMatrix), kFreeListBlockSize, kFreeListAlignment,
                                            kFreeListPreallocBlocks, &matrixFreeListSpace, kFreeListHint);
    if (globals().matrixFreeList == nullptr) return nullptr;
    const RwReal defaultTolerance = std::bit_cast<RwReal>(0x3C23D70Au);  // 0.01 (0x8051D3D8..)
    const RwMatrixTolerance tolerance{defaultTolerance, defaultTolerance, defaultTolerance};
    globals().matrixOptimizations = rwMATRIXOPTIMIZE_IDENTITY;
    globals().multMatrix = MatrixMultiply;
    RwEngineSetMatrixTolerances(&tolerance);
    ++matrixModule.numInstances;
    return instance;
}

void* _rwMatrixClose(void* instance, RwInt32 /*offset*/, RwInt32 /*size*/) {
    if (globals().matrixFreeList != nullptr) {
        RwFreeListDestroy(globals().matrixFreeList);
        globals().matrixFreeList = nullptr;
    }
    --matrixModule.numInstances;
    return instance;
}
