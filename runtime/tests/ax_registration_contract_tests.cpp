// Original-DOL contract for the AX registration owners (gekko/native_ax_registration.h, audio-ax step 1):
// AXRegisterCallback 0x80385F60, AXRegisterAuxACallback 0x80385050, AXRegisterAuxBCallback 0x8038505C and
// AXSetMode 0x80385784. The original side is the pinned DOL's instructions in the reference interpreter; the owner
// side is the installed hook on a second machine. Compared: r3, the machine state register and every word of guest
// memory (0x80000000..0x80600000) after each call.
// liveness-roots: reference-interpreter-only
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_ax_registration.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kStack = 0x817E0000u, kReturn = 0x817FF000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kLow = 0x80000000u, kHigh = 0x80600000u;

void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    explicit Machine(const char* path) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
    }
    std::vector<u8> snapshot() {
        std::vector<u8> v(kHigh - kLow);
        memory.read_block(kLow, v.data(), v.size());
        return v;
    }
    void sda(u32 offset, u32 value) { memory.write32(kR13 + offset, value); }
    void original(u32 entry, u32 r3, u32 r4) {
        cpu.reset();
        cpu.pc = entry;
        cpu.npc = entry + 4;
        cpu.lr = kReturn;
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 1000) cpu.run_steps(1);
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call did not return cleanly");
    }
};

struct Owner {
    Machine m;
    explicit Owner(const char* path) : m(path) { ax_registration::install(m.cpu); }
    void call(u32 entry, u32 r3, u32 r4) {
        m.cpu.reset();
        m.cpu.gpr[13] = kR13;
        m.cpu.gpr[3] = r3;
        m.cpu.gpr[4] = r4;
        require(m.cpu.try_hle(entry), "the owner declined the call");
    }
};

// Both machines start from the same preset state, run one call and must end identical.
unsigned compare(Machine& a, Owner& b, u32 entry, u32 r3, u32 r4, const std::array<u32, 7>& preset) {
    static constexpr u32 kOffsets[7] = {ax_registration::kSdaFrameCallback, ax_registration::kSdaAuxACallback,
                                        ax_registration::kSdaAuxAContext,   ax_registration::kSdaAuxBCallback,
                                        ax_registration::kSdaAuxBContext,   ax_registration::kSdaMode, 0x5D5Cu};
    for (unsigned i = 0; i < 7; ++i) {
        a.sda(kOffsets[i], preset[i]);
        b.m.sda(kOffsets[i], preset[i]);
    }
    a.original(entry, r3, r4);
    b.call(entry, r3, r4);
    require(a.cpu.gpr[3] == b.m.cpu.gpr[3], "r3 differs");
    require(a.cpu.msr() == b.m.cpu.msr(), "the machine state register differs");
    require(a.snapshot() == b.m.snapshot(), "guest memory differs");
    return 1;
}

void run(const char* dol) {
    Machine original(dol);
    Owner owner(dol);
    const std::array<std::array<u32, 7>, 3> presets{{{0, 0, 0, 0, 0, 0, 0},
                                                      {0x8043F3E4u, 0x803885A0u, 0x80600000u, 0x803890B0u, 0x1234u, 2u, 0xFFFFFFFFu},
                                                      {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                                       0xFFFFFFFFu, 0x5A5A5A5Au}}};
    const std::array<u32, 5> values{0u, 1u, 0x8043F3E4u, 0x80600000u, 0xFFFFFFFFu};
    unsigned cases = 0;
    for (const auto& preset : presets)
        for (const u32 entry : {ax_registration::kRegisterCallback, ax_registration::kRegisterAuxA,
                                ax_registration::kRegisterAuxB, ax_registration::kSetMode})
            for (const u32 r3 : values)
                for (const u32 r4 : {0u, 0xDEADBEEFu}) cases += compare(original, owner, entry, r3, r4, preset);
    // AXSetMode with the stored value: the original does not write (no change either way).
    cases += compare(original, owner, ax_registration::kSetMode, 2u, 0u, {0, 0, 0, 0, 0, 2u, 0});
    // Negative control: the comparison sees a difference (aux A against aux B on the same arguments).
    original.sda(ax_registration::kSdaAuxACallback, 0);
    original.sda(ax_registration::kSdaAuxBCallback, 0);
    owner.m.sda(ax_registration::kSdaAuxACallback, 0);
    owner.m.sda(ax_registration::kSdaAuxBCallback, 0);
    original.original(ax_registration::kRegisterAuxA, 7u, 8u);
    owner.call(ax_registration::kRegisterAuxB, 7u, 8u);
    require(original.snapshot() != owner.m.snapshot(), "negative control: the comparison missed a wrong owner");
    std::printf("PASS AX registration owners: %u cases (3 presets x 4 entries x 5 values x 2) equal the original in r3, "
                "MSR and all guest memory; negative control differs\n", cases);
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc > 1, "usage: ax_registration_contract_tests <main.dol>");
        run(argv[1]);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
