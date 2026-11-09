// SCAFFOLDING (R2): the Im2D line and triangle emitters of the RenderWare driver - deleted by R6: the lift of RenderWare's immediate mode owns them
#pragma once

// The Im2D emitters the device's function table reaches (data words 0x8056F524 / 0x8056F528), owned natively
// (docs/agents/os-other.md, step 12).
//
// Original (main.dol, PAL GUPP8P), RwIm2DRenderLine(vertices, numVertices, v1, v2) 0x80491D0C and
// RwIm2DRenderTriangle(vertices, numVertices, v1, v2, v3) 0x80491B08: no validation of any kind. Vertex k of the 24-byte
// RwIm2DVertex array is `vertices + k * 0x18`; the body sets up GX state (0x8049148c), GXBegin(GX_LINES 0xA8 / GX_TRIANGLES
// 0x90, format 0, 2 / 3 vertices), stores the vertices in the order v1, v2 (, v3) to the GX FIFO (position, RGBA, u, v;
// the u, v pair only when the device record at *(r13 + 0x620C) says the pass is textured), restores the viewport and
// scissor (0x80491774) and returns 1. The GX FIFO is hardware the lifted build refuses, so today an entry would stop the game.
//
// Owner: the same draw the primitive entry GcnIm2DRenderIndexedPrimitive makes, as a line list (RenderWare primitive type
// 1) or triangle list (type 3) over the vertex array with the indices (v1, v2 [, v3]); r3 = 1.
//   - The vertex array count handed to the immediate path is max(numVertices, highest index + 1), so an index past
//     numVertices draws the vertex the original would have read from memory instead of being refused.
//   - The two or three indices are written to guest memory just below the caller's stack pointer (the immediate path
//     reads indices from guest memory): garbage the original's own callees would have overwritten there.
//   - Not carried (stated differences): the GX state writes, the FIFO stores and the viewport/scissor restore - no consumer
//     of the GX state remains in the lifted build (the GX owners are sinks) and the native renderer takes the draw as values.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::scaffold {

constexpr u32 kIm2DRenderTriangle = 0x80491B08u;
constexpr u32 kIm2DRenderLine = 0x80491D0Cu;
constexpr u32 kRwLineList = 1u;
constexpr u32 kRwTriList = 3u;
constexpr u32 kIm2DVertexBytes = 0x18u;

// The sink a draw goes to: GcnIm2DRenderIndexedPrimitive's arguments (type, vertices, count, indices, index count).
using Im2dIndexedSink = u32 (*)(Cpu& cpu, u32 type, u32 vertices, u32 count, u32 indices, u32 index_count);

void serve_im2d_line(Cpu& cpu, Im2dIndexedSink sink);
void serve_im2d_triangle(Cpu& cpu, Im2dIndexedSink sink);

// Installs both owners over rw_im2d_render_indexed_primitive (im2d_prims_install.cpp).
void install_im2d_prim_owners(Cpu& cpu);

}  // namespace gekko::scaffold
