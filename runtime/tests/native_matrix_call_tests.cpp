// Actual generated matrix-copy boundaries: extent validation, overlap, memory
// observation/journalling, and ABI. The independent original-word oracle lives
// in native_matrix_copy_tests and cannot enter the generated code.
#include "math/matrix.h"
#include "recomp_decls.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void check_composition() {
    using namespace gekko;
    GuestMemory memory;
    Cpu cpu(memory);
    constexpr u32 base = 0x81001000u;
    const std::array<std::pair<LiftSiteRecord::Lower, u32>, 68> sites{{
        {recomp::lift_site_80019448, 0x8001944Cu},
        {recomp::lift_site_80019490, 0x80019494u},
        {recomp::lift_site_8001951C, 0x80019520u},
        {recomp::lift_site_80019564, 0x80019568u},
        {recomp::lift_site_800195F0, 0x800195F4u},
        {recomp::lift_site_80019638, 0x8001963Cu},
        {recomp::lift_site_80019CB8, 0x80019CBCu},
        {recomp::lift_site_80019CD0, 0x80019CD4u},
        {recomp::lift_site_80019CE8, 0x80019CECu},
        {recomp::lift_site_80019D00, 0x80019D04u},
        {recomp::lift_site_80019D18, 0x80019D1Cu},
        {recomp::lift_site_8001DD80, 0x8001DD84u},
        {recomp::lift_site_8001DE24, 0x8001DE28u},
        {recomp::lift_site_8001E0CC, 0x8001E0D0u},
        {recomp::lift_site_8001E0F0, 0x8001E0F4u},
        {recomp::lift_site_8001E114, 0x8001E118u},
        {recomp::lift_site_8001E13C, 0x8001E140u},
        {recomp::lift_site_8001E160, 0x8001E164u},
        {recomp::lift_site_8001E184, 0x8001E188u},
        {recomp::lift_site_8001E1AC, 0x8001E1B0u},
        {recomp::lift_site_8001E1D0, 0x8001E1D4u},
        {recomp::lift_site_8001E1F4, 0x8001E1F8u},
        {recomp::lift_site_8002B77C, 0x8002B780u},
        {recomp::lift_site_8002ED4C, 0x8002ED50u},
        {recomp::lift_site_8002F7E0, 0x8002F7E4u},
        {recomp::lift_site_80030FA4, 0x80030FA8u},
        {recomp::lift_site_80031768, 0x8003176Cu},
        {recomp::lift_site_80031FE8, 0x80031FECu},
        {recomp::lift_site_80032A98, 0x80032A9Cu},
        {recomp::lift_site_8005F2BC, 0x8005F2C0u},
        {recomp::lift_site_800636A8, 0x800636ACu},
        {recomp::lift_site_800A722C, 0x800A7230u},
        {recomp::lift_site_800B2864, 0x800B2868u},
        {recomp::lift_site_800B2910, 0x800B2914u},
        {recomp::lift_site_800B29B8, 0x800B29BCu},
        {recomp::lift_site_8015E3F0, 0x8015E3F4u},
        {recomp::lift_site_8018FBB8, 0x8018FBBCu},
        {recomp::lift_site_80194E30, 0x80194E34u},
        {recomp::lift_site_80196980, 0x80196984u},
        {recomp::lift_site_801FC260, 0x801FC264u},
        {recomp::lift_site_801FD54C, 0x801FD550u},
        {recomp::lift_site_8028879C, 0x802887A0u},
        {recomp::lift_site_802ACD44, 0x802ACD48u},
        {recomp::lift_site_802AE0EC, 0x802AE0F0u},
        {recomp::lift_site_802B1484, 0x802B1488u},
        {recomp::lift_site_80357350, 0x80357354u},
        {recomp::lift_site_8040C880, 0x8040C884u},
        {recomp::lift_site_8040C8BC, 0x8040C8C0u},
        {recomp::lift_site_8040C8F8, 0x8040C8FCu},
        {recomp::lift_site_8040C934, 0x8040C938u},
        {recomp::lift_site_804126DC, 0x804126E0u},
        {recomp::lift_site_80412730, 0x80412734u},
        {recomp::lift_site_80418780, 0x80418784u},
        {recomp::lift_site_804187F8, 0x804187FCu},
        {recomp::lift_site_8041890C, 0x80418910u},
        {recomp::lift_site_80418EFC, 0x80418F00u},
        {recomp::lift_site_80418FB8, 0x80418FBCu},
        {recomp::lift_site_804190DC, 0x804190E0u},
        {recomp::lift_site_80419218, 0x8041921Cu},
        {recomp::lift_site_804193AC, 0x804193B0u},
        {recomp::lift_site_80419438, 0x8041943Cu},
        {recomp::lift_site_804194A4, 0x804194A8u},
        {recomp::lift_site_80419588, 0x8041958Cu},
        {recomp::lift_site_804195E8, 0x804195ECu},
        {recomp::lift_site_8041AA90, 0x8041AA94u},
        {recomp::lift_site_8041BAC0, 0x8041BAC4u},
        {recomp::lift_site_8041F15C, 0x8041F160u},
        {recomp::lift_site_8041FF04, 0x8041FF08u},
    }};
    unsigned cases = 0, refused = 0;
    for (const auto& site : sites) for (int offset : {-48,-44,-28,-12,-4,0,4,12,28,44,48})
        for (bool flush : {false,true}) for (bool journal : {false,true}) for (bool compiled_call : {false,true}) {
            std::array<float,64> expected{};
            for (unsigned i = 0; i < expected.size(); ++i) expected[i] = float(int(i * 71) % 127 - 63) / 32.0f;
            const unsigned left = unsigned(24 + offset / 4), right = 24;
            for (unsigned i = 0; i < expected.size(); ++i) {
                u32 bits; std::memcpy(&bits, &expected[i], 4); memory.write32(base + i * 4, bits);
            }
            shadow::math::compose_affine_matrix(expected.data() + left, expected.data() + right,
                flush ? shadow::math::SubnormalMode::Flush : shadow::math::SubnormalMode::Preserve);
            cpu.reset(); cpu.gpr[3] = base + left * 4; cpu.gpr[4] = base + right * 4;
            cpu.gpr[1] = 0x817E0000u; cpu.fpscr = flush ? 4u : 0u;
            for (unsigned i = 14; i < 32; ++i) {
                cpu.gpr[i] = i * 31;
                cpu.fpr[i].bits[0] = 0x1234567800000000ull + i;
                cpu.fpr[i].bits[1] = 0x2345678900000000ull + i;
            }
            std::array<u8,256> initial{};
            memory.peek_block(base, initial.data(), initial.size());
            unsigned reads = 0, writes = 0;
            memory.set_access_trace([&](u32 at, std::size_t size, bool writing) {
                require(size == 48 && (writing ? at == cpu.gpr[3] : at == cpu.gpr[3] || at == cpu.gpr[4]),
                        "composition observed an unexpected span");
                (writing ? writes : reads) += unsigned(size);
            });
            if (journal) memory.journal_begin();
            const auto tick = cpu.game_clock().now();
            if (compiled_call) {
                require(!recomp::call_matrix_compose(cpu, site.second - 4u) && cpu.pc == site.second,
                        "valid compiled composition cut or published the wrong continuation");
            } else site.first(cpu);
            memory.set_access_trace({});
            if (journal) memory.journal_end();
            require(!cpu.stopped() && cpu.lr == site.second && cpu.gpr[1] == 0x817E0000u &&
                    cpu.game_clock().now() == tick && cpu.fpscr == (flush ? 4u : 0u), "composition ABI/time failed");
            require(reads == 96 && writes == 48, "composition spans were not observed");
            for (unsigned i = 14; i < 32; ++i)
                require(cpu.gpr[i] == i * 31 && cpu.fpr[i].bits[0] == 0x1234567800000000ull + i &&
                        cpu.fpr[i].bits[1] == 0x2345678900000000ull + i, "composition clobbered nonvolatile state");
            for (unsigned i = 0; i < expected.size(); ++i) {
                u32 bits; std::memcpy(&bits, &expected[i], 4);
                require(memory.peek32(base + i * 4) == bits, "composition endian/overlap boundary failed");
            }
            if (journal) {
                memory.journal_rollback(); std::array<u8,256> actual{};
                memory.peek_block(base, actual.data(), actual.size());
                require(actual == initial, "composition write escaped journal rollback");
            }
            ++cases;
        }
    for (const auto& site : sites) for (unsigned invalid = 0; invalid < 4; ++invalid) for (bool compiled_call : {false,true}) {
        cpu.reset(); cpu.gpr[3] = base; cpu.gpr[4] = base + 64;
        cpu.gpr[3 + (invalid & 1)] = invalid < 2 ? 0u : base + 1;
        std::array<u8,256> before{}, after{};
        memory.peek_block(base, before.data(), before.size());
        if (compiled_call) require(recomp::call_matrix_compose(cpu, site.second - 4u) && cpu.pc == site.second,
                                   "failed compiled composition did not unwind at its continuation");
        else site.first(cpu);
        memory.peek_block(base, after.data(), after.size());
        require(cpu.stop().reason == StopReason::ExecutionFault && before == after,
                "invalid composition extent accepted or partially written");
        ++refused;
    }
    for (const auto& site : sites) {
        cpu.reset(); cpu.set_unmapped_access_stops(true);
        cpu.gpr[3] = base; cpu.gpr[4] = base + 64u;
        cpu.pc = base; cpu.lr = 17u;
        (void)cpu.mem_read32(0u);
        require(cpu.memory_fault().latched, "composition cut did not latch prior fault");
        const u32 pc = cpu.pc, lr = cpu.lr;
        unsigned accesses = 0;
        memory.set_access_trace([&](u32, std::size_t, bool) { ++accesses; });
        require(recomp::call_matrix_compose(cpu, site.second - 4u), "prior fault did not cut composition");
        memory.set_access_trace({});
        require(cpu.pc == pc && cpu.lr == lr && accesses == 0, "composition cut changed state or accessed data");
        cpu.set_unmapped_access_stops(false);
    }
    std::printf("PASS 68 generated composition sites: %u overlap/endian/journal cases; %u invalid-span refusals\n", cases, refused);
}
}

int main(int argc, char** argv) {
    using namespace gekko;
    try {
        check_composition();
        GuestMemory memory;
        Cpu cpu(memory);
        constexpr u32 base = 0x81001000u, stack = 0x817E0000u;
        require(argc == 2, "usage: native_matrix_call_tests lift-sites-math.txt");
        std::ifstream catalog(argv[1]);
        require(bool(catalog), "missing native math site catalog");
        std::map<u32, u32> copy_words;
        std::string row;
        while (std::getline(catalog, row)) {
            std::istringstream fields(row);
            std::string directive, address, word, kind;
            if (!(fields >> directive >> address >> word >> kind) || kind != "affine_matrix_copy") continue;
            require(directive == "site", "invalid copy-site catalog directive");
            require(copy_words.emplace(u32(std::stoul(address, nullptr, 16)),
                                       u32(std::stoul(word, nullptr, 16))).second, "duplicate copy test site");
        }
        // These are lowered call records, never execution of their former target.
        // Read the maintained catalog instead of rooting retired code with test-only
        // target literals. Independent DOL/edge checks pin its complete membership.
        std::vector<std::pair<LiftSiteRecord::Lower, u32>> sites;
        for (std::size_t i = 0; i < recomp::kLiftSiteCount; ++i) {
            const auto& site = recomp::kLiftSites[i];
            const auto expected = copy_words.find(site.address);
            if (expected == copy_words.end()) continue;
            require(expected->second == recomp::kLiftSiteWords[i], "copy-site generated word differs from catalog");
            sites.emplace_back(site.lower, site.address + 4u);
        }
        require(sites.size() == 684u && sites.size() == copy_words.size(),
                "all 684 copy-family sites must be present");
        std::array<u8, 256> initial{};
        for (unsigned i = 0; i < initial.size(); ++i) initial[i] = u8(i * 71u);
        unsigned cases = 0;
        for (const auto& site : sites) for (int offset : {-48, -4, 0, 4, 44, 48})
            for (bool journal : {false, true}) for (bool compiled_call : {false, true}) {
                memory.write_block(base, initial.data(), initial.size());
                cpu.reset();
                cpu.gpr[1] = stack;
                cpu.gpr[3] = base + 96u + offset;
                cpu.gpr[4] = base + 96u;
                for (u32 i = 14; i < 32; ++i) {
                    cpu.gpr[i] = 0x12131400u + i;
                    cpu.fpr[i].bits[0] = 0xdeadbeef12345000ull + i;
                }
                unsigned read_bytes = 0, written_bytes = 0;
                memory.set_access_trace([&](u32 address, std::size_t size, bool writing) {
                    require(address == (writing ? cpu.gpr[3] : cpu.gpr[4]) && size == 48u,
                            "boundary observed an unexpected extent");
                    (writing ? written_bytes : read_bytes) += unsigned(size);
                });
                if (journal) memory.journal_begin();
                const auto time = cpu.game_clock().now();
                if (compiled_call) {
                    require(!recomp::call_matrix_copy(cpu, site.second - 4u) && cpu.pc == site.second,
                            "valid compiled copy unexpectedly cut or published the wrong continuation");
                } else site.first(cpu);
                memory.set_access_trace({});
                if (journal) memory.journal_end();
                require(!cpu.stopped() && cpu.lr == site.second && cpu.gpr[1] == stack &&
                        cpu.gpr[3] == base + 96u + offset && cpu.gpr[4] == base + 96u &&
                        cpu.game_clock().now() == time, "matrix call ABI or game time changed");
                require(read_bytes == 48 && written_bytes == 48, "source/destination observation missing");
                for (u32 i = 14; i < 32; ++i)
                    require(cpu.gpr[i] == 0x12131400u + i && cpu.fpr[i].bits[0] == 0xdeadbeef12345000ull + i,
                            "matrix call changed nonvolatile state");
                auto expected = initial;
                shadow::math::copy_affine_matrix(expected.data() + 96 + offset, expected.data() + 96);
                std::array<u8, 256> actual{};
                memory.peek_block(base, actual.data(), actual.size());
                require(actual == expected, "generated boundary changed matrix or adjacent bytes");
                if (journal) {
                    memory.journal_rollback();
                    memory.peek_block(base, actual.data(), actual.size());
                    require(actual == initial, "boundary write is not fully reversible by the journal");
                }
                ++cases;
            }
        for (const auto& site : sites) for (bool invalid_source : {false, true})
            for (bool compiled_call : {false, true}) {
            memory.write_block(base, initial.data(), initial.size());
            cpu.reset();
            cpu.gpr[3] = invalid_source ? base : 0u;
            cpu.gpr[4] = invalid_source ? 0u : base;
            if (compiled_call) {
                require(recomp::call_matrix_copy(cpu, site.second - 4u) && cpu.pc == site.second,
                        "failed compiled copy did not unwind at its continuation");
            } else site.first(cpu);
            std::array<u8, 256> actual{};
            memory.peek_block(base, actual.data(), actual.size());
            require(cpu.stop().reason == StopReason::ExecutionFault && actual == initial,
                    "invalid span was accepted or partially changed memory");
        }
        // A fault from an earlier instruction must cut before LR, PC or any
        // native buffer is touched, including when the copy arguments are valid.
        for (const auto& site : sites) {
            memory.write_block(base, initial.data(), initial.size());
            cpu.reset(); cpu.set_unmapped_access_stops(true);
            cpu.gpr[3] = base + 64u; cpu.gpr[4] = base;
            cpu.pc = base; cpu.lr = 17u;
            (void)cpu.mem_read32(0u);
            require(cpu.memory_fault().latched, "cut test did not latch the prior memory fault");
            const u32 pc_before = cpu.pc, lr_before = cpu.lr;
            unsigned accesses = 0;
            memory.set_access_trace([&](u32, std::size_t, bool) { ++accesses; });
            require(recomp::call_matrix_copy(cpu, site.second - 4u), "prior fault did not cut compiled copy");
            memory.set_access_trace({});
            std::array<u8, 256> actual{};
            memory.peek_block(base, actual.data(), actual.size());
            require(actual == initial && accesses == 0 && cpu.pc == pc_before && cpu.lr == lr_before,
                    "cut copy accessed storage or changed the prior fault continuation");
        }
        std::printf("PASS 684 generated matrix-copy sites: %u overlap/observation/journal cases; 2736 invalid-span refusals; 684 prior-fault cuts\n", cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL generated matrix callers: %s\n", error.what());
        return 1;
    }
}
