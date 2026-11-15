#pragma once

// Five small callbacks of the GameCube SDK / RenderWare GameCube driver that the image reaches only through function-pointer words,
// owned natively (docs/agents/sdk-rwdrv.md). The words keep pointing at the same addresses; the addresses are owners now, so the
// original bodies are never emitted.
//
// Group B (both trees, install): the GX library's default texture-region callbacks, the words __GXData + 0x4C8 / + 0x4CC that
// GXInit leaves (native_gx_sync.cpp writes the two addresses into its image of __GXData). They are called only by GXLoadTexObj,
// the pre-loaded texture load and GXLoadTlut (0x803983C0, 0x8039845C, 0x803984E8 are the only loads of those two words in the
// image), which are silent owners in the lifted build and original bodies in the legacy tree.
//
//   0x80393634 default texture region(obj r3, id r4): the region record (16 bytes each, at gx + 0x208 / 0x288 / 0x308 + 16 * (id % 8),
//              % signed, truncating) chosen by the object's format word (obj + 0x14) and its mipmap bit (obj + 0x1F bit 0): format 6
//              (colour index) takes the second set without mipmaps and the third with them; formats 8..10 always the first set; every
//              other format the first without mipmaps and the second with them.
//   0x80393730 default TLUT region(id r3): gx + 0x388 + 16 * id for id < 20, else 0.
//
// Group C (lifted tree, install_lifted): the three 0x28-byte wrappers of the RenderWare driver's raster render core 0x80499924
// (the device standard-function table's words 0x8051D494 / 0x8051D49C / 0x8051D4A4). Each calls the core with its raster and rect
// arguments unchanged and the two flags r5 (scaled) and r6 (fast) fixed. The core is an owner in the lifted build
// (scaffold::install_raster_render_owner), so the wrapper serves it through the owner table and returns its r3.
//
// Kept: r1, r14..r31, f14..f31, LR (the owners never move them). Not reproduced, dead below the caller: the wrapper's 0x10-byte frame
// words and the LR it saves at 4(caller r1), r0, r5, r6 and CR.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::driver_callbacks {

struct Address {
    u32 address;
    const char* name;
};

constexpr u32 kTexRegion = 0x80393634u;
constexpr u32 kTlutRegion = 0x80393730u;
constexpr u32 kRasterRenderCore = 0x80499924u;
constexpr u32 kRasterRenderScaled0Fast1 = 0x80499A9Cu;  // table word 0x8051D494
constexpr u32 kRasterRenderScaled0Fast0 = 0x80499AC4u;  // table word 0x8051D4A4
constexpr u32 kRasterRenderScaled1Fast1 = 0x80499AECu;  // table word 0x8051D49C

const Address* addresses(std::size_t& count) noexcept;         // group B, both trees
const Address* lifted_addresses(std::size_t& count) noexcept;  // group C, lifted tree only

// The bodies, callable by the contract test: `address` is one of the five above.
void serve(Cpu& cpu, u32 address);

void install(Cpu& cpu);
void install_lifted(Cpu& cpu);

struct Owners {
    static const Address* addresses(std::size_t& count) noexcept { return driver_callbacks::addresses(count); }
};

}  // namespace gekko::driver_callbacks
