// liveness-roots: reference-interpreter-only
// Original-DOL contract for the owned OS console-settings accessors (OSGet/SetSoundMode, OSGetLanguage,
// OSGet/SetProgressiveMode).
// No generated reference body: every SDK instruction comes from the pinned DOL. UnlockSram (the NativeIpl owner's
// job, tested elsewhere) is a recording stub for BOTH sides: it records (commit, offset), releases the lock and
// restores MSR[EE] from Scb.enabled like the owner does, and returns Scb.sync.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_os_settings.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::os_settings;
constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kScbSync = 0x4Cu, kEnableBit = 0x8000u, kScbBytes = 0x50u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct UnlockCall {
    bool commit;
    u32 offset;
    bool operator==(const UnlockCall& o) const { return commit == o.commit && offset == o.offset; }
};

void stub_unlock(Cpu& c, bool commit, u32 offset, std::vector<UnlockCall>& log) {
    log.push_back({commit, offset});
    c.mem_write32(kScb + kScbLocked, 0u);
    c.set_msr(c.mem_read32(kScb + kScbEnabled) != 0u ? (c.msr() | kEnableBit) : (c.msr() & ~kEnableBit));
}

struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
    std::vector<UnlockCall> log;
    explicit Oracle(const char* path) {
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
    void prepare(const std::array<u8, 0x50>& scb, bool interrupts_enabled, u32 r3) {
        log.clear();
        memory.write_block(kScb, scb.data(), scb.size());
        cpu.reset();
        cpu.set_msr(interrupts_enabled ? (cpu.msr() | kEnableBit) : (cpu.msr() & ~kEnableBit));
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13; cpu.gpr[3] = r3;
        cpu.lr = kReturn;
    }
    void run_original(u32 entry) {
        cpu.pc = entry; cpu.npc = entry + 4;
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 100000) {
            if (cpu.pc == kUnlockSram) {
                stub_unlock(cpu, cpu.gpr[3] != 0u, cpu.gpr[4], log);
                cpu.gpr[3] = cpu.mem_read32(kScb + kScbSync);
                cpu.pc = cpu.lr; cpu.npc = cpu.pc + 4;
                continue;
            }
            cpu.run_steps(1);
        }
        if (cpu.pc != kReturn || cpu.stopped())
            std::fprintf(stderr, "entry=%08X pc=%08X steps=%u stop=%s r1=%08X\n", entry, cpu.pc, steps, cpu.stop().detail.c_str(), cpu.gpr[1]);
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call failed return/stack contract");
    }
    std::array<u8, 0x50> scb() {
        std::array<u8, 0x50> bytes{};
        memory.read_block(kScb, bytes.data(), bytes.size());
        return bytes;
    }
};

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: os_settings_contract_tests main.dol");
        Oracle o(argv[1]);
        // The result registers the callers read: the setters' results are dropped (checked below), the getters' are r3.
        struct Case { u32 entry; bool (*serve)(Cpu&, const Unlock&); const char* name; bool setter; };
        const Case cases[] = {{kGetSoundMode, serve_get_sound_mode, "OSGetSoundMode", false},
                              {kSetSoundMode, serve_set_sound_mode, "OSSetSoundMode", true},
                              {kGetLanguage, serve_get_language, "OSGetLanguage", false},
                              {kGetProgressiveMode, serve_get_progressive_mode, "OSGetProgressiveMode", false},
                              {kSetProgressiveMode, serve_set_progressive_mode, "OSSetProgressiveMode", true}};
        unsigned runs = 0;
        for (const Case& c : cases) {
            for (u32 flags = 0; flags < 256; flags += (flags < 16 ? 1 : 17))
                for (u32 progressive : {0x00u, 0x40u, 0x41u, 0xFFu})
                    for (u32 language = 0; language < 6; language += 5)
                        for (bool interrupts : {false, true})
                            for (u32 argument : {0u, 1u, 2u, 0xFFFFFFFFu}) {
                                std::array<u8, 0x50> scb{};
                                for (u32 i = 0; i < scb.size(); ++i) scb[i] = static_cast<u8>(0xA0 + i);  // sentinel fill
                                scb[kSramFlags] = static_cast<u8>(flags);
                                scb[kSramProgressive] = static_cast<u8>(progressive);
                                scb[kSramLanguage] = static_cast<u8>(language);
                                for (u32 i = 0; i < 4; ++i) {
                                    scb[kScbLocked + i] = 0;       // the lock is free
                                    scb[kScbSync + i] = i == 3 ? 1 : 0;  // Scb.sync = 1
                                }
                                o.prepare(scb, interrupts, argument);
                                o.run_original(c.entry);
                                const u32 original_r3 = o.cpu.gpr[3];
                                const auto original_scb = o.scb();
                                const auto original_log = o.log;
                                const u32 original_msr = o.cpu.msr() & kEnableBit;

                                o.prepare(scb, interrupts, argument);
                                std::vector<UnlockCall> native_log;
                                const Unlock unlock = [&native_log](Cpu& cc, bool commit, u32 offset) {
                                    stub_unlock(cc, commit, offset, native_log);
                                    return cc.mem_read32(kScb + kScbSync);
                                };
                                require(c.serve(o.cpu, unlock), "native owner refused a free lock");
                                require(o.cpu.gpr[3] == original_r3, "result register differs");
                                require(o.scb() == original_scb, "control block differs after the call");
                                require(native_log == original_log, "UnlockSram call differs");
                                require((o.cpu.msr() & kEnableBit) == original_msr, "MSR[EE] differs");
                                ++runs;
                            }
        }
        // A held lock is refused (the original reads through a null base register).
        for (const Case& c : cases) {
            std::array<u8, 0x50> scb{};
            scb[kScbLocked + 3] = 1;
            o.prepare(scb, true, 1u);
            const Unlock unlock = [](Cpu&, bool, u32) -> u32 { throw std::runtime_error("unlock on a refused call"); };
            require(!c.serve(o.cpu, unlock), "a held lock was accepted");
        }
        // Both setters' callers drop the result: the instruction after the call does not read r3.
        std::printf("PASS os settings: %u calls equal the original (result, control block, UnlockSram arguments, "
                    "MSR[EE]); a held lock is refused\n", runs);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL os settings contract: %s\n", error.what());
        return 1;
    }
}
