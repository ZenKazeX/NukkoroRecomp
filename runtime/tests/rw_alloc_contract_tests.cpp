// liveness-roots: reference-interpreter-only
// Contract of the native allocation chain (gekko/native_rw_alloc.h) against what the image ran before it.
//
// Machine A is the chain as it was: the original instructions of RwMalloc 0x80050E28, RwFree 0x80050EF0, RenderWare's
// free-list pair 0x80486C1C / 0x80486C4C, the game's wrappers 0x80049BB8 / 0x80049B5C, the two empty critical-region
// functions and the interrupt gate (OSDisableInterrupts / OSRestoreInterrupts) in the reference interpreter, over the
// image's own owners of the scheduler count and the OS heap (hooks, as in the image). Machine B calls the six native
// entries. Both hold the same OS heap, engine table and free lists and get the same random sequence of calls with random
// registers, CR, XER, CTR, MSR[EE] and stack depth. After EVERY call every GPR, LR, CTR, CR, XER, MSR and every byte of
// the heap, the stack, the small-data words and the engine block must be equal.
// B alone: a deliverable interrupt, a set busy byte and a foreign engine slot must each be declined with nothing changed.
// Controls (each must FAIL): --control-size (A allocates 0x20 more), --control-stack (A writes one dead stack word
// more), --control-result (A's RwFree leaves 2 in r3).
// usage: rw_alloc_contract_tests <sys/main.dol> [--control-size | --control-stack | --control-result]
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_os_heap.h"
#include "gekko/native_os_threads.h"
#include "gekko/native_rw_alloc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kStack = 0x817E0000u, kR13 = 0x805EC500u, kR2 = 0x805FA780u, kReturn = 0x80100000u;
constexpr u32 kLo = 0x80700000u, kHi = 0x80710000u;  // the OS heap
constexpr u32 kEngine = 0x80760000u, kLists = 0x80760800u;
constexpr u32 kListSizes[] = {24u, 180u, 188u, 0u, 36u, 12u, 84u, 0x9000u};
constexpr u32 kListCount = sizeof(kListSizes) / sizeof(kListSizes[0]);

struct Region {
    u32 base, size;
    const char* name;
};
constexpr Region kRegions[] = {{kLo, kHi - kLo, "heap"},
                               {kStack - 0x1000u, 0x1100u, "stack"},
                               {kR13 + 0x2000u, 0x1000u, "small data (busy byte, current heap)"},
                               {kR13 + 0x5800u, 0x0C00u, "small data (heap array, scheduler count, engine)"},
                               {kEngine, 0x1000u, "engine and free lists"}};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    u32 base_msr = 0;
    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](std::size_t o) {
            return u32{dol[o]} << 24 | u32{dol[o + 1]} << 16 | u32{dol[o + 2]} << 8 | u32{dol[o + 3]};
        };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (size == 0u) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            memory.write_block(address, dol.data() + offset, size);
        }
        require(memory.read32(rw_alloc::kRwMalloc) == 0x9421FFF0u && memory.read32(rw_alloc::kRwFree) == 0x9421FFF0u &&
                    memory.read32(rw_alloc::kFreeListAlloc) == 0x9421FFF0u && memory.read32(rw_alloc::kFreeListFree) == 0x9421FFF0u &&
                    memory.read32(rw_alloc::kGameMalloc) == 0x9421FFF0u && memory.read32(rw_alloc::kGameFree) == 0x9421FFF0u,
                "unexpected words at the chain's entries");
        cpu.reset();
        base_msr = (cpu.msr() | kMsrDR | kMsrIR | kMsrFP) & ~kMsrEE;
        cpu.set_msr(base_msr);
        cpu.gpr[13] = kR13;
        // The heap, the engine's two memory slots and the free lists.
        cpu.gpr[3] = kLo;
        cpu.gpr[4] = kHi;
        cpu.gpr[5] = 1u;
        require(os_heap::serve_init_alloc(cpu), "OSInitAlloc");
        cpu.gpr[4] = kHi;
        require(os_heap::serve_create_heap(cpu), "OSCreateHeap");
        memory.write32(kR13 + os_heap::kCurrentHeap, cpu.gpr[3]);
        memory.write32(kR13 + rw_alloc::kEngineSda, kEngine);
        memory.write32(kEngine + rw_alloc::kEngineMalloc, rw_alloc::kRwMalloc);
        memory.write32(kEngine + rw_alloc::kEngineFree, rw_alloc::kRwFree);
        for (u32 i = 0; i < kListCount; ++i) memory.write32(kLists + 0x20u * i, kListSizes[i]);
    }
    // The image's owners under the chain, as hooks (machine A).
    void install_image_owners() {
        os_heap::install(cpu);
        cpu.install_hle(os_threads::kDisableScheduler, [](Cpu& c) {
            (void)os_threads::serve_disable_scheduler(c, os_threads::kDisableScheduler);
            return true;
        }, "OSDisableScheduler");
        cpu.install_hle(os_threads::kEnableScheduler, [](Cpu& c) {
            (void)os_threads::serve_enable_scheduler(c, os_threads::kEnableScheduler);
            return true;
        }, "OSEnableScheduler");
    }
    void prepare(u32 r3, u32 r4, u32 seed, bool enabled, u32 depth) {
        std::mt19937 rng(seed);
        for (u32 i = 0; i < 32u; ++i) cpu.gpr[i] = rng();
        cpu.gpr[1] = kStack - 0x10u * depth;
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.lr = kReturn;
        cpu.cr = rng();
        cpu.xer = (rng() & 0xE0000000u) | (rng() & 0x7Fu);
        cpu.ctr = rng();
        cpu.set_msr(enabled ? (base_msr | kMsrEE) : base_msr);
    }
    bool run_original(u32 entry) {
        cpu.pc = entry;
        cpu.npc = entry + 4u;
        for (unsigned steps = 0; steps < 400000 && cpu.pc != kReturn && !cpu.stopped(); ++steps) cpu.run_steps(1u);
        return cpu.pc == kReturn && !cpu.stopped();
    }
    std::vector<u8> snapshot() {
        std::vector<u8> bytes;
        for (const Region& region : kRegions) {
            const std::size_t at = bytes.size();
            bytes.resize(at + region.size);
            memory.read_block(region.base, bytes.data() + at, region.size);
        }
        return bytes;
    }
};

struct Registers {
    u32 gpr[32], lr, ctr, cr, xer, msr;
    explicit Registers(const Cpu& c) : lr(c.lr), ctr(c.ctr), cr(c.cr), xer(c.xer), msr(c.msr()) {
        std::memcpy(gpr, c.gpr, sizeof(gpr));
    }
};

void same_registers(const Registers& a, const Registers& b, const std::string& where) {
    for (u32 r = 0; r < 32u; ++r)
        require(a.gpr[r] == b.gpr[r], where + ": r" + std::to_string(r) + " differs (" + hex(a.gpr[r]) + " / " + hex(b.gpr[r]) + ")");
    require(a.lr == b.lr, where + ": LR differs");
    require(a.ctr == b.ctr, where + ": CTR differs (" + hex(a.ctr) + " / " + hex(b.ctr) + ")");
    require(a.cr == b.cr, where + ": CR differs (" + hex(a.cr) + " / " + hex(b.cr) + ")");
    require(a.xer == b.xer, where + ": XER differs (" + hex(a.xer) + " / " + hex(b.xer) + ")");
    require(a.msr == b.msr, where + ": MSR differs (" + hex(a.msr) + " / " + hex(b.msr) + ")");
}

void same_memory(const std::vector<u8>& a, const std::vector<u8>& b, const std::string& where) {
    if (a == b) return;
    std::size_t at = 0;
    for (const Region& region : kRegions) {
        for (u32 i = 0; i < region.size; ++i)
            if (a[at + i] != b[at + i])
                throw std::runtime_error(where + ": memory differs in the " + region.name + " at " + hex(region.base + i) + " (" +
                                         std::to_string(a[at + i]) + " / " + std::to_string(b[at + i]) + ")");
        at += region.size;
    }
}

enum Op : unsigned { kMalloc, kFree, kListAlloc, kListFree, kGameMalloc, kGameFree, kOps };
constexpr u32 kEntries[kOps] = {rw_alloc::kRwMalloc,      rw_alloc::kRwFree,     rw_alloc::kFreeListAlloc,
                                rw_alloc::kFreeListFree,  rw_alloc::kGameMalloc, rw_alloc::kGameFree};
const char* const kNames[kOps] = {"RwMalloc", "RwFree", "free list alloc", "free list free", "game malloc", "game free"};

bool serve(Cpu& c, unsigned op) {
    switch (op) {
    case kMalloc: return rw_alloc::serve_malloc(c);
    case kFree: return rw_alloc::serve_free(c);
    case kListAlloc: return rw_alloc::serve_free_list_alloc(c);
    case kListFree: return rw_alloc::serve_free_list_free(c);
    case kGameMalloc: return rw_alloc::serve_game_malloc(c);
    default: return rw_alloc::serve_game_free(c);
    }
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2 || argc == 3, "usage: rw_alloc_contract_tests main.dol [--control-size | --control-stack | --control-result]");
        const std::string control = argc == 3 ? argv[2] : "";
        require(control.empty() || control == "--control-size" || control == "--control-stack" || control == "--control-result",
                "unknown control");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");

        rw_alloc::set_enabled(true);
        u64 calls = 0, per_op[kOps] = {}, refused = 0, zero_arguments = 0, enabled_calls = 0, masked_line_calls = 0, declines = 0;
        // 0x7FFFFFC0 rounds to the largest positive cell size (refused); 0xFFFFFFE1 wraps to a header-only cell (served).
        // Not drawn: 0x7FFFFFC1 .. 0xFFFFFFC0, whose rounded size is negative as a signed word. The original's compare
        // then takes the first free cell and splits it at an address outside the heap: it destroys its own lists, so
        // no state after such a call is a contract (the heap owner reproduces the wild writes; os_heap_contract_tests).
        const u32 sizes[] = {0u, 1u, 12u, 20u, 24u, 28u, 31u, 32u, 33u, 36u, 63u, 64u, 96u, 180u, 188u, 0x200u, 0x1000u, 0x3000u, 0x20000u, 0x7FFFFFC0u, 0xFFFFFFE1u};
        for (unsigned seed = 1; seed <= 24; ++seed) {
            Machine a(dol), b(dol);
            a.install_image_owners();
            if (control == "--control-size") {
                require(a.memory.read32(0x80050E94u) == 0x7FC4F378u, "control: unexpected word");
                a.memory.write32(0x80050E94u, 0x389E0020u);  // mr r4, r30 -> addi r4, r30, 0x20
            } else if (control == "--control-stack") {
                require(a.memory.read32(0x80050E4Cu) == 0x48358221u, "control: unexpected word");
                a.memory.write32(0x80050E4Cu, 0x9061FFE0u);  // bl __begin_critical_region -> stw r3, -0x20(r1)
            } else if (control == "--control-result") {
                require(a.memory.read32(0x80050F88u) == 0x38600001u, "control: unexpected word");
                a.memory.write32(0x80050F88u, 0x38600002u);  // li r3, 1 -> li r3, 2
            }
            if (control.empty()) same_memory(a.snapshot(), b.snapshot(), "before the calls");
            std::mt19937 rng(seed * 7919u);
            std::vector<u32> live;
            for (unsigned n = 0; n < 4000; ++n) {
                const std::string where = "seed " + std::to_string(seed) + " call " + std::to_string(n);
                // More frees once the heap holds many cells, so it fills and empties.
                const bool want_free = !live.empty() && (rng() % 100u) < (live.size() > 300u ? 70u : 40u);
                const unsigned kind = rng() % 3u;
                const unsigned op = want_free ? (kind == 0u ? kFree : kind == 1u ? kListFree : kGameFree)
                                              : (kind == 0u ? kMalloc : kind == 1u ? kListAlloc : kGameMalloc);
                const bool is_free = op == kFree || op == kListFree || op == kGameFree;
                u32 pointer = 0u;
                if (is_free && (rng() % 16u) != 0u) pointer = live[rng() % live.size()];
                const u32 size = sizes[rng() % (sizeof(sizes) / sizeof(sizes[0]))];
                const u32 list = kLists + 0x20u * (rng() % kListCount), other = rng();
                u32 r3 = 0u, r4 = other;
                switch (op) {
                case kMalloc: r3 = size; break;
                case kGameMalloc: r3 = size; break;
                case kListAlloc: r3 = list; break;
                case kListFree: r3 = list, r4 = pointer; break;
                default: r3 = pointer; break;
                }
                const bool zero = is_free ? pointer == 0u : (op == kListAlloc ? a.memory.read32(list) == 0u : size == 0u);
                const u32 regs = rng(), depth = rng() % 64u;
                const bool enabled = (rng() & 1u) != 0u;
                // An asserted interrupt line with MSR[EE] clear is not deliverable: the chain runs, and the gate keeps EE clear.
                const bool masked_line = !enabled && (rng() % 8u) == 0u;

                // B alone, now and then: the three cases the native entries must hand back, with nothing changed.
                if ((n % 97u) == 0u) {
                    const unsigned why = (n / 97u) % 3u;
                    const bool wrapper = op != kMalloc && op != kFree;
                    if (why != 2u || wrapper) {
                        b.prepare(r3, r4, regs, why == 0u ? true : enabled, depth);
                        const u32 slot = kEngine + (is_free ? rw_alloc::kEngineFree : rw_alloc::kEngineMalloc);
                        const u32 slot_word = b.memory.read32(slot);
                        if (why == 0u) b.cpu.set_external_interrupt(true);
                        else if (why == 1u) b.memory.write8(kR13 + rw_alloc::kBusySda, 1u);
                        else b.memory.write32(slot, slot_word + 4u);
                        const Registers before(b.cpu);
                        const std::vector<u8> memory_before = b.snapshot();
                        require(!serve(b.cpu, op), where + ": " + kNames[op] + " did not decline (case " + std::to_string(why) + ")");
                        same_registers(before, Registers(b.cpu), where + " (declined)");
                        same_memory(memory_before, b.snapshot(), where + " (declined)");
                        if (why == 0u) b.cpu.set_external_interrupt(false);
                        else if (why == 1u) b.memory.write8(kR13 + rw_alloc::kBusySda, 0u);
                        else b.memory.write32(slot, slot_word);
                        ++declines;
                    }
                }

                a.prepare(r3, r4, regs, enabled, depth);
                b.prepare(r3, r4, regs, enabled, depth);
                if (masked_line) {
                    a.cpu.set_external_interrupt(true);
                    b.cpu.set_external_interrupt(true);
                }
                require(a.run_original(kEntries[op]), where + ": the original " + kNames[op] + " did not return");
                require(serve(b.cpu, op), where + ": the native " + kNames[op] + " declined");
                if (masked_line) {
                    a.cpu.set_external_interrupt(false);
                    b.cpu.set_external_interrupt(false);
                }
                same_registers(Registers(a.cpu), Registers(b.cpu), where + " " + kNames[op]);
                same_memory(a.snapshot(), b.snapshot(), where + " " + kNames[op]);
                if (!is_free) {
                    if (a.cpu.gpr[3] != 0u) live.push_back(a.cpu.gpr[3]);
                    else if (!zero) ++refused;
                } else if (pointer != 0u) {
                    live.erase(std::find(live.begin(), live.end(), pointer));
                }
                ++calls;
                ++per_op[op];
                if (zero) ++zero_arguments;
                if (enabled) ++enabled_calls;
                if (masked_line) ++masked_line_calls;
            }
        }
        const rw_alloc::Stats& stats = rw_alloc::stats();
        u64 served = 0, declined = 0;
        for (unsigned i = 0; i < kOps; ++i) {
            require(stats.served[i] == per_op[i], std::string("served count of ") + kNames[i] + " differs from the calls made");
            served += stats.served[i];
            declined += stats.declined[i];
        }
        require(declined == declines, "declined count differs from the declines provoked");
        require(stats.declined_interrupt != 0u && stats.declined_busy != 0u && stats.declined_engine != 0u, "a decline case was never provoked");
        std::printf("PASS rw alloc chain: %llu calls (RwMalloc %llu, RwFree %llu, free list %llu / %llu, game %llu / %llu; %llu with a zero "
                    "argument, %llu refused by a full heap, %llu with interrupts enabled, %llu with a masked line asserted): every GPR, LR, "
                    "CTR, CR, XER, MSR and the heap, stack, small data and engine bytes equal after each; %llu declines (interrupt %llu, "
                    "busy %llu, engine slot %llu) changed nothing\n",
                    static_cast<unsigned long long>(calls), static_cast<unsigned long long>(per_op[kMalloc]),
                    static_cast<unsigned long long>(per_op[kFree]), static_cast<unsigned long long>(per_op[kListAlloc]),
                    static_cast<unsigned long long>(per_op[kListFree]), static_cast<unsigned long long>(per_op[kGameMalloc]),
                    static_cast<unsigned long long>(per_op[kGameFree]), static_cast<unsigned long long>(zero_arguments),
                    static_cast<unsigned long long>(refused), static_cast<unsigned long long>(enabled_calls),
                    static_cast<unsigned long long>(masked_line_calls), static_cast<unsigned long long>(declines),
                    static_cast<unsigned long long>(stats.declined_interrupt), static_cast<unsigned long long>(stats.declined_busy),
                    static_cast<unsigned long long>(stats.declined_engine));
        (void)served;
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL rw alloc contract: %s\n", error.what());
        return 1;
    }
}
