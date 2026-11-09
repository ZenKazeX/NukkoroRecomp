// SCAFFOLDING (R2): installs the Im2D line and triangle owners over the driver (im2d_prims.h) - deleted by R6
#include "im2d_prims.h"

#include "gekko/cpu.h"
#include "rw_driver.h"

namespace gekko::scaffold {

// Defined in rw_driver.cpp; the lowered sites reach it through the generated declarations.
u32 rw_im2d_render_indexed_primitive(Cpu& c, u32 type, u32 vertices, u32 count, u32 indices, u32 index_count);

void install_im2d_prim_owners(Cpu& cpu) {
    cpu.install_hle(kIm2DRenderLine, [](Cpu& c) {
        serve_im2d_line(c, rw_im2d_render_indexed_primitive);
        return true;
    }, "RwIm2DRenderLine (device table owner)");
    cpu.install_hle(kIm2DRenderTriangle, [](Cpu& c) {
        serve_im2d_triangle(c, rw_im2d_render_indexed_primitive);
        return true;
    }, "RwIm2DRenderTriangle (device table owner)");
}

}  // namespace gekko::scaffold
