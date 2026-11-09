// The RenderWare driver's plugin-registry entries, native. Contract: include/gekko/native_rw_registry.h.

#include "gekko/native_rw_registry.h"

#include "gekko/native_guest_call.h"

#include <cstring>

namespace gekko::rw_registry {
namespace {

// ---- small data (r13-relative)
constexpr u32 kSdaEngine = 0x615Cu;         // the engine instance
constexpr u32 kSdaRasterPlugin = 0x6200u;   // set by the plugin init
constexpr u32 kSdaTexturePlugin = 0x6218u;  // set by the plugin init
constexpr u32 kSdaDriverInstance = 0x6228u;
constexpr u32 kSdaDriverOffset = 0x622Cu;
constexpr u32 kSdaDriverRefs = 0x6230u;
constexpr u32 kSdaPoolOffset = 0x6238u;
constexpr u32 kSdaPoolReady = 0x6240u;
constexpr u32 kSdaPoolBlock = 0x6244u;
constexpr u32 kSdaPoolCfgA = 0x2AF8u, kSdaPoolCfgB = 0x2AFCu, kSdaPoolCfgC = 0x2B00u, kSdaPoolCfgD = 0x2B04u;

// ---- RenderWare core functions the bodies call (guest_call targets)
constexpr u32 kFreeListCreateA = 0x8048ACA4u;   // (0x34, 0x40C, 0, 0, 0)
constexpr u32 kFreeListCreateB = 0x8048E2D0u;   // (0x24, 0x40C, init, term, 0)
constexpr u32 kListRelease20 = 0x804A09E0u;
constexpr u32 kListRelease1C = 0x804A0998u;
constexpr u32 kListBuild1C = 0x804A08FCu;
constexpr u32 kListBuild20 = 0x804A0A6Cu;
constexpr u32 kBlockCreate = 0x804A01B8u;
constexpr u32 kBlockDestroy = 0x804A011Cu;
constexpr u32 kPoolCreate = 0x80480C54u;
constexpr u32 kPoolDestroy = 0x80480D2Cu;
constexpr u32 kPoolListInit = 0x804A02F4u;

constexpr u32 kBoundTextureSlots = 0x8056FCDCu;  // eight words: the textures the driver has bound
constexpr u32 kPoolDescriptor = 0x805E4338u;  // the pool's static descriptor (an argument of kPoolCreate)

// The link address of a guest call: a return address inside the owner's own row (4 * n past its entry), never fetched (see
// native_guest_call.h). The callee sees an LR inside the owner's row instead of inside the original (inlined) body; the
// RenderWare core functions called here do not read their LR.
constexpr u32 link_of(u32 entry, u32 n) { return entry + 4u * n; }

u32 rd(Cpu& c, u32 sda_offset) { return c.mem_read32(c.gpr[13] + sda_offset); }
void wr(Cpu& c, u32 sda_offset, u32 value) { c.mem_write32(c.gpr[13] + sda_offset, value); }

// The original's address arithmetic: engine instance + plugin offset (+ field), re-read from small data at each use.
u32 instance_field(Cpu& c, u32 offset_slot, u32 field) { return rd(c, kSdaEngine) + rd(c, offset_slot) + field; }

// 0x8049C2A8: the device pool's creation. result 0 or 1 (r3).
bool pool_create(Cpu& c, u32& result) {
    constexpr u32 E = kPoolConstruct + 0u;  // the row address, the base of the link addresses
    result = 0u;
    if (rd(c, kSdaPoolReady) != 0u) return true;
    u32 block = 0;
    if (!guest_call(c, kBlockCreate, link_of(E, 1), {rd(c, kSdaPoolCfgA)}, block)) return false;
    wr(c, kSdaPoolBlock, block);
    if (block == 0u) return true;
    u32 pool = 0;
    if (!guest_call(c, kPoolCreate, link_of(E, 2), {0x34u, rd(c, kSdaPoolCfgC), 4u, rd(c, kSdaPoolCfgD), kPoolDescriptor, 0x40409u},
                    pool))
        return false;
    c.mem_write32(rd(c, kSdaEngine) + rd(c, kSdaPoolOffset), pool);
    const u32 slot = rd(c, kSdaEngine) + rd(c, kSdaPoolOffset);
    if (c.mem_read32(slot) == 0u) {
        u32 ignored = 0;
        if (!guest_call(c, kBlockDestroy, link_of(E, 3), {rd(c, kSdaPoolBlock)}, ignored)) return false;
        wr(c, kSdaPoolBlock, 0u);
        return true;
    }
    c.mem_write32(slot + 0x38u, rd(c, kSdaPoolCfgB));
    u32 ignored = 0;
    if (!guest_call(c, kPoolListInit, link_of(E, 4), {instance_field(c, kSdaPoolOffset, 4u)}, ignored)) return false;
    c.mem_write32(instance_field(c, kSdaPoolOffset, 0x34u), 0u);
    c.mem_write32(instance_field(c, kSdaPoolOffset, 0x30u), 0u);
    wr(c, kSdaPoolReady, 1u);
    result = 1u;
    return true;
}

// 0x8049C248: the device pool's destruction (always r3 = 1).
bool pool_destroy(Cpu& c) {
    constexpr u32 E = kPoolDestruct + 0u;  // the row address, the base of the link addresses
    if (rd(c, kSdaPoolReady) != 0u) {
        u32 ignored = 0;
        if (!guest_call(c, kPoolDestroy, link_of(E, 1), {c.mem_read32(instance_field(c, kSdaPoolOffset, 0u))}, ignored)) return false;
        c.mem_write32(instance_field(c, kSdaPoolOffset, 0u), 0u);
        if (!guest_call(c, kBlockDestroy, link_of(E, 2), {rd(c, kSdaPoolBlock)}, ignored)) return false;
        wr(c, kSdaPoolBlock, 0u);
        wr(c, kSdaPoolReady, 0u);
    }
    c.gpr[3] = 1u;
    return true;
}


// ---- engine function table slots ([engine + slot]: the engine's allocator hooks)
constexpr u32 kEngineFree = 0x10Cu, kEngineAlloc = 0x118u, kEnginePoolFree = 0x11Cu;
constexpr u32 kSdaWorldPlugin = 0x60D8u;
constexpr u32 kWorldRegisterPlugin = 0x8045BAF0u;
constexpr u32 kErrorGenerate = 0x8047EC78u, kErrorSet = 0x8047EBD4u;
constexpr u32 kStreamFindChunk = 0x8047DFA0u, kStreamRead = 0x8047E8B8u, kStreamBytes = 0x80481D9Cu;
constexpr u32 kWorldSectorDestroy = 0x804638DCu;  // the object's destructor callback (stored at +0x14)

u32 r32(Cpu& c, u32 address) { return c.mem_read32(address); }
void w32(Cpu& c, u32 address, u32 value) { c.mem_write32(address, value); }

bool engine_free(Cpu& c, u32 pointer, u32 link) {
    u32 ignored = 0;
    return guest_call(c, r32(c, rd(c, kSdaEngine) + kEngineFree), link, {pointer}, ignored);
}

// 0x800054F4: the byte copy the original calls (forward when src >= dst, backward otherwise).
void guest_copy(Cpu& c, u32 dst, u32 src, u32 n) {
    if (src >= dst) {
        for (u32 i = 0; i < n; ++i) c.mem_write8(dst + i, c.mem_read8(src + i));
    } else {
        for (u32 i = n; i > 0; --i) c.mem_write8(dst + i - 1u, c.mem_read8(src + i - 1u));
    }
}
void guest_zero(Cpu& c, u32 dst, u32 n) {
    for (u32 i = 0; i < n; ++i) c.mem_write8(dst + i, 0u);
}

// The pool manager field the original reads as engine[+[r13+0x6238]+0x38] (the per-entry block count).
u32 pool_k(Cpu& c) { return r32(c, rd(c, kSdaEngine) + rd(c, kSdaPoolOffset) + 0x38u); }

// 0x8049C3A0 (r3 = entry e, r4 = pool object P): returns false when a guest call stopped the run.
bool entry_remove(Cpu& c, u32 e, u32 P) {
    constexpr u32 E = kPoolEntryRemove + 0u;  // the row address, the base of the link addresses
    u32 ignored = 0;
    if (r32(c, P) == 0u) {
        u32 obj = r32(c, e);
        u32 fn = r32(c, obj + 0x14u);
        if (fn != 0u && !guest_call(c, fn, link_of(E, 1), {e}, ignored)) return false;
        obj = r32(c, e);
        w32(c, obj + 0x3Cu, r32(c, obj + 0x3Cu) - 1u);
        obj = r32(c, e);
        if (r32(c, obj + 0x3Cu) == 0u) {
            fn = r32(c, obj + 0xCu);
            if (fn != 0u && !guest_call(c, fn, link_of(E, 2), {obj}, ignored)) return false;
            obj = r32(c, e);
            if (r32(c, obj + 0x38u) != 0u) {
                if (!engine_free(c, obj, link_of(E, 3))) return false;
                w32(c, e, 0u);
            }
        }
        const u32 block = r32(c, e + 0x20u);
        if (block != 0u) {
            if (!engine_free(c, block, link_of(E, 4))) return false;
            w32(c, e + 0x20u, 0u);
            w32(c, e + 0x24u, 0u);
        }
        guest_zero(c, e, 0x28u);
    } else {
        const u32 block = r32(c, e + 0x20u);
        if (block != 0u) {
            if (!engine_free(c, block, link_of(E, 5))) return false;
            w32(c, e + 0x20u, 0u);
            w32(c, e + 0x24u, 0u);
        }
        const u32 obj = r32(c, e);
        if (r32(c, obj + 0x3Cu) == 0u && r32(c, obj + 0x38u) != 0u) {
            if (!engine_free(c, obj, link_of(E, 6))) return false;
            w32(c, e, 0u);
        }
        const u32 base = r32(c, P + 8u);
        const s32 difference = static_cast<s32>(e - base);
        const s32 high = static_cast<s32>((static_cast<s64>(static_cast<s32>(0x66666667)) * difference) >> 32);
        const s32 quotient = high >> 4;
        const u32 idx = static_cast<u32>(quotient) + (static_cast<u32>(quotient) >> 31);
        if (idx < r32(c, P + 4u) - 1u) {
            // the 0x80-byte blocks
            u32 dst = base + pool_k(c) * 0x28u + idx * 0x80u;
            u32 src = dst + 0x80u;
            for (u32 i = idx; i < r32(c, P + 4u) - 1u; ++i) {
                guest_copy(c, dst, src, 0x80u);
                dst = src;
                src += 0x80u;
            }
            // the 12-byte records (the original's start address does not depend on idx)
            const u32 k = pool_k(c);
            const u32 base2 = r32(c, P + 8u);
            dst = base2 + k * 0x28u + k * 0x80u;
            src = dst + 0xCu;
            for (u32 i = idx; i < r32(c, P + 4u) - 1u; ++i) {
                guest_copy(c, dst, src, 0xCu);
                dst = src;
                src += 0xCu;
            }
            // the entries, with their block / record pointers moved back by one slot
            u32 offset = idx * 0x28u;
            for (u32 i = idx; i < r32(c, P + 4u) - 1u; ++i) {
                guest_copy(c, r32(c, P + 8u) + offset, r32(c, P + 8u) + (i + 1u) * 0x28u, 0x28u);
                const u32 array = r32(c, P + 8u);
                const u32 field8 = offset + 8u, field1c = offset + 0x1Cu;
                offset += 0x28u;
                w32(c, array + field8, r32(c, array + field8) - 0x80u);
                const u32 array2 = r32(c, P + 8u);
                w32(c, array2 + field1c, r32(c, array2 + field1c) - 0xCu);
            }
            // the indices the entries hold: those past idx move down, idx itself becomes -1
            u32 entry_offset = 0;
            for (u32 i = 0; i < r32(c, P + 4u) - 1u; ++i, entry_offset += 0x28u) {
                u32 byte_offset = 0;
                for (u32 j = 0;; ++j, byte_offset += 4u) {
                    const u32 entry = r32(c, P + 8u) + entry_offset;
                    if (!(j < r32(c, entry + 4u))) break;
                    const u32 list = r32(c, entry + 8u);
                    const u32 v = r32(c, list + byte_offset);
                    if (v < idx) continue;
                    w32(c, list + byte_offset, v == idx ? 0xFFFFFFFFu : v - 1u);
                }
            }
        }
    }
    w32(c, P + 4u, r32(c, P + 4u) - 1u);
    c.gpr[3] = e;
    return true;
}

// 0x8049CE40 (r3 = pool object P, r4 = index b); recursive; makes no guest call.
void entry_swap(Cpu& c, u32 P, u32 b) {
    const u32 a = r32(c, P + 4u);
    if (a != b) {
        const u32 q = r32(c, P);
        const u32 arr0 = r32(c, q + 8u);
        const u32 off_a = a * 0x28u, off_b = b * 0x28u;
        const u32 block_a = r32(c, arr0 + 8u + off_a), block_b = r32(c, arr0 + 8u + off_b);
        for (u32 w = 0; w < 32u; ++w) {  // swap the two 0x80-byte blocks, word by word
            const u32 x = r32(c, block_a + 4u * w), y = r32(c, block_b + 4u * w);
            w32(c, block_a + 4u * w, y);
            w32(c, block_b + 4u * w, x);
        }
        w32(c, r32(c, r32(c, P) + 8u) + off_a + 8u, block_b);
        w32(c, r32(c, r32(c, P) + 8u) + off_b + 8u, block_a);
        // the 12-byte records
        const u32 arr1 = r32(c, r32(c, P) + 8u);
        const u32 rec_a = r32(c, arr1 + 0x1Cu + off_a), rec_b = r32(c, arr1 + 0x1Cu + off_b);
        const u32 a0 = r32(c, rec_a), a1 = r32(c, rec_a + 4u), a2 = r32(c, rec_a + 8u);
        const u32 b0 = r32(c, rec_b), b1 = r32(c, rec_b + 4u);
        w32(c, rec_a, b0);
        w32(c, rec_a + 4u, b1);
        w32(c, rec_a + 8u, r32(c, rec_b + 8u));
        w32(c, rec_b, a0);
        w32(c, rec_b + 4u, a1);
        w32(c, rec_b + 8u, a2);
        w32(c, r32(c, r32(c, P) + 8u) + off_a + 0x1Cu, rec_b);
        w32(c, r32(c, r32(c, P) + 8u) + off_b + 0x1Cu, rec_a);
        // the two 0x28-byte entries
        const u32 arr2 = r32(c, r32(c, P) + 8u);
        const u32 ea = arr2 + off_a, eb = arr2 + off_b;
        u32 saved[10];
        for (u32 w = 0; w < 10u; ++w) saved[w] = r32(c, ea + 4u * w);
        for (u32 w = 0; w < 10u; ++w) w32(c, ea + 4u * w, r32(c, eb + 4u * w));
        const u32 eb2 = r32(c, r32(c, P) + 8u) + off_b;
        for (u32 w = 0; w < 10u; ++w) w32(c, eb2 + 4u * w, saved[w]);
        // the indices that name the two entries trade places
        u32 entry_offset = 0;
        for (u32 i = 0; i < r32(c, r32(c, P) + 4u); ++i, entry_offset += 0x28u) {
            const u32 array = r32(c, r32(c, P) + 8u);
            u32 byte_offset = 0;
            for (u32 j = 0; j < r32(c, array + entry_offset + 4u); ++j, byte_offset += 4u) {
                const u32 list = r32(c, array + entry_offset + 8u);
                const u32 v = r32(c, list + byte_offset);
                if (a == v) w32(c, list + byte_offset, b);
                else if (b == v) w32(c, list + byte_offset, a);
            }
        }
    }
    // count it, then follow the indices the entry at the old count holds
    const u32 count = r32(c, P + 4u);
    const u32 entry_array = r32(c, r32(c, P) + 8u);
    w32(c, P + 4u, count + 1u);
    const u32 entry = entry_array + count * 0x28u;
    if (r32(c, entry + 4u) != 0u) {
        u32 byte_offset = 0;
        for (u32 k = 0; k < r32(c, entry + 4u); ++k, byte_offset += 4u) {
            const u32 v = r32(c, r32(c, entry + 8u) + byte_offset);
            if (v == 0xFFFFFFFFu) continue;
            const u32 other = r32(c, r32(c, P) + 8u) + v * 0x28u;
            const u32 record = r32(c, other + 0x1Cu);
            w32(c, record + 4u, r32(c, record + 4u) + 1u);
            const u32 record2 = r32(c, other + 0x1Cu);
            if (r32(c, record2) == r32(c, record2 + 4u)) entry_swap(c, P, v);
        }
    }
}

// Holds a stack frame below the caller's stack pointer while a native body builds temporaries that RenderWare functions read
// through pointers: guest_call runs the callee with the CURRENT r1 as its frame top, so without this the callee's own frame
// would overwrite the temporaries (the original's frame sits above its callees' frames). Restores r1 on exit.
struct Frame {
    Cpu& c;
    u32 saved;
    Frame(Cpu& cpu, u32 bytes) : c(cpu), saved(cpu.gpr[1]) { c.gpr[1] = saved - bytes; }
    ~Frame() { c.gpr[1] = saved; }
};

// 0x804662B0 -> 0x80465AE4 (readWorldSectorNative__sub0 and the body it calls): reads one world-sector chunk. The frame the
// original's callee makes is 0x50 below the entry stack pointer (0x10 for 0x804662B0, 0x40 for 0x80465AE4); the RenderWare
// stream functions are handed pointers into it, so the native body uses the same addresses.
bool world_sector_read(Cpu& c, u32 E, u32 stream, u32 argument, u32 out, u32 count) {
    // E: the row address, the base of the link addresses (0x804662B0 reads a world sector, 0x804661D0 a geometry: the
    // same body 0x80465AE4 under two one-call wrappers of the same frame size)
    Frame hold(c, 0x50u);  // the original's frames: 0x10 (0x804662B0) + 0x40 (0x80465AE4)
    const u32 frame = c.gpr[1];
    const u32 kTag = frame + 8u, kSecond = frame + 0xCu, kFirst = frame + 0x10u, kChunk = frame + 0x14u, kVersion = frame + 0x18u;
    u32 r = 0, ignored = 0;
    if (!guest_call(c, kStreamFindChunk, link_of(E, 1), {stream, 1u, kChunk, kVersion}, r)) return false;
    if (r == 0u) { c.gpr[3] = 0u; return true; }
    const u32 version = r32(c, kVersion);
    if (version < 0x35000u || version > 0x37002u) {
        w32(c, frame + 0x1Cu, 2u);
        u32 code = 0;
        if (!guest_call(c, kErrorGenerate, link_of(E, 2), {0x80000004u}, code)) return false;
        w32(c, frame + 0x20u, code);
        if (!guest_call(c, kErrorSet, link_of(E, 3), {frame + 0x1Cu}, ignored)) return false;
        c.gpr[3] = 0u;
        return true;
    }
    if (!guest_call(c, kStreamRead, link_of(E, 4), {stream, kTag, 4u}, r)) return false;
    if (r == 0u || r32(c, kTag) != 6u) { c.gpr[3] = 0u; return true; }
    if (!guest_call(c, kStreamRead, link_of(E, 5), {stream, kFirst, 4u}, r)) return false;
    if (r == 0u) { c.gpr[3] = 0u; return true; }
    if (!guest_call(c, kStreamRead, link_of(E, 6), {stream, kSecond, 4u}, r)) return false;
    if (r == 0u) { c.gpr[3] = 0u; return true; }
    const u32 engine = rd(c, kSdaEngine);
    u32 object = 0;
    if (!guest_call(c, r32(c, engine + 0x108u), link_of(E, 7), {r32(c, kFirst) + r32(c, kSecond) + 0x37u, 0x3050Du}, object))
        return false;
    w32(c, out, object);
    const u32 header = r32(c, out) + 0x18u;
    if (!guest_call(c, kStreamBytes, link_of(E, 8), {stream, header, r32(c, kFirst)}, r)) return false;
    if (r32(c, kFirst) != r) { c.gpr[3] = 0u; return true; }
    const u32 second_target = (header + r32(c, kFirst) + 0x1Fu) & 0xFFFFFFE0u;
    if (!guest_call(c, kStreamBytes, link_of(E, 9), {stream, second_target, r32(c, kSecond)}, r)) return false;
    if (r32(c, kSecond) != r) { c.gpr[3] = 0u; return true; }
    // pointer relocation: the second block's address is added to the table words
    const u32 entries = r32(c, header + 8u);
    const u32 tail = header + (entries - 1u) * 8u + 0x14u;
    const u32 base = (tail + count * 8u + 0x1Fu) & 0xFFFFFFE0u;
    for (u32 i = 0; i < r32(c, header + 8u); ++i) {
        const u32 word = header + 8u * i + 0xCu;
        w32(c, word, r32(c, word) + base);
    }
    for (u32 i = 0; i < count; ++i) {
        const u32 word = tail + 8u * i;
        w32(c, word, r32(c, word) + base);
    }
    // the object's own header; DCFlushRange and GXInvalidateVtxCache (owned silent: no cache, no vertex cache) are dropped
    w32(c, r32(c, out), 0u);
    w32(c, r32(c, out) + 4u, 0u);
    w32(c, r32(c, out) + 0xCu, argument);
    w32(c, r32(c, out) + 8u, r32(c, kChunk));
    w32(c, r32(c, out) + 0x10u, out);
    w32(c, r32(c, out) + 0x14u, kWorldSectorDestroy);
    c.mem_write16(header, static_cast<u16>(c.mem_read16(c.gpr[13] + 0x2AF0u)));
    c.gpr[3] = argument;
    return true;
}

// 0x8049A604 (GcnNativeTextureWrite): serializes a texture to a RenderWare stream. r4 = the texture; [r4] its raster; the
// texture's platform data (a GXTexObj) sits at r4 + [r13+0x6218], the raster's at parent + [r13+0x6200]. The four GX texture
// object getters the original calls (0x803982AC wrap S, 0x8039828C wrap T, 0x80398298 mipmap flag, 0x8039824C LOD bias) are tiny
// bit extractions of the texture object's first word and are inlined here. 0x804965A8 (all mip levels' bytes) is another
// driver row: it is entered through its native owner (Cpu::serve_owner), never as guest code; without that owner the run stops
// with its name (texture writing is not part of the game's run: no recording enters this row).
constexpr u32 kStreamChunkHeader = 0x8047DF40u, kStreamWrite = 0x80481F20u, kRasterFilter = 0x8048AD34u;
constexpr u32 kRasterBytes = 0x804965A8u;
constexpr u32 kSdaRasterPlugin2 = 0x6200u, kSdaTexturePlugin2 = 0x6218u;

u32 texture_raster_bytes(Cpu& c, u32 raster, bool& ok) {
    const u32 saved3 = c.gpr[3];
    c.gpr[3] = raster;
    ok = c.serve_owner(kRasterBytes);
    const u32 bytes = c.gpr[3];
    c.gpr[3] = saved3;
    if (!ok)
        c.request_stop(StopReason::ExecutionFault,
                       "native texture write: the raster size row 0x804965A8 has no native owner in this build");
    return bytes;
}

bool native_texture_write(Cpu& c, u32 stream, u32 texture_arg) {
    constexpr u32 E = kNativeTextureWrite + 0u;
    Frame hold(c, 0x90u);
    const u32 sp = c.gpr[1];
    const u32 r2 = c.gpr[2];
    u32 r = 0;
    // the stream size of the chunk
    u32 raster = r32(c, texture_arg);
    if (raster == 0u) {
        w32(c, sp + 8u, 0x64u);
    } else {
        const u32 flags = c.mem_read8(raster + 0x23u);
        u32 size = 0x74u;
        if ((flags & 0x60u) != 0u) {
            const u32 shift = r32(c, raster + 0x14u) & 0x3Fu;
            size = (shift >= 32u ? 0u : (1u << shift)) * 2u + 0x74u;
        }
        size += 4u;
        bool ok = false;
        const u32 bytes = texture_raster_bytes(c, raster, ok);
        if (!ok) return false;
        w32(c, sp + 8u, size + bytes);
    }
    w32(c, sp + 8u, r32(c, sp + 8u) - 0xCu);
    if (!guest_call(c, kStreamChunkHeader, link_of(E, 1), {stream, 1u, r32(c, sp + 8u), 0x37002u, 0x37u}, r)) return false;
    if (r == 0u) { c.gpr[3] = 0u; return true; }
    // the texture header
    const u32 platform = texture_arg + rd(c, kSdaTexturePlugin2);
    w32(c, sp + 0x20u, 6u);
    w32(c, sp + 0x24u, r32(c, texture_arg + 0x50u) & 0xFFFFu);
    if ((r32(c, platform + 0x20u) & 0x01000000u) != 0u) {
        w32(c, sp + 0x28u, 0u);
        w32(c, sp + 0x2Cu, 1u);
        w32(c, sp + 0x30u, 1u);
        w32(c, sp + 0x34u, r32(c, r2 + 0x1D10u));
    } else {
        const u32 word = r32(c, platform);
        w32(c, sp + 0x28u, (word >> 19) & 3u);
        w32(c, sp + 0x2Cu, (word >> 21) & 1u);
        w32(c, sp + 0x30u, ((word >> 8) & 1u) == 0u ? 1u : 0u);
        const s8 bias = static_cast<s8>((word >> 9) & 0xFFu);
        const u64 magic_bits = (static_cast<u64>(0x43300000u) << 32) | (static_cast<u32>(static_cast<s32>(bias)) ^ 0x80000000u);
        double value, magic;
        std::memcpy(&value, &magic_bits, sizeof(value));
        const u64 constant_bits = (static_cast<u64>(r32(c, r2 + 0x1070u)) << 32) | r32(c, r2 + 0x1074u);
        std::memcpy(&magic, &constant_bits, sizeof(magic));
        const u32 scale_bits = r32(c, r2 + 0x1068u);
        float scale;
        std::memcpy(&scale, &scale_bits, sizeof(scale));
        const float lod = static_cast<float>(static_cast<float>(value - magic) * scale);
        u32 lod_bits;
        std::memcpy(&lod_bits, &lod, sizeof(lod));
        w32(c, sp + 0x34u, lod_bits);
    }
    guest_copy(c, sp + 0x38u, texture_arg + 0x10u, 0x20u);
    guest_copy(c, sp + 0x58u, texture_arg + 0x30u, 0x20u);
    if (!guest_call(c, kStreamWrite, link_of(E, 2), {stream, sp + 0x20u, 0x58u}, r)) return false;
    if (r == 0u) { c.gpr[3] = 0u; return true; }
    // the raster header
    raster = r32(c, texture_arg);
    const u32 format = c.mem_read8(raster + 0x23u);
    const u32 parent = r32(c, raster);
    const u32 raster_data = parent + rd(c, kSdaRasterPlugin2);
    w32(c, sp + 8u, r32(c, sp + 8u) - 0x58u);
    w32(c, sp + 0x10u, c.mem_read8(raster + 0x20u) | (format << 8));
    c.mem_write16(sp + 0x14u, static_cast<u16>(r32(c, raster + 0xCu)));
    c.mem_write16(sp + 0x16u, static_cast<u16>(r32(c, raster + 0x10u)));
    c.mem_write8(sp + 0x18u, static_cast<u8>(r32(c, raster + 0x14u)));
    u32 filter = 0;
    if (!guest_call(c, kRasterFilter, link_of(E, 3), {raster}, filter)) return false;
    c.mem_write8(sp + 0x19u, static_cast<u8>(filter));
    c.mem_write8(sp + 0x1Au, static_cast<u8>(r32(c, raster_data + 0xCu)));
    c.mem_write8(sp + 0x1Bu, static_cast<u8>(r32(c, raster_data + 0x10u)));
    w32(c, sp + 0x1Cu, r32(c, raster_data + 0x14u) & 1u);
    if (!guest_call(c, kStreamWrite, link_of(E, 4), {stream, sp + 0x10u, 0x10u}, r)) return false;
    if (r == 0u) { c.gpr[3] = 0u; return true; }
    w32(c, sp + 8u, r32(c, sp + 8u) - 0x10u);
    if ((c.mem_read8(raster + 0x23u) & 0x60u) != 0u) {
        const u32 shift = r32(c, raster + 0x14u) & 0x3Fu;
        const u32 palette_bytes = (shift >= 32u ? 0u : (1u << shift)) * 2u;
        if (!guest_call(c, kStreamWrite, link_of(E, 5), {stream, r32(c, raster_data + 0x20u), palette_bytes}, r)) return false;
        if (r == 0u) { c.gpr[3] = 0u; return true; }
        w32(c, sp + 8u, r32(c, sp + 8u) - palette_bytes);
    }
    bool ok = false;
    const u32 bytes = texture_raster_bytes(c, raster, ok);
    if (!ok) return false;
    w32(c, sp + 0xCu, bytes);
    if (!guest_call(c, kStreamWrite, link_of(E, 6), {stream, sp + 0xCu, 4u}, r)) return false;
    if (r == 0u) { c.gpr[3] = 0u; return true; }
    w32(c, sp + 8u, r32(c, sp + 8u) - 4u);
    if (!guest_call(c, kStreamWrite, link_of(E, 7), {stream, r32(c, raster_data + 0x1Cu), r32(c, sp + 0xCu)}, r)) return false;
    if (r == 0u) { c.gpr[3] = 0u; return true; }
    w32(c, sp + 8u, r32(c, sp + 8u) - r32(c, sp + 0xCu));
    c.gpr[3] = 1u;
    return true;
}
}  // namespace

bool serve_plugin_init(Cpu& c) {
    constexpr u32 E = kPluginInit + 0u;
    const u32 init_callback = kElementInit + 0u, term_callback = kElementTerm + 0u;  // addresses as values
    u32 raster = 0;
    if (!guest_call(c, kFreeListCreateA, link_of(E, 1), {0x34u, 0x40Cu, 0u, 0u, 0u}, raster)) return false;
    wr(c, kSdaRasterPlugin, raster);
    u32 texture = 0;
    if (!guest_call(c, kFreeListCreateB, link_of(E, 2), {0x24u, 0x40Cu, init_callback, term_callback, 0u}, texture)) return false;
    wr(c, kSdaTexturePlugin, texture);
    c.gpr[3] = 1u;
    return true;
}

bool serve_driver_destruct(Cpu& c) {
    constexpr u32 E = kDriverDestruct + 0u;
    const u32 object = c.gpr[3];
    u32 ignored = 0;
    if (!guest_call(c, kListRelease20, link_of(E, 1), {instance_field(c, kSdaDriverOffset, 0x20u)}, ignored)) return false;
    if (!guest_call(c, kListRelease1C, link_of(E, 2), {instance_field(c, kSdaDriverOffset, 0x1Cu)}, ignored)) return false;
    wr(c, kSdaDriverRefs, rd(c, kSdaDriverRefs) - 1u);
    c.gpr[3] = object;
    return true;
}

bool serve_driver_construct(Cpu& c) {
    constexpr u32 E = kDriverConstruct + 0u;
    const u32 object = c.gpr[3], offset = c.gpr[4];
    const u32 refs = rd(c, kSdaDriverRefs);
    const u32 instance = rd(c, kSdaEngine) + offset;
    wr(c, kSdaDriverOffset, offset);
    wr(c, kSdaDriverInstance, instance);
    wr(c, kSdaDriverRefs, refs + 1u);
    for (u32 i = 0; i < 0x74u; ++i) c.mem_write8(instance + i, 0u);  // memset(instance, 0, 0x74)
    u32 result = 0;
    if (!guest_call(c, kListBuild1C, link_of(E, 1), {instance_field(c, kSdaDriverOffset, 0x1Cu)}, result)) return false;
    if (result != 0u && !guest_call(c, kListBuild20, link_of(E, 2), {instance_field(c, kSdaDriverOffset, 0x20u)}, result)) return false;
    if (result != 0u) {
        c.gpr[3] = object;
        return true;
    }
    u32 ignored = 0;
    if (!guest_call(c, kListRelease20, link_of(E, 3), {instance_field(c, kSdaDriverOffset, 0x20u)}, ignored)) return false;
    if (!guest_call(c, kListRelease1C, link_of(E, 4), {instance_field(c, kSdaDriverOffset, 0x1Cu)}, ignored)) return false;
    wr(c, kSdaDriverRefs, rd(c, kSdaDriverRefs) - 1u);
    c.gpr[3] = 0u;
    return true;
}

bool serve_pool_construct(Cpu& c) {
    const u32 object = c.gpr[3];
    wr(c, kSdaPoolOffset, c.gpr[4]);
    u32 result = 0;
    if (!pool_create(c, result)) return false;
    c.gpr[3] = result != 0u ? object : 0u;
    return true;
}

bool serve_pool_destruct(Cpu& c) {
    const u32 object = c.gpr[3];
    if (!pool_destroy(c)) return false;
    c.gpr[3] = object;
    return true;
}

bool serve_pool_entry_remove(Cpu& c) {
    return entry_remove(c, c.gpr[3], c.gpr[4]);
}

bool serve_pool_entry_swap(Cpu& c) {
    entry_swap(c, c.gpr[3], c.gpr[4]);
    return true;
}

bool serve_pool_entry_create(Cpu& c) {
    constexpr u32 E = kPoolEntryCreate + 0u;  // the row address, the base of the link addresses
    const u32 engine = rd(c, kSdaEngine);
    const u32 target = r32(c, engine + kEngineAlloc);
    const u32 argument = r32(c, engine + rd(c, kSdaPoolOffset));
    u32 object = 0;
    if (!guest_call(c, target, link_of(E, 1), {argument, 0x30409u}, object)) return false;
    if (object != 0u) {
        guest_zero(c, object, 0x34u);
        w32(c, object, 0u);
        c.gpr[3] = object;
        return true;
    }
    // the allocation failed: RenderWare error 0x80000013 with the failed size 0x34 (the error record is built in the frame
    // the original's prologue makes, 8 bytes above its stack pointer)
    Frame hold(c, 0x20u);
    const u32 frame = c.gpr[1];
    w32(c, frame + 8u, 1u);
    u32 code = 0, ignored = 0;
    if (!guest_call(c, kErrorGenerate, link_of(E, 2), {0x80000013u, 0x34u}, code)) return false;
    w32(c, frame + 0xCu, code);
    if (!guest_call(c, kErrorSet, link_of(E, 3), {frame + 8u}, ignored)) return false;
    c.gpr[3] = 0u;
    return true;
}

bool serve_pool_destroy_all(Cpu& c) {
    constexpr u32 E = kPoolDestroyAll + 0u;  // the row address, the base of the link addresses
    const u32 object = c.gpr[3];
    if (object == 0u) return true;
    u32 entry = r32(c, object + 8u);
    const u32 count = r32(c, object + 4u);
    for (u32 i = 0; i < count; ++i, entry += 0x28u)
        if (!entry_remove(c, entry, object)) return false;
    w32(c, object + 8u, 0u);
    const u32 block = r32(c, object + 0x20u);
    if (block != 0u) {
        if (!engine_free(c, block, link_of(E, 1))) return false;
        w32(c, object + 0x20u, 0u);
        w32(c, object + 0x24u, 0u);
    }
    const u32 engine = rd(c, kSdaEngine);
    u32 result = 0;
    if (!guest_call(c, r32(c, engine + kEnginePoolFree), link_of(E, 2), {r32(c, engine + rd(c, kSdaPoolOffset)), object}, result))
        return false;
    c.gpr[3] = result;
    return true;
}

bool serve_world_attach(Cpu& c) {
    constexpr u32 E = kWorldAttach + 0u;
    const u32 clear_callback = kWorldElementClear + 0u, empty_a = kWorldEmptyA + 0u, empty_b = kWorldEmptyB + 0u;  // as values
    u32 handle = 0;
    if (!guest_call(c, kWorldRegisterPlugin, link_of(E, 3),
                    {0x1Cu, 0x505u, clear_callback, empty_a, empty_b}, handle))
        return false;
    wr(c, kSdaWorldPlugin, handle);
    c.gpr[3] = (handle >> 31) ^ 1u;
    return true;
}

void serve_world_element_clear(Cpu& c) noexcept {
    c.mem_write32(c.gpr[3] + rd(c, kSdaWorldPlugin), 0u);
}

bool serve_world_sector_read(Cpu& c) {
    constexpr u32 E = kWorldSectorRead + 0u;   // the row address, the base of the link addresses
    const u32 argument = c.gpr[4];
    return world_sector_read(c, E, c.gpr[3], argument, argument + 0x34u, c.mem_read16(r32(c, argument + 0x78u) + 4u));
}

// ---- the stream callbacks of the native geometry and the world sector (gx-rw-plan cluster rwR4) ----------------------
// Callbacks the engine's chunk registry holds for the two native-data plugins (size, write, read); no recording reaches them
// (a stage is read through 0x804662B0, written by nothing the game runs), so each is the original's body, not a stop.
//   0x8046615C  size of the geometry's native chunk: ([r3+8] & 0x01000000) && [r3+0x58] ? [[r3+0x58]+8] - 7 : 0
//   0x804661FC  the same for a world sector: looks the object up first (0x80460EF0 -> its flag word), then [r3+0x34]
//   0x804661D0  geometry read  = the world-sector reader over (stream, geometry, geometry+0x58, count of [[geometry+0x54]+4])
//   0x80466188 / 0x80466250  geometry / world-sector write: (flag) the chunk body 0x80465DFC(stream, data, count); r3 = stream
// 0x80465DFC writes chunk 1 (version 0x37002, library 0x37): the tag 6, the two block sizes, the pointer tables un-relocated
// by the second block's aligned address while the first block is written, relocated back, then the second block. A stream
// write that fails answers 0 and leaves the object as it is at that point, exactly as the original (the tables stay
// un-relocated when the first block's write fails). DCFlushRange and GXInvalidateVtxCache, owned silent, are dropped.
constexpr u32 kChunkHeaderWrite = 0x8047DF40u, kWordWrite = 0x8047E4A8u, kBytesWrite = 0x80481F20u;
constexpr u32 kFlagNativeData = 0x01000000u;
constexpr u32 kWorldSectorLookup = 0x80460EF0u;   // the object lookup the world-sector callbacks start with

bool geometry_chunk_write(Cpu& c, u32 E, u32 stream, u32 object, u32 count) {
    u32 r = 0;
    const u32 sp = c.gpr[1];   // the 0x30-byte frame of 0x80465DFC: +8 second block size, +0xC first block size, +0x10 the tag
    const u32 header = object + 0x18u;
    const u32 entries = r32(c, object + 0x20u);
    const u32 table = header + (entries - 1u) * 8u + 0x14u;
    w32(c, sp + 0x10u, 6u);
    if (!guest_call(c, kChunkHeaderWrite, link_of(E, 1), {stream, 1u, r32(c, object + 8u) - 0x13u, 0x37002u, 0x37u}, r)) return false;
    if (r == 0u) { c.gpr[3] = 0u; return true; }
    if (!guest_call(c, kWordWrite, link_of(E, 2), {stream, sp + 0x10u, 4u}, r)) return false;
    if (r == 0u) { c.gpr[3] = 0u; return true; }
    w32(c, sp + 0xCu, (r32(c, header + 8u) - 1u) * 8u + count * 8u + 0x14u);
    if (!guest_call(c, kWordWrite, link_of(E, 3), {stream, sp + 0xCu, 4u}, r)) return false;
    if (r == 0u) { c.gpr[3] = 0u; return true; }
    w32(c, sp + 8u, r32(c, object + 8u) - r32(c, sp + 0xCu) - 0x1Fu);
    if (!guest_call(c, kWordWrite, link_of(E, 4), {stream, sp + 8u, 4u}, r)) return false;
    if (r == 0u) { c.gpr[3] = 0u; return true; }
    const u32 base = (table + count * 8u + 0x1Fu) & 0xFFFFFFE0u;
    for (u32 i = 0; i < r32(c, header + 8u); ++i) w32(c, header + 8u * i + 0xCu, r32(c, header + 8u * i + 0xCu) - base);
    for (u32 i = 0; i < count; ++i) w32(c, table + 8u * i, r32(c, table + 8u * i) - base);
    if (!guest_call(c, kBytesWrite, link_of(E, 5), {stream, header, r32(c, sp + 0xCu)}, r)) return false;
    if (r == 0u) { c.gpr[3] = 0u; return true; }
    for (u32 i = 0; i < r32(c, header + 8u); ++i) w32(c, header + 8u * i + 0xCu, r32(c, header + 8u * i + 0xCu) + base);
    for (u32 i = 0; i < count; ++i) w32(c, table + 8u * i, r32(c, table + 8u * i) + base);
    if (!guest_call(c, kBytesWrite, link_of(E, 6), {stream, r32(c, table), r32(c, sp + 8u)}, r)) return false;
    if (r == 0u) { c.gpr[3] = 0u; return true; }
    c.gpr[3] = stream;
    return true;
}

bool serve_geometry_size(Cpu& c) {
    const u32 geometry = c.gpr[3];
    u32 size = 0u;
    if ((r32(c, geometry + 8u) & kFlagNativeData) != 0u) {
        const u32 data = r32(c, geometry + 0x58u);
        if (data != 0u) size = r32(c, data + 8u) - 7u;
    }
    c.gpr[3] = size;
    return true;
}

bool serve_geometry_write(Cpu& c) {
    const u32 stream = c.gpr[3], geometry = c.gpr[4];
    Frame hold(c, 0x40u);   // the wrapper's 0x10 and 0x80465DFC's 0x30
    if ((r32(c, geometry + 8u) & kFlagNativeData) != 0u) {
        const u32 count = c.mem_read16(r32(c, geometry + 0x54u) + 4u);
        const u32 data = r32(c, geometry + 0x58u);
        constexpr u32 E = kGeometryWrite + 0u;   // the row address, the base of the link addresses
        if (!geometry_chunk_write(c, E, stream, data, count)) return false;
    }
    c.gpr[3] = stream;
    return true;
}

bool serve_geometry_read(Cpu& c) {
    constexpr u32 E = kGeometryRead + 0u;   // the row address, the base of the link addresses
    const u32 geometry = c.gpr[4];
    const u32 count = c.mem_read16(r32(c, geometry + 0x54u) + 4u);
    return world_sector_read(c, E, c.gpr[3], geometry, geometry + 0x58u, count);
}

bool serve_sector_size(Cpu& c) {
    const u32 sector = c.gpr[3];
    Frame hold(c, 0x10u);
    u32 record = 0;
    if (!guest_call(c, kWorldSectorLookup, link_of(kSectorSize, 1), {sector}, record)) return false;
    u32 size = 0u;
    if ((r32(c, record + 8u) & kFlagNativeData) != 0u) {
        const u32 data = r32(c, sector + 0x34u);
        if (data != 0u) size = r32(c, data + 8u) - 7u;
    }
    c.gpr[3] = size;
    return true;
}

bool serve_sector_write(Cpu& c) {
    const u32 stream = c.gpr[3], sector = c.gpr[4];
    Frame hold(c, 0x40u);
    u32 record = 0;
    if (!guest_call(c, kWorldSectorLookup, link_of(kSectorWrite, 1), {sector}, record)) return false;
    if ((r32(c, record + 8u) & kFlagNativeData) != 0u) {
        const u32 count = c.mem_read16(r32(c, sector + 0x78u) + 4u);
        if (!geometry_chunk_write(c, kSectorWrite + 4u, stream, r32(c, sector + 0x34u), count)) return false;
    }
    c.gpr[3] = stream;
    return true;
}

bool serve_native_texture_write(Cpu& c) {
    return native_texture_write(c, c.gpr[3], c.gpr[4]);
}

void serve_element_init(Cpu& c) noexcept {
    c.mem_write32(c.gpr[3] + rd(c, kSdaTexturePlugin) + 0x20u, 0x01000000u);
}

void serve_element_term(Cpu& c) noexcept {
    for (u32 i = 0; i < 8u; ++i) {
        const u32 slot = kBoundTextureSlots + 4u * i;
        if (c.mem_read32(slot) == c.gpr[3]) c.mem_write32(slot, 0u);
    }
}

void install(Cpu& cpu) {
    cpu.install_hle(kPluginInit, [](Cpu& c) {
        (void)serve_plugin_init(c);  // a failed guest call has stopped the run
        return true;
    }, "RenderWare registry: plugin init");
    cpu.install_hle(kDriverDestruct, [](Cpu& c) {
        (void)serve_driver_destruct(c);
        return true;
    }, "RenderWare registry: driver plugin destruct");
    cpu.install_hle(kDriverConstruct, [](Cpu& c) {
        (void)serve_driver_construct(c);
        return true;
    }, "RenderWare registry: driver plugin construct");
    cpu.install_hle(kPoolConstruct, [](Cpu& c) {
        (void)serve_pool_construct(c);
        return true;
    }, "RenderWare registry: device pool construct");
    cpu.install_hle(kPoolDestruct, [](Cpu& c) {
        (void)serve_pool_destruct(c);
        return true;
    }, "RenderWare registry: device pool destruct");
    cpu.install_hle(kPoolEntryCreate, [](Cpu& c) {
        (void)serve_pool_entry_create(c);
        return true;
    }, "RenderWare registry: device pool object create");
    cpu.install_hle(kPoolDestroyAll, [](Cpu& c) {
        (void)serve_pool_destroy_all(c);
        return true;
    }, "RenderWare registry: device pool object destroy");
    cpu.install_hle(kPoolEntryRemove, [](Cpu& c) {
        (void)serve_pool_entry_remove(c);
        return true;
    }, "RenderWare registry: device pool entry remove");
    cpu.install_hle(kPoolEntrySwap, [](Cpu& c) {
        (void)serve_pool_entry_swap(c);
        return true;
    }, "RenderWare registry: device pool entry swap");
    cpu.install_hle(kWorldSectorRead, [](Cpu& c) {
        (void)serve_world_sector_read(c);
        return true;
    }, "RenderWare registry: world sector read");
    cpu.install_hle(kNativeTextureWrite, [](Cpu& c) {
        (void)serve_native_texture_write(c);
        return true;
    }, "RenderWare registry: native texture write");
    cpu.install_hle(kWorldAttach, [](Cpu& c) {
        (void)serve_world_attach(c);
        return true;
    }, "RenderWare registry: world plugin attach");
    cpu.install_hle(kWorldElementClear, [](Cpu& c) {
        serve_world_element_clear(c);
        return true;
    }, "RenderWare registry: world plugin element destructor");
    cpu.install_hle(kWorldEmptyA, [](Cpu&) { return true; }, "RenderWare registry: world plugin empty callback");
    cpu.install_hle(kWorldEmptyB, [](Cpu&) { return true; }, "RenderWare registry: world plugin empty callback");
    cpu.install_hle(kGeometrySize, [](Cpu& c) {
        (void)serve_geometry_size(c);
        return true;
    }, "RenderWare registry: geometry native chunk size");
    cpu.install_hle(kGeometryWrite, [](Cpu& c) {
        (void)serve_geometry_write(c);
        return true;
    }, "RenderWare registry: geometry native chunk write");
    cpu.install_hle(kGeometryRead, [](Cpu& c) {
        (void)serve_geometry_read(c);
        return true;
    }, "RenderWare registry: geometry native chunk read");
    cpu.install_hle(kSectorSize, [](Cpu& c) {
        (void)serve_sector_size(c);
        return true;
    }, "RenderWare registry: world sector native chunk size");
    cpu.install_hle(kSectorWrite, [](Cpu& c) {
        (void)serve_sector_write(c);
        return true;
    }, "RenderWare registry: world sector native chunk write");
    cpu.install_hle(kElementInit, [](Cpu& c) {
        serve_element_init(c);
        return true;
    }, "RenderWare registry: texture free-list element initialiser");
    cpu.install_hle(kElementTerm, [](Cpu& c) {
        serve_element_term(c);
        return true;
    }, "RenderWare registry: texture free-list element terminator");
}

}  // namespace gekko::rw_registry
