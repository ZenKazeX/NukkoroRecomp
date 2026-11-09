// Actual generated caller and dispatch integration. Exception probes below
// observe transfer mechanics; native_type_unwind_tests executes the original
// catch/cleanup/rethrow runtime independently. No production sites are enabled.
#include "recomp_decls.h"
#include "native_type_catalog.inc"
#include "type_call_fixture.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using namespace gekko;
namespace fixture = type_cast_catalog_fixture;
static_assert(sizeof(LiftSiteRecord) == 16, "continuation policy must not grow the dispatch table");
constexpr u32 kObject = 0x81001000u, kTable = kObject + 0x1000u;
constexpr u32 kStack = 0x817E0000u, kReturn = 0x817FF000u;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct State {
    std::array<u8, 512> object{};
    std::array<u32, 19> nonvolatile{};
    u32 result, stack, link, pc, cr, fpscr;
    u64 time;
    bool operator==(const State& b) const {
        return object == b.object && nonvolatile == b.nonvolatile && result == b.result &&
               stack == b.stack && link == b.link && pc == b.pc && cr == b.cr && fpscr == b.fpscr && time == b.time;
    }
};

unsigned throw_probes = 0, continuation_probes = 0, probe_mode = 0;
void after_throw(Cpu& c) {
    ++continuation_probes; c.pc = kReturn;
    c.request_stop(StopReason::HostRequest, "observed exceptional continuation");
}
void observe_throw(Cpu& c) {
    ++throw_probes;
    require(c.pc == kThrow && c.lr == kThrowReturn && c.gpr[1] == kStack - 32u,
            "generated call lost the exception entry or frame");
    require(c.mem_read32(kStack + 4u) == kSite + 4u &&
            c.mem_read32(kStack - 32u) == kStack && c.gpr[4] == kStack - 24u,
            "generated call lost the caller link or exception payload");
    if (probe_mode == 0) c.request_stop(StopReason::HostRequest, "observed exception entry");
    else if (probe_mode == 1) c.pc = kReturn;
    else c.transfer_to(after_throw);
}

struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
    std::vector<RecompiledCode::Entry> entries;
    RecompiledCode code;
    std::array<u8, 612> original_cast{};
    u64 instructions = 0;

    explicit Oracle(const char* path) {
        std::ifstream file(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(file), {}};
        require(dol.size() >= 256, "missing original DOL");
        u64 hash = 14695981039346656037ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 1099511628211ull;
        require(hash == 0xd2cfb4a1ca897f9bull, "original caller image changed");
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = load_be32(dol.data() + i * 4);
            const u32 address = load_be32(dol.data() + 0x48 + i * 4);
            const u32 size = load_be32(dol.data() + 0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        memory.read_block(kCast, original_cast.data(), original_cast.size());
        require(recomp::kFunctionCount == 2 && recomp::kRangeCount == 1 && recomp::kLiftSiteCount == 1 &&
                recomp::kFunctions[0].address == kCaller && recomp::kFunctions[1].address == kSite + 4u,
                "unexpected generated fixture population");
        entries.assign(recomp::kFunctions, recomp::kFunctions + recomp::kFunctionCount);
        entries.push_back({kThrow, 4, observe_throw, "exception transfer probe"});
        entries.push_back({kReturn, 4, after_throw, "unexpected continuation probe"});
        code.install(entries.data(), entries.size());
        code.install_ranges(recomp::kRanges, recomp::kRangeCount);
        require(!code.lookup(kCast), "old cast remains available to the generated test");
    }

    void reset(u32 descriptor, u32 adjustment, bool null_object = false) {
        cpu.reset();
        cpu.attach_recompiled(nullptr);
        cpu.set_lift_sites(nullptr, 0);
        cpu.set_verify_recompiled(false);
        for (u32 i = 0; i < 512; ++i) memory.write8(kObject - 128u + i, static_cast<u8>(i * 17u));
        for (u32 i = 0; i < 128; i += 4) memory.write32(kStack - 64u + i, 0xA5A5A5A5u);
        memory.write32(kObject + 24u, kTable);
        memory.write32(kTable, descriptor);
        memory.write32(kTable + 4u, adjustment);
        cpu.pc = kCaller; cpu.lr = kReturn; cpu.gpr[1] = kStack; cpu.gpr[13] = kSda;
        cpu.gpr[3] = null_object ? 0 : kObject;
        cpu.gpr[4] = 24; cpu.gpr[5] = kTarget; cpu.gpr[6] = kSource; cpu.gpr[7] = 0;
        cpu.cr = 0x12345678u; cpu.fpscr = 0xF1234567u;
        for (u32 i = 14; i < 32; ++i) cpu.gpr[i] = 0x12340000u + i;
    }

    State state() {
        State result{};
        memory.peek_block(kObject - 128u, result.object.data(), result.object.size());
        for (u32 i = 13; i < 32; ++i) result.nonvolatile[i - 13] = cpu.gpr[i];
        result.result = cpu.gpr[3]; result.stack = cpu.gpr[1]; result.link = cpu.lr;
        result.pc = cpu.pc; result.cr = cpu.cr; result.fpscr = cpu.fpscr; result.time = cpu.game_clock().now();
        return result;
    }

    State run(u32 descriptor, u32 adjustment, unsigned mode, bool null_object = false) {
        reset(descriptor, adjustment, null_object);
        if (mode == 0) {
            memory.write_block(kCast, original_cast.data(), original_cast.size());
            unsigned steps = 0;
            while (cpu.pc != kReturn && ++steps < 10000) {
                cpu.run_steps(1); ++instructions;
                require(!cpu.stopped(), "original caller stopped");
            }
            require(steps < 10000, "original caller did not return");
        } else {
            memory.clear_block(kCast, original_cast.size());
            if (mode == 2) {
                // Execute only the unchanged stack-allocation instruction, then
                // enter at the next instruction to exercise the cold body.
                cpu.run_steps(1); ++instructions;
                require(cpu.pc == kCaller + 4u, "cold-entry prologue did not execute");
            }
            cpu.attach_recompiled(&code);
            cpu.set_lift_sites(recomp::kLiftSites, recomp::kLiftSiteCount);
            if (mode == 1) code.lookup(kCaller)(cpu);
            else code.find_range(kCaller)->at(cpu, kCaller + 4u);
            require(!cpu.stopped() && !cpu.exiting(), "generated caller unexpectedly left native execution");
        }
        require(cpu.pc == kReturn && cpu.gpr[1] == kStack, "caller return/stack changed");
        return state();
    }

    void continuation_checks() {
        // Direct translated call: a prepared throw must not become site+4.
        for (probe_mode = 0; probe_mode < 3; ++probe_mode) {
            reset(0, 0); cpu.pc = kSite; cpu.gpr[7] = 1;
            cpu.attach_recompiled(&code);
            throw_probes = continuation_probes = 0;
            require(recomp::call_type_query(cpu, kSite, kQuery, kTarget), "throw fell through the caller");
            require(throw_probes == 1, "exception entry was skipped or repeated");
            if (probe_mode == 0) require(cpu.stopped() && cpu.pc == kThrow, "stop at throw was overwritten");
            else {
                if (cpu.exiting()) cpu.finish_transfers();
                require(cpu.pc == kReturn && continuation_probes == 1 && cpu.stopped(),
                        "unexpected return or published tail transfer was lost");
            }
        }
        // Starting at a lowered site through either reference execution or the
        // dispatcher must publish the same continuation without dispatching it twice.
        for (bool generated : {false, true}) for (u32 flag : {0u, 1u, 0x10000u}) {
            reset(0, 0); cpu.pc = kSite; cpu.gpr[7] = flag;
            cpu.set_lift_sites(recomp::kLiftSites, recomp::kLiftSiteCount);
            if (generated) cpu.attach_recompiled(&code);
            cpu.run_steps(1);
            require(!cpu.stopped() && cpu.pc == (flag == 1 ? kThrow : kSite + 4u),
                    "dispatcher overwrote the lowered continuation");
            require(cpu.lr == (flag == 1 ? kThrowReturn : kSite + 4u), "dispatcher lost the link");
        }
        // Existing lowerings still request sequential completion even if a
        // subsystem temporarily changes PC. The dispatcher applies that policy.
        reset(0, 0); cpu.pc = kSite; cpu.gpr[10] = 0;
        const LiftSiteRecord sequential{kSite, LiftSiteContinuation::Sequential, +[](Cpu& c) {
            ++c.gpr[10]; c.pc = kThrow;
        }};
        cpu.set_lift_sites(&sequential, 1);
        cpu.run_steps(1);
        require(cpu.pc == kSite + 4u && cpu.npc == cpu.pc && cpu.gpr[10] == 1,
                "legacy sequential completion policy changed");
        // Verification cuts precede LR publication and all data access.
        reset(0, 0); cpu.pc = kSite; cpu.set_verify_recompiled(true);
        const auto before = state();
        require(recomp::call_type_query(cpu, kSite, kQuery, kTarget), "verification did not cut");
        require(state() == before, "verification cut changed caller state");
        // A rejected target must remain stopped at the site, never advance.
        for (bool dispatch : {false, true}) {
            reset(0, 0); cpu.pc = kSite; cpu.gpr[5] ^= 4u;
            if (dispatch) {
                cpu.set_lift_sites(recomp::kLiftSites, recomp::kLiftSiteCount);
                cpu.run_steps(1);
            } else require(recomp::call_type_query(cpu, kSite, kQuery, kTarget), "fault fell through");
            require(cpu.stopped() && cpu.pc == kSite, "fault continuation was overwritten");
        }
    }
};
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected original DOL path");
        Oracle oracle(argv[1]);
        unsigned comparisons = 0;
        for (const auto& descriptor : fixture::descriptors) {
            const u32 adjustment = std::array<u32, 3>{0u - 32u, 0u, 48u}[comparisons % 3];
            const auto original = oracle.run(descriptor.address, adjustment, 0);
            for (unsigned mode : {1u, 2u}) {
                require(oracle.run(descriptor.address, adjustment, mode) == original,
                        "generated caller changed original object flags, return, nonvolatile state or time");
                ++comparisons;
            }
        }
        const auto original_null = oracle.run(0, 0, 0, true);
        for (unsigned mode : {1u, 2u}) require(oracle.run(0, 0, mode, true) == original_null, "null caller changed");
        oracle.continuation_checks();
        std::printf("PASS type-query calls: %u original-caller comparisons plus hot/cold null cases; "
                    "exception/dispatcher/fault/verification continuations; %llu original instructions. "
                    "One original caller, old cast erased in generated runs; production integration pending.\n",
                    comparisons, static_cast<unsigned long long>(oracle.instructions));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL type-query calls: %s\n", error.what());
        return 1;
    }
}
