// liveness-roots: reference-interpreter-only
// Original-DOL contract for the world-sector vertex-format decoders (gekko/native_rw_world_decode.h): 0x8046798C, 0x804679F0,
// 0x80467A3C, 0x80467AA4. Each owner runs against the ORIGINAL body (through its jump table for 0x80467AA4) on byte-identical
// memory: r3, the callee-saved registers, r1 and lr must agree.
//
// Inputs: every value of the bit fields each decoder reads (exhaustive over the selector and multiplier fields, with random
// bits elsewhere in the words), every index byte 0..255 for the indexed decoders, plus a random sweep. Negative controls:
// owners with one answer changed, and an ORIGINAL code word patched, are told apart.
//
// usage: rw_world_decode_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_rw_world_decode.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
namespace dec = gekko::rw_world_decode;

constexpr u32 kSda = 0x805EC500u, kSda2 = 0x805FA780u, kStack = 0x817E0000u, kFinish = 0x817FF000u, kDescriptor = 0x81302000u;

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};

    Machine(const char* path, bool native, const std::function<void(Cpu&)>& wrong, const std::vector<std::pair<u32, u32>>& patch) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        unsigned long long hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(static_cast<unsigned long long>(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        if (native) {
            dec::install(cpu);
            if (wrong) wrong(cpu);
        } else {
            for (const auto& [address, value] : patch) memory.write32(address, value);
        }
    }

    u32 run(u32 entry, u32 w4, u32 w8, u32 wc, u32 index) {
        memory.write32(kDescriptor + 4u, w4);
        memory.write32(kDescriptor + 8u, w8);
        memory.write32(kDescriptor + 0xCu, wc);
        cpu.reset();
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kFinish;
        for (unsigned i = 0; i < 32; ++i) cpu.gpr[i] = 0x1000u + i * 0x111u;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kSda2; cpu.gpr[13] = kSda; cpu.gpr[3] = kDescriptor; cpu.gpr[4] = index;
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 2000) cpu.run_steps(1);
        require(cpu.pc == kFinish && !cpu.stopped(), "the call did not return");
        for (unsigned i = 14; i < 32; ++i) require(cpu.gpr[i] == 0x1000u + i * 0x111u, "a callee-saved register changed");
        require(cpu.gpr[1] == kStack && cpu.lr == kFinish, "r1 / lr changed");
        require(memory.peek32(kDescriptor + 4u) == w4 && memory.peek32(kDescriptor + 8u) == w8 && memory.peek32(kDescriptor + 0xCu) == wc,
                "the descriptor was written");
        return cpu.gpr[3];
    }
};

unsigned g_cases = 0;
const std::function<void(Cpu&)>* g_wrong = nullptr;
const std::vector<std::pair<u32, u32>>* g_patch = nullptr;

struct Pair {
    Machine original, native;
    explicit Pair(const char* path)
        : original(path, false, nullptr, g_patch ? *g_patch : std::vector<std::pair<u32, u32>>{}),
          native(path, true, g_wrong ? *g_wrong : std::function<void(Cpu&)>{}, {}) {}
    void check(const char* what, u32 entry, u32 w4, u32 w8, u32 wc, u32 index) {
        const u32 a = original.run(entry, w4, w8, wc, index), b = native.run(entry, w4, w8, wc, index);
        ++g_cases;
        if (a != b) {
            char text[160];
            std::snprintf(text, sizeof text, "%s %08X words %08X %08X %08X index %u: original %u native %u", what, entry, w4, w8, wc,
                          index, a, b);
            throw std::runtime_error(text);
        }
    }
};

struct Rng {
    u32 state = 7u;
    u32 next() {
        state = state * 1664525u + 1013904223u;
        return state ^ (state >> 15);
    }
};

void run_all(const char* path) {
    g_cases = 0;
    Pair p(path);
    Rng rng;
    for (u32 low = 0; low < 16u; ++low)
        for (int k = 0; k < 8; ++k) p.check("A", dec::kComponentsA, (rng.next() & ~0xFu) | low, rng.next(), rng.next(), 0u);
    for (u32 field = 0; field < 8u; ++field)
        for (int k = 0; k < 8; ++k)
            p.check("B", dec::kComponentsB, (rng.next() & ~(7u << 10)) | (field << 10), rng.next(), rng.next(), 0u);
    for (u32 index = 0; index < 256u; ++index)
        for (int k = 0; k < 10; ++k) p.check("C", dec::kComponentsC, rng.next(), rng.next(), rng.next(), index);
    // C also with indices whose shift passes 31 / wraps (the index is used whole: (index << 2) & 0x3FC)
    for (u32 k = 0; k < 400u; ++k) p.check("C wide", dec::kComponentsC, rng.next(), rng.next(), rng.next(), rng.next());
    for (u32 index = 0; index < 256u; ++index)
        for (int k = 0; k < 24; ++k)
            p.check("D", dec::kComponentsD, rng.next(), rng.next(), rng.next(), index | (k % 3 == 0 ? (rng.next() & 0xFFFFFF00u) : 0u));
    // the indexed decoder with each selector's bit and field walked exhaustively
    const u32 bits[8][3] = {{4, 21, 22}, {8, 0, 1}, {8, 9, 10}, {8, 18, 19}, {8, 27, 28}, {12, 5, 6}, {12, 14, 15}, {12, 23, 24}};
    for (u32 selector = 0; selector < 8u; ++selector)
        for (u32 bit = 0; bit < 2u; ++bit)
            for (u32 field = 0; field < 8u; ++field) {
                u32 words[3] = {rng.next(), rng.next(), rng.next()};
                u32& w = words[bits[selector][0] / 4 - 1];
                w = (w & ~(1u << bits[selector][1]) & ~(7u << bits[selector][2])) | (bit << bits[selector][1]) |
                    (field << bits[selector][2]);
                p.check("D walked", dec::kComponentsD, words[0], words[1], words[2], selector);
            }
}

bool caught(const char* path, const std::function<void(Cpu&)>* wrong, const std::vector<std::pair<u32, u32>>* patch) {
    g_wrong = wrong;
    g_patch = patch;
    bool found = false;
    try {
        run_all(path);
    } catch (const std::exception&) {
        found = true;
    }
    g_wrong = nullptr;
    g_patch = nullptr;
    return found;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rw_world_decode_tests main.dol");
        run_all(argv[1]);
        const unsigned cases = g_cases;
        const std::function<void(Cpu&)> wrong_b = [](Cpu& cpu) {
            cpu.install_hle(dec::kComponentsB, [](Cpu& c) {
                const u32 field = (c.mem_read32(c.gpr[3] + 4u) >> 10) & 7u;
                c.gpr[3] = field == 1u ? 3u : field == 3u ? 6u : field == 4u ? 11u : 0u;   // 11 instead of 12
                return true;
            }, "wrong: 11");
        };
        require(caught(argv[1], &wrong_b, nullptr), "negative control: a decoder with one answer changed was not told apart");
        const std::function<void(Cpu&)> wrong_d = [](Cpu& cpu) {
            cpu.install_hle(dec::kComponentsD, [](Cpu& c) {
                c.gpr[3] = c.gpr[4] > 8u ? 0u : 1u;
                return true;
            }, "wrong: constant");
        };
        require(caught(argv[1], &wrong_d, nullptr), "negative control: a constant decoder was not told apart");
        // an ORIGINAL code word patched: 0x804679F8 `cmpwi r0, 3` -> `cmpwi r0, 2` moves one answer
        const std::vector<std::pair<u32, u32>> patch = {{0x804679F8u, 0x2C000002u}};
        require(caught(argv[1], nullptr, &patch), "mutation: a patched original word was not told apart");
        std::printf("PASS rw world decode: %u original-vs-native cases\n", cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL rw world decode: %s\n", error.what());
        return 1;
    }
}
