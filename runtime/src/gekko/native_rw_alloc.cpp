// The game's allocation chain above the OS heap, served natively (gekko/native_rw_alloc.h).

#include "gekko/native_rw_alloc.h"

#include "gekko/cpu.h"
#include "gekko/native_os_heap.h"

#include <cstdio>
#include <cstdlib>

namespace gekko::rw_alloc {

namespace {

#ifdef SHADOW_RW_ALLOC_DEFAULT
bool g_enabled = true;  // CMake SHADOW_RW_ALLOC=ON; GEKKO_RW_ALLOC=0 disables
#else
bool g_enabled = false;  // GEKKO_RW_ALLOC=1 enables
#endif
Stats g_stats;  // the hooks are served on the game thread only

enum Entry : unsigned { kMallocEntry, kFreeEntry, kListAllocEntry, kListFreeEntry, kGameMallocEntry, kGameFreeEntry };

constexpr u32 kFrame = 0x10u;       // every function of the chain: stwu r1, -0x10(r1)
constexpr u32 kCrGreater = 4u, kCrEqual = 2u;

// What every entry checks first. A deliverable interrupt is the original's to take (inside OSDisableInterrupts, before
// anything is written), and a set busy byte sends the original into its wait loop.
inline bool ready(Cpu& c, Entry entry) {
    if (!g_enabled) return false;
    if (c.interrupt_pending()) {
        ++g_stats.declined_interrupt;
        ++g_stats.declined[entry];
        return false;
    }
    if (c.mem_read8(c.gpr[13] + kBusySda) != 0u) {
        ++g_stats.declined_busy;
        ++g_stats.declined[entry];
        return false;
    }
    return true;
}

// A wrapper's engine slot: the function the indirect call would enter. False when it is not `expected`.
inline bool engine_function(Cpu& c, Entry entry, u32 slot, u32 expected, u32& engine) {
    engine = c.mem_read32(c.gpr[13] + kEngineSda);
    if (c.mem_read32(engine + slot) == expected) return true;
    ++g_stats.declined_engine;
    ++g_stats.declined[entry];
    return false;
}

GEKKO_NOINLINE void cyclic(Cpu& c, const char* what) {
    c.request_stop(StopReason::ExecutionFault, std::string(what) + ": a heap list walk does not end (cyclic list)");
}

// The frame RwMalloc and RwFree build alike: stwu r1, -0x10(r1); the link word above it; stmw r30, 8(r1).
inline void body_frame(Cpu& c, u32 sp, u32 link) {
    const u32 frame = sp - kFrame;
    c.mem_write32(frame, sp);
    c.mem_write32(sp + 4u, link);
    c.mem_write32(frame + 8u, c.gpr[30]);
    c.mem_write32(frame + 12u, c.gpr[31]);
}

// What the bracket leaves in the scratch registers once the heap call is done: the interrupt gate's two MSR images
// (OSRestoreInterrupts: r4 = the MSR it found, with EE clear; r5 = the one it wrote), CR0 of `cmplwi r0, 0` on the busy
// byte (1: greater), and the carry of `addic r0, r3, -1; subfe r0, r0, r3` on OSDisableInterrupts' result.
inline void bracket_registers(Cpu& c, u32 link) {
    const u32 msr = c.msr();
    const bool was_enabled = (msr & kMsrEE) != 0u;
    c.gpr[0] = link;
    c.gpr[4] = msr & ~kMsrEE;
    c.gpr[5] = msr;
    c.set_cr_field(0, kCrGreater | ((c.xer & kXerSO) != 0u ? 1u : 0u));
    c.xer = was_enabled ? (c.xer | kXerCA) : (c.xer & ~kXerCA);
}

// RwMalloc from its first instruction: r1 = sp, r3 = size, the caller's link = `link`.
inline void malloc_body(Cpu& c, u32 sp, u32 link) {
    body_frame(c, sp, link);
    const u32 size = c.gpr[3];
    if (size == 0u) {  // mr. r30, r3: CR0 = equal; r3 = 0
        c.gpr[0] = link;
        c.set_cr_field(0, kCrEqual | ((c.xer & kXerSO) != 0u ? 1u : 0u));
        return;
    }
    c.gpr[3] = c.mem_read32(c.gpr[13] + os_heap::kCurrentHeap);
    c.gpr[4] = size;
    if (!os_heap::serve_alloc_from_heap(c)) {
        cyclic(c, "RwMalloc");
        return;
    }
    bracket_registers(c, link);  // r3 = the heap's result
}

// RwFree from its first instruction: r1 = sp, r3 = pointer, the caller's link = `link`.
inline void free_body(Cpu& c, u32 sp, u32 link) {
    body_frame(c, sp, link);
    const u32 pointer = c.gpr[3];
    if (pointer == 0u) {  // mr. r30, r3: CR0 = equal; r3 stays 0
        c.gpr[0] = link;
        c.set_cr_field(0, kCrEqual | ((c.xer & kXerSO) != 0u ? 1u : 0u));
        return;
    }
    c.gpr[3] = c.mem_read32(c.gpr[13] + os_heap::kCurrentHeap);
    c.gpr[4] = pointer;
    if (!os_heap::serve_free_to_heap(c)) {
        cyclic(c, "RwFree");
        return;
    }
    bracket_registers(c, link);
    c.gpr[3] = 1u;  // li r3, 1 before __end_critical_region, which changes nothing
}

// A wrapper's own frame (stwu r1, -0x10(r1); the link word above it) and its indirect call's registers.
inline u32 wrapper_frame(Cpu& c, u32 function) {
    const u32 sp = c.gpr[1];
    c.mem_write32(sp - kFrame, sp);
    c.mem_write32(sp + 4u, c.lr);
    c.gpr[12] = function;
    c.ctr = function;
    return sp - kFrame;
}

void print_stats() {
    static const char* const kNames[6] = {"RwMalloc", "RwFree", "free list alloc", "free list free", "game malloc", "game free"};
    std::fprintf(stderr, "rw alloc (%s):", g_enabled ? "on" : "off");
    for (unsigned i = 0; i < 6u; ++i)
        std::fprintf(stderr, " %s %llu served / %llu declined;", kNames[i], static_cast<unsigned long long>(g_stats.served[i]),
                     static_cast<unsigned long long>(g_stats.declined[i]));
    std::fprintf(stderr, " declined for an interrupt %llu, a set busy byte %llu, an engine slot %llu\n",
                 static_cast<unsigned long long>(g_stats.declined_interrupt), static_cast<unsigned long long>(g_stats.declined_busy),
                 static_cast<unsigned long long>(g_stats.declined_engine));
}

}  // namespace

void set_enabled(bool enabled) noexcept { g_enabled = enabled; }
bool enabled() noexcept { return g_enabled; }
const Stats& stats() noexcept { return g_stats; }

bool serve_malloc(Cpu& c) {
    if (!ready(c, kMallocEntry)) return false;
    malloc_body(c, c.gpr[1], c.lr);
    ++g_stats.served[kMallocEntry];
    return true;
}

bool serve_free(Cpu& c) {
    if (!ready(c, kFreeEntry)) return false;
    free_body(c, c.gpr[1], c.lr);
    ++g_stats.served[kFreeEntry];
    return true;
}

// 0x80486C1C: lwz r5, engine; lwz r3, 0(r3); lwz r12, 0x108(r5); bctrl.
bool serve_free_list_alloc(Cpu& c) {
    u32 engine = 0u;
    if (!ready(c, kListAllocEntry) || !engine_function(c, kListAllocEntry, kEngineMalloc, kRwMalloc, engine)) return false;
    const u32 link = c.lr;
    const u32 frame = wrapper_frame(c, kRwMalloc);
    c.gpr[5] = engine;
    c.gpr[3] = c.mem_read32(c.gpr[3]);  // the list's entry size
    malloc_body(c, frame, kFreeListAllocReturn);
    c.gpr[0] = link;
    ++g_stats.served[kListAllocEntry];
    return true;
}

// 0x80486C4C: stw r31, 0xC(r1); mr r31, r3; mr r3, r4; lwz r5, engine; lwz r12, 0x10C(r5); bctrl; mr r3, r31.
bool serve_free_list_free(Cpu& c) {
    u32 engine = 0u;
    if (!ready(c, kListFreeEntry) || !engine_function(c, kListFreeEntry, kEngineFree, kRwFree, engine)) return false;
    const u32 link = c.lr, list = c.gpr[3], saved_r31 = c.gpr[31];
    const u32 frame = wrapper_frame(c, kRwFree);
    c.mem_write32(frame + 12u, saved_r31);
    c.gpr[31] = list;  // RwFree saves it in its own frame
    c.gpr[3] = c.gpr[4];
    c.gpr[5] = engine;
    free_body(c, frame, kFreeListFreeReturn);
    c.gpr[0] = link;
    c.gpr[3] = list;
    c.gpr[31] = saved_r31;
    ++g_stats.served[kListFreeEntry];
    return true;
}

// 0x80049BB8: addi r4, r4, 0x32; lwz r5, engine; lwz r12, 0x108(r5); bctrl.
bool serve_game_malloc(Cpu& c) {
    u32 engine = 0u;
    if (!ready(c, kGameMallocEntry) || !engine_function(c, kGameMallocEntry, kEngineMalloc, kRwMalloc, engine)) return false;
    const u32 link = c.lr;
    const u32 frame = wrapper_frame(c, kRwMalloc);
    c.gpr[4] += kGameHintBias;
    c.gpr[5] = engine;
    malloc_body(c, frame, kGameMallocReturn);
    c.gpr[0] = link;
    ++g_stats.served[kGameMallocEntry];
    return true;
}

// 0x80049B5C: lwz r4, engine; lwz r12, 0x10C(r4); bctrl.
bool serve_game_free(Cpu& c) {
    u32 engine = 0u;
    if (!ready(c, kGameFreeEntry) || !engine_function(c, kGameFreeEntry, kEngineFree, kRwFree, engine)) return false;
    const u32 link = c.lr;
    const u32 frame = wrapper_frame(c, kRwFree);
    c.gpr[4] = engine;
    free_body(c, frame, kGameFreeReturn);
    c.gpr[0] = link;
    ++g_stats.served[kGameFreeEntry];
    return true;
}

void install(Cpu& cpu) {
    if (const char* v = std::getenv("GEKKO_RW_ALLOC"); v != nullptr && (v[0] == '0' || v[0] == '1')) g_enabled = v[0] == '1';
    if (const char* v = std::getenv("GEKKO_RW_ALLOC_STATS"); v != nullptr && v[0] == '1') std::atexit(print_stats);
    cpu.install_hle(kRwMalloc, [](Cpu& c) { return serve_malloc(c); }, "RwMalloc chain (native, hand-back)");
    cpu.install_hle(kRwFree, [](Cpu& c) { return serve_free(c); }, "RwFree chain (native, hand-back)");
    cpu.install_hle(kFreeListAlloc, [](Cpu& c) { return serve_free_list_alloc(c); }, "RenderWare free list alloc (native, hand-back)");
    cpu.install_hle(kFreeListFree, [](Cpu& c) { return serve_free_list_free(c); }, "RenderWare free list free (native, hand-back)");
    cpu.install_hle(kGameMalloc, [](Cpu& c) { return serve_game_malloc(c); }, "game malloc wrapper (native, hand-back)");
    cpu.install_hle(kGameFree, [](Cpu& c) { return serve_game_free(c); }, "game free wrapper (native, hand-back)");
}

}  // namespace gekko::rw_alloc
