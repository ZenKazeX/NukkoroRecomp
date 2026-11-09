#include "gekko/native_rw_world.h"

#include "gekko/native_rw_raster.h"
#include "gekko/native_rw_world_decode.h"

#include <cstdio>
#include <string>

namespace gekko {
namespace {

// 0x804651D8: the world-sector pipeline's node-definition init. r3 = the pipeline object; *(r3 + 0x14) points at the
// definition's callback block, whose four words it fills with the instance, (none), render and pipeline callbacks.
constexpr u32 kPipelineInit = 0x804651D8u;
constexpr u32 kCallbackBlockOffset = 0x14u;
constexpr u32 kInstanceCallback = 0x80464F60u;
constexpr u32 kRenderCallback = 0x80464E44u;
constexpr u32 kPipelineCallback = 0x80463964u;

// 0x80465210: lis/addi/blr returning the address of the world pipeline's static descriptor.
constexpr u32 kDescriptorAccessor = 0x80465210u;
constexpr u32 kDescriptor = 0x8056E9F4u;

// Structural twins (tools/platform/structure_match.py): 0x8049C198 is an empty body (`blr`), 0x8049C6BC returns a
// small-data word (`lwz r3, 0x6244(r13); blr`).
constexpr u32 kEmptyHook = 0x8049C198u;
constexpr u32 kWordGetter = 0x8049C6BCu;
constexpr u32 kGetterSdaOffset = 0x6244u;

// ---- the plugin / node registry's self-contained rows (gx-rw-plan cluster rwR5; docs/agents/vi.md "Step 7") ---------
// RenderWare's engine globals live behind r13 + 0x615C (the RwGlobals pointer); r13 + 0x2B08 holds the two ends of the
// engine's static arena (+0: the top, +4: the floor); r13 + 0x622C is the plugin-globals slot offset.
constexpr u32 kGlobalsPointer = 0x615Cu;
constexpr u32 kSlotOffset = 0x622Cu;
constexpr u32 kSlotNext = 0x1Cu;          // the slot's saved previous value
constexpr u32 kArena = 0x2B08u;
constexpr u32 kPluginSlotSetter = 0x8049BCE0u;   // sets / restores the current plugin-globals slot, returns it
constexpr u32 kTrueReturn = 0x8049C190u;         // li r3, 1; blr
constexpr u32 kDistinctCount = 0x8049C9F4u;      // the number of distinct sort keys of a table
constexpr u32 kArenaAllocTop = 0x8049C90Cu;      // bump allocation from the top of the static arena
constexpr u32 kArenaAllocFloor = 0x8049C97Cu;    // bump allocation from the floor of the static arena
constexpr u32 kSlotStack = 0x8049BD30u;          // push / pop of the plugin-globals slot of a given kind
constexpr u32 kNodeRegister = 0x8049CC9Cu;       // registers a node record in a pipeline's table
constexpr u32 kUnusedNode = 0x8049CA80u;         // 0 hits on three recordings

// 0x8049BD30(value, kind): the slot of `kind` (1..6) is pushed (value != 0) or popped (value == 0). Offsets of the
// pushed slot, of its saved copy and the word the pop returns, per kind; any other kind is the original's RwError.
struct SlotKind {
    u32 slot;     // G + off + slot: the current value
    u32 saved;    // G + off + saved: the copy a pop restores from
};
constexpr SlotKind kSlotKinds[7] = {{0, 0}, {0x10u, 0x2Cu}, {0x14u, 0x30u}, {0x04u, 0x20u}, {0x0Cu, 0x28u}, {0x08u, 0x24u}, {0x18u, 0x34u}};

void slot_stack(Cpu& c) {
    const u32 sda = c.gpr[13];
    const u32 value = c.gpr[3], kind = c.gpr[4];
    if (kind < 1u || kind > 6u) {  // signed compares in the original: <= 0 and > 6 are both errors
        c.request_stop(StopReason::ExecutionFault,
                       "RenderWare plugin-globals slot of kind " + std::to_string(static_cast<s32>(kind)) +
                           " (0x8049BD30): the original raises RwError(1)");
        return;
    }
    const u32 base = c.mem_read32(sda + kGlobalsPointer) + c.mem_read32(sda + kSlotOffset);
    const SlotKind k = kSlotKinds[kind];
    if (value != 0u) {
        c.mem_write32(base + k.slot, value);  // r3 stays the value
        return;
    }
    const u32 saved = c.mem_read32(base + k.saved);
    c.mem_write32(base + k.slot, saved != 0u ? saved : 0u);
    // kinds 3 and 5 return the slot just stored; the others return the saved word (the original reloads +0x28, +0x2C, ...)
    c.gpr[3] = c.mem_read32(base + ((kind == 3u || kind == 5u) ? k.slot : k.saved));
}

// 0x8049CC9C(table, record, definition): registers a definition's node record in the table. The three limits the original
// checks raise RwError and make it return 0 with nothing registered; the owner stops the run instead.
void node_register(Cpu& c) {
    const u32 sda = c.gpr[13];
    const u32 table = c.gpr[3], record = c.gpr[4], definition = c.gpr[5];
    const u32 count = c.mem_read32(definition + 0x2Cu);
    for (u32 i = 0; i < 0x28u; i += 4u) c.mem_write32(record + i, 0u);  // memset(record, 0, 0x28)
    const u32 globals = c.mem_read32(sda + kGlobalsPointer);
    const u32 limit = c.mem_read32(globals + c.mem_read32(sda + 0x6238u) + 0x38u);
    if (count > 0x20u || c.mem_read32(definition + 0x20u) > 0x20u || count >= limit) {
        c.request_stop(StopReason::ExecutionFault,
                       "RenderWare node record limits exceeded (0x8049CC9C): the original raises RwError and returns 0");
        return;
    }
    const u32 slots = c.mem_read32(table + 4u);
    const u32 first = c.mem_read32(table + 8u) + limit * 0x28u + (slots << 7);
    c.mem_write32(record + 8u, first);
    c.mem_write32(record + 4u, count);
    for (u32 i = 0; i < count; ++i) c.mem_write32(first + 4u * i, 0xFFFFFFFFu);
    const u32 tail = c.mem_read32(table + 8u) + limit * 0x28u + (limit << 7) + slots * 0xCu;
    c.mem_write32(tail + 0u, 0u);
    c.mem_write32(tail + 4u, 0u);
    c.mem_write32(tail + 8u, 0u);
    c.mem_write32(record + 0x1Cu, tail);
    c.mem_write32(record + 0x20u, 0u);
    c.mem_write32(record + 0x24u, 0u);
    c.mem_write32(record + 0u, definition);
    c.mem_write32(table + 4u, slots + 1u);
    c.gpr[3] = 1u;
}

// The error the original raises when the arena is exhausted (RwError with code 1 and the formatted message): the owner
// stops the run naming the request instead.
void arena_exhausted(Cpu& c, const char* which, u32 size) {
    char text[160];
    std::snprintf(text, sizeof(text),
                  "RenderWare static arena exhausted (%s, %u bytes): the original raises RwError(1) and returns NULL", which,
                  size);
    c.request_stop(StopReason::ExecutionFault, text);
}


// The 9 entries nothing enters: the node bodies and the instance helpers of the world-sector pipeline (names from the
// symbol hints of test_data/platform/gx_rw_frontier.csv). (The five native-data stream callbacks and the four vertex-format
// decoders that were stops here are exact owners now: native_rw_registry.cpp, native_rw_world_decode.cpp.)
constexpr RwWorldStopEntry kStopEntries[] = {
    {0x8046521Cu, "world pipeline node 0x8046521C"},
    {0x80465438u, "world pipeline node 0x80465438"},
    {0x80465864u, "world pipeline 0x80465864"},
    {0x80466624u, "world pipeline 0x80466624"},
    {0x80466D2Cu, "world pipeline 0x80466D2C"},
    {0x80466F24u, "world pipeline 0x80466F24"},
    {0x804671E4u, "world pipeline 0x804671E4"},
    {0x80467BA8u, "world pipeline 0x80467BA8"},
    {0x80468C18u, "world pipeline 0x80468C18"},
    // (0x8049A124 and its switch arms, with the device wrappers 0x8049A52C / 0x8049A508, are exact owners in
    // native_rw_raster.cpp: a rectangle fill of the current raster, not a texture-dictionary export.)
};

}  // namespace

// ---- rwR3: the Im3D entry points -------------------------------------------------------------------------------------
// The state lives in the plugin-globals slot S = [r13+0x615C] + [r13+0x622C]: +0 the Im3D pipeline, +4/+8/+0xC/+0x10/+0x14 the
// pipelines of the primitive types 3 / 5 / 4 / 1 / 2, +0x38 (u16) the vertex count, +0x3C the vertex array (0: no transform in
// progress), +0x40 the vertex stride flags, +0x44.. the execution record the pipeline reads (+0x20 pipeline, +0x24 primitive
// type, +0x28 index array, +0x2C index count).

u32 im3d_slot(Cpu& c) { return c.mem_read32(c.gpr[13] + kGlobalsPointer) + c.mem_read32(c.gpr[13] + kSlotOffset); }

// The pipeline of a primitive type, or 0 for a type the original rejects (RwError 0x25).
u32 im3d_pipeline_offset(s32 type) {
    switch (type) {
        case 3: return 0x04u;
        case 5: return 0x08u;
        case 4: return 0x0Cu;
        case 1: return 0x10u;
        case 2: return 0x14u;
        default: return 0u;
    }
}

const RwWorldStopEntry* rw_world_stop_entries(std::size_t* count) noexcept {
    *count = sizeof(kStopEntries) / sizeof(kStopEntries[0]);
    return kStopEntries;
}

void install_native_rw_im3d_owners(Cpu& cpu, Im3dExecuteFn transform, Im3dExecuteFn render) {
    // 0x8049B87C RwIm3DTransform(vertices, count, ltm, flags)
    cpu.install_hle(0x8049B87Cu, [transform](Cpu& c) {
        const u32 vertices = c.gpr[3], count = c.gpr[4], ltm = c.gpr[5], flags = c.gpr[6];
        if (count > 0x10000u) {   // RwError 0x32 and answers 0 (nothing else written)
            if (rw_raster::report_rw_error(c, 0x32u, 0x8049B87Cu + 4u)) c.gpr[3] = 0u;
            return true;
        }
        const u32 slot = im3d_slot(c);
        c.mem_write16(slot + 0x38u, static_cast<u16>(count));
        c.mem_write32(slot + 0x3Cu, vertices);
        c.mem_write32(slot + 0x40u, 0x24u);
        c.mem_write32(slot + 0x48u, ltm);
        c.mem_write32(slot + 0x44u, flags | 0x18u);
        const u32 result = transform(c, c.mem_read32(slot), slot + 0x38u, 1u);
        c.gpr[3] = result != 0u ? vertices : 0u;
        return true;
    }, "RwIm3DTransform (rwR3 owner)");
    // 0x8049B95C RwIm3DEnd()
    cpu.install_hle(0x8049B95Cu, [](Cpu& c) {
        const u32 slot = im3d_slot(c);
        if (c.mem_read32(slot + 0x3Cu) == 0u) {
            c.gpr[3] = 0u;
            return true;
        }
        for (u32 i = 0; i < 0x3Cu; i += 4u) c.mem_write32(slot + 0x38u + i, 0u);  // memset(slot + 0x38, 0, 0x3C)
        c.gpr[3] = 1u;
        return true;
    }, "RwIm3DEnd (rwR3 owner)");
    // 0x8049BB48 RwIm3DRenderPrimitive(type)
    cpu.install_hle(0x8049BB48u, [render](Cpu& c) {
        const u32 type = c.gpr[3];
        const u32 slot = im3d_slot(c);
        if (c.mem_read32(slot + 0x3Cu) == 0u) {
            if (rw_raster::report_rw_error(c, 0x23u, 0x8049BB48u + 4u)) c.gpr[3] = 0u;   // RwError 0x23, answers 0
            return true;
        }
        const u32 record = slot + 0x44u;
        c.mem_write32(record + 0x20u, 0u);
        c.mem_write32(record + 0x24u, type);
        c.mem_write32(record + 0x28u, 0u);
        c.mem_write32(record + 0x2Cu, c.mem_read16(slot + 0x38u));
        const u32 offset = im3d_pipeline_offset(static_cast<s32>(type));
        if (offset == 0u) {   // RwError 0x25, then the original carries on: the pipeline stays NULL and the executor is called with it
            if (!rw_raster::report_rw_error(c, 0x25u, 0x8049BB48u + 4u)) return true;
        } else {
            c.mem_write32(record + 0x20u, c.mem_read32(slot + offset));
        }
        c.gpr[3] = render(c, c.mem_read32(record + 0x20u), record, 0u) != 0u ? 1u : 0u;
        return true;
    }, "RwIm3DRenderPrimitive (rwR3 owner)");
    // 0x8049B9AC RwIm3DRenderIndexedPrimitive(type, indices, count)
    cpu.install_hle(0x8049B9ACu, [render](Cpu& c) {
        const u32 type = c.gpr[3], indices = c.gpr[4], count = c.gpr[5];
        const u32 slot = im3d_slot(c);
        if (c.mem_read32(slot + 0x3Cu) == 0u) {
            if (rw_raster::report_rw_error(c, 0x23u, 0x8049B9ACu + 4u)) c.gpr[3] = 0u;   // RwError 0x23, answers 0
            return true;
        }
        const u32 record = slot + 0x44u;
        c.mem_write32(record + 0x20u, 0u);
        c.mem_write32(record + 0x24u, type);
        c.mem_write32(record + 0x28u, indices);
        c.mem_write32(record + 0x2Cu, count);
        const u32 offset = im3d_pipeline_offset(static_cast<s32>(type));
        if (offset == 0u) {   // RwError 0x25, then the original carries on with the NULL pipeline
            if (!rw_raster::report_rw_error(c, 0x25u, 0x8049B9ACu + 4u)) return true;
        } else {
            c.mem_write32(record + 0x20u, c.mem_read32(slot + offset));
        }
        if (type == 3u) {  // whole triangles only
            c.mem_write32(record + 0x2Cu, static_cast<u32>(static_cast<s32>(count) - static_cast<s32>(count) % 3));
        } else if (type == 1u) {  // whole line segments only
            c.mem_write32(record + 0x2Cu, static_cast<u32>(static_cast<s32>(count) - static_cast<s32>(count) % 2));
        }
        c.gpr[3] = render(c, c.mem_read32(record + 0x20u), record, 0u) != 0u ? 1u : 0u;
        return true;
    }, "RwIm3DRenderIndexedPrimitive (rwR3 owner)");
}

void install_native_rw_world_owners(Cpu& cpu) {
    rw_world_decode::install(cpu);
    cpu.install_hle(kPipelineInit, [](Cpu& c) {
        const u32 block = c.mem_read32(c.gpr[3] + kCallbackBlockOffset);
        c.mem_write32(block + 0x0u, kInstanceCallback);
        c.mem_write32(block + 0x4u, 0u);
        c.mem_write32(block + 0x8u, kRenderCallback);
        c.mem_write32(block + 0xCu, kPipelineCallback);
        c.gpr[3] = 1u;
        return true;
    }, "world-sector pipeline node-definition init (rwR4 owner)");
    cpu.install_hle(kDescriptorAccessor, [](Cpu& c) {
        c.gpr[3] = kDescriptor;
        return true;
    }, "world-sector pipeline descriptor accessor (rwR4 owner)");
    cpu.install_hle(kEmptyHook, [](Cpu&) { return true; }, "empty RenderWare hook 0x8049C198 (twin owner)");
    cpu.install_hle(kTrueReturn, [](Cpu& c) {
        c.gpr[3] = 1u;
        return true;
    }, "RenderWare plugin callback returning TRUE 0x8049C190 (rwR5 owner)");
    cpu.install_hle(kPluginSlotSetter, [](Cpu& c) {
        const u32 sda = c.gpr[13];
        const u32 slot = c.mem_read32(sda + kGlobalsPointer) + c.mem_read32(sda + kSlotOffset);
        if (c.gpr[3] != 0u) {
            c.mem_write32(slot, c.gpr[3]);
        } else {
            const u32 saved = c.mem_read32(slot + kSlotNext);
            c.mem_write32(slot, saved != 0u ? saved : 0u);
        }
        c.gpr[3] = c.mem_read32(c.mem_read32(sda + kGlobalsPointer) + c.mem_read32(sda + kSlotOffset));
        return true;
    }, "RenderWare current plugin-globals slot 0x8049BCE0 (rwR5 owner)");
    cpu.install_hle(kDistinctCount, [](Cpu& c) {
        // For each pass the smallest key above the previous pass's key (and below 0xFFFFFFFF); the passes until none.
        const u32 table = c.gpr[3];
        const u32 count = c.mem_read32(table + 4u);
        u32 passes = 0u, previous = 0u;
        for (;;) {
            u32 smallest = 0xFFFFFFFFu;
            for (u32 i = 0u; i < count; ++i) {
                const u32 entry = c.mem_read32(c.mem_read32(table + 8u) + 0x28u * i);
                const u32 keys = c.mem_read32(entry + 0x20u);
                for (u32 k = 0u; k < keys; ++k) {
                    const u32 key = c.mem_read32(c.mem_read32(entry + 0x24u) + 12u * k);
                    if (key > previous && key < smallest) smallest = key;
                }
            }
            if (smallest == 0xFFFFFFFFu) break;
            ++passes;
            previous = smallest;
        }
        c.gpr[3] = passes;
        return true;
    }, "RenderWare distinct key count 0x8049C9F4 (rwR5 owner)");
    cpu.install_hle(kArenaAllocTop, [](Cpu& c) {
        const u32 arena = c.gpr[13] + kArena;
        const u32 size = (c.gpr[3] + 3u) & ~3u;
        const u32 top = c.mem_read32(arena);
        const u32 next = top - size;
        c.mem_write32(arena, next);
        if (next >= c.mem_read32(arena + 4u)) {
            c.gpr[3] = next;
        } else {
            c.mem_write32(arena, top);
            arena_exhausted(c, "top", size);
        }
        return true;
    }, "RenderWare static arena allocation from the top 0x8049C90C (rwR5 owner)");
    cpu.install_hle(kArenaAllocFloor, [](Cpu& c) {
        const u32 arena = c.gpr[13] + kArena;
        const u32 size = (c.gpr[3] + 3u) & ~3u;
        const u32 floor = c.mem_read32(arena + 4u);
        const u32 next = floor + size;
        c.mem_write32(arena + 4u, next);
        if (next <= c.mem_read32(arena)) {
            c.gpr[3] = floor;
        } else {
            c.mem_write32(arena + 4u, floor);
            arena_exhausted(c, "floor", size);
        }
        return true;
    }, "RenderWare static arena allocation from the floor 0x8049C97C (rwR5 owner)");
    cpu.install_hle(kSlotStack, [](Cpu& c) {
        slot_stack(c);
        return true;
    }, "RenderWare plugin-globals slot push / pop 0x8049BD30 (rwR5 owner)");
    cpu.install_hle(kNodeRegister, [](Cpu& c) {
        node_register(c);
        return true;
    }, "RenderWare node record registration 0x8049CC9C (rwR5 owner)");
    for (const u32 address : {kUnusedNode}) {
        cpu.install_hle(address, [address](Cpu& c) {
            char text[200];
            std::snprintf(text, sizeof(text),
                          "RenderWare plugin / node registry entry 0x%08X was entered: no recording reaches it", address);
            c.request_stop(StopReason::ExecutionFault, text);
            return true;
        }, "RenderWare registry entry nothing enters (rwR5 stop owner)");
    }
    cpu.install_hle(kWordGetter, [](Cpu& c) {
        c.gpr[3] = c.mem_read32(c.gpr[13] + kGetterSdaOffset);
        return true;
    }, "RenderWare small-data getter 0x8049C6BC (twin owner)");
    for (const RwWorldStopEntry& entry : kStopEntries) {
        cpu.install_hle(entry.address, [entry](Cpu& c) {
            char address[16];
            std::snprintf(address, sizeof(address), "0x%08X", entry.address);
            c.request_stop(StopReason::ExecutionFault,
                           std::string("RenderWare world-sector pipeline entry ") + address + " (" + entry.name +
                               ") was entered: the native renderer replaces the pipeline's execution "
                               "(rw_pipeline_execute_*), no recording reaches this entry");
            return true;
        }, std::string("world-sector pipeline entry ") + entry.name + " (rwR4 stop owner)");
    }
}

}  // namespace gekko
