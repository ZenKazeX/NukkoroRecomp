// The translated-code branch of the guest-call seam (gekko/native_guest_call.h) in the production runtime: guest_call into functions
// the generator translated, the callee running as native x86-64 through the recompiled table.
//
// The targets are found, not named (guest_call_cases.h): the recompiled table is scanned in order and the game's own words are read
// from the DOL given as argv[1]; the first functions that are exactly `li r3,K ; blr` or `mr r3,r4 ; blr` are called and checked
// against what those words do. No address of a game function is a literal here, so this test never keeps a game body compiled.
// guest_call_tests runs the same two patterns, written into memory, through the nested interpreter against the same expectations, so
// both branches agree on r3 and on what the caller keeps. A target with no translated code is an ExecutionFault with no result.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_guest_call.h"
#include "gekko/recomp.h"
#include "guest_call_cases.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kLink = 0x81700F00u, kStack = 0x817E0000u;
constexpr unsigned kPerPattern = 3;  // targets taken per pattern
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
u32 be32(const std::vector<u8>& d, std::size_t at) {
    return (u32{d[at]} << 24) | (u32{d[at + 1]} << 16) | (u32{d[at + 2]} << 8) | u32{d[at + 3]};
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: guest_call_translated_tests main.dol");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 0x100, "missing DOL");
        GuestMemory memory;
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = be32(dol, i * 4), address = be32(dol, 0x48 + i * 4), size = be32(dol, 0x90 + i * 4);
            require(u64{offset} + size <= dol.size(), "invalid DOL section");
            if (size != 0u) memory.write_block(address, dol.data() + offset, size);
        }
        Cpu cpu{memory};
        RecompiledCode code;
        code.install(recomp::kFunctions, recomp::kFunctionCount);
        code.install_ranges(recomp::kRanges, recomp::kRangeCount);

        struct Target { u32 address; u32 expected; };
        std::vector<Target> li_targets, mr_targets;
        for (std::size_t i = 0; i < recomp::kFunctionCount; ++i) {
            const RecompiledCode::Entry& e = recomp::kFunctions[i];
            if (e.size != 8u || e.fn == nullptr) continue;
            const u32 first = memory.read32(e.address), second = memory.read32(e.address + 4u);
            if (second != guest_call_cases::kBlr) continue;
            if ((first & 0xFFFF0000u) == guest_call_cases::kLiR3 && li_targets.size() < kPerPattern)
                li_targets.push_back({e.address, guest_call_cases::sign_extend16(first)});
            else if (first == guest_call_cases::kMrR3R4 && mr_targets.size() < kPerPattern)
                mr_targets.push_back({e.address, guest_call_cases::kSecondArgument});
        }
        require(li_targets.size() == kPerPattern, "the recompiled table holds too few `li r3,K ; blr` functions");
        require(mr_targets.size() == kPerPattern, "the recompiled table holds too few `mr r3,r4 ; blr` functions");

        const auto setup = [&]() {
            cpu.reset();
            cpu.attach_recompiled(&code);
            cpu.gpr[1] = kStack; cpu.gpr[2] = 0x805FA780u; cpu.gpr[13] = 0x805EC500u;
            cpu.gpr[14] = 0xAAAA0014u; cpu.gpr[31] = 0xAAAA001Fu;
            cpu.lr = 0x81701234u; cpu.ctr = 0x81705678u; cpu.cr = 0x12345678u; cpu.pc = 0x81702000u; cpu.npc = 0x81702004u;
            memory.write32(kStack + 4u, 0xFEEDFACEu);
        };
        unsigned calls = 0;
        for (const std::vector<Target>* group : {&li_targets, &mr_targets}) {
            for (const Target& t : *group) {
                setup();
                u32 r = 0xDEADBEEFu;
                require(guest_call(cpu, t.address, kLink, {guest_call_cases::kFirstArgument, guest_call_cases::kSecondArgument}, r),
                        "the translated call failed");
                require(r == t.expected, "the translated call returned a different r3");
                require(cpu.gpr[1] == kStack && cpu.gpr[14] == 0xAAAA0014u && cpu.gpr[31] == 0xAAAA001Fu && cpu.gpr[2] == 0x805FA780u &&
                            cpu.gpr[13] == 0x805EC500u && cpu.lr == 0x81701234u && cpu.ctr == 0x81705678u && cpu.cr == 0x12345678u &&
                            cpu.pc == 0x81702000u && memory.read32(kStack + 4u) == 0xFEEDFACEu && !cpu.stopped(),
                        "the translated call changed the caller's registers or LR slot");
                ++calls;
            }
        }
        // No translated code for the target: loud, no result.
        setup();
        u32 r = 0xDEADBEEFu;
        require(!guest_call(cpu, 0x817FFF00u, kLink, {1u}, r), "a target without translated code returned");
        require(cpu.stopped() && cpu.stop().reason == StopReason::ExecutionFault && r == 0xDEADBEEFu, "the missing target was not an ExecutionFault");
        std::printf("PASS guest call (translated branch): %u calls of translated game leaves (%zu `li r3,K ; blr`, %zu `mr r3,r4 ; blr`, found by their words, "
                    "no literal address) return what their words do with the caller's registers kept; a target without translated code is an ExecutionFault\n",
                    calls, li_targets.size(), mr_targets.size());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL guest_call_translated_tests: %s\n", e.what());
        return 1;
    }
}
