#pragma once

// The GX library's state API, owned natively (docs/agents/card-saves.md, removal plan service graphics.gx, cluster
// gxA; docs/agents/gx-rw-plan.md).
//
// Original behaviour (main.dol, PAL GUPP8P): GXSetCullMode, GXSetBlendMode, GXSetZMode, GXSetZCompLoc,
// GXSetAlphaCompare, GXSetFog, GXSetColorUpdate, GXSetAlphaUpdate, GXSetDstAlpha, GXSetViewport(Jitter),
// GXSetScissor(BoxOffset), GXSetProjection(v), GXLoadPosMtxImm / NrmMtxImm / TexMtxImm, GXSetCurrentMtx,
// GXSetNumChans, GXSetChanCtrl, GXSetChanMatColor / AmbColor, GXLoadLightObjImm, GXSetNumTexGens and GXSetTexCoordGen2
// each write the library's shadow of the state they were told (fields of __GXData, *(r2 + 0xFD8)) and send the matching
// BP / XF command words through the write-gather pipe. Every one of those pipe stores is dead transport on this platform
// (extract/gx-sites.txt: Flipper has no consumer); the draw state the renderer uses reaches it as values from the
// RenderWare driver (scaffold/rw_driver/render_states.cpp, projection.cpp, frames.cpp), never from GX registers.
//
// Native implementation: the call is served and writes nothing. The shadow words are retired; their only readers are
// other GX library functions that write them again and send dead pipe stores (measured on the replay with the guest
// watch, docs/agents/card-saves.md). Nothing here models a GX register, a FIFO or a command stream.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::gx_state {

struct Address {
    u32 address;
    const char* name;
};

// Must equal this owner's rows of extract/native-owners.txt (native_file_api_tests).
const Address* addresses(std::size_t& count) noexcept;

// The same list as an owner type (native_file_api_tests collects owners this way).
struct Owners {
    static const Address* addresses(std::size_t& count) noexcept { return gx_state::addresses(count); }
};

// The vertex path (cluster gxC), owned only in the lifted build (extract/native-owners-r2.txt, installed by
// LiftedRender::install_owners): there the GX immediate sites are dead - a GXBegin header or an immediate vertex store
// stops the machine (extract/gx-fail-sites-lifted.txt) - so GXBegin is refused with its own message and the vertex
// descriptor / format / array setters, whose only readers were GXBegin's flush and the legacy immediate builder
// (native_immediate.h, not compiled here), are served and silent. The legacy tree keeps the original bodies.
const Address* lifted_addresses(std::size_t& count) noexcept;
void install_lifted(Cpu& cpu);

// Installs the owners (before the guest boots). In the tree with the legacy native capture the capture's observers
// chain in front of these (System::add_hle_observer).
void install(Cpu& cpu);

}  // namespace gekko::gx_state
