#pragma once

// A MODEL OF THE GAMECUBE GX SYNCHRONISATION CONTRACT (not native; NATIVE_AUDIT.md C12, lies list 10): the pixel
// engine's draw-sync token (BP 0x47 / 0x48 payloads) and draw-done (BP 0x45), the command processor's breakpoint
// delivered as an OS interrupt to RenderWare's XFB-ring callback, the PE interrupt enables, and the CPU / GP FIFO
// pointers GXGetFifoPtrs reads back. No hardware is touched, but the contract modelled is the hardware's. To be
// deleted by NE-VIDEO / NE-RW (the RenderWare frame end becomes a native frame boundary).
//
// docs/native-time-20261005/STAGE4_GX_SYNC.md. On the console the CPU wrote
// GX commands into a FIFO in main memory through the write-gather pipe, the
// command processor read them behind it, and three things came back to the
// game: the draw-sync token the pixel engine latched when it reached a
// token command (GXReadDrawSync), the CP breakpoint interrupt when the read
// pointer reached an armed address (RenderWare's XFB ring is driven by it),
// and the draw-done interrupt when a GXDrawDone had drained the pipe. Here
// every command is consumed when the guest writes it - the native renderer
// takes ownership of everything it needs at the command - so:
//
//   - the draw-sync token is the last token submitted: a completion counter
//     that is complete at submission (GXReadDrawSync);
//   - a breakpoint armed at the CPU FIFO write position is reached at once;
//     its delivery is the SDK handler's breakpoint branch (the CP status copy,
//     the shadow's interrupt bit, RenderWare's callback in interrupt context);
//   - a GXDrawDone completes at once; its delivery sets DrawDone and wakes the
//     GXDrawDone waiter;
//   - the FIFO is always drained, no watermark is ever crossed, the GP is
//     never busy, and nothing models FIFO positions beyond what the SDK reads
//     back: GXGetFifoPtrs answers from the positions the FIFO was bound with.
//
// The SDK entry points that touched CP / PE / PI-FIFO / MI hardware are native
// owners (D1, extract/native-owners.txt); each keeps every guest-memory effect
// of the original (the SDK's CP-control and clear shadows in __GXData, the
// FIFO globals, the GXFifoObj fields, the reset-path statics) and drops the
// register traffic. Without them nothing reaches the CP-control helpers, the
// three interrupt handlers or __GXAbort, and the generator prunes them. The
// CP performance-select stores in GXInit and GXSetGPMetric are listed
// platform sites (D6). The rest of the GX library stays game code.
//
// Two of the three GX interrupts are native sources of NativeInterrupts (OS
// interrupts 17 and 19), raised where the hardware raised its PI causes: the
// breakpoint when GXEnableBreakPt arms it, the finish interrupt at a draw-done
// command (BP 0x45 = 2: GXDrawDone's lowered site, gekko/gx_sites.h). The token
// interrupt (18, requested by BP 0x48) is not delivered at all: its handler's
// only work is the draw-sync callback, which this image never sets
// (GXSetDrawSyncCallback's only caller is __GXShutdown, with NULL), so a token
// command is a completed token and nothing more (stage 4b,
// STAGE4_GX_SYNC.md section 7). The draw-done callback is never set either. A
// set callback stops the machine with the reason: it is proven unreachable and
// nothing here serves it.
//
// State (savestate version 13): the token, the two PE interrupt enables
// __GXPEInit sets, the FIFO positions GXGetFifoPtrs reads back. The
// breakpoint's pending status is the native interrupt block's.

#include <cstddef>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class Cpu;
class NativeInterrupts;

class NativeGxSync final {
public:
    NativeGxSync(Cpu& cpu, NativeInterrupts& interrupts) noexcept;

    // Installs the owners and the three deliveries. Before the guest boots.
    void install();

    // ---- the lowered draw-sync sites (gekko/gx_sites.h, routed by System) -----
    // GXSetDrawSync's BP 0x47 / 0x48 payload: the token, completed at once.
    void on_draw_sync_token(u16 token, bool interrupt);
    // GXDrawDone's BP 0x45 payload with low byte 2: draw done, completed at once.
    void on_draw_done();

    // The listed platform sites (Cpu::platform_write16): the CP performance
    // select in GXInit / GXSetGPMetric. Nothing to select: counted.
    u16 platform_read16(Cpu& c, u32 site, u32 effective);
    void platform_write16(Cpu& c, u32 site, u32 effective, u16 value);

    // Every guest address the generator must route to an owner here
    // (extract/native-owners.txt), and every platform site it lowers
    // (extract/platform-sites.txt).
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;
    struct Site {
        u32 address;      // the instruction
        u32 instruction;  // its word in the DOL
        const char* kind;
    };
    static const Site* platform_sites(std::size_t& count) noexcept;
    static bool is_platform_site(u32 address) noexcept;

    // Host diagnostics.
    u16 token() const noexcept { return token_; }
    bool token_interrupt_enabled() const noexcept { return token_interrupt_enabled_; }
    bool finish_interrupt_enabled() const noexcept { return finish_interrupt_enabled_; }
    u32 cpu_fifo_write() const noexcept { return cpu_fifo_write_; }
    u32 gp_fifo_read() const noexcept { return gp_fifo_read_; }
    u32 gp_fifo_count() const noexcept { return gp_fifo_count_; }

    struct Stats {
        u64 tokens = 0;                 // token commands consumed (BP 0x47 / 0x48)
        u64 token_interrupts = 0;       // token commands that requested the interrupt (BP 0x48)
        u64 draw_dones = 0;             // draw-done commands consumed (BP 0x45 = 2)
        u64 finish_deliveries = 0;
        u64 breakpoints = 0;            // breakpoints reached (armed: at once)
        u64 breakpoint_deliveries = 0;
        u64 breakpoint_callbacks = 0;   // RenderWare's XFB-ring callback calls
        u64 breakpoints_disabled = 0;   // GXDisableBreakPt calls
        u64 fifo_binds = 0;             // GXSetCPUFifo + GXSetGPFifo calls
        u64 fifo_reads = 0;             // GXGetFifoPtrs calls
        u64 draw_sync_reads = 0;        // GXReadDrawSync calls
        u64 shutdown_calls = 0;         // __GXShutdown calls
        u64 platform_writes = 0;        // CP performance-select stores
    };
    const Stats& stats() const noexcept { return stats_; }

    // `--measure gx-inputs` (observation only, off by default): an FNV-1a 64
    // digest of the token sequence this receives, (token, interrupt) in order -
    // the phase-4 equivalence instrument (GX_DATA_PLANE.md section 4).
    void enable_input_digest() noexcept { input_digest_enabled_ = true; }
    bool input_digest_enabled() const noexcept { return input_digest_enabled_; }
    u64 input_digest() const noexcept { return input_digest_; }

    // Savestate (gekko/savestate.h, version 13).
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end);
    void reset() noexcept;

private:
    void set_cpu_fifo(Cpu& c);
    void set_gp_fifo(Cpu& c);
    void get_fifo_ptrs(Cpu& c);
    void enable_breakpoint(Cpu& c);
    void disable_breakpoint(Cpu& c);
    void fifo_init(Cpu& c);
    void pe_init(Cpu& c);
    void shutdown(Cpu& c);
    bool breakpoint_handler(Cpu& c, u32 frame, u32 step);
    bool finish_handler(Cpu& c, u32 frame, u32 step);

    Cpu& cpu_;
    NativeInterrupts& interrupts_;
    u16 token_ = 0;
    bool token_interrupt_enabled_ = false;
    bool finish_interrupt_enabled_ = false;
    // The positions GXGetFifoPtrs reads back, as the FIFO was bound: the CPU
    // FIFO write position (GXSetCPUFifo: wrPtr, 32-byte aligned), the GP FIFO
    // read position and read-write distance (GXSetGPFifo: rdPtr, count) - the
    // game's own addresses (no register held them on the way).
    // Nothing advances them: the FIFO is consumed as it is written.
    u32 cpu_fifo_write_ = 0;
    u32 gp_fifo_read_ = 0;
    u32 gp_fifo_count_ = 0;
    Stats stats_;
    bool input_digest_enabled_ = false;
    u64 input_digest_ = 0xCBF29CE484222325ull;
};

}  // namespace gekko
