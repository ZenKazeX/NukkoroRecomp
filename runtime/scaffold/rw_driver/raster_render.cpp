// SCAFFOLDING (R2): RwRasterRender, the driver's raster-to-target blit - deleted by R6: the lift of RenderWare's raster path owns it
#include "raster_render.h"

#include "gekko/cpu.h"

namespace gekko::scaffold {

void serve_raster_render(Cpu& cpu, RasterQuadSink quad, RasterErrorSink error) {
    const u32 raster = cpu.gpr[3], source_rect = cpu.gpr[4];
    const bool scaled = cpu.gpr[5] != 0u, fast = cpu.gpr[6] != 0u;
    u32 rect[4] = {cpu.mem_read32(source_rect), cpu.mem_read32(source_rect + 4u), 0u, 0u};
    if (scaled) {
        rect[2] = cpu.mem_read32(source_rect + 8u);
        rect[3] = cpu.mem_read32(source_rect + 12u);
    } else {
        rect[2] = cpu.mem_read32(raster + 0x0Cu);
        rect[3] = cpu.mem_read32(raster + 0x10u);
    }
    const u32 target = cpu.mem_read32(cpu.gpr[13] + kRasterTargetWord);
    const u32 t = cpu.mem_read8(target + kRasterTypeByte);
    const u32 s = cpu.mem_read8(raster + kRasterTypeByte);
    enum class Action { Nothing, Draw, Error } action = Action::Nothing;
    RasterErrorSite site = RasterErrorSite::C;
    if (t == 2u) {
        if (s == 0u || s == 4u || s == 5u) action = Action::Draw;
        else if (s == 1u || s == 2u) { action = Action::Error; site = RasterErrorSite::B; }
    } else if (t == 0u || t == 4u || t == 5u) {
        if (s != 3u && s < 6u) { action = Action::Error; site = RasterErrorSite::A; }
    } else {
        action = Action::Error;
        site = RasterErrorSite::C;
    }
    if (action == Action::Error) {
        if (!error(cpu, site)) return;  // the guest call stopped the run
        cpu.gpr[3] = 0u;
        return;
    }
    if (action == Action::Draw) quad(cpu, raster, rect, scaled, fast);
    cpu.gpr[3] = 1u;
}

}  // namespace gekko::scaffold
