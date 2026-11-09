// SCAFFOLDING (R2): RwRasterRender, the driver's raster-to-target blit - deleted by R6: the lift of RenderWare's raster path owns it
#include "raster_render.h"

#include "gekko/cpu.h"
#include "gekko/native_guest_call.h"
#include "gekko/ppc_ops.h"
#include "rw_driver.h"

#include <cstdint>

namespace gekko::scaffold {

namespace {
constexpr u32 kErrorFormat = 0x8047EC78u;   // (code, string) -> message
constexpr u32 kErrorSet = 0x8047EBD4u;      // (&{1, message})
constexpr u32 kErrorString = 0x8056FB9Cu;
constexpr u32 kFrameBytes = 0x30u;

// The three sites' stack slot and the return addresses the original's two calls come back to.
struct Site {
    u32 slot, format_link, set_link;
};
constexpr Site kSites[3] = {{0x18u, 0x804999E0u, 0x804999ECu},
                            {0x10u, 0x80499A44u, 0x80499A50u},
                            {0x08u, 0x80499A74u, 0x80499A80u}};

bool report_error(Cpu& c, RasterErrorSite which) {
    const Site& site = kSites[static_cast<unsigned>(which)];
    // The original's own 0x30-byte frame: the record sits in it, and the callees run below it. Held for the whole
    // report (the guest-call seam runs a callee with the current r1 as the frame top).
    const u32 sp0 = c.gpr[1], sp = sp0 - kFrameBytes;
    c.mem_write32(sp, sp0);
    c.gpr[1] = sp;
    u32 message = 0, ignored = 0;
    c.mem_write32(sp + site.slot, 1u);
    ppc::set_cr_bit(c, 6u, 0u);  // crclr cr1eq: no vector arguments
    bool ok = guest_call(c, kErrorFormat, site.format_link, {2u, kErrorString}, message);
    if (ok) {
        c.mem_write32(sp + site.slot + 4u, message);
        ok = guest_call(c, kErrorSet, site.set_link, {sp + site.slot}, ignored);
    }
    c.gpr[1] = sp0;
    return ok;
}

void draw(Cpu& c, u32 raster, const u32 rect[4], bool scaled, bool fast) {
    if (RwDriver* d = RwDriver::installed())
        d->raster_quad(c, raster, rect, scaled, fast);
}
}  // namespace

// The draw body 0x80499394: the raster drawn as a textured 2D quad at the rect of the current target. The original sets
// the render states through RwRenderStateSet, so the Im texture's raster (state 1), its filter (9) and its addressing
// (2) are written and then restored by the same setters; the blend (5, 6 alpha when `fast`, else 2, 1) is GX-only in the
// original (0x80498910: the driver's cache is not touched), so here it is the recipe's blend for this one draw. The
// quad is its four corners (top left, bottom left, bottom right, top right) with the sub-rectangle of the raster as the
// texture window; vertex positions are 16-bit sums as the original's halfword stores, texture coordinates are single-
// precision products with 1 / the parent's size. GX-only state (tev REPLACE with no vertex colour, texgen, vertex
// format, viewport and scissor of the rect) is not carried: the quad is white-modulated, so the texel is the colour.
void RwDriver::raster_quad(Cpu& c, u32 raster, const u32 rect[4], bool scaled, bool fast) {
    const u32 parent = c.mem_read32(raster + layout::kRasterParent);
    const u32 target = c.mem_read32(c.gpr[13] + kRasterTargetWord);
    const u32 scratch = (c.gpr[1] - 0x40u) & ~3u;
    const auto saved = [&](u32 state) {
        c.mem_write32(scratch, 0u);
        render_state_get(c, state, scratch);
        return c.mem_read32(scratch);
    };
    const u32 saved_fog = saved(14u), saved_filter = saved(9u), saved_u = saved(3u), saved_v = saved(4u),
              saved_raster = saved(1u);
    const u32 saved_src = states_.src_blend, saved_dst = states_.dst_blend;
    states_.src_blend = fast ? 5u : 2u;
    states_.dst_blend = fast ? 6u : 1u;
    render_state_set(c, 14u, 0u);
    render_state_set(c, 9u, scaled ? 2u : 1u);
    render_state_set(c, 2u, 3u);
    render_state_set(c, 1u, parent);

    const auto s16 = [&](u32 address) { return static_cast<s32>(static_cast<std::int16_t>(c.mem_read16(address))); };
    const s32 tx = s16(target + layout::kRasterOffsetX), ty = s16(target + layout::kRasterOffsetY);
    const s32 sx = s16(raster + layout::kRasterOffsetX), sy = s16(raster + layout::kRasterOffsetY);
    const s32 sw = static_cast<s32>(c.mem_read32(raster + layout::kRasterWidth));
    const s32 sh = static_cast<s32>(c.mem_read32(raster + layout::kRasterHeight));
    const s32 rx = static_cast<s32>(rect[0]), ry = static_cast<s32>(rect[1]);
    // Far corners: the rect's size when scaled, the source's otherwise.
    const s32 dw = scaled ? static_cast<s32>(rect[2]) : sw, dh = scaled ? static_cast<s32>(rect[3]) : sh;
    const float inv_w = 1.0f / static_cast<float>(static_cast<s32>(c.mem_read32(parent + layout::kRasterWidth)));
    const float inv_h = 1.0f / static_cast<float>(static_cast<s32>(c.mem_read32(parent + layout::kRasterHeight)));
    const auto pos = [](s32 v) { return static_cast<float>(static_cast<std::int16_t>(v)); };
    const auto make = [&](s32 x, s32 y, s32 u, s32 v) {
        shadow::render::ImmediateVertex out;
        out.position = shadow::render::Float3{pos(tx + x), pos(ty + y), 0.0f};
        out.colour = 0xFFFFFFFFu;
        out.uv[0] = static_cast<float>(u) * inv_w;
        out.uv[1] = static_cast<float>(v) * inv_h;
        return out;
    };
    const shadow::render::ImmediateVertex quad[4] = {make(rx, ry, sx, sy), make(rx, ry + dh, sx, sy + sh),
                                             make(rx + dw, ry + dh, sx + sw, sy + sh), make(rx + dw, ry, sx + sw, sy)};
    im2d(c, 5u, 0u, 4u, 0u, 0u, nullptr, quad);  // a triangle fan: (TL, BL, BR), (TL, BR, TR)

    states_.src_blend = saved_src;
    states_.dst_blend = saved_dst;
    render_state_set(c, 14u, saved_fog);
    render_state_set(c, 9u, saved_filter);
    render_state_set(c, 3u, saved_u);
    render_state_set(c, 4u, saved_v);
    render_state_set(c, 1u, saved_raster);
}

void install_raster_render_owner(Cpu& cpu) {
    cpu.install_hle(kRasterRenderCore, [](Cpu& c) {
        serve_raster_render(c, draw, report_error);
        return true;
    }, "RwRasterRender core (device table owner)");
}

}  // namespace gekko::scaffold
