#pragma once

// The game's memory services, native (docs/memory-architecture-20261006/README.md).
//
// The home of every native owner that replaces a piece of the game's runtime
// that dealt with memory as the GameCube had it. M1: the OS's RealMode helper
// (0x80376B48), whose only use is __OSInitMemoryProtection entering
// Config24MB / Config48MB with translation off to program the BATs; the
// game's address window has no BATs and no real mode, so there is nothing to
// configure. M2 adds the native allocator and the owners of every allocation
// entry point (the OS heap and arena, RenderWare's memory functions, the C/C++
// runtime's malloc and new).
//
// EV (2026-10-19): the game's two bulk byte movers, which were translated
// byte-at-a-time loops on every stage load (findings-20261019-EV/EV_REPORT.md
// section 7): the C runtime's "memcpy" (0x800054F4, memmove semantics) and the
// SEGA PRS expander (0x80043FE0). Their copy / decode cores are plain native
// code over the game's memory; only their edges reproduce the translated
// callers' contract (game_memory.cpp, "the boundary").
//
// NE-OS-0 (EV, 2026-10-20): the SDK's cache maintenance - PPCSync, DCFlushRange, DCStoreRange, the DOL's only three
// `sc` - owned at the SDK API: there is no cache, so the owners return (proof: GLE; game_memory.cpp, kPpcSync).

#include <cstddef>
#include <cstdio>
#include <functional>
#include <optional>
#include <memory>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class Cpu;

// A PRS stream decoded ahead of its expand call (gekko/prs_ahead.h): the output and the register file the expander
// leaves, relative to its source and destination. Made by prs_decode_ahead from the stream's bytes alone, with the
// same token decoder the expand owner runs; only streams that never copy from before their output qualify (such a
// copy reads guest memory the stream does not carry).
struct PrsDecoded {
    std::vector<u8> output;  // what the expander writes at dst
    const u8* stream = nullptr;  // the stream as decoded (kept valid by its owner)
    u32 consumed = 0;        // stream bytes the expander reads: r8 ends at src + consumed
    u32 r9 = 0, r10 = 0;     // the flag word and its count
    bool ca = false;         // XER[CA]
    bool any_match = false;  // a match ran: r5 = dst + r5_from_dst, r6 = 0, CTR = 0 (else the caller's stay)
    u32 r5_from_dst = 0;
};
// Decodes the stream at `stream` (`available` bytes readable; `size_hint` = the declared output size) as the expand
// owner would from a call with no hand-back due. False when the stream does not qualify (a copy from before the
// output, an output beyond size_hint + 256, a stream that ends within its token reach of `available`).
// `hint_short` (optional) is set when a decline came from the output reaching the hint's end, the one decline a
// larger hint can change (an input ending, or a copy from before the output, declines at any hint).
bool prs_decode_ahead(const u8* stream, std::size_t available, u32 size_hint, PrsDecoded& out,
                      bool* hint_short = nullptr);

// DCInvalidateRange / DCFlushRangeNoSync's owner body: leaves the registers their per-line loop leaves (game_memory.cpp;
// proven against the original words by cache_range_owner_tests).
void serve_cache_range_loop(Cpu& c);
// The same, resumed at the loop head (0x80372524 / 0x803725B0) with CTR lines left.
void serve_cache_range_loop_resume(Cpu& c);

class GameMemory {
public:
    explicit GameMemory(Cpu& cpu) noexcept : cpu_(cpu) {}
    ~GameMemory();
    GameMemory(const GameMemory&) = delete;
    GameMemory& operator=(const GameMemory&) = delete;

    // Installs the owners. Before the guest boots.
    void install();

    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    // Every guest address the generator must route to an owner here
    // (extract/native-owners.txt).
    static const Address* addresses(std::size_t& count) noexcept;

    struct Stats {
        // memmove (0x800054F4)
        u64 moves = 0;               // calls entered at the start
        u64 move_resumes = 0;        // entries at a loop head after a dispatch hand-back
        u64 move_yields = 0;         // hand-backs at a loop back edge (needs_dispatch)
        u64 move_bytes_bulk = 0;     // bytes moved as one host memmove
        u64 move_bytes_single = 0;   // bytes moved one at a time through the CPU accessors
        // PRS expand (0x80043FE0)
        u64 expansions = 0;          // calls entered at the start
        u64 expansion_resumes = 0;
        u64 expansion_yields = 0;
        u64 expanded_bytes = 0;      // bytes written by expansions that ran start to end without a hand-back
        u64 expansions_in_place = 0; // of those: the consumed source overlapped the written output
        // Game-thread host time of the expand owner (measurement, docs/NATIVE_THREADS.md): served from a stream
        // decoded ahead (a copy) vs run by the engine here (a decode slice; a yield ends the slice).
        u64 prs_ahead_served = 0, prs_ahead_ns = 0, prs_ahead_ns_max = 0;
        u64 prs_engine_slices = 0, prs_engine_ns = 0, prs_engine_ns_max = 0;
        u64 accessor_bytes = 0;      // PRS bytes read or written through the CPU accessors (not host memory)
        // NE-OS-0: the SDK's cache maintenance (no cache here: the owners return)
        u64 cache_syncs = 0;         // PPCSync 0x80370B14
        u64 cache_flushes = 0;       // DCFlushRange 0x80372534
        u64 cache_stores = 0;        // DCStoreRange 0x80372564
        u64 cache_invalidates = 0;   // DCInvalidateRange 0x80372508 (registers as the loop leaves them)
        u64 cache_flushes_no_sync = 0;  // DCFlushRangeNoSync 0x80372594 (likewise)
    };
    const Stats& stats() const noexcept { return stats_; }

    // The expand call served from a stream decoded ahead (gekko/prs_ahead.h): `ahead(first, accept)` gets the first
    // 16 source bytes and returns a decoded stream that `accept` (serve_prs_ahead's checks) took, or null. Unset:
    // every call decodes. Served only when no hand-back is due, no journal is open, the source bytes in guest memory
    // equal the decoded stream, and source and output are host memory that do not overlap; otherwise the call
    // decodes as before.
    using PrsAheadLookup = std::function<std::shared_ptr<const PrsDecoded>(
        const u8* first, const std::function<bool(const PrsDecoded&)>& accept)>;
    void set_prs_ahead(PrsAheadLookup lookup) { prs_ahead_ = std::move(lookup); }
    using PrsAheadReady =
        std::function<bool(const u8* first, const std::function<bool(const PrsDecoded&)>& check)>;
    void set_prs_ahead_ready(PrsAheadReady ready) { prs_ahead_ready_ = std::move(ready); }
    // Whether the decode-ahead holds a decoded stream equal to the guest's bytes at src now (nothing taken): a native
    // caller need not prepare that stream itself (Phase 3).
    bool ahead_has_stream(Cpu& c, u32 src) const;
    // Serves a PRS expand call (r3 = src, r4 = dst) from `decoded`: true when it did (output written, registers set
    // as the expander leaves them), false when a condition above fails (nothing changed).
    bool serve_prs_ahead(Cpu& c, const PrsDecoded& decoded);
    // The first 16 source bytes of an expand call at `src` when they are host memory (null otherwise).
    static const u8* prs_ahead_key_bytes(Cpu& c, u32 src);
    u64 prs_ahead_served() const noexcept { return prs_ahead_served_; }
    u64 prs_ahead_refused() const noexcept { return prs_ahead_refused_; }

    // A PRS expand for a NATIVE caller (no guest call, no register file: docs/NATIVE_STREAMING_REPLACEMENT_PLAN.md
    // Phase 1): src -> dst in guest memory, from the stream decoded ahead when there is one (the same checks as
    // serve_prs_ahead), else decoded here by the host decoder (prs_decode_ahead) straight from the guest's bytes.
    // Returns the bytes written, or nothing when it does not apply - a journal is open, the memory is not direct host
    // memory, the stream copies from before its output or overlaps it (in place) - and then nothing was changed: the
    // caller expands through the guest.
    // `prepared` (Phase 3): a stream the caller had decoded off the game thread from a copy of the guest's bytes
    // (snapshot_stream); used after the decode-ahead and before decoding here, under the same checks - the guest's
    // stream bytes must still equal the copy for every byte the decode consumed. With `decode_here` false it stops
    // before decoding on this thread (nothing changed, not counted as declined): the caller has other work to wait for.
    std::optional<u32> expand_for_native(Cpu& c, u32 src, u32 dst, const PrsDecoded* prepared = nullptr,
                                         bool decode_here = true);
    // memmove(dst, src, n) for a NATIVE caller, as the memcpy owner (0x800054F4) moves the bytes when no hand-back is
    // due: one host memmove when both ranges are direct host memory and no journal is open. False (nothing changed)
    // otherwise: the caller calls the owner.
    bool move_for_native(Cpu& c, u32 dst, u32 src, u32 n) const;
    // A copy of up to `max_bytes` guest bytes from src (fewer at the end of src's region). False when the memory is
    // not direct host memory or a journal is open (expand_for_native would decline anyway).
    bool snapshot_stream(Cpu& c, u32 src, std::size_t max_bytes, std::vector<u8>& out) const;
    struct NativeExpandStats {
        u64 served_ahead = 0;  // from a stream decoded ahead on the host workers (a copy)
        u64 served_prepared = 0;  // from a stream the caller decoded off the game thread (Phase 3)
        u64 decoded_here = 0;  // decoded on the calling (game) thread by the host decoder
        u64 declined = 0;      // nothing applied: the caller used the guest path
        u64 ns = 0, ns_max = 0;
    };
    const NativeExpandStats& native_expand_stats() const noexcept { return native_expand_stats_; }

private:
    Cpu& cpu_;
    Stats stats_;
    PrsAheadLookup prs_ahead_;
    PrsAheadReady prs_ahead_ready_;
    u64 prs_ahead_served_ = 0, prs_ahead_refused_ = 0;
    NativeExpandStats native_expand_stats_;
    // SHADOW_GAME_MEMORY_JOURNAL=<file>: one line per memmove / expansion (the
    // equality proof's per-call record); nullptr otherwise.
    std::FILE* journal_ = nullptr;
};

}  // namespace gekko
