// RenderWare Graphics 3.7 core: matrices (module bamatrix), native x64.
//
// Rebuilt from the game's own RenderWare functions (PAL main.dol), not from the GameCube hardware path: the originals
// are paired-single or scalar Gekko code; this is plain C++ with the same arithmetic in the same order (fused
// multiply-adds where the original fuses, plain single operations where it does not), so results are bit-identical
// for finite inputs. Where an original interleaves loads and stores, the port keeps the order, so in-place calls
// (dst == src) give the original's result too. Proof: runtime/tests/rwcore_matrix_tests.cpp. Names and constants
// are RenderWare's own (RW37SDK rwplcore.h).
//
// Not provided: the bamatrix functions the game does not link (RwMatrixQueryRotate, _rwMatrixSetMultFn,
// _rwMatrixSetOptimizations, _rwMatrixIdentityError, RwEngineGetMatrixTolerances, RwMatrixSetFreeListCreateParams).
#pragma once

#include "rw/core/engine.h"
#include "rw/core/types.h"

/// RenderWare's 4x4 affine matrix, stored as four 16-byte rows (row vectors: p' = p * M).
/// The layout holds no pointers, so it is identical on the GameCube and on x64.
struct RwMatrix {
    RwV3d right;
    RwUInt32 flags;  ///< RwMatrixType bits | RwMatrixFlag bits
    RwV3d up;
    RwUInt32 pad1;
    RwV3d at;
    RwUInt32 pad2;
    RwV3d pos;
    RwUInt32 pad3;
};
static_assert(sizeof(RwMatrix) == 64, "RwMatrix must keep RenderWare's 64-byte layout");

enum RwMatrixType : RwUInt32 {
    rwMATRIXTYPENORMAL = 0x00000001,
    rwMATRIXTYPEORTHOGONAL = 0x00000002,
    rwMATRIXTYPEORTHONORMAL = 0x00000003,
    rwMATRIXTYPEMASK = 0x00000003,
};

enum RwMatrixFlag : RwUInt32 {
    rwMATRIXINTERNALIDENTITY = 0x00020000,
};

enum RwMatrixOptimizations : RwInt32 {
    rwMATRIXOPTIMIZE_IDENTITY = 0x00020000,
};

/// How a transformation combines with a matrix (rwplcore.h RwOpCombineType).
enum RwOpCombineType : RwInt32 {
    rwCOMBINEREPLACE = 0,
    rwCOMBINEPRECONCAT = 1,
    rwCOMBINEPOSTCONCAT = 2,
};

/// The error bounds RwMatrixOptimize tests against (rwplcore.h RwMatrixTolerance).
struct RwMatrixTolerance {
    RwReal Normal;
    RwReal Orthogonal;
    RwReal Identity;
};

using rwMatrixMultFn = void (*)(RwMatrix* dstMat, const RwMatrix* matA, const RwMatrix* matB);

/// The matrix module's globals in the engine instance, as the originals use them (+0 free list, +4 optimizations,
/// +8 multiply function, +0xC tolerances on the GameCube).
struct rwMatrixGlobals {
    RwFreeList* matrixFreeList;
    RwInt32 matrixOptimizations;
    rwMatrixMultFn multMatrix;
    RwMatrixTolerance tolerance;
};

extern RwModuleInfo matrixModule;

/// Opens the module in an engine instance (its plugin-registry open callback): the matrix free list (64-byte entries,
/// 256 to a block, aligned to 4, one block preallocated, a static in-place header, hint rwMEMHINTDUR_GLOBAL |
/// rwID_MATRIX), identity optimization on, MatrixMultiply, tolerances 0.01. NULL when the free list cannot be made.
/// Original: 0x8047F8DC.
void* _rwMatrixOpen(void* instance, RwInt32 offset, RwInt32 size);

/// Destroys the matrix free list. Returns instance. Original: 0x8047F878.
void* _rwMatrixClose(void* instance, RwInt32 offset, RwInt32 size);

/// RwMatrixCreate's memory hint (rwMEMHINTDUR_EVENT | rwID_MATRIX), as the original passes it.
inline constexpr RwUInt32 rwMATRIXCREATEHINT = 0x0003000Du;

/// dst = a * b. dst must not overlap a or b (RenderWare's contract). Only the four rows and dst->flags are written;
/// the pad words keep their contents. dst->flags = a->flags & b->flags. Returns dst. Original: 0x8047FC08.
RwMatrix* RwMatrixMultiply(RwMatrix* dst, const RwMatrix* a, const RwMatrix* b);

/// The module's scalar multiply (its multMatrix): dst = a * b, every row of a against the rows of b. dst may be a
/// or b. Writes the four rows only (not flags, not pads). Original: 0x8047F150 (bamatrix's static MatrixMultiply).
void MatrixMultiply(RwMatrix* dst, const RwMatrix* a, const RwMatrix* b);

/// Marks a matrix as modified: clears its type bits and the internal identity flag. Returns matrix.
/// Original: 0x8047FBF0.
RwMatrix* RwMatrixUpdate(RwMatrix* matrix);

/// The determinant of the 3x3 part. Original: 0x8047F754.
RwReal _rwMatrixDeterminant(const RwMatrix* matrix);

/// (right.up)^2 + (up.at)^2 + (at.right)^2 summed as the original does: 0 for an orthogonal matrix.
/// Original: 0x8047F7A0.
RwReal _rwMatrixOrthogonalError(const RwMatrix* matrix);

/// (|up|^2 - 1)^2 + (|right|^2 - 1)^2 + (|at|^2 - 1)^2: 0 for unit axes. Original: 0x8047F7F8.
RwReal _rwMatrixNormalError(const RwMatrix* matrix);

/// Sets matrix->flags from its errors: the normal bit when the normal error is within tolerance->Normal, the
/// orthogonal bit when the orthogonal error is within tolerance->Orthogonal, and rwMATRIXINTERNALIDENTITY when both
/// hold and the identity error is within tolerance->Identity. tolerance NULL: the engine's. Returns matrix.
/// Original: 0x8047F9E0.
const RwMatrix* RwMatrixOptimize(RwMatrix* matrix, const RwMatrixTolerance* tolerance);

/// Sets the engine's tolerances. Returns TRUE. Original: 0x8047F9B0.
RwBool RwEngineSetMatrixTolerances(const RwMatrixTolerance* tolerance);

/// Makes dst's 3x3 part orthonormal from src (axes normalized with _rwInvSqrt; the axis pair closest to orthogonal
/// is kept, the third rebuilt by cross products); pos is copied; dst->flags gets the orthonormal type and loses the
/// identity flag. dst may be src. Returns dst. Originals: 0x8047FD04 (RwMatrixOrthoNormalize) -> 0x8047F2E8.
RwMatrix* RwMatrixOrthoNormalize(RwMatrix* dst, const RwMatrix* src);

/// The rotation about unitAxis given 1 - cos and sin, combined with matrix by combineOp. Returns matrix, or NULL
/// for another combineOp (an RwError is set). The pad words: the original copies a temporary whose pads it never
/// set (stack contents); these write 0. Original: 0x8047FD24.
RwMatrix* RwMatrixRotateOneMinusCosineSine(RwMatrix* matrix, const RwV3d* unitAxis, RwReal oneMinusCosine,
                                           RwReal sine, RwOpCombineType combineOp);

/// The rotation by angle degrees about axis (normalized here; the game's MSL sin / cos), combined by combineOp.
/// Returns matrix, also when combineOp is invalid (the error is set, matrix unchanged). Original: 0x80480018.
RwMatrix* RwMatrixRotate(RwMatrix* matrix, const RwV3d* axis, RwReal angle, RwOpCombineType combineOp);

/// dst = the inverse of src: a copy when src carries the identity flag (and the engine optimizes identities), the
/// transpose and negated rotated translation when src is orthonormal, else by cofactors (1 / determinant; a zero
/// determinant counts as 1). dst->flags: src's / orthonormal / 0. dst may be src (RenderWare forbids it and the original then
/// returns a wrong inverse; the native reads from a copy). Returns dst. Original: 0x80480100.
RwMatrix* RwMatrixInvert(RwMatrix* dst, const RwMatrix* src);

/// The scale combined with matrix: replace (a scaling matrix), pre-concatenation (scales the axes) or
/// post-concatenation (scales every row's components). Clears the type bits and the identity flag. Returns matrix,
/// or NULL for another combineOp (an RwError is set; the original then also writes to address 0xC, which this does
/// not). Original: 0x804803E8.
RwMatrix* RwMatrixScale(RwMatrix* matrix, const RwV3d* scale, RwOpCombineType combineOp);

/// The translation combined with matrix: replace (a translation matrix), pre-concatenation (pos += translation in
/// the matrix's axes) or post-concatenation (pos += translation). Clears the identity flag. Returns matrix, or NULL
/// for another combineOp (as RwMatrixScale). Original: 0x804805EC.
RwMatrix* RwMatrixTranslate(RwMatrix* matrix, const RwV3d* translation, RwOpCombineType combineOp);

/// transform combined with matrix: replace (a copy, all 64 bytes), pre-concatenation (transform * matrix) or
/// post-concatenation (matrix * transform), through RwMatrixMultiply. Returns matrix, or NULL for another combineOp
/// (an RwError is set). The pads after a concatenation: 0 (the original's are stack contents). Original: 0x8048077C.
RwMatrix* RwMatrixTransform(RwMatrix* matrix, const RwMatrix* transform, RwOpCombineType combineOp);

/// An identity matrix from the engine's matrix free list (RwGlobals::memoryAlloc), or NULL. Original: 0x804809E4.
RwMatrix* RwMatrixCreate();

/// Returns matrix to the engine's matrix free list (RwGlobals::memoryFree). Returns TRUE. Original: 0x804809A8.
RwBool RwMatrixDestroy(RwMatrix* matrix);
