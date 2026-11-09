// SCAFFOLDING (R2): RwRasterRender, the driver's raster-to-target blit - deleted by R6: the lift of RenderWare's raster path owns it
#pragma once
//
// Original (read in full from the pinned DOL; docs/agents/os-other.md step 13):
//   r3 = source raster, r4 = rect {x, y, w, h} (words), r5 = scaled flag, r6 = fast (alpha blended) flag.
//   rect = words 0, 1 of r4; words 2, 3 from r4 when r5 != 0, else the raster's width / height (+0xC, +0x10).
//   T = byte +0x20 of the record at *(r13 + 0x6210) (the current target), S = that byte of the raster:
//     T == 2        : S == 0, 4, 5 draw; S == 1, 2 error (site B); S == 3 or S >= 6 nothing, return 1.
//     T == 0, 4, 5  : S == 3 or S >= 6 nothing, return 1; S == 0, 1, 2, 4, 5 error (site A).
//     any other T   : error (site C).
//   An error is RwErrorSet of {1, message} - message = 0x8047EC78(2, 0x8056FB9C) - and the function returns 0; every other
//   path returns 1. Nothing else is written to guest memory but those 8 stack bytes.
// The draw body (0x80499394, one caller: this) is carried by RwDriver::raster_quad.
#include "gekko/types.h"

namespace gekko {
class Cpu;
}

namespace gekko::scaffold {

constexpr u32 kRasterRenderCore = 0x80499924;
constexpr u32 kRasterTargetWord = 0x6210;  // r13-relative: the current target's record
constexpr u32 kRasterTypeByte = 0x20;

enum class RasterErrorSite : unsigned { A = 0, B = 1, C = 2 };

// The draw: RwDriver::raster_quad (the rect holds the four words after the unscaled substitution).
using RasterQuadSink = void (*)(Cpu&, u32 raster, const u32 rect[4], bool scaled, bool fast);
// The RW core's error record for a site; false when the guest call stopped the run.
using RasterErrorSink = bool (*)(Cpu&, RasterErrorSite site);

// The core, entered with the original's registers. Leaves r3 = 1 / 0 as the original does.
void serve_raster_render(Cpu& cpu, RasterQuadSink quad, RasterErrorSink error);

void install_raster_render_owner(Cpu& cpu);

}  // namespace gekko::scaffold
