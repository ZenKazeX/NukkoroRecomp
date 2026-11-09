#pragma once

// Native immediate geometry: GXBegin's primitives built from the lowered GX
// sites, with no command stream underneath.
//
// docs/native-time-20261005/GX_DATA_PLANE.md. RenderWare's immediate
// producers (RwIm2D, RwIm3D, PTank, RasterRender, the disc-error screen) open a
// primitive with GXBegin(type, format, n) and then store its vertices one
// attribute at a time. On the console those stores were bytes in the
// write-gather pipe, and the command processor sized the primitive from the
// vertex descriptor and attribute format it held. Here the generator lowers
// GXBegin's two header stores and every vertex store
// (gekko/gx_sites.h: PrimitiveOpcode, PrimitiveCount, Vertex) to calls into
// this builder, and the layout comes from where the GX library keeps it - its
// own __GXData shadows, which GXBegin's dirty-state flush has just written to
// the command processor before the header (VCD gx+0x14 / +0x18, VAT A / B / C
// gx+0x1C / +0x3C / +0x5C + 4 * format; equal to the decoded registers at every
// primitive on 21 routes, phase4/digests-a). When n * stride bytes have arrived
// the primitive goes to the capture exactly as the decoder delivered it.
//
// Fail-closed: vertex data with no open primitive, a header while a primitive
// is incomplete, a count with no header, a header byte that is not a draw
// opcode, a store past the primitive's end - each stops the machine with the
// site. None happens on a correct site list.
//
// State (savestate version 14): an open primitive's phase, opcode, count,
// layout words and the payload so far.

#include <cstddef>
#include <vector>

#include "gekko/gx.h"
#include "gekko/types.h"

namespace gekko {

class Cpu;

// Receiver of complete immediate primitives (the RenderWare capture). Called on
// the guest thread at the store that completes the primitive; `payload` holds
// vertex_count * format.stride bytes in the order the guest stored them, or is
// null when there are none.
class ImmediateSink {
public:
    virtual ~ImmediateSink() = default;
    virtual void on_primitive(u8 opcode, u32 vertex_count, const VertexFormat& format, u32 vat_a,
                              u32 vat_b, u32 vat_c, const u8* payload, std::size_t payload_bytes) = 0;
};

class NativeImmediate {
public:
    explicit NativeImmediate(Cpu& cpu) noexcept : cpu_(cpu) {}

    void set_sink(ImmediateSink* sink) noexcept { sink_ = sink; }

    // ---- the lowered GX sites (Cpu::gx_*, routed by System) -----------------
    void primitive_opcode(u32 site, u8 opcode);
    void primitive_count(u32 site, u16 count);
    void vertex(u32 site, u32 value, u32 bytes);

    bool idle() const noexcept { return phase_ == Phase::Idle; }

    struct Stats {
        u64 primitives = 0;      // primitives completed
        u64 vertices = 0;
        u64 payload_bytes = 0;
        u64 empty = 0;           // primitives with no payload (n or the stride 0)
    };
    const Stats& stats() const noexcept { return stats_; }

    // Savestate (gekko/savestate.h, version 14).
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end);
    void reset() noexcept;

private:
    enum class Phase : u8 { Idle = 0, Opcode = 1, Payload = 2 };
    void open();
    void complete();
    void refuse(u32 site, const char* what);

    Cpu& cpu_;
    ImmediateSink* sink_ = nullptr;
    Phase phase_ = Phase::Idle;
    u8 opcode_ = 0;
    u32 count_ = 0;
    u32 vcd_low_ = 0;
    u32 vcd_high_ = 0;
    u32 vat_a_ = 0;
    u32 vat_b_ = 0;
    u32 vat_c_ = 0;
    VertexFormat format_;
    std::size_t expected_ = 0;
    std::vector<u8> payload_;
    Stats stats_;
};

}  // namespace gekko
