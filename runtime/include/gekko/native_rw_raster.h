#pragma once

// The GameCube RenderWare driver's raster life cycle, owned natively (docs/agents/card-saves.md, service
// graphics.rw-gamecube-driver, cluster rwR2; docs/agents/gx-rw-plan.md).
//
// Original behaviour (main.dol, PAL GUPP8P): the RwDevice standard functions of the GameCube driver (the table the
// driver's system function fills, words 0x8051D4xx) create, lock, unlock and destroy rasters. A raster's platform data
// sits at raster + the plugin offset (*(r13 + 0x6200)): the texture object words, the mip count (+0x32), the pixel
// block (+0x18 / +0x1C), the lock state and the draw-sync token of its last use (+0x30). The driver talks to the GX
// library (GXInitTexObj, GXSetDrawSync / GXReadDrawSync) and to the data cache (DCFlushRange / DCInvalidateRange) and
// allocates through the engine's memory functions (*(r13 + 0x615C) + 0x108 / 0x10C).
//
// Native implementation: each owner is the original's arithmetic over the same guest words - it writes what the game
// (and the renderer) reads and nothing else: no texture object is built for hardware, no cache line is flushed, the
// draw-sync token the original waited on is the one NativeGxSync completes at once. Allocation and release still go
// through the engine's memory functions (guest code, called as the original called them), so the heap contents are
// the original's.
//
// This is the first slice of the cluster: the small table rows, the token test and RasterDestroy. Create, Lock and the
// unlock family follow in later slices.

#include "gekko/types.h"

#include <cstddef>
#include <cstdint>

namespace gekko {
class Cpu;
class NativeGxSync;
}

namespace gekko::rw_raster {

struct Address {
    u32 address;
    const char* name;
};

// Must equal this owner's rows of extract/native-owners.txt (native_file_api_tests).
const Address* addresses(std::size_t& count) noexcept;

struct Owners {
    static const Address* addresses(std::size_t& count) noexcept { return rw_raster::addresses(count); }
};

// The original's error report as other owners need it: RwErrorSet(&{1, code}) (RenderWare core 0x8047EBD4) through the
// guest-call seam, the record in a frame held below r1. `link` is an address inside the calling owner's original function.
// False when the call itself could not run (the run is stopped).
bool report_rw_error(Cpu& cpu, u32 code, u32 link);

// The renderer's raster-level clear (RwDriver::raster_clear through scaffold::rw_raster_clear): x, y, w, h in raster pixels,
// the colour as rgba with R in the low byte, whether the colour and / or the depth are cleared. Set by the lifted
// renderer's install; without one a clear of a z-buffer / camera raster stops the run (no renderer to clear).
using RasterClearFn = std::uint32_t (*)(Cpu&, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, std::uint32_t rgba,
                                        bool colour, bool depth);
void set_raster_clear(RasterClearFn clear) noexcept;

// The native texture read (GcnNativeTextureRead 0x8049A8DC, lifted build only): the driver's last step, the texture's LOD
// values, is the scaffold's (RwDriver::native_texture_loaded: texture, bias clamp, edge LOD, max aniso, LOD bias).
using TextureLoadedFn = std::uint32_t (*)(Cpu&, std::uint32_t texture, std::uint32_t bias_clamp, std::uint32_t edge_lod,
                                          std::uint32_t max_aniso, double lod_bias);
void install_lifted(Cpu& cpu, TextureLoadedFn loaded);

// Installs the owners (before the guest boots). `sync` supplies the draw-sync token (GXReadDrawSync) and issues
// GXSetDrawSync; it must outlive the Cpu's use of the owners.
void install(Cpu& cpu, NativeGxSync& sync);

// GcnRasterSetImage and the image-to-raster conversions (native_rw_raster_image.cpp); install() calls it.
void install_image_owners(Cpu& cpu);

// The raster size arithmetic: log2 helper, mip level bytes, all-mips bytes, GcnRasterGetSize (native_rw_raster_size.cpp).
void install_size_owners(Cpu& cpu);

// The display swap: ShowRaster, the retrace and draw-sync callbacks, the video mode, GcnSystem and the plugin / state setup
// (native_rw_display.cpp; docs/agents/vi.md "Step 12").
struct DisplayHooks {
    u32 (*submit_frame)(Cpu&, u32 xfb, u32 clear);   // the lowered GXCopyDisp site of ShowRaster's single-field path
    u32 (*state_reset)(Cpu&, u32, u32, u32);         // the lowered GXSetZMode(1, LEQUAL, 1) site of 0x804981E8
};
// Lifted build only (LiftedRender::install_owners via scaffold::install_display_owners): the legacy renderer observes ShowRaster instead.
void install_display_owners(Cpu& cpu, const DisplayHooks& hooks);

}  // namespace gekko::rw_raster
