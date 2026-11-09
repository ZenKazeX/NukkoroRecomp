// RenderWare Graphics 3.7 core: vectors (module bavector), native x64.
//
// Rebuilt from the game's own functions (PAL main.dol); every function here is proven bit-identical to the original
// instructions by runtime/tests/rwcore_vector_tests.cpp. RenderWare's square roots are table lookups: _rwVectorOpen
// builds two 4096-entry tables in the vector module's globals, as the original does, and _rwSqrt / _rwInvSqrt /
// RwV2dLength / RwV3dLength / RwV3dNormalize index them. Functions of bavector that the game does not link
// (RwV2dNormalize, RwV3dTransformVectors, _rwVectorSetMultFn) have no original here and are not provided.
#pragma once

#include "rw/core/engine.h"
#include "rw/core/matrix.h"
#include "rw/core/types.h"

/// The vector module's globals in the engine instance: the square-root and inverse-square-root tables.
/// Entry i covers the floats whose exponent LSB and top 11 mantissa bits are i ((bits >> 12) & 0xFFF).
struct rwVectorGlobals {
    RwUInt32* SqrtTab;
    RwUInt32* InvSqrtTab;
};

/// The module's registration: its globals' offset in RwEngineInstance and its open count.
extern RwModuleInfo vectorModule;

/// Opens the vector module for an engine instance: allocates and fills both tables (RwMalloc, 0x4000 bytes each).
/// Returns instance, or NULL when an allocation fails (an RwError is set). Original: 0x8048363C.
void* _rwVectorOpen(void* instance, RwInt32 offset, RwInt32 size);

/// Closes the vector module: frees both tables (the inverse first) and clears them. Returns instance.
/// Original: 0x80483598.
void* _rwVectorClose(void* instance, RwInt32 offset, RwInt32 size);

/// sqrt(x) by table: x's bits + 0x800, the table entry plus the halved exponent. +0 returns +0. Original: 0x80483390.
RwReal _rwSqrt(RwReal x);

/// 1 / sqrt(x) by table, as _rwSqrt with the complemented exponent. +0 returns +0. Original: 0x804833E4.
RwReal _rwInvSqrt(RwReal x);

/// |in|: x * x + y * y (single multiplies and add, not fused), then _rwSqrt. Original: 0x80483440.
RwReal RwV2dLength(const RwV2d* in);

/// out = in / |in| (in times _rwInvSqrt of its squared length; out may equal in). Returns |in| (_rwSqrt); a length
/// <= 0 sets E_RW_ZEROLENGTH (out is still written). Original: 0x8048327C.
RwReal RwV3dNormalize(RwV3d* out, const RwV3d* in);

/// |in|: _rwSqrt of z * z + (x * x + y * y) (single operations, not fused). Original: 0x80483350.
RwReal RwV3dLength(const RwV3d* in);

/// pointOut = pointIn * matrix, translation included. pointOut may equal pointIn. Returns pointOut.
/// Original: 0x804834A8. The name aligner first called it RwV3dTransformVector; the code adds matrix->pos, which is
/// RwV3dTransformPoint's contract (RenderWare API reference), so the name follows the code.
RwV3d* RwV3dTransformPoint(RwV3d* pointOut, const RwV3d* pointIn, const RwMatrix* matrix);

/// pointsOut[i] = pointsIn[i] * matrix for numPoints >= 1 points (the original loops 2^32 times on 0; this does
/// nothing). Each input is read before the previous output is stored, as the original's pipeline does, so pointsOut
/// may equal pointsIn or overlap it one point ahead. Returns pointsOut. Original: 0x804834F4.
RwV3d* RwV3dTransformPoints(RwV3d* pointsOut, const RwV3d* pointsIn, RwInt32 numPoints, const RwMatrix* matrix);

/// vectorOut = vectorIn * matrix, no translation. vectorOut may equal vectorIn. Returns vectorOut.
/// Original: 0x80483554.
RwV3d* RwV3dTransformVector(RwV3d* vectorOut, const RwV3d* vectorIn, const RwMatrix* matrix);
