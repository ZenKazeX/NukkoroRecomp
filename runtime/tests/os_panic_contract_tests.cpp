// liveness-roots: reference-interpreter-only
// Original-DOL contract for the owned OSPanic (gekko/native_os_panic.h).
// The original instructions run in the reference interpreter with the C library vprintf (0x803AA414) and OSReport
// (0x803731CC) replaced by recording stubs (the text only goes to the debug UART, which a retail console lacks). The
// test checks what the owner documentation claims about the original, over file / line / message / return-address /
// stack-chain variations: it prints the message with the message pointer as the format, reports the file and line with
// its " in "%s" on line %d." format, prints the frame header and one row per stack frame (at most 17, ending at a null
// or 0xFFFF0000 back chain), then reaches PPCHalt 0x80370B1C and stays in its endless loop (never returns); and that
// the native owner stops the machine with HleExit exactly when the return address is the game end routine, naming
// the same file, line and message.
// usage: os_panic_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_os_panic.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::os_panic;
constexpr u32 kVprintf = 0x803AA414u, kOSReport = 0x803731CCu, kHalt = 0x80370B1Cu;
constexpr u32 kFmtFile = 0x80561788u, kFmtHeader = 0x805617A0u, kFmtFrame = 0x805617C8u;
constexpr u32 kStack = 0x817E0000u, kR13 = 0x805EC500u, kR2 = 0x805FA780u;
constexpr u32 kFile = 0x80800000u, kMessage = 0x80800100u;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
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
        require(memory.read32(kOSPanic) == 0x7C0802A6u, "unexpected OSPanic words");
        cpu.reset();
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
    }
    void text(u32 address, const std::string& s) {
        memory.write_block(address, reinterpret_cast<const u8*>(s.c_str()), s.size() + 1);
    }
    // `frames` saved frames above the caller: kStack -> kStack+0x100 -> ... ending in `terminator`.
    void chain(u32 frames, u32 terminator) {
        if (frames == 0) {
            memory.write32(kStack, terminator);
            memory.write32(kStack + 4u, 0x80100000u);
            return;
        }
        u32 at = kStack;
        for (u32 i = 0; i < frames; ++i) {
            const u32 next = i + 1 == frames ? terminator : at + 0x100u;
            memory.write32(at, next);
            memory.write32(at + 4u, 0x80100000u + i * 4u);
            at += 0x100u;
        }
    }
};

struct Report {
    u32 r3, r4, r5, r6;
};

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: os_panic_contract_tests main.dol");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");

        Machine o(dol), n(dol);
        unsigned runs = 0, game_ends = 0;
        const u32 lrs[] = {kGameEndReturn, 0x80051014u, 0x8037AF54u, 0u};
        const u32 lines[] = {0x318u, 0u, 1u, 0xFFFFFFFFu};
        const std::pair<const char*, const char*> texts[] = {
            {"RwEngine.c", "The End."}, {"", ""}, {"file with spaces/x.c", "100% sure"}};
        for (u32 lr : lrs)
            for (u32 line : lines)
                for (const auto& text : texts)
                    for (u32 frames : {0u, 1u, 5u, 16u, 17u, 25u})
                        for (u32 terminator : {0u, 0xFFFF0000u}) {
                            o.text(kFile, text.first);
                            o.text(kMessage, text.second);
                            o.chain(frames, terminator);
                            Cpu& c = o.cpu;
                            c.reset();
                            c.set_msr(c.msr() | kMsrDR | kMsrIR | kMsrFP | 0x8000u);
                            c.gpr[1] = kStack;
                            c.gpr[2] = kR2;
                            c.gpr[13] = kR13;
                            c.gpr[3] = kFile;
                            c.gpr[4] = line;
                            c.gpr[5] = kMessage;
                            c.lr = lr;
                            c.pc = kOSPanic;
                            c.npc = kOSPanic + 4u;
                            c.cr = 0;  // crclr cr1eq in the caller: the varargs prologue branches on cr1
                            std::vector<Report> prints, vprintfs;
                            unsigned steps = 0;
                            u32 in_halt = 0;
                            while (steps++ < 200000 && !c.stopped() && in_halt < 64) {
                                if (c.pc == kVprintf || c.pc == kOSReport) {
                                    (c.pc == kVprintf ? vprintfs : prints).push_back({c.gpr[3], c.gpr[4], c.gpr[5], c.gpr[6]});
                                    c.pc = c.lr;
                                    c.npc = c.pc + 4u;
                                    continue;
                                }
                                if (c.pc >= kHalt && c.pc <= kHalt + 0x10u) ++in_halt;
                                c.run_steps(1u);
                            }
                            require(in_halt >= 64, "the original did not reach PPCHalt loop (pc " + hex(c.pc) + ")");
                            require(vprintfs.size() == 1 && vprintfs[0].r3 == kMessage,
                                    "the message was not printed through vprintf");
                            require(prints.size() >= 2, "too few OSReport calls");
                            require(prints[0].r3 == kFmtFile && prints[0].r4 == kFile && prints[0].r5 == line,
                                    "file/line report differs");
                            require(prints[1].r3 == kFmtHeader, "frame header differs");
                            for (std::size_t i = 2; i < prints.size(); ++i)
                                require(prints[i].r3 == kFmtFrame, "frame row format differs");
                            const u32 rows = static_cast<u32>(prints.size()) - 2u;
                            require(rows >= 1u && rows <= 17u, "frame rows out of range: " + std::to_string(rows));

                            Cpu& d = n.cpu;
                            d.reset();
                            d.gpr[1] = kStack;
                            d.gpr[3] = kFile;
                            d.gpr[4] = line;
                            d.gpr[5] = kMessage;
                            d.lr = lr;
                            n.text(kFile, text.first);
                            n.text(kMessage, text.second);
                            const bool ended = serve_panic(d);
                            require(ended == (lr == kGameEndReturn), "HleExit differs from the return address rule");
                            require(d.stopped() &&
                                        d.stop().reason == (ended ? StopReason::HleExit : StopReason::ExecutionFault),
                                    "stop reason differs");
                            const std::string detail = d.stop().detail;
                            const std::string file = text.first, message = text.second;
                            require(detail.find(file + ":") != std::string::npos && detail.find(message) != std::string::npos,
                                    "stop detail lacks the file or message: " + detail);
                            ++runs;
                            if (ended) ++game_ends;
                        }
        std::printf("PASS os panic: %u variations; the original prints the message, the file and line and the frame rows, "
                    "then halts in PPCHalt loop without returning; the owner stops with HleExit for the game own end (%u "
                    "variations) and a fault otherwise\n", runs, game_ends);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL os panic contract: %s\n", error.what());
        return 1;
    }
}
