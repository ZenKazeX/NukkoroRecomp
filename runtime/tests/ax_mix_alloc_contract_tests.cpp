// liveness-roots: reference-interpreter-only
// Original-DOL contract for the owned AX MIX heap wrappers 0x8038A0DC / 0x8038A104 (gekko/native_ax_mix_alloc.h).
// Machine A runs the original instructions (the wrappers AND the OS heap routines they tail into) in the reference
// interpreter; machine B runs the native wrappers over the native heap owners. Both build the same heap (OSInitAlloc +
// OSCreateHeap, one 64 KB heap, made current at r13+0x26E0), then perform the same random sequence of allocations (sizes
// 0 .. beyond the heap) and frees of earlier results. After every call r3 (allocations), the preserved registers and
// every byte of the heap range plus the heap descriptor cells must be equal.
// usage: ax_mix_alloc_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_ax_mix_alloc.h"
#include "gekko/native_os_heap.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kStack = 0x817E0000u, kR13 = 0x805EC500u, kR2 = 0x805FA780u, kReturn = 0x80100000u;
constexpr u32 kLo = 0x80700000u, kHi = 0x80710000u;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
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
        require(memory.read32(ax_mix_alloc::kMixAlloc) == 0x7C0802A6u, "unexpected wrapper words");
        cpu.reset();
    }
    void prepare(u32 r3, u32 r4, u32 seed) {
        std::mt19937 rng(seed);
        cpu.reset();
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
        for (u32 i = 5; i < 13; ++i) cpu.gpr[i] = rng();
        for (u32 i = 14; i < 32; ++i) cpu.gpr[i] = rng();
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.lr = kReturn;
        cpu.cr = 0;
    }
    bool run_original(u32 entry) {
        cpu.pc = entry;
        cpu.npc = entry + 4u;
        for (unsigned steps = 0; steps < 400000 && cpu.pc != kReturn && !cpu.stopped(); ++steps) cpu.run_steps(1u);
        return cpu.pc == kReturn;
    }
    u64 hash() {
        u64 h = 0xCBF29CE484222325ull;
        std::vector<u8> block(0x1000);
        for (u32 base = kLo; base < kHi; base += 0x1000u) {
            memory.read_block(base, block.data(), block.size());
            for (u8 b : block) h = (h ^ b) * 0x100000001B3ull;
        }
        for (u32 base : {kR13 + 0x5000u, kR13 + 0x2000u}) {
            memory.read_block(base, block.data(), 0x800);
            for (u32 i = 0; i < 0x800; ++i) h = (h ^ block[i]) * 0x100000001B3ull;
        }
        return h;
    }
};
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: ax_mix_alloc_contract_tests main.dol");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");

        unsigned calls = 0, refused = 0;
        for (unsigned seed = 1; seed <= 6; ++seed) {
            Machine a(dol), b(dol);
            std::mt19937 rng(seed * 7919u);
            // the same heap on both: the original routines on A, the owners (contract-tested in os_heap_contract_tests) on B
            a.prepare(kLo, kHi, seed);
            a.cpu.gpr[5] = 1u;
            require(a.run_original(os_heap::kInitAlloc), "OSInitAlloc did not return (A)");
            a.prepare(a.cpu.gpr[3], kHi, seed);
            require(a.run_original(os_heap::kCreateHeap), "OSCreateHeap did not return (A)");
            const u32 heap = a.cpu.gpr[3];
            b.prepare(kLo, kHi, seed);
            b.cpu.gpr[5] = 1u;
            require(os_heap::serve_init_alloc(b.cpu), "OSInitAlloc (B)");
            b.prepare(b.cpu.gpr[3], kHi, seed);
            require(os_heap::serve_create_heap(b.cpu), "OSCreateHeap (B)");
            require(b.cpu.gpr[3] == heap, "heap ids differ");
            a.memory.write32(kR13 + os_heap::kCurrentHeap, heap);
            b.memory.write32(kR13 + os_heap::kCurrentHeap, heap);
            require(a.hash() == b.hash(), "heaps differ before the calls");

            std::vector<u32> live;
            const u32 sizes[] = {0u,     1u,      31u,    32u,    63u,    64u,       65u,
                                 100u,   0x200u,  0x400u, 0x1000u, 0x3000u, 0x7FFFu, 0x20000u};
            for (unsigned op = 0; op < 160; ++op) {
                const bool alloc = live.empty() || (rng() % 3u) != 0u;
                const u32 arg = alloc ? sizes[rng() % (sizeof(sizes) / sizeof(sizes[0]))] : live[rng() % live.size()];
                const u32 regs = rng(), stale_r4 = rng();
                a.prepare(arg, stale_r4, regs);
                b.prepare(arg, stale_r4, regs);
                require(a.run_original(alloc ? ax_mix_alloc::kMixAlloc : ax_mix_alloc::kMixFree),
                        "the original did not return");
                require(alloc ? ax_mix_alloc::serve_alloc(b.cpu) : ax_mix_alloc::serve_free(b.cpu),
                        "the native wrapper refused");
                if (alloc) {
                    require(a.cpu.gpr[3] == b.cpu.gpr[3], "allocation result differs (op " + std::to_string(op) + ")");
                    if (a.cpu.gpr[3] != 0u) live.push_back(a.cpu.gpr[3]);
                    else ++refused;
                } else {
                    live.erase(std::find(live.begin(), live.end(), arg));
                }
                for (u32 r : {1u, 2u, 13u, 14u, 20u, 31u})
                    require(a.cpu.gpr[r] == b.cpu.gpr[r], "r" + std::to_string(r) + " differs");
                require(a.cpu.lr == b.cpu.lr, "LR differs");
                require(a.hash() == b.hash(), "heap memory differs after op " + std::to_string(op));
                ++calls;
            }
        }
        std::printf("PASS ax mix alloc: %u wrapper calls (%u allocations refused): result, preserved registers, LR and the "
                    "whole heap equal to the original\n", calls, refused);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL ax mix alloc contract: %s\n", error.what());
        return 1;
    }
}
