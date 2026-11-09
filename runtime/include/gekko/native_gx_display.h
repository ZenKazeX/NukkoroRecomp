#pragma once

// The GX library's display-copy and FIFO-setup leaves, owned natively (docs/agents/axfx-effects.md, "Step H").
//
// They are the GX callees of the RenderWare driver's device handler (0x80494EDC) and ShowRaster (0x80495CBC), which the
// display-swap unit in graphics.vi needs gone before it can be owned, and of GXInit's interior (0x80393B0C..0x80394930).
//
//   0x80396BD4 GXSetDispCopySrc, 0x80396CCC GXSetDispCopyDst, 0x80396E54 the copy clamp, 0x80397430 the copy gamma,
//   0x80397228 GXSetCopyFilter, 0x80399E58 the pixel-format setter, 0x80399FCC a BP mask setter: each writes only words
//   of __GXData (the library's shadow of the copy / pixel state) and sends BP commands through the write-gather pipe,
//   which are dead transport (extract/gx-sites.txt). Served and silent, like the state setters (gekko/native_gx_state.h);
//   the shadow they kept is retired - its readers are these same functions and GXCopyDisp (measured, Step H).
//   0x803971B0 (BP 0x4F/0x50/0x51 from a 4-byte colour and a value) and 0x803992C8 (BP with the state register at gx+0x124):
//   each ends by clearing the halfword gx+2; the only other writes are dead pipe stores. Served and silent.
//   0x80396098 GXFlush: the dirty-state flush (gx+0x5AC bits -> setter calls whose only products are shadow words and
//   dead pipe stores), eight zero words into the pipe and the `sc` of PPCSync. Served and silent.
//   0x80394AF4 GXInitFifoBase(fifo, base, size): the one row that writes guest memory a caller can read - the FIFO
//   object (r3): [0]=base [4]=base+size-4 [8]=size [0xC]=size-0x4000 [0x10]=(size>>1)&~0x1F [0x14]=[0x18]=base
//   [0x1C]=0 (its callees GXInitFifoLimits / GXInitFifoPtrs inlined, interrupts disabled around the pointer update and
//   restored). Reproduced exactly.
//   0x80397444 GXCopyDisp (the XFB copy, BP 0x52 = a copy_trigger site): in the lifted build every copy_trigger is a dead
//   site (extract/gx-fail-sites-lifted.txt) and the display copy is the lowered render_submit_frame; a call that is not
//   lowered would have stopped at that site. The lifted owner refuses the call with its own message.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::gx_display {

struct Address {
    u32 address;
    const char* name;
};

constexpr u32 kGxInitFifoBase = 0x80394AF4u;
constexpr u32 kGxCopyDisp = 0x80397444u;

const Address* addresses(std::size_t& count) noexcept;          // both trees
const Address* lifted_addresses(std::size_t& count) noexcept;   // lifted tree only (GXCopyDisp, refused)

// The exact FIFO setup (a pure function of the arguments, used by the contract test too).
void serve_init_fifo_base(Cpu& cpu);

void install(Cpu& cpu);
void install_lifted(Cpu& cpu);

struct Owners {
    static const Address* addresses(std::size_t& count) noexcept { return gx_display::addresses(count); }
};

}  // namespace gekko::gx_display
