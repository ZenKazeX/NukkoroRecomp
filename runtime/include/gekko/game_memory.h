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
bool prs_decode_ahead(const u8* stream, std::size_t available, u32 size_hint, PrsDecoded& out);

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
        u64 bat_configurations = 0;  // RealMode(Config24MB / Config48MB): nothing to configure
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
        u64 accessor_bytes = 0;      // PRS bytes read or written through the CPU accessors (not host memory)
        // NE-OS-0: the SDK's cache maintenance (no cache here: the owners return)
        u64 cache_syncs = 0;         // PPCSync 0x80370B14
        u64 cache_flushes = 0;       // DCFlushRange 0x80372534
        u64 cache_stores = 0;        // DCStoreRange 0x80372564
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
    // Serves a PRS expand call (r3 = src, r4 = dst) from `decoded`: true when it did (output written, registers set
    // as the expander leaves them), false when a condition above fails (nothing changed).
    bool serve_prs_ahead(Cpu& c, const PrsDecoded& decoded);
    // The first 16 source bytes of an expand call at `src` when they are host memory (null otherwise).
    static const u8* prs_ahead_key_bytes(Cpu& c, u32 src);
    u64 prs_ahead_served() const noexcept { return prs_ahead_served_; }
    u64 prs_ahead_refused() const noexcept { return prs_ahead_refused_; }

private:
    Cpu& cpu_;
    Stats stats_;
    PrsAheadLookup prs_ahead_;
    u64 prs_ahead_served_ = 0, prs_ahead_refused_ = 0;
    // SHADOW_GAME_MEMORY_JOURNAL=<file>: one line per memmove / expansion (the
    // equality proof's per-call record); nullptr otherwise.
    std::FILE* journal_ = nullptr;
};

}  // namespace gekko
