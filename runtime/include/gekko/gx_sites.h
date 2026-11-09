#pragma once

// The GX write-gather pipe's store sites, as the generator lowered them.
//
// docs/native-time-20261005/GX_DATA_PLANE.md. On the console every GX command
// reached Flipper as bytes the CPU stored to the write-gather pipe (EA
// 0xCC008000). There is no pipe here and no command decoder: every store of
// the image to the pipe page is listed (extract/gx-sites.txt, produced by
// tools/ppcrecomp/gx_sites.py from tools/gx_site_inventory.py) with what it
// meant for the game, and the generator emits that instead of the store:
//
//   Dead              transport with no consumer (Flipper state, display-list
//                     calls, FIFO padding): nothing is emitted
//   DrawSyncToken     BP 0x47 payload (GXSetDrawSync): the token, completed
//   DrawSyncTokenIrq  BP 0x48 payload: the token, with its interrupt request
//   DrawDone          BP 0x45 payload (GXDrawDone): draw done when its low
//                     byte is 2
//   CopyTrigger       BP 0x52 payload (GXCopyDisp / GXCopyTex): counted - the
//                     copy itself is the RenderWare driver's
//   PrimitiveOpcode   GXBegin's header byte (type | vertex format)
//   PrimitiveCount    GXBegin's vertex count: opens the immediate primitive
//   Vertex            immediate vertex data, appended to the open primitive
//
// The interpreter applies the same table to the same words (Cpu::execute),
// so the two engines agree; a store to the pipe page that is not listed stops
// the machine with its location.

#include <cstddef>

#include "gekko/types.h"

namespace gekko {

enum class GxSiteFamily : u8 {
    Dead = 0,
    DrawSyncToken,
    DrawSyncTokenIrq,
    DrawDone,
    CopyTrigger,
    PrimitiveOpcode,
    PrimitiveCount,
    Vertex,
};

// One listed site: the instruction's address, its DOL word, its family.
struct GxSiteRecord {
    u32 address;
    u32 word;
    GxSiteFamily family;
};

// The receiver of the live sites' calls (the System routes them to the native
// GX synchronisation, the immediate geometry builder and the capture). Called
// on the guest thread at the store; `site` is the store's address.
class GxSiteSink {
public:
    virtual ~GxSiteSink() = default;
    virtual void draw_sync_token(u32 site, u16 token, bool interrupt) = 0;
    virtual void draw_done(u32 site, u32 value) = 0;
    virtual void copy_trigger(u32 site, u32 value) = 0;
    virtual void primitive_opcode(u32 site, u8 opcode) = 0;
    virtual void primitive_count(u32 site, u16 count) = 0;
    // `bytes` (1, 2 or 4) big-endian bytes of `value`, in the order the store
    // wrote them.
    virtual void vertex(u32 site, u32 value, u32 bytes) = 0;
};

}  // namespace gekko
