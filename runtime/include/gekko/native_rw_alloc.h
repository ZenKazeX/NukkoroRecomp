#pragma once

// The game's allocation chain above the one OS heap, served natively (docs/re/heap.md sections 1-3; docs/hal/ALLOC_CHAIN.md).
//
// Original behaviour (main.dol, PAL GUPP8P). RenderWare's memory functions are the game's own:
//   RwMalloc 0x80050E28 (r3 = size; r4 ignored)        RwFree 0x80050EF0 (r3 = pointer)
//     size / pointer 0: returns at once (0 / nothing)
//     __begin_critical_region(1)                        0x803A906C, an empty function
//     while (busy) OSReport("Critical Region [begin critical] Wait\n")     busy = the byte r13 + 0x2D94
//     busy = 1
//     enabled = OSDisableInterrupts() != 0              0x8037611C
//     OSDisableScheduler()                              0x803784CC
//     p = OSAllocFromHeap(current heap, size)           0x80371FB0   /   OSFreeToHeap(current heap, pointer) 0x803720AC
//     OSEnableScheduler()                               0x8037850C
//     OSRestoreInterrupts(enabled)                      0x80376144
//     if (!busy) OSReport("Critical Region [end critical] Warning ... \n")
//     busy = 0
//     __end_critical_region(1)                          0x803A9068, an empty function
// Every caller reaches them through the RenderWare engine's function table (r13 + 0x615C: +0x108 malloc, +0x10C free):
//   0x80486C1C (r3 = free list, r4 = hint)   engine malloc(list->entrySize): RenderWare in its "no free lists" mode, so
//                                            every atomic, frame, material and sector tie is its own OS cell
//   0x80486C4C (r3 = free list, r4 = p)      engine free(p); returns the list
//   0x80049BB8 (r3 = size, r4 = hint)        the game's wrapper: engine malloc(size, hint + 0x32)
//   0x80049B5C (r3 = p)                      the game's wrapper: engine free(p)
//
// What the image did before (MEASURED, Westopolis tape, .build/gate/n1-b003): each RwMalloc / RwFree ran its translated
// body and made five guest -> native owner dispatches (the interrupt gate twice, the scheduler count twice, the heap
// routine): 2,430,698 calls, 12.2 M of the tape's 147.0 M owner dispatches, 641 a frame. The bracket guards a heap that
// several GameCube threads and interrupt handlers could enter; this image runs the game on one thread and delivers
// interrupts only at recognition points, none of which lies between the gate's two halves.
//
// Native form: one hook per entry. It makes the heap call directly (os_heap::serve_alloc_from_heap /
// serve_free_to_heap: the cells, lists and addresses are the OS heap's own, so every allocation lands where it landed
// before) and leaves what the chain left:
//   memory     the heap words; the stack frames of the entry and, under a wrapper, of RwMalloc / RwFree below it (back
//              chain, the link word above, r30 / r31); the busy byte and the scheduler count end as they began and are
//              not written
//   registers  r3 the result (RwFree: 1, or the pointer's 0; 0x80486C4C: the list), r0 the caller's link, r4 / r5 the
//              interrupt gate's last MSR images, CR0 and XER[CA] of the body's last compare and carry, r12 / CTR / r5
//              as a wrapper's indirect call leaves them; r30, r31, LR, MSR unchanged
// SCAFFOLDING: the frames and the scratch registers. No reader is known; the state report digests the dead stack and
// the register file, and the tape gate compares it. They go when the callers are native.
//
// It declines (the translated body then runs, exactly as before) when
//   - a deliverable interrupt is pending at the entry (the original takes it inside OSDisableInterrupts),
//   - the busy byte is set (the wait loop and its report are the body's), or
//   - a wrapper's engine slot does not hold RwMalloc / RwFree.
// Hooks with a hand-back, not native-owners rows: the bodies stay in the image, so GEKKO_RW_ALLOC=0 turns the chain off
// in the same executable (the A arm of a timing). CMake SHADOW_RW_ALLOC (default ON) sets the default.

#include "gekko/types.h"

namespace gekko {
class Cpu;
}

namespace gekko::rw_alloc {

constexpr u32 kRwMalloc = 0x80050E28u;
constexpr u32 kRwFree = 0x80050EF0u;
constexpr u32 kFreeListAlloc = 0x80486C1Cu;
constexpr u32 kFreeListFree = 0x80486C4Cu;
constexpr u32 kGameMalloc = 0x80049BB8u;
constexpr u32 kGameFree = 0x80049B5Cu;

// Where each wrapper's indirect call returns: the link word RwMalloc / RwFree saves above its frame.
constexpr u32 kFreeListAllocReturn = 0x80486C3Cu;  // liveness-roots: value-only
constexpr u32 kFreeListFreeReturn = 0x80486C74u;  // liveness-roots: value-only
constexpr u32 kGameMallocReturn = 0x80049BD8u;  // liveness-roots: value-only
constexpr u32 kGameFreeReturn = 0x80049B78u;  // liveness-roots: value-only

constexpr u32 kBusySda = 0x2D94u;     // r13-relative: the critical-region byte
constexpr u32 kEngineSda = 0x615Cu;   // r13-relative: the RenderWare engine
constexpr u32 kEngineMalloc = 0x108u; // engine + : the memory functions
constexpr u32 kEngineFree = 0x10Cu;
constexpr u32 kGameHintBias = 0x32u;  // the game wrapper's r4 = hint + 0x32

// One call each, with the registers as the caller set them. False = declined, nothing changed: the translated body runs.
bool serve_malloc(Cpu& cpu);
bool serve_free(Cpu& cpu);
bool serve_free_list_alloc(Cpu& cpu);
bool serve_free_list_free(Cpu& cpu);
bool serve_game_malloc(Cpu& cpu);
bool serve_game_free(Cpu& cpu);

void set_enabled(bool enabled) noexcept;
bool enabled() noexcept;

// Calls served and declined since the start (GEKKO_RW_ALLOC_STATS=1 prints them at exit).
struct Stats {
    u64 served[6]{};    // in the order of the serve functions above
    u64 declined[6]{};
    u64 declined_interrupt = 0, declined_busy = 0, declined_engine = 0;
};
const Stats& stats() noexcept;

void install(Cpu& cpu);

}  // namespace gekko::rw_alloc
