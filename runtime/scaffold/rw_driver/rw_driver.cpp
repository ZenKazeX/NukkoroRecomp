// SCAFFOLDING (R2): the entry points of the lowered RenderWare driver sites and the layer's guest access - deleted by R6: RenderWare's lift removes the translated callers
#include "rw_driver.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "gekko/cpu.h"
#include "gekko/native_rw_raster.h"

namespace gekko::scaffold {

namespace {

std::atomic<RwDriver*> g_installed{nullptr};

// The generated sites reach the installed driver; a site that runs without one is a build whose generated code lowers
// the driver while its runtime did not install the scaffolding - it stops, naming the site's function.
RwDriver* driver(Cpu& c, const char* entry) {
    RwDriver* d = g_installed.load(std::memory_order_acquire);
    if (d == nullptr) {
        c.request_stop(StopReason::ExecutionFault,
                       std::string("lowered RenderWare driver site (scaffold::") + entry +
                           ") ran without the R2 scaffolding installed");
    }
    return d;
}

}  // namespace

RwDriver::RwDriver(shadow::render::Renderer& renderer, shadow::render::PacketQueue& frames)
    : renderer_(renderer), frames_(frames) {
    // Diagnostics: SHADOW_R2_TRACE_FRAME=<n> lists every atomic the pipeline site draws in frame n (stderr);
    // SHADOW_R2_NO_DISTORTION=1 leaves the heat-haze and refraction draws out (determinism bisection).
    if (const char* trace = std::getenv("SHADOW_R2_TRACE_FRAME"))
        trace_frame_ = std::strtoull(trace, nullptr, 10);
    if (const char* off = std::getenv("SHADOW_R2_NO_DISTORTION"))
        no_distortion_ = off[0] == '1';
    // SHADOW_2D_ZOOM_FILL=1: a sprite zoomed past both canvas sides fills the view as before (the title logo's
    // after-images stretched) - a same-frame A/B of shadow::ui::overflows_canvas_width.
    if (const char* fill = std::getenv("SHADOW_2D_ZOOM_FILL"))
        hud_.set_zoomed_sprites_fill(fill[0] == '1');
    // SHADOW_BOUNDARY_TRACE=<n>: the first n world-to-screen projections with every intermediate value
    // (docs/boundary-20261007/DIVERGENCE.md); SHADOW_R2_TRACE_FRAME prints the traced frame's too.
    if (const char* trace = std::getenv("SHADOW_BOUNDARY_TRACE"))
        trace_projections_ = std::strtoull(trace, nullptr, 10);
    // SHADOW_GAME_WINDOW_SCREEN_TEST=1: the game's on-screen decisions measured in the camera's own 4:3 window, as the
    // original measured them (displayed_screen_point; docs/ASPECT_INVENTORY.md G1 validation A/B in one binary).
    if (const char* original = std::getenv("SHADOW_GAME_WINDOW_SCREEN_TEST"))
        game_window_screen_test_ = original[0] == '1';
}

RwDriver::~RwDriver() {
    uninstall();
}

void RwDriver::install() {
    g_installed.store(this, std::memory_order_release);
}

void RwDriver::uninstall() {
    RwDriver* expected = this;
    g_installed.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

RwDriver* RwDriver::installed() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

u32 RwDriver::im_texture(Cpu& c) {
    return c.mem_read32(layout::kImTexture);
}

u32 RwDriver::raster_ext(Cpu& c, u32 raster) {
    return raster + c.mem_read32(layout::kRasterExtOffset);
}

u32 RwDriver::texture_ext(Cpu& c, u32 texture) {
    return texture + c.mem_read32(layout::kTextureExtOffset);
}

float RwDriver::read_f32(Cpu& c, u32 address) {
    const u32 bits = c.mem_read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void RwDriver::write_f32(Cpu& c, u32 address, float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    c.mem_write32(address, bits);
}

// ---- the generated sites' functions (declared by the generator in recomp_table.cpp) ---------------------------------

u32 rw_render_state_set(Cpu& c, u32 state, u32 value) {
    RwDriver* d = driver(c, "rw_render_state_set");
    if (d != nullptr)
        d->note_land_class(c);
    return d != nullptr ? d->render_state_set(c, state, value) : 0u;
}

u32 rw_render_state_get(Cpu& c, u32 state, u32 out) {
    RwDriver* d = driver(c, "rw_render_state_get");
    return d != nullptr ? d->render_state_get(c, state, out) : 0u;
}

u32 rw_render_state_reset(Cpu& c, u32, u32, u32) {
    if (RwDriver* d = driver(c, "rw_render_state_reset"))
        d->render_state_reset();
    return 0u;
}

u32 rw_alpha_test_mode(Cpu& c, u32 off) {
    if (RwDriver* d = driver(c, "rw_alpha_test_mode"))
        d->alpha_test_mode(off);
    return 0u;
}

u32 rw_alpha_compare(Cpu& c, u32 cmp0, u32 ref0, u32 op, u32 cmp1, u32 ref1) {
    if (RwDriver* d = driver(c, "rw_alpha_compare"))
        d->alpha_compare(cmp0, ref0, op, cmp1, ref1);
    return 0u;
}

u32 rw_texture_set_lod(Cpu& c, u32 texture, u32 bias_clamp, u32 edge_lod, u32 max_aniso, double lod_bias) {
    if (RwDriver* d = driver(c, "rw_texture_set_lod"))
        d->texture_set_lod(c, texture, bias_clamp, edge_lod, max_aniso, lod_bias);
    return 0u;
}

u32 rw_texture_get_lod(Cpu& c, u32 texture, u32 bias, u32 bias_clamp, u32 edge_lod, u32 max_aniso) {
    if (RwDriver* d = driver(c, "rw_texture_get_lod"))
        d->texture_get_lod(c, texture, bias, bias_clamp, edge_lod, max_aniso);
    return 0u;
}

u32 rw_native_texture_loaded(Cpu& c, u32 texture, u32 bias_clamp, u32 edge_lod, u32 max_aniso, double lod_bias) {
    if (RwDriver* d = driver(c, "rw_native_texture_loaded"))
        d->native_texture_loaded(c, texture, bias_clamp, edge_lod, max_aniso, lod_bias);
    return 0u;
}

u32 rw_camera_begin_update(Cpu& c, u32, u32 camera, u32) {
    RwDriver* d = driver(c, "rw_camera_begin_update");
    return d != nullptr ? d->camera_begin(c, camera) : 0u;
}

u32 rw_camera_end_update(Cpu& c, u32, u32 camera, u32) {
    RwDriver* d = driver(c, "rw_camera_end_update");
    return d != nullptr ? d->camera_end(c, camera) : 0u;
}

u32 rw_raster_clear(Cpu& c, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, std::uint32_t rgba, bool colour,
                    bool depth) {
    RwDriver* d = driver(c, "rw_raster_clear");
    return d != nullptr ? d->raster_clear(c, x, y, w, h, rgba, colour, depth) : 0u;
}

u32 rw_camera_clear(Cpu& c, u32 camera, u32 colour, u32 mode) {
    RwDriver* d = driver(c, "rw_camera_clear");
    return d != nullptr ? d->camera_clear(c, camera, colour, mode) : 0u;
}

u32 rw_im2d_render_primitive(Cpu& c, u32 type, u32 vertices, u32 count) {
    RwDriver* d = driver(c, "rw_im2d_render_primitive");
    return d != nullptr ? d->im2d_primitive(c, type, vertices, count) : 0u;
}

// The movie player's frame blit (0x80036764).
u32 rw_movie_frame(Cpu& c, u32 type, u32 vertices, u32 count) {
    RwDriver* d = driver(c, "rw_movie_frame");
    return d != nullptr ? d->movie_frame(c, type, vertices, count) : 0u;
}

u32 rw_im2d_render_indexed_primitive(Cpu& c, u32 type, u32 vertices, u32 count, u32 indices, u32 index_count) {
    RwDriver* d = driver(c, "rw_im2d_render_indexed_primitive");
    return d != nullptr ? d->im2d_indexed(c, type, vertices, count, indices, index_count) : 0u;
}

u32 rw_camera_texture_copy(Cpu& c, u32 raster, u32 half_resolution) {
    if (RwDriver* d = driver(c, "rw_camera_texture_copy"))
        d->camera_texture_copy(c, raster, half_resolution);
    return 0u;
}

// The screen passes (screens.cpp): the heat haze's and a refraction element's copies of the screen (0x80436EEC,
// 0x80435FA4), SaveScreen's first copy (0x8042E8C0), and the Im2D primitives sampling them or standing for them -
// DrawBlur's quad (0x8042E738), the heat-haze grid (0x80437040), the refraction patch (0x804360D4), DrawScreenColor's
// multiply (0x804392DC).
u32 rw_screen_snapshot(Cpu& c, u32 raster, u32 half_resolution) {
    if (RwDriver* d = driver(c, "rw_screen_snapshot"))
        d->screen_snapshot(c, raster, half_resolution);
    return 0u;
}

u32 rw_trail_save(Cpu& c, u32 raster, u32 half_resolution) {
    if (RwDriver* d = driver(c, "rw_trail_save"))
        d->trail_save(c, raster, half_resolution);
    return 0u;
}

u32 rw_trail_draw(Cpu& c, u32 type, u32 vertices, u32 count) {
    RwDriver* d = driver(c, "rw_trail_draw");
    return d != nullptr ? d->trail_draw(c, type, vertices, count) : 0u;
}

u32 rw_shimmer_draw(Cpu& c, u32 type, u32 vertices, u32 count) {
    RwDriver* d = driver(c, "rw_shimmer_draw");
    return d != nullptr ? d->shimmer_draw(c, type, vertices, count) : 0u;
}

u32 rw_refraction_draw(Cpu& c, u32 type, u32 vertices, u32 count) {
    RwDriver* d = driver(c, "rw_refraction_draw");
    return d != nullptr ? d->refraction_draw(c, type, vertices, count) : 0u;
}

u32 rw_screen_multiply(Cpu& c, u32 type, u32 vertices, u32 count) {
    RwDriver* d = driver(c, "rw_screen_multiply");
    return d != nullptr ? d->screen_multiply(c, type, vertices, count) : 0u;
}

// PreGlare's copy of the finished scene (0x80430FAC) and PostGlare's copy of the glare-source image (0x80430B58).
u32 rw_glare_sources_begin(Cpu& c, u32 raster, u32) {
    if (RwDriver* d = driver(c, "rw_glare_sources_begin"))
        d->glare_sources_begin(c, raster);
    return 0u;
}

u32 rw_glare_composite(Cpu& c, u32 raster, u32) {
    if (RwDriver* d = driver(c, "rw_glare_composite"))
        d->glare_composite(c, raster);
    return 0u;
}

u32 rw_pipeline_execute_atomic(Cpu& c, u32 pipeline, u32 atomic, u32) {
    RwDriver* d = driver(c, "rw_pipeline_execute_atomic");
    return d != nullptr ? d->pipeline_atomic(c, pipeline, atomic) : 0u;
}

u32 rw_pipeline_execute_sector(Cpu& c, u32 pipeline, u32 sector, u32) {
    RwDriver* d = driver(c, "rw_pipeline_execute_sector");
    return d != nullptr ? d->pipeline_sector(c, pipeline, sector) : 0u;
}

u32 rw_geometry_instance_release(Cpu& c, u32 entry) {
    RwDriver* d = driver(c, "rw_geometry_instance_release");
    return d != nullptr ? d->geometry_instance_release(c, entry) : 0u;
}

u32 rw_pipeline_execute_im3d_transform(Cpu& c, u32 pipeline, u32 pool, u32) {
    RwDriver* d = driver(c, "rw_pipeline_execute_im3d_transform");
    return d != nullptr ? d->im3d_transform(c, pipeline, pool) : 0u;
}

u32 rw_pipeline_execute_im3d_render(Cpu& c, u32 pipeline, u32 stash, u32) {
    RwDriver* d = driver(c, "rw_pipeline_execute_im3d_render");
    return d != nullptr ? d->im3d_render(c, pipeline, stash) : 0u;
}

u32 render_submit_frame(Cpu& c, u32, u32) {
    if (RwDriver* d = driver(c, "render_submit_frame"))
        d->submit_frame(c);
    return 0u;
}

// ---- the device surface as native owners (extract/native-owners-r2.txt) --------------------------------------------
// The RwDevice standard-function table of the GameCube driver (GcnSystem's static table at 0x8056F4xx / 0x8051D4xx)
// names these bodies. Every dispatch site of them is lowered into the functions above, so no call reaches them in the
// lifted build; the table words still name them. Each owner is the SAME scaffold function the lowered sites call, with
// the sites' argument list (r3..), so an entry through the table does exactly what a lowered dispatch does.
void install_display_owners(Cpu& cpu) {
    rw_raster::install_display_owners(cpu, rw_raster::DisplayHooks{render_submit_frame, rw_render_state_reset});
}

void install_device_surface_owners(Cpu& cpu) {
    cpu.install_hle(0x80498954u, [](Cpu& c) {  // GcnSetRenderState(state, value)
        c.gpr[3] = rw_render_state_set(c, c.gpr[3], c.gpr[4]);
        return true;
    }, "GcnSetRenderState (device surface owner)");
    cpu.install_hle(0x804984C4u, [](Cpu& c) {  // GcnGetRenderState(state, out)
        c.gpr[3] = rw_render_state_get(c, c.gpr[3], c.gpr[4]);
        return true;
    }, "GcnGetRenderState (device surface owner)");
    cpu.install_hle(0x804956F4u, [](Cpu& c) {  // GcnCameraBeginUpdate(out, camera, n)
        c.gpr[3] = rw_camera_begin_update(c, c.gpr[3], c.gpr[4], c.gpr[5]);
        return true;
    }, "GcnCameraBeginUpdate (device surface owner)");
    cpu.install_hle(0x80495CACu, [](Cpu& c) {  // GcnCameraEndUpdate(out, camera, n)
        c.gpr[3] = rw_camera_end_update(c, c.gpr[3], c.gpr[4], c.gpr[5]);
        return true;
    }, "GcnCameraEndUpdate (device surface owner)");
    cpu.install_hle(0x80495638u, [](Cpu& c) {  // GcnCameraClear(camera, colour, mode)
        c.gpr[3] = rw_camera_clear(c, c.gpr[3], c.gpr[4], c.gpr[5]);
        return true;
    }, "GcnCameraClear (device surface owner)");
    cpu.install_hle(0x80491E80u, [](Cpu& c) {  // GcnIm2DRenderPrimitive(type, vertices, count)
        c.gpr[3] = rw_im2d_render_primitive(c, c.gpr[3], c.gpr[4], c.gpr[5]);
        return true;
    }, "GcnIm2DRenderPrimitive (device surface owner)");
    cpu.install_hle(0x8049245Cu, [](Cpu& c) {  // GcnIm2DRenderIndexedPrimitive(type, vertices, count, indices, n)
        c.gpr[3] = rw_im2d_render_indexed_primitive(c, c.gpr[3], c.gpr[4], c.gpr[5], c.gpr[6], c.gpr[7]);
        return true;
    }, "GcnIm2DRenderIndexedPrimitive (device surface owner)");
    cpu.install_hle(0x80496208u, [](Cpu& c) {  // the camera-texture copy (raster, half resolution): void, as its sites
        (void)rw_camera_texture_copy(c, c.gpr[3], c.gpr[4]);
        return true;
    }, "GcnCameraTextureCopy (device surface owner)");
    cpu.install_hle(0x804960F0u, [](Cpu& c) {  // the device record's address: lis/addi 0x8056F50C; blr
        c.gpr[3] = 0x8056F50Cu;
        return true;
    }, "device record accessor (device surface owner)");
}

}  // namespace gekko::scaffold
