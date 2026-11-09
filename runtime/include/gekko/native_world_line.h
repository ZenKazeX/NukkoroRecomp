#pragma once

// RenderWare's world line query, rebuilt natively (docs/engine/WORLD_COLLISION.md).
//
// fn_804421D8(r3 = world, r4 = line, r5 = callback, r6 = data) is RpCollisionWorldForAllIntersections for a line,
// inlined: it walks the world's plane-sector BSP with an explicit stack of {node, t0, t1} (the line's parameter
// interval, clipped at each split plane widened by a tolerance), and in every world sector that has collision data it
// walks that sector's collision tree the same way down to triangle ranges; each triangle is tested against the line
// (determinant above 1e-8, the edge and t tests with a 1e-5 relative slack); for a hit it fills an
// RpCollisionTriangle in its frame (normal scaled by RenderWare's table reciprocal square root 0x804833E4, the first
// vertex, the triangle index, three vertex pointers) and calls callback(line, sector, &triangle, t, data); a NULL
// return ends the query. It is the largest translated self-cost of the game thread (2.0 %, docs/CPU_FINAL_HAUNT.md).
//
// This owner runs the same algorithm in native C++: the same traversal order, the same comparisons, every float
// operation through the core's own single-precision helpers (ppc_ops.h fps_*), so every value is the translated
// code's bit for bit (NaN and denormal behaviour included). It builds the frame the callback sees (back chain, the
// triangle at r1 + 8) and calls the guest callback with the original's argument registers; it leaves the original's
// contract (r1 - r3, r13 - r31, f14 - f31, LR, CR2 - 4, FPSCR's control byte) and the caller's LR save word.
// Declines (the guest body runs) when FP is disabled, an FPSCR enable bit is set or RN is not round-to-nearest
// (NI = 1 is the game's mode throughout and is served).
//
// GEKKO_NATIVE_WORLD_LINE=0 leaves every call to the guest (A/B). GEKKO_VERIFY_WORLD_LINE=n: every n-th served call
// also runs the guest body from the same state (memory rolled back through the write journal) and compares the
// contract registers and every byte either run wrote outside the current thread's dead stack (below the caller's r1);
// the guest's result is kept.

#include <array>
#include <map>

#include "gekko/types.h"

namespace gekko {

class Cpu;

class NativeWorldLine {
public:
    explicit NativeWorldLine(Cpu& cpu) noexcept;
    ~NativeWorldLine();

    void install();

    struct Stats {
        u64 calls = 0;
        u64 served = 0;
        u64 declined = 0;      // FP state outside the contract: the guest body ran
        u64 sectors = 0;       // world sectors with collision data visited
        u64 triangles = 0;     // triangles tested
        u64 hits = 0;          // callbacks made
        u64 stopped = 0;       // queries ended by a NULL callback return
        u64 verified = 0;
        u64 differed = 0;
        u64 unverifiable = 0;  // nested, journal overflow or device touched: compared nothing
    };
    const Stats& stats() const noexcept { return stats_; }
    // While another owner runs the original code for verification, this one declines too.
    void set_reference(bool reference) noexcept { reference_ = reference; }

private:
    bool serve(Cpu& c);
    bool run(Cpu& c);
    bool verify(Cpu& c);

    Cpu& cpu_;
    bool enabled_ = true;
    u64 verify_every_ = 0;
    bool reference_ = false;  // the guest body is running for verification: decline
    u32 depth_ = 0;           // nested queries (a callback querying again)
    Stats stats_;
    // GEKKO_WORLD_LINE_CENSUS: per {callback, three return addresses up the stack}: calls, hits, stops, triangles.
    bool census_ = false;
    std::map<std::array<u32, 4>, std::array<u64, 4>> census_rows_;
};

}  // namespace gekko
