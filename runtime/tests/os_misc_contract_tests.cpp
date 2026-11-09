// liveness-roots: reference-interpreter-only
// Original-DOL contract for the owned OSGetConsoleType (0x80370E04) and the empty callback 0x80378320
// (gekko/native_os_misc.h). The original instructions run in the reference interpreter on a scrambled machine; the
// native owner runs on an identical one. Compared after each call: r3 (console type), every other general register,
// LR, and every byte of the memory ranges the routines could touch. CR0 is scratch and not compared.
// usage: os_misc_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_os_misc.h"
#include "gekko/native_os_report.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::os_misc;
constexpr u32 kR13 = 0x805EC500u, kInfo = 0x80700000u, kReturn = 0x80100000u;

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
        require(memory.read32(kGetConsoleType) == 0x806D5A20u && memory.read32(kEmptyCallback) == 0x4E800020u,
                "unexpected routine words");
        cpu.reset();
    }
    u64 hash() {
        u64 h = 0xCBF29CE484222325ull;
        std::vector<u8> block(0x1000);
        for (u32 base : {kInfo, kR13 + 0x5000u - 0x1000u, 0x80586000u}) {
            memory.read_block(base, block.data(), block.size());
            for (u8 b : block) h = (h ^ b) * 0x100000001B3ull;
        }
        return h;
    }
    void setup(u32 pointer, u32 type, u32 seed) {
        memory.write32(kR13 + kBootInfoCell, pointer);
        if (pointer != 0u) memory.write32(pointer + kConsoleTypeOffset, type);
        cpu.reset();
        u32 x = seed * 2654435761u + 7u;
        for (u32 i = 0; i < 32; ++i) {
            x = x * 1664525u + 1013904223u;
            cpu.gpr[i] = x;
        }
        cpu.gpr[13] = kR13;
        cpu.lr = kReturn;
    }
};
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: os_misc_contract_tests main.dol");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");

        Machine o(dol), n(dol);
        unsigned runs = 0;
        const u32 pointers[] = {0u, kInfo, kInfo + 0x40u};
        const u32 types[] = {0u, 1u, 0x10000002u, 0x00000003u, 0xFFFFFFFFu, 0x80000000u, 0x12345678u};
        for (u32 entry : {kGetConsoleType, kEmptyCallback, kLoadCell5DD0})
            for (u32 pointer : pointers)
                for (u32 type : types)
                    for (u32 seed = 1; seed <= 4; ++seed) {
                        o.setup(pointer, type, seed);
                        n.setup(pointer, type, seed);
                        o.memory.write32(kR13 + kCell5DD0, type ^ seed);  // the getter's cell
                        n.memory.write32(kR13 + kCell5DD0, type ^ seed);
                        Cpu& c = o.cpu;
                        c.pc = entry;
                        c.npc = entry + 4u;
                        for (unsigned steps = 0; steps < 64 && c.pc != kReturn && !c.stopped(); ++steps) c.run_steps(1u);
                        require(c.pc == kReturn, "the original did not return");
                        Cpu& d = n.cpu;
                        if (entry == kGetConsoleType) serve_get_console_type(d);
                        if (entry == kLoadCell5DD0) serve_load_cell_5dd0(d);
                        for (u32 i = 0; i < 32; ++i)
                            require(c.gpr[i] == d.gpr[i], "r" + std::to_string(i) + " differs at entry " + std::to_string(entry));
                        require(c.lr == d.lr, "LR differs");
                        require(o.hash() == n.hash(), "memory differs");
                        ++runs;
                    }
        // 0x8037177C: OSReport with the "%s"+newline format at r13+0x26DC and r3 as its argument. The original runs with
        // OSReport (0x803731CC) replaced by a recorder; the native owner's console text must equal that line, and a
        // retail console must print nothing.
        constexpr u32 kOSReport = 0x803731CCu, kText = 0x80710000u;
        unsigned report_runs = 0;
        for (const char* text : {"", "AX version 1.0", "100% sure %d %s", "a line with spaces and a tab here"})
            for (u32 type : {0x10000002u, 0x00000002u}) {
                o.setup(kInfo, type, 3);
                n.setup(kInfo, type, 3);
                for (Machine* m : {&o, &n}) {
                    for (u32 i = 0;; ++i) {
                        m->memory.write8(kText + i, static_cast<u8>(text[i]));
                        if (text[i] == 0) break;
                    }
                    m->cpu.gpr[3] = kText;
                    m->cpu.gpr[1] = 0x817E0000u;
                    m->cpu.cr = 0xFFFFFFFFu;  // crclr cr1eq in the original; the owner clears the same bit
                }
                Cpu& c = o.cpu;
                c.pc = kReportString;
                c.npc = kReportString + 4u;
                std::string original;
                for (unsigned steps = 0; steps < 200 && c.pc != kReturn && !c.stopped(); ++steps) {
                    if (c.pc == kOSReport) {
                        std::string format;
                        for (u32 i = 0; i < 16; ++i) {
                            const u8 ch = o.memory.read8(c.gpr[3] + i);
                            if (ch == 0) break;
                            format.push_back(static_cast<char>(ch));
                        }
                        require(format == std::string("%s") + char(10), "the original's format is not the %s newline string");
                        for (u32 i = 0; i < 200; ++i) {
                            const u8 ch = o.memory.read8(c.gpr[4] + i);
                            if (ch == 0) break;
                            original.push_back(static_cast<char>(ch));
                        }
                        original.push_back(char(10));
                        c.pc = c.lr;
                        c.npc = c.pc + 4u;
                        continue;
                    }
                    c.run_steps(1u);
                }
                require(c.pc == kReturn, "the original did not return from 0x8037177C");
                os_report::DebugConsole console;
                serve_report_string(n.cpu, console);
                if ((type & 0x10000000u) != 0u) {
                    std::string native;
                    for (const std::string& line : console.recent()) native += line + char(10);
                    if (original.size() == 1u) require(native.empty() || native == original, "empty text differs");
                    else require(native == original, "native text [" + native + "] differs from [" + original + "]");
                } else {
                    require(console.stats().calls == 0, "a retail console must print nothing");
                }
                ++report_runs;
            }
        std::printf("PASS os misc report string: %u calls; the native console line equals the original's OSReport text\n",
                    report_runs);
        std::printf("PASS os misc: %u calls (OSGetConsoleType, the empty callback and the r13+0x5DD0 getter): result, all general registers, LR and "
                    "memory equal to the original\n", runs);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL os misc contract: %s\n", error.what());
        return 1;
    }
}
