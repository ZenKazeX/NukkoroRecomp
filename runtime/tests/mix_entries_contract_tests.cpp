// Original-DOL contract for the owned mixer entries MIXQuit and MIXSetSoundMode.
// liveness-roots: reference-interpreter-only
// Every SDK instruction comes from the pinned DOL: the original bodies run in the reference interpreter and the owner's
// effects (memory written, registers, return) are compared with them for many arguments and r13 bases.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_mix_entries.h"

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
constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kWindowLo = 0x805F1000u, kWindowBytes = 0x2000u;  // around the mixer's SDA words (r13 + 0x5DC0..)

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void load_dol(GuestMemory& memory, const char* path) {
    std::ifstream input(path, std::ios::binary);
    const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
    require(dol.size() >= 256, "missing DOL");
    u64 hash = 0xCBF29CE484222325ull;
    for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
    // SHA-256: fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af.
    require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
    const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
    for (unsigned i = 0; i < 18; ++i) {
        const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
        require(u64(offset) + size <= dol.size(), "invalid DOL section");
        if (size) memory.write_block(address, dol.data() + offset, size);
    }
}

struct Outcome {
    u32 gpr[32];
    u32 lr, ctr, cr, xer, pc;
    std::vector<u8> window;
};

// A machine with a deterministic non-trivial state in the SDA window and the registers. `owned` installs the owner,
// otherwise the original body runs. `wrong_owner` (the negative control) installs a deliberately wrong owner.
Outcome run(const char* dol_path, bool owned, u32 entry, u32 r3, u32 r13,
            const std::function<void(Cpu&)>& wrong_owner = nullptr) {
    GuestMemory memory;
    load_dol(memory, dol_path);
    Cpu cpu(memory);
    if (owned) mix_entries::install(cpu);
    if (owned && wrong_owner) wrong_owner(cpu);
    for (u32 a = kWindowLo; a < kWindowLo + kWindowBytes; a += 4) memory.write32(a, 0xA5000000u ^ (a * 2654435761u));
    cpu.reset();
    cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kReturn;
    for (unsigned i = 0; i < 32; ++i) cpu.gpr[i] = 0x1000u + i * 0x111u;
    cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = r13; cpu.gpr[3] = r3;
    cpu.ctr = 0x77u; cpu.cr = 0x12345678u;
    u64 steps = 0;
    while (cpu.pc != kReturn && !cpu.stopped() && steps < 100) { cpu.run_steps(1); ++steps; }
    require(cpu.pc == kReturn && !cpu.stopped(), "the call did not return to its caller");
    Outcome o{};
    for (unsigned i = 0; i < 32; ++i) o.gpr[i] = cpu.gpr[i];
    o.lr = cpu.lr; o.ctr = cpu.ctr; o.cr = cpu.cr; o.xer = cpu.xer; o.pc = cpu.pc;
    o.window.resize(kWindowBytes);
    for (u32 i = 0; i < kWindowBytes; ++i) o.window[i] = memory.peek8(kWindowLo + i);
    return o;
}

bool same(const Outcome& a, const Outcome& b, std::string& why) {
    if (std::memcmp(a.gpr, b.gpr, sizeof a.gpr) != 0) { why = "a GPR differs"; return false; }
    if (a.lr != b.lr || a.ctr != b.ctr || a.cr != b.cr || a.xer != b.xer || a.pc != b.pc) {
        why = "lr/ctr/cr/xer/pc differ";
        return false;
    }
    for (u32 i = 0; i < kWindowBytes; ++i)
        if (a.window[i] != b.window[i]) {
            char text[96];
            std::snprintf(text, sizeof text, "memory differs at 0x%08X", kWindowLo + i);
            why = text;
            return false;
        }
    return true;
}

unsigned compare_all(const char* path, const std::function<void(Cpu&)>& wrong_owner = nullptr) {
    const u32 modes[] = {0u, 1u, 2u, 3u, 4u, 0x7Fu, 0x80000000u, 0xFFFFFFFFu, 0xDEADBEEFu};
    const u32 bases[] = {kR13, kR13 + 0x40u};  // the real SDA base and a shifted one (the store is r13-relative)
    unsigned cases = 0;
    for (u32 entry : {mix_entries::kMixQuit, mix_entries::kMixSetSoundMode})
        for (u32 base : bases)
            for (u32 mode : modes) {
                const Outcome original = run(path, false, entry, mode, base);
                const Outcome native = run(path, true, entry, mode, base, wrong_owner);
                std::string why;
                if (!same(original, native, why)) {
                    char text[200];
                    std::snprintf(text, sizeof text, "entry %08X r3=%08X r13=%08X: %s", entry, mode, base, why.c_str());
                    throw std::runtime_error(text);
                }
                ++cases;
            }
    return cases;
}

// The original MIXSetSoundMode changes exactly one word, sda+0x5DC8 (to r3); MIXQuit changes nothing.
void original_effect(const char* path) {
    const Outcome quit = run(path, false, mix_entries::kMixQuit, 5u, kR13);
    const Outcome set = run(path, false, mix_entries::kMixSetSoundMode, 5u, kR13);
    const Outcome set_again = run(path, false, mix_entries::kMixSetSoundMode, 0x5u, kR13);
    require(set.window == set_again.window, "MIXSetSoundMode must be deterministic");
    unsigned differing = 0;
    for (u32 i = 0; i < kWindowBytes; ++i)
        if (quit.window[i] != set.window[i]) ++differing;
    require(differing > 0 && differing <= 4, "MIXSetSoundMode must change only one word of the window");
    const u32 word = kR13 + mix_entries::kSdaSoundMode - kWindowLo;
    require(set.window[word] == 0u && set.window[word + 3] == 5u, "MIXSetSoundMode stores r3 at sda+0x5DC8");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: mix_entries_contract_tests main.dol");
        original_effect(argv[1]);
        const unsigned cases = compare_all(argv[1]);
        // The negative control: an owner that stores to the neighbouring word (sda+0x5DC4) must be caught.
        bool caught = false;
        try {
            compare_all(argv[1], [](Cpu& cpu) {
                cpu.install_hle(mix_entries::kMixSetSoundMode, [](Cpu& c) {
                    c.mem_write32(c.gpr[13] + 0x5DC4u, c.gpr[3]);
                    return true;
                }, "wrong owner");
            });
        } catch (const std::exception&) {
            caught = true;
        }
        require(caught, "negative control: the comparison did not catch a store to the wrong word");
        std::printf("PASS mix entries contract: %u original-vs-native cases\n", cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL mix entries contract: %s\n", error.what());
        return 1;
    }
}
