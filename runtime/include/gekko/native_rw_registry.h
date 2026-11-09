#pragma once

// The RenderWare GameCube driver's plugin-registry entries (gx-rw-plan cluster rwR5) as exact native owners
// (docs/agents/native-boot.md Step 8). RenderWare's engine start-up (0x80487548) registers these with
// RwEngineRegisterPlugin as plugin constructor / destructor / init callbacks; each is a short body over the engine's
// small-data slots and a few RenderWare core calls. The owners reproduce every guest-visible write and call the same
// RenderWare core functions through gekko::guest_call (never an SDK or driver function).
//
// Small data (r13-relative): 0x615C the engine instance, 0x6200 / 0x6218 plugin offsets, 0x6228 / 0x622C / 0x6230 the
// driver plugin's instance pointer, offset and reference count, 0x6238 the device-pool plugin offset, 0x6240 pool
// initialised, 0x6244 the pool's backing block, 0x2AF8..0x2B04 the pool's configuration words.
//
//   0x80494EB4  plugin init: r13+0x6200 = RwCore 0x8048ACA4(0x34, 0x40C, 0, 0, 0)   (the raster plugin's free list)
//                            r13+0x6218 = RwCore 0x8048E2D0(0x24, 0x40C, 0x8049AB74, 0x8049AB88, 0)
//               returns 1. (0x80496440 and 0x8049AC48 are inlined into it.)
//   0x8049BFE4  plugin destruct (r3 = object): releases the two lists of the driver plugin instance
//               (0x804A09E0 at +0x20, 0x804A0998 at +0x1C), decrements the reference count, returns r3.
//   0x8049C048  plugin construct (r3 = object, r4 = plugin offset): records offset / instance, counts the reference,
//               clears 0x74 bytes of the instance, builds its two lists (0x804A08FC +0x1C, 0x804A0A6C +0x20); on failure
//               releases them and returns 0, else returns r3.
//   0x8049C11C  device-pool construct (r3 = object, r4 = plugin offset): r13+0x6238 = r4; creates the pool
//               (0x8049C2A8, inlined); returns r3 or 0.
//   0x8049C160  device-pool destruct (r3 = object): destroys the pool (0x8049C248, inlined); returns r3.
//   0x8049C7BC  device-pool object create (no args): allocates through the engine's allocator ([engine+0x118]) and clears
//               0x34 bytes; on failure raises RenderWare error 0x80000013 (0x8047EC78 / 0x8047EBD4) and returns 0.
//   0x8049C850  device-pool object destroy (r3 = object or 0): removes each of its entries (0x8049C3A0), frees its block and
//               hands the object back to the engine's pool-free ([engine+0x11C]).
//   0x8049C3A0  entry remove (r3 = entry, r4 = pool object): releases the entry's blocks, compacts the pool's three
//               parallel arrays (0x28-byte entries, 0x80-byte blocks, 12-byte records), renumbers the indices the other
//               entries hold, and decrements the pool's count. Returns r3 = entry.
//   0x8049CE40  entry swap (r3 = pool object, r4 = index): swaps the entry at index [object+4] with the entry at r4 (the
//               entries, their 0x80-byte blocks and 12-byte records), renumbers the indices that name them, then raises the
//               count and re-balances the entries those indices name (recursively). Returns nothing (both callers ignore r3).
//   0x804662B0  world sector read (r3 = stream, r4 = world data): reads one chunk with the RenderWare stream functions
//               (find chunk, three 4-byte reads, two block reads), allocates the object through the engine ([engine+0x108]),
//               relocates the pointer tables by the second block's aligned address, fills the object header (r4+0x34) and returns
//               r4. (0x80465AE4 is inlined; DCFlushRange / GXInvalidateVtxCache, owned silent, are dropped.)
//   0x8046615C / 0x80466188 / 0x804661D0 / 0x804661FC / 0x80466250  the geometry's and the world sector's native-data stream
//               callbacks (size, write, read; rwR4, 0 hits on every recording): the original's bodies, exact (the write
//               un-relocates and relocates the pointer tables around the first block write; every failure answers as the
//               original does). The object lookup 0x80460EF0 is entered by guest_call.
//   0x8049A604  native texture write (r3 = stream, r4 = texture): the chunk header and the texture / raster headers, the
//               palette and the pixel block through RwStreamWrite (0x80481F20); the four GX texture-object getters the original
//               calls are inlined; the raster size row 0x804965A8 is entered through its native owner (stops with its name
//               when none is installed: no recording writes a texture).
//   0x80465818  world plugin attach: r13+0x60D8 = RwCore 0x8045BAF0(0x1C, 0x505, element clear, empty, empty);
//               returns r3 >= 0.
//   0x80465800 / 0x80465810 / 0x80465814  the world plugin's element destructor ([r3 + [r13+0x60D8]] = 0) and two empty
//               callbacks, passed to the plugin registration above.
//   0x8049AB74  texture free-list element initialiser (r3 = element): [r3 + [r13+0x6218] + 0x20] = 0x01000000.
//   0x8049AB88  texture free-list element terminator (r3 = element): clears every one of the eight bound-texture
//               slots 0x8056FCDC..0x8056FCF8 that holds r3.

#include "gekko/cpu.h"

#include <cstddef>

namespace gekko::rw_registry {

constexpr u32 kPluginInit = 0x80494EB4u;
constexpr u32 kDriverDestruct = 0x8049BFE4u;
constexpr u32 kDriverConstruct = 0x8049C048u;
constexpr u32 kPoolConstruct = 0x8049C11Cu;
constexpr u32 kPoolDestruct = 0x8049C160u;
constexpr u32 kPoolEntryCreate = 0x8049C7BCu;
constexpr u32 kPoolDestroyAll = 0x8049C850u;
constexpr u32 kPoolEntryRemove = 0x8049C3A0u;
constexpr u32 kPoolEntrySwap = 0x8049CE40u;
constexpr u32 kWorldAttach = 0x80465818u;
constexpr u32 kNativeTextureWrite = 0x8049A604u;
constexpr u32 kWorldSectorRead = 0x804662B0u;
constexpr u32 kGeometrySize = 0x8046615Cu;      // the native-data stream callbacks of the geometry / world-sector plugins (rwR4)
constexpr u32 kGeometryWrite = 0x80466188u;
constexpr u32 kGeometryRead = 0x804661D0u;
constexpr u32 kSectorSize = 0x804661FCu;
constexpr u32 kSectorWrite = 0x80466250u;
constexpr u32 kWorldElementClear = 0x80465800u;  // the world plugin's element destructor (row of three entries)
constexpr u32 kWorldEmptyA = 0x80465810u;        // two `blr` entries inside that row
constexpr u32 kWorldEmptyB = 0x80465814u;
constexpr u32 kElementInit = 0x8049AB74u;     // the texture free list's element initialiser (RenderWare core calls it)
constexpr u32 kElementTerm = 0x8049AB88u;     // ... and its element terminator

struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {
            {kPluginInit, "RenderWare plugin init"},
            {kDriverDestruct, "driver plugin destruct"},
            {kDriverConstruct, "driver plugin construct"},
            {kPoolConstruct, "device pool construct"},
            {kPoolDestruct, "device pool destruct"},
            {kPoolEntryCreate, "device pool object create"},
            {kPoolDestroyAll, "device pool object destroy"},
            {kPoolEntryRemove, "device pool entry remove"},
            {kPoolEntrySwap, "device pool entry swap"},
            {kWorldSectorRead, "world sector read"},
            {kGeometrySize, "geometry native chunk size"},
            {kGeometryWrite, "geometry native chunk write"},
            {kGeometryRead, "geometry native chunk read"},
            {kSectorSize, "world sector native chunk size"},
            {kSectorWrite, "world sector native chunk write"},
            {kNativeTextureWrite, "native texture write"},
            {kWorldAttach, "world plugin attach"},
            {kWorldElementClear, "world plugin element destructor"},
            {kWorldEmptyA, "world plugin empty callback"},
            {kWorldEmptyB, "world plugin empty callback"},
            {kElementInit, "texture free-list element initialiser"},
            {kElementTerm, "texture free-list element terminator"},
        };
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

// The served bodies (for the contract test). A guest call may stop the run: they return false then.
bool serve_plugin_init(Cpu& cpu);
bool serve_driver_destruct(Cpu& cpu);
bool serve_driver_construct(Cpu& cpu);
bool serve_pool_construct(Cpu& cpu);
bool serve_pool_destruct(Cpu& cpu);
bool serve_pool_entry_create(Cpu& cpu);
bool serve_pool_destroy_all(Cpu& cpu);
bool serve_pool_entry_remove(Cpu& cpu);
bool serve_pool_entry_swap(Cpu& cpu);
bool serve_world_attach(Cpu& cpu);
bool serve_native_texture_write(Cpu& cpu);
bool serve_world_sector_read(Cpu& cpu);
bool serve_geometry_size(Cpu& cpu);
bool serve_geometry_write(Cpu& cpu);
bool serve_geometry_read(Cpu& cpu);
bool serve_sector_size(Cpu& cpu);
bool serve_sector_write(Cpu& cpu);
void serve_world_element_clear(Cpu& cpu) noexcept;
void serve_element_init(Cpu& cpu) noexcept;
void serve_element_term(Cpu& cpu) noexcept;

// Installs the owners (LiftedRender::install_owners).
void install(Cpu& cpu);

}  // namespace gekko::rw_registry
