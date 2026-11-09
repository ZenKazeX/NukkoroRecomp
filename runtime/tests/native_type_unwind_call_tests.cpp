// A real original reference-cast caller enters the native query boundary.
// The original instruction interpreter then executes the actual exception
// runtime. This validates composition, not an entirely generated unwinder.
#include "support/type_unwind_oracle.h"
#include "recomp_decls.h"
#include "type_call_fixture.h"

namespace {
using namespace gekko;
namespace oracle = type_unwind_oracle;
namespace fixture = type_cast_catalog_fixture;
namespace queries = shadow::object::game_type_queries;
RecompiledCode generated;
u32 dynamic_descriptor = 0;
bool cold_entry = false, corrupt_saved_link = false;
unsigned entered_throw = 0;

void handoff_to_original_runtime(Cpu& cpu) {
    ++entered_throw;
    cpu.request_stop(StopReason::HostRequest, "reference-oracle handoff at actual throw entry");
}

u32 run_caller(oracle::Oracle& test, u32 parent_stack, bool native) {
    Cpu& cpu = test.cpu;
    cpu.attach_recompiled(nullptr);
    cpu.set_lift_sites(nullptr, 0);
    const u32 table = test.reserve(12);
    test.memory.write32(table, dynamic_descriptor);
    test.memory.write32(table + 4u, 0);
    test.memory.write32(table + 8u, oracle::kCallback);
    test.memory.write32(oracle::kObject, table);
    cpu.pc = kCaller;
    cpu.gpr[3] = oracle::kObject + 256u; // receiver is not dereferenced before the failed cast
    cpu.gpr[5] = oracle::kObject;
    if (native) {
        if (cold_entry) {
            test.step();
            oracle::require(cpu.pc == kCaller + 4u, "reference caller cold-entry prologue changed");
        }
        cpu.attach_recompiled(&generated);
        cpu.set_lift_sites(recomp::kLiftSites, recomp::kLiftSiteCount);
        entered_throw = 0;
        if (cold_entry) generated.find_range(kCaller)->at(cpu, kCaller + 4u);
        else generated.lookup(kCaller)(cpu);
        if (entered_throw != 1 || !cpu.stopped() || cpu.pc != oracle::kThrow)
            std::fprintf(stderr, "handoff: entries=%u pc=%08X lr=%08X sp=%08X stopped=%u reason=%u detail=%s\n",
                         entered_throw, cpu.pc, cpu.lr, cpu.gpr[1], unsigned(cpu.stopped()),
                         unsigned(cpu.stop().reason), cpu.stop().detail.c_str());
        oracle::require(entered_throw == 1 && cpu.stopped() && cpu.pc == oracle::kThrow,
                        "generated reference caller lost the actual throw continuation");
        oracle::require(cpu.stop().reason == StopReason::HostRequest,
                        "generated caller faulted before the reference-runtime handoff");
        cpu.clear_stop();
        cpu.attach_recompiled(nullptr);
        cpu.set_lift_sites(nullptr, 0);
    } else {
        unsigned steps = 0;
        while (cpu.pc != oracle::kThrow && ++steps < 2000) test.step();
        oracle::require(cpu.pc == oracle::kThrow && steps < 2000,
                        "original reference caller did not reach its failed-cast throw");
    }
    // The genuine caller has its own 128-byte frame between the outer catch
    // owner and the cast's 32-byte exception frame. Both links must survive.
    const u32 cast_stack = parent_stack - 128u;
    oracle::require(cpu.gpr[1] == cast_stack - 32u && cpu.lr == kThrowReturn &&
                    test.memory.peek32(cast_stack + 4u) == kSite + 4u,
                    "reference caller/cast frame chain changed");
    if (corrupt_saved_link) test.memory.write32(cast_stack + 4u, oracle::kReturn);
    return cast_stack;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        oracle::require(argc == 2, "expected original DOL path");
        oracle::Oracle test(argv[1]);
        // The generated call dispatches through RecompiledCode, not the HLE
        // registry. This entry only stops at the boundary; the original runtime
        // instructions execute after the fixture detaches the generated table.
        std::vector<RecompiledCode::Entry> entries(recomp::kFunctions, recomp::kFunctions + recomp::kFunctionCount);
        entries.push_back({oracle::kThrow, 4u, handoff_to_original_runtime, "original runtime handoff"});
        generated.install(entries.data(), entries.size());
        generated.install_ranges(recomp::kRanges, recomp::kRangeCount);
        oracle::require(generated.lookup(kCaller) && generated.find_range(kCaller) &&
                        !generated.lookup(oracle::kCast) && recomp::kLiftSiteCount == 1 &&
                        recomp::kLiftSites[0].address == kSite,
                        "reference caller fixture contains wrong entries or the retired cast");
        std::vector<u32> descriptors{0};
        std::vector<shadow::object::TypeId> identities;
        for (const auto& d : fixture::descriptors) {
            const auto identity = fixture::classes[d.class_index].type;
            if (!queries::find_subobject(identity, static_cast<queries::Query>(kQuery)).found &&
                std::find(identities.begin(), identities.end(), identity) == identities.end()) {
                descriptors.push_back(d.address); identities.push_back(identity);
                if (descriptors.size() == 6) break;
            }
        }
        oracle::require(descriptors.size() == 6, "not enough independent failed dynamic types");
        // The registered original exception fragment covers 0x803A1258 through
        // 0x8043BACB. This gameplay caller is outside it; its failed reference
        // cast terminates before the outer catch, even with a valid frame chain.
        // Do not invent an unwind record or make the outer catch reachable.
        oracle::require(test.memory.peek32(0x80008D1Cu) == 0x803A1258u &&
                        test.memory.peek32(0x80008D20u) == 0x0009A874u && kSite + 4u < 0x803A1258u,
                        "original exception fragment changed");
        unsigned pairs = 0;
        for (u32 descriptor : descriptors) for (bool caught : {false, true})
            for (unsigned seed : {0u, 1u, 19u}) for (bool cold : {false, true}) {
                dynamic_descriptor = descriptor; cold_entry = cold;
                const auto original = test.run(false, caught, 0, 1, seed, false, run_caller);
                const auto native = test.run(true, caught, 0, 1, seed, false, run_caller);
                oracle::require(oracle::equal(original, native),
                                "generated reference caller changed original exception-runtime behavior");
                oracle::require(original.catches == 0 && original.callbacks == 0 &&
                                original.cleanup_value == 0 && original.throws == 1 &&
                                original.terminal == oracle::kTerminate,
                                "original caller unexpectedly acquired unwind coverage");
                ++pairs;
            }
        dynamic_descriptor = 0; cold_entry = false;
        const auto good = test.run(false, true, 0, 1, 0, false, run_caller);
        corrupt_saved_link = true;
        const auto broken = test.run(true, true, 0, 1, 0, false, run_caller);
        corrupt_saved_link = false;
        oracle::require(!oracle::equal(good, broken) && broken.catches == 0,
                        "mutation failed to detect the cast's broken saved caller link");
        std::printf("PASS generated reference caller/unwind: %u paired original callers, hot/cold entry, "
                    "missing metadata and five known failing types; original termination with/without outer catch; "
                    "saved caller-link mutation rejected; %zu metadata bytes and 612 cast instruction bytes "
                    "erased in native cases; %llu original instructions. "
                    "Reference-runtime handoff; production generated unwinder remains unvalidated.\n",
                    pairs, test.metadata_size, static_cast<unsigned long long>(test.instructions));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL generated reference caller/unwind: %s\n", error.what());
        return 1;
    }
}
