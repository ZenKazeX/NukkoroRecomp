#pragma once

// The game's world collision queries, rebuilt as native engine code (docs/engine/WORLD_COLLISION.md).
//
// Every world query the game makes goes through RenderWare's dispatcher fn_80443AF4(world, intersection, callback,
// data) from two wrappers that first store a filter function and its data in [r13 + 0x2BF0] / [r13 + 0x2BF4]:
//   - the line pick fn_80021678: a line (type 1), callback 0x80020DC0, data = &t_min. The query fn_804421D8 walks
//     the world BSP and each sector's collision tree along the line; 0x80020DC0 keeps the closest accepted hit:
//     t_min, the triangle record at 0x80571CCC and the surface attribute at [r13 + 0x2BEC].
//   - the sphere query fn_800215C0: a sphere (type 3), callback 0x80020C24. The query fn_80443260 walks the trees
//     with the sphere's box (and per tree node the sides still outside the cube inscribed in the sphere); triangles
//     of nodes inside that cube are reported with their plane distance, the others go through RenderWare's
//     sphere-triangle test fn_8047C770. 0x80020C24 filters, drops hits behind the plane unless [r13 + 0x2BE8] is
//     set, and builds the game's result objects (fn_80020D40, fn_800201D4).
// The surface attribute of a triangle is the sector's first user-data array [triangle index]; the filters are mask
// tests on it.
//
// This owner replaces the dispatcher, both queries, both callbacks' query-side work and the filters:
//   - The collision geometry is mirrored once into native structures (host floats; per triangle its vertices and
//     edges; the trees as index arrays with each node's leaf range) and dropped when RenderWare's collision plugin
//     constructs, reads or destroys the sector (hooks on 0x80441DF8, 0x80441ED0, 0x80441DA8).
//   - The traversals and the triangle tests are the original algorithms in host single precision; the operations the
//     original fuses (fmadds / fmsubs) use the host's fused multiply-add. No PowerPC state is emulated.
//   - The filters seen in the game are reconstructed natively; any other filter is called as guest code. The sphere
//     path's result objects are still built by the game's own code (fn_800201D4), once per accepted hit.
//
// GEKKO_NATIVE_LINE_PICK=0 / GEKKO_NATIVE_SPHERE_QUERY=0 leave that path to the guest. GEKKO_VERIFY_WORLD_COLLISION=n:
// every n-th served call also runs the original from the same state (memory rolled back through the write journal)
// and compares r1 - r3, r13 - r31, f14 - f31, LR, CR2 - 4 and every byte either run wrote outside the current thread's
// dead stack. GEKKO_WORLD_COLLISION_STATS=1 prints the counters.

#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class Cpu;
class NativeWorldLine;

class NativeWorldCollision {
public:
    NativeWorldCollision(Cpu& cpu, NativeWorldLine* world_line) noexcept;
    ~NativeWorldCollision();

    void install();

    struct Stats {
        u64 calls = 0;
        u64 lines = 0;          // line picks served
        u64 spheres = 0;        // sphere queries served
        u64 declined = 0;       // another intersection type or callback, or geometry the mirror rejected
        u64 sectors = 0;        // sectors with collision data visited
        u64 triangles = 0;      // triangles tested
        u64 hits = 0;           // triangles the shape touches
        u64 accepted = 0;       // line: closer hits the filter accepted; sphere: hits handed to the result builder
        u64 guest_filters = 0;  // filters called as guest code
        u64 mirrored = 0;       // sectors mirrored
        u64 worlds = 0;         // world trees mirrored
        u64 dropped = 0;        // mirrors dropped by the plugin's constructor / reader / destructor
        u64 verified = 0;
        u64 differed = 0;
        u64 unverifiable = 0;
    };
    const Stats& stats() const noexcept { return stats_; }

    struct Sector;
    struct World;

private:
    bool serve(Cpu& c);
    bool run_line(Cpu& c);
    bool run_sphere(Cpu& c);
    bool verify(Cpu& c, bool sphere);
    void drop_sector(u32 sector);
    const Sector* sector_mirror(Cpu& c, u32 sector, u32 plugin);
    const World* world_mirror(Cpu& c, u32 world, u32 plugin);
    void load_rsqrt(Cpu& c);
    // The filter for one hit; a guest filter is called with the original's frames (query frame size, the record's
    // offset in it, the filter call's return address).
    bool filter(Cpu& c, u32 fn, u32 data, u32 attribute, u32 record, u32 query_frame, u32 link);

    Cpu& cpu_;
    NativeWorldLine* world_line_;
    bool lines_ = true, spheres_ = true;
    u64 verify_every_ = 0;
    bool reference_ = false;
    u32 depth_ = 0;  // nested queries (guest code inside a query querying again)
    std::unordered_map<u32, std::unique_ptr<Sector>> sectors_;
    std::unordered_map<u32, std::unique_ptr<World>> worlds_;
    u32 last_world_at_ = 0;
    const World* last_world_ = nullptr;  // the last world queried (cleared by any drop)
    std::unordered_map<u32, std::vector<u32>> sector_worlds_;  // sector -> worlds whose mirror lists it
    u32 rsqrt_table_ = 0;
    std::vector<u32> rsqrt_;  // RenderWare's reciprocal square root table, mirrored
    bool census_ = false;              // GEKKO_WORLD_COLLISION_STATS: count hits per filter
    u32 trace_left_ = 0;               // GEKKO_WORLD_COLLISION_TRACE: queries still to print
    std::map<u32, u64> filters_seen_;  // filter function -> hits
    Stats stats_;
};

}  // namespace gekko
