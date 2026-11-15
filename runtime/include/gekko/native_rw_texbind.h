#pragma once

// The RenderWare GameCube driver's texture bind, 0x8049AC88 (docs/agents/axfx-effects.md, Step L), as an exact native owner in the
// lifted tree: bind(texture r3, unit r4). The driver keeps a texture's GXTexObj inside its plugin data (texture + *(SDA+0x6218));
// the bind (re)initialises that object from the raster when the texture is new (flag 0x01000000) or its version halfword changed, and
// records the texture as the bound one of the unit. It calls only functions that are owners themselves: the GX texture-object
// initialisers and getters (gekko/native_gx_texobj.h, called with the original's arguments, the TLUT name through the stack word the
// original writes at 8(r1) of its own frame), GcnTextureSetRaster for the default texture (r3 = 0), and the three GX loads, which
// the lifted build serves silent (their pipe sites are dead). The legacy tree keeps the original body: its loads draw.
//
// Kept registers: r1, r14..r31, f14..f31 (f31 through the same paired-single save and restore as the original, so its second half
// comes back rounded to single). Left changed and dead at every one of the 16 call sites (checked): the volatile registers, CR and
// CTR; the original's frame words (saved registers, the int-to-float scratch) are not written, except 8(r1) of the frame and the
// f31 save slots, which the owner writes like the original.

#include "gekko/types.h"

namespace gekko {
class Cpu;
}

namespace gekko::rw_texbind {

constexpr u32 kTextureBind = 0x8049AC88u;

void bind(Cpu& cpu);  // the body, callable by the contract test
void install_lifted(Cpu& cpu);

}  // namespace gekko::rw_texbind
