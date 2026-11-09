// RenderWare Graphics 3.7 core: geometric primitives (rwplcore.h / rwcore.h) and bounding boxes (module babbox),
// native x64. Proof: runtime/tests/rwcore_camera_tests.cpp (RwBBoxCalculate runs inside the camera sync).
#pragma once

#include "rw/core/types.h"

/// A plane: points p with dot(p, normal) == distance (rwplcore.h RwPlane).
struct RwPlane {
    RwV3d normal;
    RwReal distance;
};

/// A sphere (rwplcore.h RwSphere).
struct RwSphere {
    RwV3d center;
    RwReal radius;
};

/// An axis-aligned box (rwcore.h RwBBox): sup is the maximum corner, inf the minimum, in this order.
struct RwBBox {
    RwV3d sup;
    RwV3d inf;
};

/// The box around numVertices points (both bounds start at the first; NaN coordinates never replace a bound, as the
/// original's unordered compares skip them). numVertices >= 1. Returns boundBox. Original: 0x804844EC.
RwBBox* RwBBoxCalculate(RwBBox* boundBox, const RwV3d* verts, RwInt32 numVertices);
